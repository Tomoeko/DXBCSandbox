// SPDX-License-Identifier: GPL-3.0-only

#include "app/release_shader_certificate_job.h"

#include "common/file_io.h"
#include "common/sha256.h"
#include "common/string_builder.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    CERTIFICATE_FORMAT_NONE = 0,
    CERTIFICATE_FORMAT_TABLE,
    CERTIFICATE_FORMAT_JSON,
} CertificateFormat;

typedef struct {
    const char* expected_input;
    const char* actual_input;
    const char* pairs_path;
    const char* registry_path;
    const char* references_path;
    const char* report_path;
    CertificateFormat format;
} CliOptions;

static void print_usage(FILE* output, const char* program) {
    fprintf(output,
        "Usage:\n"
        "  %s compare --expected ORIGINAL_INPUT --actual ACTUAL_INPUT "
        "--pairs PAIRS.tsv --schema-registry REGISTRY "
        "[--reference-map REFERENCES.tsv] --format table|json "
        "--report NEW_REPORT\n\n"
        "PAIRS.tsv header (six exact columns):\n"
        "  expected_occurrence_id  expected_content_id  "
        "expected_serialized_sha256  actual_occurrence_id  "
        "actual_content_id  actual_serialized_sha256\n"
        "Columns are tab-separated. Occurrence/content IDs and source "
        "SHA-256 values are authoritative; Shader names are display-only.\n\n"
        "Optional REFERENCES.tsv header (five exact columns):\n"
        "  side  owner_serialized_sha256  file_id  path_id  "
        "stable_id_sha256\n"
        "side is expected or actual. Null PPtrs need no row. Every non-null "
        "PPtr without an exact row is authority-unavailable; raw PathID "
        "equality is never used.\n\n"
        "Exit status: 0 exact, 2 mismatch, 3 authority unavailable, "
        "1 malformed/infrastructure. Reports certify neither source nor "
        "pixels. NEW_REPORT is never overwritten.\n",
        program);
}

static bool assign_option(const char* name, const char* value,
                          const char** destination) {
    if (!value || !value[0] || *destination) {
        fprintf(stderr, "Error: %s must appear exactly once.\n", name);
        return false;
    }
    *destination = value;
    return true;
}

static bool parse_cli(int argc, char** argv, CliOptions* options,
                      bool* help) {
    memset(options, 0, sizeof(*options));
    *help = false;
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 ||
                      strcmp(argv[1], "-h") == 0)) {
        *help = true;
        return true;
    }
    if (argc < 3 || strcmp(argv[1], "compare") != 0) return false;
    for (int index = 2; index < argc; ++index) {
        const char* option = argv[index];
        if ((strcmp(option, "--help") == 0 ||
             strcmp(option, "-h") == 0) && argc == 3) {
            *help = true;
            return true;
        }
        if (index + 1 >= argc) return false;
        const char* value = argv[++index];
        if (strcmp(option, "--expected") == 0) {
            if (!assign_option(option, value, &options->expected_input))
                return false;
        } else if (strcmp(option, "--actual") == 0) {
            if (!assign_option(option, value, &options->actual_input))
                return false;
        } else if (strcmp(option, "--pairs") == 0) {
            if (!assign_option(option, value, &options->pairs_path))
                return false;
        } else if (strcmp(option, "--schema-registry") == 0) {
            if (!assign_option(option, value, &options->registry_path))
                return false;
        } else if (strcmp(option, "--reference-map") == 0) {
            if (!assign_option(option, value, &options->references_path))
                return false;
        } else if (strcmp(option, "--report") == 0) {
            if (!assign_option(option, value, &options->report_path))
                return false;
        } else if (strcmp(option, "--format") == 0) {
            if (options->format != CERTIFICATE_FORMAT_NONE) return false;
            if (strcmp(value, "table") == 0) {
                options->format = CERTIFICATE_FORMAT_TABLE;
            } else if (strcmp(value, "json") == 0) {
                options->format = CERTIFICATE_FORMAT_JSON;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    return options->expected_input && options->actual_input &&
        options->pairs_path && options->registry_path &&
        options->report_path && options->format != CERTIFICATE_FORMAT_NONE;
}

static void json_string(StringBuilder* output, const char* value) {
    if (!value) {
        sb_append(output, "null");
        return;
    }
    static const char hex[] = "0123456789abcdef";
    sb_append_char(output, '"');
    for (const unsigned char* cursor = (const unsigned char*)value;
         *cursor; ++cursor) {
        unsigned char byte = *cursor;
        switch (byte) {
            case '"': sb_append(output, "\\\""); break;
            case '\\': sb_append(output, "\\\\"); break;
            case '\b': sb_append(output, "\\b"); break;
            case '\f': sb_append(output, "\\f"); break;
            case '\n': sb_append(output, "\\n"); break;
            case '\r': sb_append(output, "\\r"); break;
            case '\t': sb_append(output, "\\t"); break;
            default:
                if (byte < 0x20U || byte >= 0x80U) {
                    sb_append(output, "\\u00");
                    sb_append_char(output, hex[byte >> 4U]);
                    sb_append_char(output, hex[byte & 0x0fU]);
                } else {
                    sb_append_char(output, (char)byte);
                }
                break;
        }
    }
    sb_append_char(output, '"');
}

static void table_text(StringBuilder* output, const char* value) {
    if (!value) {
        sb_append(output, "<unavailable>");
        return;
    }
    for (const unsigned char* cursor = (const unsigned char*)value;
         *cursor; ++cursor) {
        sb_append_char(output, *cursor < 0x20U ? ' ' : (char)*cursor);
    }
}

static void append_optional_int(StringBuilder* output, int value) {
    if (value < 0) sb_append(output, "null");
    else sb_appendf(output, "%d", value);
}

static void append_endpoint_json(
    StringBuilder* output, const ReleaseShaderCertificateEndpoint* endpoint) {
    sb_append(output, "{\"occurrence_id\":");
    json_string(output, endpoint->occurrence_id);
    sb_append(output, ",\"content_id\":");
    json_string(output, endpoint->content_id);
    sb_append(output, ",\"serialized_sha256\":");
    json_string(output, endpoint->serialized_sha256);
    sb_appendf(output, ",\"path_id\":\"%" PRId64 "\",\"name\":",
               endpoint->path_id);
    json_string(output, endpoint->name);
    sb_append(output, ",\"name_role\":\"display-only\",\"source\":");
    json_string(output, endpoint->outer_path);
    sb_append(output, ",\"member\":");
    json_string(output, endpoint->member_name);
    sb_appendf(output, ",\"member_index\":%zu,\"bundle_member\":%s}",
               endpoint->member_index,
               endpoint->is_bundle_member ? "true" : "false");
}

static void append_certificate_json(
    StringBuilder* output,
    const ReleaseShaderObjectCertificateReport* certificate) {
    sb_append(output, "{\"status\":");
    json_string(output, release_shader_object_certificate_status_name(
                            certificate->status));
    sb_append(output, ",\"first_problem\":");
    if (certificate->first_problem_field == RELEASE_SHADER_FIELD_COUNT) {
        sb_append(output, "null");
    } else {
        sb_append(output, "{\"field\":");
        json_string(output, release_shader_object_field_name(
                                certificate->first_problem_field));
        sb_append(output, ",\"subshader_index\":");
        append_optional_int(output, certificate->subshader_index);
        sb_append(output, ",\"pass_index\":");
        append_optional_int(output, certificate->pass_index);
        sb_append(output, ",\"stage_index\":");
        append_optional_int(output, certificate->stage_index);
        sb_append(output, ",\"element_index\":");
        append_optional_int(output, certificate->element_index);
        sb_append(output, ",\"platform\":");
        append_optional_int(output, certificate->platform);
        sb_append(output, ",\"archive_entry_index\":");
        append_optional_int(output, certificate->archive_entry_index);
        sb_append(output, ",\"archive_byte_offset\":");
        if (certificate->archive_byte_offset == SIZE_MAX) {
            sb_append(output, "null");
        } else {
            sb_appendf(output, "%zu", certificate->archive_byte_offset);
        }
        sb_append_char(output, '}');
    }
    sb_appendf(output,
               ",\"evaluated_fields\":%zu,\"matched_fields\":%zu,"
               "\"expected_artifacts\":%zu,\"actual_artifacts\":%zu,"
               "\"matched_artifacts\":%zu,\"fields\":[",
               certificate->evaluated_field_count,
               certificate->matched_field_count,
               certificate->expected_artifact_count,
               certificate->actual_artifact_count,
               certificate->matched_artifact_count);
    for (int field = 0; field < RELEASE_SHADER_FIELD_COUNT; ++field) {
        if (field != 0) sb_append_char(output, ',');
        sb_append(output, "{\"field\":");
        json_string(output, release_shader_object_field_name(
                                (ReleaseShaderObjectField)field));
        sb_append(output, ",\"status\":");
        json_string(output, release_shader_object_field_status_name(
                                certificate->fields[field]));
        sb_append_char(output, '}');
    }
    sb_appendf(output,
               "],\"canonical_release_identity_certified\":%s,"
               "\"compiled_artifacts_exact\":%s,"
               "\"source_identity_certified\":false,"
               "\"visual_output_certified\":false}",
               certificate->canonical_release_identity_certified
                   ? "true" : "false",
               certificate->compiled_artifacts_exact ? "true" : "false");
}

static bool render_json(
    const CliOptions* options, const char* registry_sha256,
    const ReleaseShaderCertificateCompareResult* result,
    StringBuilder* output) {
    sb_append(output,
        "{\"report_schema\":\"dxbc-release-shader-certificate\","
        "\"report_version\":1,\"command\":\"compare\",\"status\":");
    json_string(output,
                release_shader_certificate_compare_status_name(
                    result->status));
    sb_append(output,
        ",\"certification_scope\":{"
        "\"canonical_release_shader_object\":true,"
        "\"compiled_artifacts\":true,"
        "\"shaderlab_source\":false,\"visual_output\":false},"
        "\"inputs\":{\"expected\":");
    json_string(output, options->expected_input);
    sb_append(output, ",\"actual\":");
    json_string(output, options->actual_input);
    sb_append(output, ",\"pairs\":");
    json_string(output, options->pairs_path);
    sb_append(output, ",\"reference_map\":");
    json_string(output, options->references_path);
    sb_append(output, ",\"schema_registry\":{\"path\":");
    json_string(output, options->registry_path);
    sb_append(output, ",\"sha256\":");
    json_string(output, registry_sha256);
    sb_appendf(output,
        "}},\"catalogs\":{\"expected_status\":");
    json_string(output, shader_catalog_status_name(
                            result->expected_catalog_status));
    sb_appendf(output, ",\"expected_issues\":%zu,\"actual_status\":",
               result->expected_catalog_issues);
    json_string(output,
                shader_catalog_status_name(result->actual_catalog_status));
    sb_appendf(output,
        ",\"actual_issues\":%zu},"
        "\"summary\":{\"pairs\":%zu,\"exact\":%zu,"
        "\"mismatch\":%zu,\"authority_unavailable\":%zu,"
        "\"authority_invalid\":%zu},\"pairs\":[",
        result->actual_catalog_issues, result->pair_count,
        result->exact_count, result->mismatch_count,
        result->unavailable_count, result->invalid_count);
    for (size_t index = 0U; index < result->pair_count; ++index) {
        const ReleaseShaderCertificatePairResult* pair =
            &result->pairs[index];
        if (index != 0U) sb_append_char(output, ',');
        sb_appendf(output, "{\"index\":%zu,\"pairs_line\":%zu,\"status\":",
                   index + 1U, pair->line_number);
        json_string(output,
                    release_shader_certificate_pair_status_name(pair->status));
        sb_append(output, ",\"diagnostic\":");
        json_string(output, release_shader_certificate_diagnostic_name(
                                pair->diagnostic));
        sb_append(output, ",\"expected\":");
        append_endpoint_json(output, &pair->expected);
        sb_append(output, ",\"actual\":");
        append_endpoint_json(output, &pair->actual);
        sb_append(output, ",\"decode\":{\"expected_schema_status\":");
        json_string(output, typetree_schema_status_name(
                                pair->expected_schema_status));
        sb_append(output, ",\"expected_object_status\":");
        json_string(output,
                    shader_object_status_name(pair->expected_decode_status));
        sb_append(output, ",\"actual_schema_status\":");
        json_string(output,
                    typetree_schema_status_name(pair->actual_schema_status));
        sb_append(output, ",\"actual_object_status\":");
        json_string(output,
                    shader_object_status_name(pair->actual_decode_status));
        sb_append(output, "},\"certificate\":");
        append_certificate_json(output, &pair->certificate);
        sb_append_char(output, '}');
    }
    sb_append(output,
        "],\"claims\":{\"source_identity_certified\":false,"
        "\"visual_output_certified\":false}}\n");
    return sb_ok(output);
}

static void append_problem_table(
    StringBuilder* output,
    const ReleaseShaderObjectCertificateReport* certificate) {
    if (certificate->first_problem_field == RELEASE_SHADER_FIELD_COUNT) {
        sb_append(output, "<none>");
        return;
    }
    sb_append(output, release_shader_object_field_name(
                          certificate->first_problem_field));
    if (certificate->subshader_index >= 0)
        sb_appendf(output, " subshader=%d", certificate->subshader_index);
    if (certificate->pass_index >= 0)
        sb_appendf(output, " pass=%d", certificate->pass_index);
    if (certificate->stage_index >= 0)
        sb_appendf(output, " stage=%d", certificate->stage_index);
    if (certificate->element_index >= 0)
        sb_appendf(output, " element=%d", certificate->element_index);
    if (certificate->platform >= 0)
        sb_appendf(output, " platform=%d", certificate->platform);
    if (certificate->archive_entry_index >= 0)
        sb_appendf(output, " archive_entry=%d",
                   certificate->archive_entry_index);
    if (certificate->archive_byte_offset != SIZE_MAX)
        sb_appendf(output, " archive_byte=%zu",
                   certificate->archive_byte_offset);
}

static bool render_table(
    const CliOptions* options, const char* registry_sha256,
    const ReleaseShaderCertificateCompareResult* result,
    StringBuilder* output) {
    sb_append(output,
        "#\tSTATUS\tEXPECTED_ID\tACTUAL_ID\tEXPECTED_NAME(display-only)\t"
        "ACTUAL_NAME(display-only)\tFIRST_PROBLEM/DIAGNOSTIC\n");
    for (size_t index = 0U; index < result->pair_count; ++index) {
        const ReleaseShaderCertificatePairResult* pair =
            &result->pairs[index];
        sb_appendf(output, "%zu\t%s\t", index + 1U,
                   release_shader_certificate_pair_status_name(pair->status));
        table_text(output, pair->expected.occurrence_id);
        sb_append_char(output, '\t');
        table_text(output, pair->actual.occurrence_id);
        sb_append_char(output, '\t');
        table_text(output, pair->expected.name);
        sb_append_char(output, '\t');
        table_text(output, pair->actual.name);
        sb_append_char(output, '\t');
        if (pair->diagnostic != RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_NONE) {
            sb_append(output, release_shader_certificate_diagnostic_name(
                                  pair->diagnostic));
        } else {
            append_problem_table(output, &pair->certificate);
        }
        sb_append_char(output, '\n');
    }
    sb_appendf(output,
        "\nSummary: status=%s pairs=%zu exact=%zu mismatch=%zu "
        "authority_unavailable=%zu authority_invalid=%zu\n",
        release_shader_certificate_compare_status_name(result->status),
        result->pair_count, result->exact_count, result->mismatch_count,
        result->unavailable_count, result->invalid_count);
    sb_append(output, "Expected input: ");
    table_text(output, options->expected_input);
    sb_append(output, "\nActual input: ");
    table_text(output, options->actual_input);
    sb_append(output, "\nPairs: ");
    table_text(output, options->pairs_path);
    sb_append(output, "\nReference map: ");
    table_text(output, options->references_path);
    sb_append(output, "\nSchema registry: ");
    table_text(output, options->registry_path);
    sb_appendf(output, "  %s\n", registry_sha256);
    sb_append(output,
        "Scope: canonical Release Shader object and compiled artifacts. "
        "ShaderLab source certified=no; visual output certified=no.\n");
    return sb_ok(output);
}

static int exit_code_for_status(ReleaseShaderCertificateCompareStatus status) {
    if (status == RELEASE_SHADER_CERTIFICATE_COMPARE_EXACT) return 0;
    if (status == RELEASE_SHADER_CERTIFICATE_COMPARE_MISMATCH) return 2;
    if (status ==
        RELEASE_SHADER_CERTIFICATE_COMPARE_AUTHORITY_UNAVAILABLE) return 3;
    return 1;
}

static int release_shader_certificate_main(int argc, char** argv) {
    CliOptions options;
    bool help = false;
    if (!parse_cli(argc, argv, &options, &help)) {
        print_usage(stderr, argv[0]);
        return 1;
    }
    if (help) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    ReleaseShaderCertificatePairTable pairs;
    ReleaseShaderCertificateReferenceMap references;
    ReleaseShaderCertificateCompareResult result;
    TypeTreeSchemaRegistry registry;
    release_shader_certificate_pair_table_init(&pairs);
    release_shader_certificate_reference_map_init(&references);
    release_shader_certificate_compare_result_init(&result);
    typetree_schema_registry_init(&registry);
    size_t error_line = 0U;
    ReleaseShaderCertificateTableStatus table_status =
        release_shader_certificate_pairs_load(
            options.pairs_path, &pairs, &error_line);
    if (table_status != RELEASE_SHADER_CERTIFICATE_TABLE_OK) {
        fprintf(stderr, "Error: pairs table failed at line %zu: %s.\n",
                error_line,
                release_shader_certificate_table_status_name(table_status));
        goto infrastructure_failure;
    }
    if (options.references_path) {
        table_status = release_shader_certificate_reference_map_load(
            options.references_path, &references, &error_line);
        if (table_status != RELEASE_SHADER_CERTIFICATE_TABLE_OK) {
            fprintf(stderr,
                    "Error: reference map failed at line %zu: %s.\n",
                    error_line,
                    release_shader_certificate_table_status_name(
                        table_status));
            goto infrastructure_failure;
        }
    }
    TypeTreeSchemaStatus registry_status =
        typetree_schema_registry_import_file_replace(
            &registry, options.registry_path);
    if (registry_status != TYPETREE_SCHEMA_OK) {
        fprintf(stderr, "Error: schema registry: %s.\n",
                typetree_schema_status_name(registry_status));
        goto infrastructure_failure;
    }
    CommonFileBytes registry_file = {0};
    if (common_file_read_regular(
            options.registry_path, SIZE_MAX, &registry_file) !=
        COMMON_FILE_OK) {
        fprintf(stderr, "Error: could not fingerprint schema registry.\n");
        goto infrastructure_failure;
    }
    uint8_t registry_digest[COMMON_SHA256_DIGEST_SIZE];
    char registry_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    common_sha256(registry_file.data, registry_file.size, registry_digest);
    common_sha256_digest_to_hex(registry_digest, registry_digest_hex);
    common_file_bytes_dispose(&registry_file);

    (void)release_shader_certificate_compare(
        options.expected_input, options.actual_input, &pairs, &references,
        &registry, &result);
    StringBuilder report;
    sb_init_with_capacity(&report, 16384U);
    bool rendered = options.format == CERTIFICATE_FORMAT_JSON
        ? render_json(&options, registry_digest_hex, &result, &report)
        : render_table(&options, registry_digest_hex, &result, &report);
    if (!rendered) {
        fprintf(stderr, "Error: report allocation failed.\n");
        sb_free(&report);
        goto infrastructure_failure;
    }
    CommonFileStatus write_status = common_file_write_new_atomic(
        options.report_path, report.buf, report.len);
    if (write_status != COMMON_FILE_OK) {
        fprintf(stderr, "Error: could not publish new report '%s': %s.\n",
                options.report_path, common_file_status_name(write_status));
        sb_free(&report);
        goto infrastructure_failure;
    }
    if (report.len != 0U &&
        fwrite(report.buf, 1U, report.len, stdout) != report.len) {
        fprintf(stderr, "Error: could not write report to stdout.\n");
        sb_free(&report);
        goto infrastructure_failure;
    }
    int exit_code = exit_code_for_status(result.status);
    sb_free(&report);
    release_shader_certificate_compare_result_dispose(&result);
    typetree_schema_registry_dispose(&registry);
    release_shader_certificate_reference_map_dispose(&references);
    release_shader_certificate_pair_table_dispose(&pairs);
    return exit_code;

infrastructure_failure:
    release_shader_certificate_compare_result_dispose(&result);
    typetree_schema_registry_dispose(&registry);
    release_shader_certificate_reference_map_dispose(&references);
    release_shader_certificate_pair_table_dispose(&pairs);
    return 1;
}

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int wmain(int argc, wchar_t** wide_argv) {
    if (argc < 0 || !wide_argv ||
        (size_t)argc > SIZE_MAX / sizeof(char*) - 1U) {
        fputs("Error: invalid Windows command line.\n", stderr);
        return 1;
    }
    char** argv = (char**)calloc((size_t)argc + 1U, sizeof(*argv));
    if (!argv) {
        fputs("Error: out of memory decoding Windows command line.\n",
              stderr);
        return 1;
    }
    int converted = 0;
    for (; converted < argc; ++converted) {
        argv[converted] = common_windows_wide_to_utf8(wide_argv[converted]);
        if (!argv[converted]) break;
    }
    if (converted != argc) {
        for (int index = 0; index < converted; ++index) free(argv[index]);
        free(argv);
        fputs("Error: Windows command line is not valid Unicode.\n", stderr);
        return 1;
    }
    int result = release_shader_certificate_main(argc, argv);
    for (int index = 0; index < argc; ++index) free(argv[index]);
    free(argv);
    return result;
}
#else
int main(int argc, char** argv) {
    return release_shader_certificate_main(argc, argv);
}
#endif
