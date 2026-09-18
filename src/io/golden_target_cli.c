// SPDX-License-Identifier: GPL-3.0-only

#include "common/common.h"
#include "common/file_io.h"
#include "common/shader_stage.h"
#include "common/windows_utf8.h"
#include "dxbc/dxbc_compare.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_parser.h"
#include "dxbc/dxbc_stage_contract.h"
#include "dxbc/usbd.h"
#include "io/bundle_archive.h"
#include "io/serialized_file.h"
#include "io/serialized_shader.h"
#include "io/shader_blob_archive.h"
#include "io/subprogram_metadata.h"
#include "io/typetree_schema_registry.h"
#include "io/unity_input.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    UNITY_PLATFORM_D3D11 = 4,
    GOLDEN_STAGE_COUNT = 5,
};

typedef enum {
    ACTION_INVALID = 0,
    ACTION_EXTRACT,
    ACTION_VERIFY,
} Action;

typedef struct {
    Action action;
    const char* shader_name;
    bool has_path_id;
    int64_t path_id;
    int subshader_index;
    int pass_index;
    const char* schema_registry_path;
    const char* target_path;
    const char** bundle_paths;
    size_t bundle_count;
} CommandLine;

typedef struct {
    uint8_t* bytes;
    size_t size;
    char* shader_name;
    int64_t path_id;
    int subshader_index;
    int pass_index;
    size_t record_count;
} ExtractedTarget;

typedef struct {
    UnitySerializedProgramStage serialized_stage;
    const char* record_name;
} GoldenStage;

static const GoldenStage k_golden_stages[GOLDEN_STAGE_COUNT] = {
    {UNITY_SERIALIZED_STAGE_VERTEX, "vertex"},
    {UNITY_SERIALIZED_STAGE_FRAGMENT, "fragment"},
    {UNITY_SERIALIZED_STAGE_HULL, "hull"},
    {UNITY_SERIALIZED_STAGE_DOMAIN, "domain"},
    {UNITY_SERIALIZED_STAGE_GEOMETRY, "geometry"},
};

static void print_usage(FILE* output, const char* executable) {
    fprintf(output,
            "Usage:\n"
            "  %s extract (--shader NAME | --path-id ID) --output FILE "
            "[selection] INPUT...\n"
            "  %s verify  (--shader NAME | --path-id ID) --target FILE "
            "[selection] INPUT...\n"
            "\n"
            "Selection:\n"
            "  --subshader N --pass N       select an exact serialized pass\n"
            "  --schema-registry FILE       import pinned TypeTree schemas\n"
            "\n"
            "Without explicit pass coordinates, the first serialized pass "
            "containing a D3D11\n"
            "subprogram is selected and its coordinates are reported. "
            "Shader names must be unique\n"
            "across all input bundles; use --path-id to resolve ambiguity. "
            "extract atomically\n"
            "creates a new USBD file and never overwrites an existing path. "
            "verify performs\n"
            "an exact full-byte comparison against an existing USBD file.\n"
            "\n"
            "This tool extracts compiled target evidence only. ShaderLab "
            "source, compiler defines,\n"
            "and platform capabilities are separate authorities and are "
            "never guessed here.\n",
            executable, executable);
}

static bool parse_i64(const char* text, int64_t* value) {
    if (!text || !value || text[0] == '\0') return false;
    errno = 0;
    char* end = NULL;
    long long parsed = strtoll(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *value = (int64_t)parsed;
    return true;
}

static bool parse_index(const char* text, int* value) {
    int64_t parsed = 0;
    if (!parse_i64(text, &parsed) || parsed < 0 || parsed > INT_MAX) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool consume_value(int argc, char** argv, int* index,
                          const char* option, const char** value) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '\0') {
        fprintf(stderr, "golden target: %s requires a value\n", option);
        return false;
    }
    *value = argv[++*index];
    return true;
}

static bool parse_command_line(int argc, char** argv, CommandLine* options) {
    if (!options) return false;
    memset(options, 0, sizeof(*options));
    options->subshader_index = -1;
    options->pass_index = -1;
    if (argc < 2) return false;
    options->bundle_paths = (const char**)calloc(
        (size_t)argc, sizeof(*options->bundle_paths));
    if (!options->bundle_paths) return false;

    if (strcmp(argv[1], "extract") == 0) {
        options->action = ACTION_EXTRACT;
    } else if (strcmp(argv[1], "verify") == 0) {
        options->action = ACTION_VERIFY;
    } else if (strcmp(argv[1], "--help") == 0 ||
               strcmp(argv[1], "-h") == 0) {
        print_usage(stdout, argv[0]);
        free(options->bundle_paths);
        options->bundle_paths = NULL;
        exit(0);
    } else {
        fprintf(stderr, "golden target: expected extract or verify\n");
        return false;
    }

    bool saw_shader = false;
    bool saw_path_id = false;
    bool saw_subshader = false;
    bool saw_pass = false;
    bool saw_schema = false;
    bool saw_target = false;
    for (int index = 2; index < argc; ++index) {
        const char* argument = argv[index];
        const char* value = NULL;
        if (strcmp(argument, "--shader") == 0) {
            if (saw_shader ||
                !consume_value(argc, argv, &index, argument, &value)) {
                return false;
            }
            saw_shader = true;
            options->shader_name = value;
        } else if (strcmp(argument, "--path-id") == 0) {
            if (saw_path_id ||
                !consume_value(argc, argv, &index, argument, &value) ||
                !parse_i64(value, &options->path_id)) {
                fprintf(stderr, "golden target: invalid --path-id\n");
                return false;
            }
            saw_path_id = true;
            options->has_path_id = true;
        } else if (strcmp(argument, "--subshader") == 0) {
            if (saw_subshader ||
                !consume_value(argc, argv, &index, argument, &value) ||
                !parse_index(value, &options->subshader_index)) {
                fprintf(stderr, "golden target: invalid --subshader\n");
                return false;
            }
            saw_subshader = true;
        } else if (strcmp(argument, "--pass") == 0) {
            if (saw_pass ||
                !consume_value(argc, argv, &index, argument, &value) ||
                !parse_index(value, &options->pass_index)) {
                fprintf(stderr, "golden target: invalid --pass\n");
                return false;
            }
            saw_pass = true;
        } else if (strcmp(argument, "--schema-registry") == 0) {
            if (saw_schema ||
                !consume_value(argc, argv, &index, argument, &value)) {
                return false;
            }
            saw_schema = true;
            options->schema_registry_path = value;
        } else if (strcmp(argument, "--output") == 0 ||
                   strcmp(argument, "--target") == 0) {
            if (saw_target ||
                !consume_value(argc, argv, &index, argument, &value)) {
                return false;
            }
            saw_target = true;
            options->target_path = value;
            if ((options->action == ACTION_EXTRACT &&
                 strcmp(argument, "--output") != 0) ||
                (options->action == ACTION_VERIFY &&
                 strcmp(argument, "--target") != 0)) {
                fprintf(stderr,
                        "golden target: extract uses --output; verify uses "
                        "--target\n");
                return false;
            }
        } else if (strcmp(argument, "--help") == 0 ||
                   strcmp(argument, "-h") == 0) {
            print_usage(stdout, argv[0]);
            free(options->bundle_paths);
            options->bundle_paths = NULL;
            exit(0);
        } else if (strncmp(argument, "--", 2u) == 0) {
            fprintf(stderr, "golden target: unknown option: %s\n", argument);
            return false;
        } else {
            options->bundle_paths[options->bundle_count++] = argument;
        }
    }

    if ((!saw_shader && !saw_path_id) || !saw_target ||
        options->bundle_count == 0u || saw_subshader != saw_pass) {
        fprintf(stderr,
                "golden target: a shader selector, target path, bundle, and "
                "both or neither pass coordinates are required\n");
        return false;
    }
    return true;
}

static void command_line_dispose(CommandLine* options) {
    if (!options) return;
    free(options->bundle_paths);
    options->bundle_paths = NULL;
    options->bundle_count = 0u;
}

static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(size + 1u);
    if (!copy) return NULL;
    memcpy(copy, value, size + 1u);
    return copy;
}

static void extracted_target_dispose(ExtractedTarget* target) {
    if (!target) return;
    if (target->bytes) mem_free(target->bytes, target->size);
    free(target->shader_name);
    memset(target, 0, sizeof(*target));
}

static bool pass_has_d3d11_subprogram(const SerializedPass* pass) {
    if (!pass) return false;
    for (int stage = 0; stage < GOLDEN_STAGE_COUNT; ++stage) {
        int serialized_stage = (int)k_golden_stages[stage].serialized_stage;
        if (pass->subprogram_count[serialized_stage] < 0) return false;
        for (int index = 0;
             index < pass->subprogram_count[serialized_stage]; ++index) {
            if (serialized_pass_subprogram_is_platform(
                    pass, serialized_stage, index,
                    UNITY_PLATFORM_D3D11)) {
                return true;
            }
        }
    }
    return false;
}

static const SerializedPass* select_pass(
    const SerializedShader* shader, int requested_subshader,
    int requested_pass, int* selected_subshader, int* selected_pass) {
    if (!shader || !selected_subshader || !selected_pass) return NULL;
    if (requested_subshader >= 0) {
        if (requested_subshader >= shader->subshader_count) return NULL;
        const SerializedSubShader* subshader =
            &shader->subshaders[requested_subshader];
        if (requested_pass < 0 || requested_pass >= subshader->pass_count) {
            return NULL;
        }
        const SerializedPass* pass = &subshader->passes[requested_pass];
        if (!pass_has_d3d11_subprogram(pass)) return NULL;
        *selected_subshader = requested_subshader;
        *selected_pass = requested_pass;
        return pass;
    }

    for (int subshader_index = 0;
         subshader_index < shader->subshader_count; ++subshader_index) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_index];
        for (int pass_index = 0; pass_index < subshader->pass_count;
             ++pass_index) {
            const SerializedPass* pass = &subshader->passes[pass_index];
            if (!pass_has_d3d11_subprogram(pass)) continue;
            *selected_subshader = subshader_index;
            *selected_pass = pass_index;
            return pass;
        }
    }
    return NULL;
}

static bool validate_selected_dxbc(
    const SerializedPass* pass, UnitySerializedProgramStage serialized_stage,
    const SerializedSubProgram* subprogram,
    const PlayerSubProgramMetadata* player,
    const DXBCContainerView* container) {
    if (!pass || !subprogram || !player || !container ||
        player->program_type != subprogram->program_type) {
        return false;
    }
    DXBCDocument document;
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContract contract;
    DXBCStageContractDiagnostic contract_diagnostic;
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    bool valid = dxbc_document_parse(&document, container->data,
                                     container->size,
                                     &document_diagnostic) &&
        dxbc_stage_contract_decode_document(&document, &contract,
                                            &contract_diagnostic);
    if (!valid) {
        dxbc_stage_contract_free(&contract);
        dxbc_document_free(&document);
        return false;
    }

    UnityCompilerProgramStage compiler_stage;
    ShaderStageTuple tuple;
    memset(&tuple, 0, sizeof(tuple));
    valid = shader_stage_serialized_to_compiler(serialized_stage,
                                                &compiler_stage);
    if (valid) {
        tuple.serialized_stage = serialized_stage;
        tuple.compiler_program = compiler_stage;
        tuple.serialized_program_mask = pass->program_mask;
        tuple.gpu_program_type =
            (UnityGPUProgramType)player->program_type;
        tuple.dxbc_program_type = contract.program_type;
        tuple.shader_model_major = contract.shader_model_major;
        tuple.shader_model_minor = contract.shader_model_minor;
        valid = shader_stage_validate_d3d11_tuple(&tuple) ==
                SHADER_STAGE_TUPLE_OK;
    }
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    return valid;
}

static bool extract_pass_target(const SerializedShader* shader,
                                const TypeTreeValue* shader_value,
                                int requested_subshader, int requested_pass,
                                ExtractedTarget* result) {
    if (!shader || !shader_value || !result) return false;
    int selected_subshader = -1;
    int selected_pass = -1;
    const SerializedPass* pass = select_pass(
        shader, requested_subshader, requested_pass, &selected_subshader,
        &selected_pass);
    if (!pass) {
        fprintf(stderr,
                "golden target: selected Shader has no D3D11 subprograms "
                "at the requested pass\n");
        return false;
    }

    ShaderBlobArchive archive;
    if (!shader_blob_archive_open(shader_value, UNITY_PLATFORM_D3D11,
                                  &archive)) {
        fprintf(stderr,
                "golden target: selected Shader has no exact D3D11 blob "
                "archive\n");
        return false;
    }

    bool success = false;
    DXBCUSBDRecordView records[GOLDEN_STAGE_COUNT];
    size_t record_count = 0u;
    memset(records, 0, sizeof(records));
    for (int stage_index = 0; stage_index < GOLDEN_STAGE_COUNT;
         ++stage_index) {
        UnitySerializedProgramStage serialized_stage =
            k_golden_stages[stage_index].serialized_stage;
        int stage = (int)serialized_stage;
        const SerializedSubProgram* selected = NULL;
        int selected_index = -1;
        for (int subprogram_index = 0;
             subprogram_index < pass->subprogram_count[stage];
             ++subprogram_index) {
            if (!serialized_pass_subprogram_is_platform(
                    pass, stage, subprogram_index,
                    UNITY_PLATFORM_D3D11)) {
                continue;
            }
            selected = &pass->subprograms[stage][subprogram_index];
            selected_index = subprogram_index;
            break;
        }
        if (!selected) continue;
        if (selected->blob_index < 0 ||
            selected->blob_index >= archive.entry_count) {
            fprintf(stderr,
                    "golden target: %s subprogram %d has invalid blob index "
                    "%d\n",
                    k_golden_stages[stage_index].record_name, selected_index,
                    selected->blob_index);
            goto cleanup;
        }

        const uint8_t* payload = NULL;
        size_t payload_size = 0u;
        if (!shader_blob_archive_get(&archive, selected->blob_index,
                                     &payload, &payload_size)) {
            fprintf(stderr,
                    "golden target: cannot read %s blob index %d\n",
                    k_golden_stages[stage_index].record_name,
                    selected->blob_index);
            goto cleanup;
        }
        ByteStream stream;
        PlayerSubProgramMetadata player;
        stream_init(&stream, payload, payload_size);
        stream_set_endian(&stream, false);
        if (!subprogram_metadata_parse_variant(&stream, &player)) {
            fprintf(stderr,
                    "golden target: cannot parse %s player subprogram %d\n",
                    k_golden_stages[stage_index].record_name,
                    selected_index);
            goto cleanup;
        }
        DXBCContainerView container;
        bool valid = player.has_player_blob_header && player.bytecode &&
            dxbc_container_view_first(player.bytecode,
                                      player.bytecode_length, &container);
        if (valid) {
            size_t prefix_size = (size_t)(container.data - player.bytecode);
            valid = prefix_size <= player.bytecode_length &&
                container.size == player.bytecode_length - prefix_size;
        }
        valid = valid &&
            validate_selected_dxbc(pass, serialized_stage, selected, &player,
                                   &container);
        if (!valid) {
            fprintf(stderr,
                    "golden target: %s TypeTree, player wrapper, and DXBC "
                    "stage authorities disagree\n",
                    k_golden_stages[stage_index].record_name);
            subprogram_metadata_free_variant(&player);
            goto cleanup;
        }
        records[record_count].name = (const uint8_t*)
            k_golden_stages[stage_index].record_name;
        records[record_count].name_size = strlen(
            k_golden_stages[stage_index].record_name);
        records[record_count].dxbc = container.data;
        records[record_count].dxbc_size = container.size;
        ++record_count;
        subprogram_metadata_free_variant(&player);
    }

    if (record_count == 0u) {
        fprintf(stderr, "golden target: selected pass has no D3D11 records\n");
        goto cleanup;
    }
    DXBCUSBDDiagnostic diagnostic;
    if (!dxbc_usbd_table_encode(records, record_count, &result->bytes,
                                &result->size, &diagnostic)) {
        fprintf(stderr,
                "golden target: cannot encode USBD: %s record=%" PRIu32
                " byte=%zu\n",
                dxbc_usbd_status_name(diagnostic.status),
                diagnostic.record_index, diagnostic.byte_offset);
        goto cleanup;
    }
    result->subshader_index = selected_subshader;
    result->pass_index = selected_pass;
    result->record_count = record_count;
    success = true;

cleanup:
    shader_blob_archive_close(&archive);
    return success;
}

typedef enum {
    OBJECT_SCAN_ERROR = -1,
    OBJECT_SCAN_NO_MATCH = 0,
    OBJECT_SCAN_MATCH = 1,
} ObjectScanResult;

static ObjectScanResult scan_shader_object(
    SerializedFile* file, const AssetObjectInfo* object,
    const CommandLine* options, ExtractedTarget* target) {
    if (!file || !object || !options || !target ||
        object->type_id_or_index < 0 ||
        object->type_id_or_index >= file->type_count) {
        return OBJECT_SCAN_ERROR;
    }
    size_t object_size = 0u;
    const uint8_t* object_data = serialized_file_get_object_data(
        file, object, &object_size);
    if (!object_data || object_size == 0u) return OBJECT_SCAN_ERROR;

    const TypeTreeType* schema = &file->types[object->type_id_or_index];
    TypeTreeValue shader_value;
    memset(&shader_value, 0, sizeof(shader_value));
    ByteStream stream;
    stream_init(&stream, object_data, object_size);
    stream_set_endian(&stream, file->big_endian);
    int node_index = 0;
    if (!typetree_parse_value_ex(schema, &node_index, &stream, &shader_value,
                                 TYPETREE_PARSE_PACK_COMPRESSED_BLOB)) {
        return OBJECT_SCAN_ERROR;
    }
    ObjectScanResult result = OBJECT_SCAN_ERROR;
    if (stream.position != object_size || node_index != schema->node_count) {
        goto cleanup_value;
    }

    SerializedShaderSchemaProfile profile;
    if (!serialized_shader_profile_from_unity_version(file->unity_version,
                                                       &profile)) {
        goto cleanup_value;
    }
    SerializedShader shader;
    serialized_shader_init(&shader);
    if (!serialized_shader_parse_with_profile(&shader, &shader_value,
                                               profile)) {
        goto cleanup_value;
    }

    bool name_matches = !options->shader_name ||
        (shader.name && strcmp(shader.name, options->shader_name) == 0);
    bool path_matches = !options->has_path_id ||
        object->path_id == options->path_id;
    if (!name_matches || !path_matches) {
        result = OBJECT_SCAN_NO_MATCH;
        goto cleanup_shader;
    }
    if (!shader.name || shader.name[0] == '\0' ||
        !extract_pass_target(&shader, &shader_value,
                             options->subshader_index, options->pass_index,
                             target)) {
        goto cleanup_shader;
    }
    target->shader_name = duplicate_string(shader.name);
    target->path_id = object->path_id;
    if (!target->shader_name) {
        extracted_target_dispose(target);
        goto cleanup_shader;
    }
    result = OBJECT_SCAN_MATCH;

cleanup_shader:
    serialized_shader_free(&shader);
cleanup_value:
    typetree_free_value(&shader_value);
    return result;
}

static bool scan_serialized_file(
    const uint8_t* bytes, size_t size, const char* member_name,
    const CommandLine* options, TypeTreeSchemaRegistry* registry,
    ExtractedTarget* target, size_t* match_count) {
    SerializedFile file;
    if (!serialized_file_open_with_schema_registry(
            &file, bytes, size, registry)) {
        fprintf(stderr,
                "golden target: cannot parse SerializedFile member %s; "
                "a matching pinned --schema-registry may be required\n",
                member_name ? member_name : "<unnamed>");
        return false;
    }
    bool success = true;
    for (int object_index = 0; object_index < file.object_count;
         ++object_index) {
        const AssetObjectInfo* object = &file.objects[object_index];
        if (object->type_id != 48) continue;
        ExtractedTarget candidate;
        memset(&candidate, 0, sizeof(candidate));
        ObjectScanResult result = scan_shader_object(
            &file, object, options, &candidate);
        if (result == OBJECT_SCAN_ERROR) {
            fprintf(stderr,
                    "golden target: malformed Shader object path=%" PRId64
                    " in member %s\n",
                    object->path_id,
                    member_name ? member_name : "<unnamed>");
            extracted_target_dispose(&candidate);
            success = false;
            break;
        }
        if (result != OBJECT_SCAN_MATCH) continue;
        ++*match_count;
        if (*match_count == 1u) {
            *target = candidate;
        } else {
            fprintf(stderr,
                    "golden target: selector is ambiguous (another match: "
                    "%s path=%" PRId64 ")\n",
                    candidate.shader_name, candidate.path_id);
            extracted_target_dispose(&candidate);
            success = false;
            break;
        }
    }
    serialized_file_close(&file);
    return success;
}

typedef struct {
    const CommandLine* options;
    TypeTreeSchemaRegistry* registry;
    ExtractedTarget* target;
    size_t* match_count;
} GoldenInputVisitor;

static bool scan_input_source(const UnitySerializedSource* source,
                              void* opaque) {
    GoldenInputVisitor* visitor = (GoldenInputVisitor*)opaque;
    return source && visitor && source->data && source->size > 0U &&
        scan_serialized_file(source->data, source->size,
                             source->member_name, visitor->options,
                             visitor->registry, visitor->target,
                             visitor->match_count);
}

static bool scan_input(const char* path, const CommandLine* options,
                       TypeTreeSchemaRegistry* registry,
                       ExtractedTarget* target, size_t* match_count) {
    GoldenInputVisitor visitor = {
        .options = options,
        .registry = registry,
        .target = target,
        .match_count = match_count,
    };
    UnityInputVisitStats stats;
    const UnityInputStatus status = unity_input_visit_serialized(
        path, scan_input_source, &visitor, &stats);
    if (status == UNITY_INPUT_OK) return true;
    fprintf(stderr, "golden target: cannot visit input %s: %s\n",
            path, unity_input_status_name(status));
    return false;
}

static bool extract_target(const CommandLine* options,
                           ExtractedTarget* target) {
    if (!options || !target) return false;
    memset(target, 0, sizeof(*target));
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    if (options->schema_registry_path) {
        TypeTreeSchemaStatus status =
            typetree_schema_registry_import_file_replace(
                &registry, options->schema_registry_path);
        if (status != TYPETREE_SCHEMA_OK) {
            fprintf(stderr,
                    "golden target: cannot import schema registry %s: %s\n",
                    options->schema_registry_path,
                    typetree_schema_status_name(status));
            typetree_schema_registry_dispose(&registry);
            return false;
        }
    }
    TypeTreeSchemaRegistry* registry_ptr = options->schema_registry_path
        ? &registry : NULL;
    size_t match_count = 0u;
    bool success = true;
    for (size_t index = 0u; index < options->bundle_count; ++index) {
        if (!scan_input(options->bundle_paths[index], options, registry_ptr,
                        target, &match_count)) {
            success = false;
            break;
        }
    }
    if (success && match_count == 0u) {
        fprintf(stderr, "golden target: shader selector matched nothing\n");
        success = false;
    }
    typetree_schema_registry_dispose(&registry);
    if (!success) extracted_target_dispose(target);
    return success;
}

static void report_usbd_difference(const CommonFileBytes* expected,
                                   const ExtractedTarget* actual) {
    DXBCUSBDTableView expected_table;
    DXBCUSBDTableView actual_table;
    DXBCUSBDDiagnostic diagnostic;
    if (!dxbc_usbd_table_open(&expected_table, expected->data,
                              expected->size, &diagnostic)) {
        fprintf(stderr,
                "golden target: target USBD is invalid: %s record=%" PRIu32
                " byte=%zu\n",
                dxbc_usbd_status_name(diagnostic.status),
                diagnostic.record_index, diagnostic.byte_offset);
        return;
    }
    if (!dxbc_usbd_table_open(&actual_table, actual->bytes, actual->size,
                              &diagnostic)) {
        return;
    }
    if (expected_table.record_count != actual_table.record_count) {
        fprintf(stderr,
                "golden target: record count differs: expected=%" PRIu32
                " extracted=%" PRIu32 "\n",
                expected_table.record_count, actual_table.record_count);
        return;
    }
    for (uint32_t index = 0u; index < expected_table.record_count; ++index) {
        DXBCUSBDRecordView expected_record;
        DXBCUSBDRecordView actual_record;
        if (!dxbc_usbd_table_record(&expected_table, index,
                                    &expected_record) ||
            !dxbc_usbd_table_record(&actual_table, index, &actual_record)) {
            return;
        }
        if (expected_record.name_size != actual_record.name_size ||
            memcmp(expected_record.name, actual_record.name,
                   expected_record.name_size) != 0) {
            fprintf(stderr,
                    "golden target: record %" PRIu32 " name differs\n",
                    index);
            return;
        }
        DXBCCompareResult comparison;
        DXBCCompareStatus status = dxbc_compare_exact(
            expected_record.dxbc, expected_record.dxbc_size,
            actual_record.dxbc, actual_record.dxbc_size, &comparison);
        if (status != DXBC_COMPARE_EQUAL) {
            fprintf(stderr,
                    "golden target: record %" PRIu32
                    " DXBC differs: %s byte=%zu chunk=%" PRIu32
                    " instruction=%" PRIu32 " token=%" PRIu32 "\n",
                    index, dxbc_compare_status_name(status),
                    comparison.first_differing_byte,
                    comparison.chunk_index, comparison.instruction_index,
                    comparison.token_index);
            return;
        }
    }
    size_t common_size = expected->size < actual->size
        ? expected->size : actual->size;
    size_t offset = 0u;
    while (offset < common_size && expected->data[offset] ==
                                      actual->bytes[offset]) {
        ++offset;
    }
    fprintf(stderr,
            "golden target: USBD bytes differ at byte=%zu "
            "expected-size=%zu extracted-size=%zu\n",
            offset, expected->size, actual->size);
}

static int golden_target_main(int argc, char** argv) {
    CommandLine options;
    if (!parse_command_line(argc, argv, &options)) {
        print_usage(stderr, argv[0]);
        command_line_dispose(&options);
        return 2;
    }

    ExtractedTarget target;
    if (!extract_target(&options, &target)) {
        command_line_dispose(&options);
        return 1;
    }
    int result = 0;
    if (options.action == ACTION_EXTRACT) {
        CommonFileStatus status = common_file_write_new_atomic(
            options.target_path, target.bytes, target.size);
        if (status != COMMON_FILE_OK) {
            fprintf(stderr, "golden target: cannot publish %s: %s\n",
                    options.target_path, common_file_status_name(status));
            result = 1;
        } else {
            printf("extracted %s path=%" PRId64
                   " subshader=%d pass=%d records=%zu bytes=%zu -> %s\n",
                   target.shader_name, target.path_id,
                   target.subshader_index, target.pass_index,
                   target.record_count, target.size, options.target_path);
        }
    } else {
        CommonFileBytes expected = {0};
        CommonFileStatus status = common_file_read_regular(
            options.target_path, SIZE_MAX, &expected);
        if (status != COMMON_FILE_OK) {
            fprintf(stderr, "golden target: cannot read %s: %s\n",
                    options.target_path, common_file_status_name(status));
            result = 1;
        } else if (expected.size != target.size ||
                   memcmp(expected.data, target.bytes, target.size) != 0) {
            report_usbd_difference(&expected, &target);
            result = 1;
        } else {
            printf("verified %s path=%" PRId64
                   " subshader=%d pass=%d records=%zu bytes=%zu exact\n",
                   target.shader_name, target.path_id,
                   target.subshader_index, target.pass_index,
                   target.record_count, target.size);
        }
        common_file_bytes_dispose(&expected);
    }

    extracted_target_dispose(&target);
    command_line_dispose(&options);
    if (atomic_load_explicit(&g_allocations_count,
                             memory_order_relaxed) != 0u ||
        atomic_load_explicit(&g_allocated_bytes,
                             memory_order_relaxed) != 0u) {
        fprintf(stderr,
                "golden target: tracked memory leak: %zu blocks, %zu bytes\n",
                atomic_load_explicit(&g_allocations_count,
                                     memory_order_relaxed),
                atomic_load_explicit(&g_allocated_bytes,
                                     memory_order_relaxed));
        result = 1;
    }
    return result;
}

COMMON_DEFINE_UTF8_MAIN(golden_target_main)
