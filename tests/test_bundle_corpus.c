/*
 * C-only structural regression gate for UnityFS shader corpora.
 *
 * This deliberately stops before ShaderLab emission or UnityShaderCompiler:
 * it verifies the portable evidence-ingestion boundary (UnityFS,
 * SerializedFile, TypeTree, SerializedShader, player blob archives, lossless
 * DXBC documents, typed stage contracts, and the stage-aware semantic/USIL
 * projection). Pass one or more bundle paths on the command line.
 */

#include "common/common.h"
#include "common/file_io.h"
#include "common/sha256.h"
#include "common/shader_stage.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "io/bundle_archive.h"
#include "io/serialized_file.h"
#include "io/serialized_shader.h"
#include "io/shader_blob_archive.h"
#include "io/subprogram_metadata.h"
#include "io/typetree_schema_registry.h"
#include "translation/usil.h"

#include <inttypes.h>
#include <limits.h>

enum {
    ENTRY_ROLE_VARIANT = 1u,
    ENTRY_ROLE_PARAMETERS = 2u,
    MAX_TAXONOMY_VALUES = 64
};

typedef struct {
    int value;
    uint64_t count;
} IntegerTaxon;

typedef struct {
    uint64_t bundle_members;
    uint64_t serialized_members;
    uint64_t auxiliary_resources;
    uint64_t auxiliary_directories;
    uint64_t deleted_members;
    uint64_t serialized_files;
    uint64_t typetree_enabled_files;
    uint64_t typetree_disabled_files;
    uint64_t types;
    uint64_t objects;
    uint64_t shaders;
    uint64_t shader_object_bytes;
    uint64_t properties;
    uint64_t subshaders;
    uint64_t passes;
    uint64_t subprograms;
    uint64_t stage_subprograms[6];
    uint64_t advertised_platforms;
    uint64_t blob_archives;
    uint64_t blob_segments;
    uint64_t stage_counts_total;
    uint64_t blob_entries;
    uint64_t blob_payload_bytes;
    uint64_t referenced_variant_entries;
    uint64_t referenced_parameter_entries;
    uint64_t player_wrapper_headers;
    uint64_t player_header_nonzero_words;
    uint64_t player_source_maps_nonzero;
    uint64_t dxbc_documents;
    uint64_t dxbc_exact_roundtrips;
    uint64_t dxbc_stage_contracts[UNITY_SERIALIZED_STAGE_COUNT];
    uint64_t dxbc_semantic_projections;
    uint64_t dxbc_operand_types[OPERAND_TYPE_INNER_COVERAGE + 1];
    uint64_t dual_role_entries;
    uint64_t unreferenced_entries;
    IntegerTaxon class_ids[MAX_TAXONOMY_VALUES];
    size_t class_id_count;
    IntegerTaxon platforms[MAX_TAXONOMY_VALUES];
    size_t platform_count;
} CorpusStats;

typedef struct {
    const char* label;
    const char* sha256;
    uint64_t members;
    uint64_t serialized_files;
    uint64_t objects;
    uint64_t shaders;
    uint64_t properties;
    uint64_t passes;
    uint64_t subprograms;
    uint64_t stage_subprograms[UNITY_SERIALIZED_STAGE_COUNT];
    uint64_t blob_entries;
    uint64_t advertised_platforms;
    uint64_t blob_segments;
    uint64_t stage_counts_total;
    uint64_t variant_entries;
    uint64_t parameter_entries;
    uint64_t player_header_nonzero_words;
    uint64_t player_source_maps_nonzero;
    uint64_t semantic_projections;
    uint64_t stage_contracts[UNITY_SERIALIZED_STAGE_COUNT];
} KnownCorpus;

static bool add_counter(uint64_t* destination, uint64_t value);

static bool accumulate_operand_types(CorpusStats* stats,
                                     const DXBCOperand* operand,
                                     unsigned depth) {
    if (!stats || !operand ||
        depth >= DXBC_MAX_NESTED_OPERAND_TOKENS || operand->type < 0 ||
        operand->type > OPERAND_TYPE_INNER_COVERAGE ||
        !add_counter(&stats->dxbc_operand_types[operand->type], 1)) {
        return false;
    }
    return (!operand->rel_op0 ||
            accumulate_operand_types(stats, operand->rel_op0, depth + 1U)) &&
           (!operand->rel_op1 ||
            accumulate_operand_types(stats, operand->rel_op1, depth + 1U)) &&
           (!operand->rel_op2 ||
            accumulate_operand_types(stats, operand->rel_op2, depth + 1U));
}

static bool accumulate_instruction_operand_types(
    CorpusStats* stats, const DXBCContainer* semantic) {
    if (!stats || !semantic || semantic->instruction_count < 0 ||
        (semantic->instruction_count > 0 && !semantic->instructions)) {
        return false;
    }
    for (int instruction = 0;
         instruction < semantic->instruction_count; ++instruction) {
        const DXBCInstruction* decoded = &semantic->instructions[instruction];
        if (decoded->operand_count < 0 ||
            decoded->operand_count > DXBC_MAX_OPERANDS) {
            return false;
        }
        for (int operand = 0; operand < decoded->operand_count; ++operand) {
            if (!accumulate_operand_types(
                    stats, &decoded->operands[operand], 0U)) {
                return false;
            }
        }
    }
    return true;
}

/* Counts here are tied to complete file digests, not to filenames or emitted
 * shader output.  Fill values only from a successful structural scan. */
static const KnownCorpus k_known_corpora[] = {
    {
        .label = "collected_shaders.bundle",
        .sha256 =
            "e2390e96306541d8f5a06050e059a79c883f64127dd88434c15fcc9d1e5ce632",
        .members = 1,
        .serialized_files = 1,
        .objects = 581,
        .shaders = 290,
        .properties = 1216,
        .passes = 707,
        .subprograms = 5349,
        .stage_subprograms = {2657, 2690, 2, 0, 0, 0},
        .blob_entries = 7538,
        .advertised_platforms = 290,
        .blob_segments = 290,
        .stage_counts_total = 574,
        .variant_entries = 4912,
        .parameter_entries = 2626,
        .player_header_nonzero_words = 13484,
        .player_source_maps_nonzero = 2433,
        .semantic_projections = 4912,
        .stage_contracts = {2438, 2472, 2, 0, 0, 0},
    },
    {
        .label = "collected_shaders_2.bundle",
        .sha256 =
            "a9f962af75fbcce147010961afe760832c138599c30c5677ac184e20d9dab114",
        .members = 1,
        .serialized_files = 1,
        .objects = 14,
        .shaders = 7,
        .properties = 32,
        .passes = 13,
        .subprograms = 54,
        .stage_subprograms = {26, 25, 1, 1, 1, 0},
        .blob_entries = 84,
        .advertised_platforms = 7,
        .blob_segments = 7,
        .stage_counts_total = 16,
        .variant_entries = 53,
        .parameter_entries = 31,
        .player_header_nonzero_words = 100,
        .player_source_maps_nonzero = 26,
        .semantic_projections = 53,
        .stage_contracts = {26, 24, 1, 1, 1, 0},
    },
    {
        .label = "collected_shaders_3.bundle",
        .sha256 =
            "805532ca839467acc4223e9ff3b34f2df6e0100f14a06ef5473584cc52b0fec5",
        .members = 1,
        .serialized_files = 1,
        .objects = 20,
        .shaders = 10,
        .properties = 23,
        .passes = 13,
        .subprograms = 29,
        .stage_subprograms = {15, 14, 0, 0, 0, 0},
        .blob_entries = 52,
        .advertised_platforms = 10,
        .blob_segments = 10,
        .stage_counts_total = 20,
        .variant_entries = 28,
        .parameter_entries = 24,
        .player_header_nonzero_words = 47,
        .player_source_maps_nonzero = 15,
        .semantic_projections = 28,
        .stage_contracts = {15, 13, 0, 0, 0, 0},
    },
};

static const char* path_basename(const char* path) {
    const char* name = path;
    if (!path) return "(null)";
    for (const char* cursor = path; *cursor; cursor++) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    }
    return name;
}

static uint32_t read_be32_bytes(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static uint64_t read_be64_bytes(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8u; i++) {
        value = (value << 8u) | bytes[i];
    }
    return value;
}

static bool looks_like_v22_serialized_file(const uint8_t* data,
                                            size_t size) {
    if (!data || size < 48u || read_be32_bytes(data + 8u) != 22u) {
        return false;
    }
    return read_be64_bytes(data + 24u) == (uint64_t)size &&
           read_be64_bytes(data + 32u) <= (uint64_t)size;
}

static void digest_to_hex(const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
                          char output[COMMON_SHA256_DIGEST_SIZE * 2u + 1u]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < COMMON_SHA256_DIGEST_SIZE; i++) {
        output[i * 2u] = digits[digest[i] >> 4u];
        output[i * 2u + 1u] = digits[digest[i] & 0x0fu];
    }
    output[COMMON_SHA256_DIGEST_SIZE * 2u] = '\0';
}

static bool add_counter(uint64_t* destination, uint64_t value) {
    if (!destination || UINT64_MAX - *destination < value) return false;
    *destination += value;
    return true;
}

static bool add_taxon(IntegerTaxon* taxa, size_t* count, int value,
                      uint64_t increment) {
    if (!taxa || !count || increment == 0) return false;
    for (size_t i = 0; i < *count; i++) {
        if (taxa[i].value == value) {
            return add_counter(&taxa[i].count, increment);
        }
    }
    if (*count >= MAX_TAXONOMY_VALUES) return false;
    taxa[*count].value = value;
    taxa[*count].count = increment;
    (*count)++;
    return true;
}

static void sort_taxa(IntegerTaxon* taxa, size_t count) {
    for (size_t i = 1; i < count; i++) {
        IntegerTaxon current = taxa[i];
        size_t destination = i;
        while (destination > 0 &&
               taxa[destination - 1u].value > current.value) {
            taxa[destination] = taxa[destination - 1u];
            destination--;
        }
        taxa[destination] = current;
    }
}

static bool accumulate_shader_shape(const SerializedShader* shader,
                                    CorpusStats* stats) {
    if (!shader || !stats || shader->property_count < 0 ||
        shader->subshader_count < 0 || shader->archive_platform_count < 0) {
        return false;
    }
    if (!add_counter(&stats->properties, (uint64_t)shader->property_count) ||
        !add_counter(&stats->subshaders,
                     (uint64_t)shader->subshader_count) ||
        !add_counter(&stats->advertised_platforms,
                     (uint64_t)shader->archive_platform_count)) {
        return false;
    }
    for (int subshader_index = 0;
         subshader_index < shader->subshader_count; subshader_index++) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_index];
        if (subshader->pass_count < 0 ||
            !add_counter(&stats->passes,
                         (uint64_t)subshader->pass_count)) {
            return false;
        }
        for (int pass_index = 0; pass_index < subshader->pass_count;
             pass_index++) {
            const SerializedPass* pass = &subshader->passes[pass_index];
            for (int stage = 0; stage < 6; stage++) {
                if (pass->subprogram_count[stage] < 0 ||
                    !add_counter(&stats->subprograms,
                                 (uint64_t)pass->subprogram_count[stage]) ||
                    !add_counter(&stats->stage_subprograms[stage],
                                 (uint64_t)pass->subprogram_count[stage])) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool validate_subprogram_platform_coverage(
    const char* bundle_name, int64_t path_id,
    const SerializedShader* shader) {
    for (int subshader_index = 0;
         subshader_index < shader->subshader_count; subshader_index++) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_index];
        for (int pass_index = 0; pass_index < subshader->pass_count;
             pass_index++) {
            const SerializedPass* pass = &subshader->passes[pass_index];
            for (int stage = 0; stage < 6; stage++) {
                for (int subprogram_index = 0;
                     subprogram_index < pass->subprogram_count[stage];
                     subprogram_index++) {
                    int matches = 0;
                    for (int platform_index = 0;
                         platform_index < shader->archive_platform_count;
                         platform_index++) {
                        if (serialized_pass_subprogram_is_platform(
                                pass, stage, subprogram_index,
                                shader->archive_platforms[platform_index])) {
                            matches++;
                        }
                    }
                    if (matches != 1) {
                        fprintf(stderr,
                                "[CORPUS ERROR] %s Shader path=%" PRId64
                                " subshader=%d pass=%d stage=%d subprogram=%d "
                                "maps to %d advertised platforms\n",
                                bundle_name, path_id, subshader_index,
                                pass_index, stage, subprogram_index, matches);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool mark_platform_entry_roles(
    const char* bundle_name, int64_t path_id,
    const SerializedShader* shader, int platform, int entry_count,
    uint8_t* roles, int32_t* expected_program_types,
    uint8_t* expected_serialized_stages,
    uint32_t* expected_program_masks) {
    for (int subshader_index = 0;
         subshader_index < shader->subshader_count; subshader_index++) {
        const SerializedSubShader* subshader =
            &shader->subshaders[subshader_index];
        for (int pass_index = 0; pass_index < subshader->pass_count;
             pass_index++) {
            const SerializedPass* pass = &subshader->passes[pass_index];
            for (int stage = 0; stage < 6; stage++) {
                UnitySerializedProgramStage serialized_stage =
                    (UnitySerializedProgramStage)stage;
                uint32_t stage_mask_bit = 0;
                if (!shader_stage_serialized_program_mask_bit(
                        serialized_stage, &stage_mask_bit)) {
                    fprintf(stderr,
                            "[CORPUS ERROR] %s Shader path=%" PRId64
                            " has unknown serialized stage %d\n",
                            bundle_name, path_id, stage);
                    return false;
                }
                for (int subprogram_index = 0;
                     subprogram_index < pass->subprogram_count[stage];
                     subprogram_index++) {
                    if (!serialized_pass_subprogram_is_platform(
                            pass, stage, subprogram_index, platform)) {
                        continue;
                    }
                    if (!pass->subprograms[stage] ||
                        !pass->subprogram_param_blob_indices[stage]) {
                        fprintf(stderr,
                                "[CORPUS ERROR] %s Shader path=%" PRId64
                                " has incomplete stage %d arrays\n",
                                bundle_name, path_id, stage);
                        return false;
                    }
                    const SerializedSubProgram* subprogram =
                        &pass->subprograms[stage][subprogram_index];
                    if ((pass->program_mask & stage_mask_bit) == 0u) {
                        fprintf(stderr,
                                "[CORPUS ERROR] %s Shader path=%" PRId64
                                " subshader=%d pass=%d stage=%d is populated "
                                "but program mask 0x%08" PRIx32
                                " omits bit 0x%08" PRIx32 "\n",
                                bundle_name, path_id, subshader_index,
                                pass_index, stage, pass->program_mask,
                                stage_mask_bit);
                        return false;
                    }
                    int variant_index = subprogram->blob_index;
                    if (variant_index < 0 || variant_index >= entry_count) {
                        fprintf(stderr,
                                "[CORPUS ERROR] %s Shader path=%" PRId64
                                " platform=%d variant entry %d is outside "
                                "[0,%d)\n",
                                bundle_name, path_id, platform,
                                variant_index, entry_count);
                        return false;
                    }
                    if ((roles[variant_index] & ENTRY_ROLE_VARIANT) != 0 &&
                        expected_program_types[variant_index] !=
                            subprogram->program_type) {
                        fprintf(stderr,
                                "[CORPUS ERROR] %s Shader path=%" PRId64
                                " platform=%d entry=%d has conflicting "
                                "program types %d/%d\n",
                                bundle_name, path_id, platform,
                                variant_index,
                                expected_program_types[variant_index],
                                subprogram->program_type);
                        return false;
                    }
                    if ((roles[variant_index] & ENTRY_ROLE_VARIANT) != 0 &&
                        expected_serialized_stages[variant_index] !=
                            (uint8_t)serialized_stage) {
                        fprintf(stderr,
                                "[CORPUS ERROR] %s Shader path=%" PRId64
                                " platform=%d entry=%d is reused by "
                                "serialized stages %u/%u\n",
                                bundle_name, path_id, platform,
                                variant_index,
                                expected_serialized_stages[variant_index],
                                (unsigned)serialized_stage);
                        return false;
                    }
                    if ((roles[variant_index] & ENTRY_ROLE_VARIANT) == 0) {
                        expected_program_types[variant_index] =
                            subprogram->program_type;
                        expected_serialized_stages[variant_index] =
                            (uint8_t)serialized_stage;
                        expected_program_masks[variant_index] =
                            pass->program_mask;
                    } else {
                        expected_program_masks[variant_index] |=
                            pass->program_mask;
                    }
                    roles[variant_index] |= ENTRY_ROLE_VARIANT;

                    int parameter_index =
                        pass->subprogram_param_blob_indices[stage]
                                                           [subprogram_index];
                    if (parameter_index < -1 ||
                        parameter_index >= entry_count) {
                        fprintf(stderr,
                                "[CORPUS ERROR] %s Shader path=%" PRId64
                                " platform=%d parameter entry %d is invalid\n",
                                bundle_name, path_id, platform,
                                parameter_index);
                        return false;
                    }
                    if (parameter_index >= 0) {
                        roles[parameter_index] |= ENTRY_ROLE_PARAMETERS;
                    }
                }
            }
        }
    }
    return true;
}

static bool validate_variant_dxbc(
    const char* bundle_name, int64_t path_id, int platform, int entry_index,
    const PlayerSubProgramMetadata* variant,
    UnitySerializedProgramStage serialized_stage,
    uint32_t serialized_program_mask, CorpusStats* stats) {
    if (!variant || !stats || !variant->bytecode ||
        variant->bytecode_length == 0u) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d has no compiled program bytes\n",
                bundle_name, path_id, platform, entry_index);
        return false;
    }

    DXBCContainerView raw_view;
    if (!dxbc_container_view_first(variant->bytecode,
                                   variant->bytecode_length, &raw_view)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d has no structurally valid DXBC "
                "container\n",
                bundle_name, path_id, platform, entry_index);
        return false;
    }

    bool success = false;
    DXBCDocument document;
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCStageContract contract;
    DXBCStageContractDiagnostic contract_diagnostic;
    DXBCContainer semantic;
    USILProgram usil;
    bool semantic_decoded = false;
    bool usil_translated = false;
    uint8_t* exact_bytes = NULL;
    size_t exact_size = 0;
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    memset(&document_diagnostic, 0, sizeof(document_diagnostic));
    memset(&contract_diagnostic, 0, sizeof(contract_diagnostic));
    memset(&semantic, 0, sizeof(semantic));
    memset(&usil, 0, sizeof(usil));

    if (!dxbc_document_parse(&document, raw_view.data, raw_view.size,
                             &document_diagnostic)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d DXBC document parse failed: %s "
                "at byte=%zu chunk=%" PRIu32 " instruction=%" PRIu32
                "\n",
                bundle_name, path_id, platform, entry_index,
                dxbc_document_diagnostic_code_name(
                    document_diagnostic.code),
                document_diagnostic.byte_offset,
                document_diagnostic.chunk_index,
                document_diagnostic.instruction_index);
        goto cleanup;
    }
    if (!dxbc_document_serialize_exact(&document, &exact_bytes, &exact_size,
                                       &document_diagnostic) ||
        exact_size != raw_view.size ||
        memcmp(exact_bytes, raw_view.data, raw_view.size) != 0) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d DXBC lossless roundtrip failed\n",
                bundle_name, path_id, platform, entry_index);
        goto cleanup;
    }
    if (!dxbc_stage_contract_decode_document(
            &document, &contract, &contract_diagnostic)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d DXBC stage contract failed: %s "
                "chunk=%" PRIu32 " instruction=%" PRIu32
                " opcode=%" PRIu32 " expected=%" PRIu64
                " actual=%" PRIu64 "\n",
                bundle_name, path_id, platform, entry_index,
                dxbc_stage_contract_status_name(
                    contract_diagnostic.status),
                contract_diagnostic.chunk_index,
                contract_diagnostic.instruction_index,
                contract_diagnostic.opcode, contract_diagnostic.expected,
                contract_diagnostic.actual);
        goto cleanup;
    }

    UnityCompilerProgramStage compiler_stage;
    ShaderStageTuple tuple;
    memset(&tuple, 0, sizeof(tuple));
    if (!shader_stage_serialized_to_compiler(serialized_stage,
                                             &compiler_stage)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d cannot map serialized stage %u\n",
                bundle_name, path_id, platform, entry_index,
                (unsigned)serialized_stage);
        goto cleanup;
    }
    tuple.serialized_stage = serialized_stage;
    tuple.compiler_program = compiler_stage;
    tuple.serialized_program_mask = serialized_program_mask;
    tuple.gpu_program_type =
        (UnityGPUProgramType)variant->program_type;
    tuple.dxbc_program_type = contract.program_type;
    tuple.shader_model_major = contract.shader_model_major;
    tuple.shader_model_minor = contract.shader_model_minor;
    ShaderStageTupleStatus tuple_status =
        shader_stage_validate_d3d11_tuple(&tuple);
    if (tuple_status != SHADER_STAGE_TUPLE_OK) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d stage authorities disagree: %s "
                "serialized=%u mask=0x%08" PRIx32
                " gpu=%d dxbc=%u sm=%u.%u\n",
                bundle_name, path_id, platform, entry_index,
                shader_stage_tuple_status_name(tuple_status),
                (unsigned)serialized_stage, serialized_program_mask,
                variant->program_type, (unsigned)contract.program_type,
                contract.shader_model_major, contract.shader_model_minor);
        goto cleanup;
    }

    /* Every stage contract represented by the portable semantic model must
     * survive both projections.  The raw contract remains authoritative for
     * ordered hull phases and geometry stream effects; USIL must retain those
     * records even when HLSL emission deliberately fails closed. */
    if (!dxbc_document_decode_semantic(&document, &semantic)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d semantic DXBC projection failed\n",
                bundle_name, path_id, platform, entry_index);
        goto cleanup;
    }
    semantic_decoded = true;
    if (!dxbc_stage_contract_validate_container(
            &contract, &semantic, &contract_diagnostic)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d raw/semantic stage mismatch: %s\n",
                bundle_name, path_id, platform, entry_index,
                dxbc_stage_contract_status_name(contract_diagnostic.status));
        goto cleanup;
    }
    if (!usil_translate_with_stage_contract(&usil, &semantic, &contract)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d entry=%d stage-aware USIL projection failed\n",
                bundle_name, path_id, platform, entry_index);
        goto cleanup;
    }
    usil_translated = true;
    if (!accumulate_instruction_operand_types(stats, &semantic) ||
        !add_counter(&stats->dxbc_semantic_projections, 1)) {
        goto cleanup;
    }

    if (!add_counter(&stats->dxbc_documents, 1) ||
        !add_counter(&stats->dxbc_exact_roundtrips, 1) ||
        (uint32_t)serialized_stage >= UNITY_SERIALIZED_STAGE_COUNT ||
        !add_counter(&stats->dxbc_stage_contracts[serialized_stage], 1)) {
        fprintf(stderr, "[CORPUS ERROR] DXBC statistics overflow\n");
        goto cleanup;
    }
    success = true;

cleanup:
    if (usil_translated) usil_free(&usil);
    if (semantic_decoded) dxbc_free(&semantic);
    if (exact_bytes) mem_free(exact_bytes, exact_size);
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    return success;
}

static bool validate_blob_archive(
    const char* bundle_name, int64_t path_id,
    const SerializedShader* shader, const TypeTreeValue* shader_value,
    int platform, CorpusStats* stats) {
    ShaderBlobArchive archive;
    if (!shader_blob_archive_open(shader_value, platform, &archive)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " cannot open advertised platform %d archive\n",
                bundle_name, path_id, platform);
        return false;
    }

    bool success = false;
    uint8_t* roles = NULL;
    int32_t* expected_program_types = NULL;
    uint8_t* expected_serialized_stages = NULL;
    uint32_t* expected_program_masks = NULL;
    if (archive.entry_count < 0 || archive.segment_count <= 0) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " platform=%d has invalid archive counts\n",
                bundle_name, path_id, platform);
        goto cleanup;
    }
    if (archive.entry_count > 0) {
        size_t count = (size_t)archive.entry_count;
        if (count > SIZE_MAX / sizeof(*roles) ||
            count > SIZE_MAX / sizeof(*expected_program_types) ||
            count > SIZE_MAX / sizeof(*expected_serialized_stages) ||
            count > SIZE_MAX / sizeof(*expected_program_masks)) {
            fprintf(stderr, "[CORPUS ERROR] archive entry allocation overflow\n");
            goto cleanup;
        }
        roles = (uint8_t*)calloc(count, sizeof(*roles));
        expected_program_types =
            (int32_t*)calloc(count, sizeof(*expected_program_types));
        expected_serialized_stages =
            (uint8_t*)malloc(count * sizeof(*expected_serialized_stages));
        expected_program_masks =
            (uint32_t*)calloc(count, sizeof(*expected_program_masks));
        if (!roles || !expected_program_types ||
            !expected_serialized_stages || !expected_program_masks) {
            fprintf(stderr,
                    "[CORPUS ERROR] allocate archive entry roles failed\n");
            goto cleanup;
        }
        memset(expected_serialized_stages,
               UNITY_SERIALIZED_STAGE_INVALID,
               count * sizeof(*expected_serialized_stages));
    }

    if (!mark_platform_entry_roles(
            bundle_name, path_id, shader, platform, archive.entry_count,
            roles, expected_program_types, expected_serialized_stages,
            expected_program_masks)) {
        goto cleanup;
    }
    if (!add_counter(&stats->blob_archives, 1) ||
        !add_counter(&stats->blob_segments,
                     (uint64_t)archive.segment_count) ||
        !add_counter(&stats->stage_counts_total,
                     (uint64_t)archive.stage_count) ||
        !add_counter(&stats->blob_entries,
                     (uint64_t)archive.entry_count) ||
        !add_taxon(stats->platforms, &stats->platform_count, platform, 1)) {
        fprintf(stderr, "[CORPUS ERROR] statistics overflow\n");
        goto cleanup;
    }

    for (int entry_index = 0; entry_index < archive.entry_count;
         entry_index++) {
        const uint8_t* payload = NULL;
        size_t payload_size = 0;
        if (!shader_blob_archive_get(&archive, entry_index, &payload,
                                     &payload_size) ||
            (!payload && payload_size != 0) ||
            !add_counter(&stats->blob_payload_bytes,
                         (uint64_t)payload_size)) {
            fprintf(stderr,
                    "[CORPUS ERROR] %s Shader path=%" PRId64
                    " platform=%d entry=%d is not readable\n",
                    bundle_name, path_id, platform, entry_index);
            goto cleanup;
        }

        uint8_t role = roles[entry_index];
        if ((role & ENTRY_ROLE_VARIANT) != 0) {
            ByteStream stream;
            PlayerSubProgramMetadata variant;
            stream_init(&stream, payload, payload_size);
            stream_set_endian(&stream, false);
            if (!subprogram_metadata_parse_variant(&stream, &variant)) {
                fprintf(stderr,
                        "[CORPUS ERROR] %s Shader path=%" PRId64
                        " platform=%d entry=%d variant parse failed\n",
                        bundle_name, path_id, platform, entry_index);
                goto cleanup;
            }
            if (!variant.has_player_blob_header ||
                !add_counter(&stats->player_wrapper_headers, 1)) {
                fprintf(stderr,
                        "[CORPUS ERROR] %s Shader path=%" PRId64
                        " platform=%d entry=%d has no player wrapper header\n",
                        bundle_name, path_id, platform, entry_index);
                subprogram_metadata_free_variant(&variant);
                goto cleanup;
            }
            for (size_t word_index = 0;
                 word_index < sizeof(variant.player_header_words) /
                                  sizeof(variant.player_header_words[0]);
                 word_index++) {
                if (variant.player_header_words[word_index] != 0 &&
                    !add_counter(&stats->player_header_nonzero_words, 1)) {
                    subprogram_metadata_free_variant(&variant);
                    goto cleanup;
                }
            }
            if (variant.source_map != 0 &&
                !add_counter(&stats->player_source_maps_nonzero, 1)) {
                subprogram_metadata_free_variant(&variant);
                goto cleanup;
            }
            int actual_program_type = variant.program_type;
            bool program_type_matches =
                actual_program_type == expected_program_types[entry_index];
            if (!program_type_matches) {
                fprintf(stderr,
                        "[CORPUS ERROR] %s Shader path=%" PRId64
                        " platform=%d entry=%d TypeTree/player-blob program "
                        "types differ (%d/%d)\n",
                        bundle_name, path_id, platform, entry_index,
                        expected_program_types[entry_index],
                        actual_program_type);
                subprogram_metadata_free_variant(&variant);
                goto cleanup;
            }
            if (platform == 4 &&
                !validate_variant_dxbc(
                    bundle_name, path_id, platform, entry_index, &variant,
                    (UnitySerializedProgramStage)
                        expected_serialized_stages[entry_index],
                    expected_program_masks[entry_index], stats)) {
                subprogram_metadata_free_variant(&variant);
                goto cleanup;
            }
            subprogram_metadata_free_variant(&variant);
            if (!add_counter(&stats->referenced_variant_entries, 1)) {
                goto cleanup;
            }
        }
        if ((role & ENTRY_ROLE_PARAMETERS) != 0) {
            ByteStream stream;
            SerializedProgramParameters parameters;
            serialized_program_parameters_init(&parameters);
            stream_init(&stream, payload, payload_size);
            stream_set_endian(&stream, false);
            if (!subprogram_metadata_parse_parameters(&stream,
                                                      &parameters)) {
                fprintf(stderr,
                        "[CORPUS ERROR] %s Shader path=%" PRId64
                        " platform=%d entry=%d parameter parse failed\n",
                        bundle_name, path_id, platform, entry_index);
                serialized_program_parameters_free(&parameters);
                goto cleanup;
            }
            serialized_program_parameters_free(&parameters);
            if (!add_counter(&stats->referenced_parameter_entries, 1)) {
                goto cleanup;
            }
        }
        if (role == (ENTRY_ROLE_VARIANT | ENTRY_ROLE_PARAMETERS) &&
            !add_counter(&stats->dual_role_entries, 1)) {
            goto cleanup;
        }
        if (role == 0 &&
            !add_counter(&stats->unreferenced_entries, 1)) {
            goto cleanup;
        }
    }
    success = true;

cleanup:
    free(expected_program_masks);
    free(expected_serialized_stages);
    free(expected_program_types);
    free(roles);
    shader_blob_archive_close(&archive);
    return success;
}

static bool validate_shader_object(
    const char* bundle_name, SerializedFile* file,
    const AssetObjectInfo* object, CorpusStats* stats) {
    if (!bundle_name || !file || !object || !stats ||
        object->type_id_or_index < 0 ||
        object->type_id_or_index >= file->type_count) {
        fprintf(stderr, "[CORPUS ERROR] invalid Shader object descriptor\n");
        return false;
    }
    size_t object_size = 0;
    const uint8_t* object_data = serialized_file_get_object_data(
        file, object, &object_size);
    if (!object_data || object_size == 0) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " has no object payload\n",
                bundle_name, object->path_id);
        return false;
    }

    const TypeTreeType* schema = &file->types[object->type_id_or_index];
    TypeTreeValue shader_value;
    memset(&shader_value, 0, sizeof(shader_value));
    ByteStream stream;
    stream_init(&stream, object_data, object_size);
    stream_set_endian(&stream, file->big_endian);
    int node_index = 0;
    if (!typetree_parse_value_ex(schema, &node_index, &stream,
                                 &shader_value,
                                 TYPETREE_PARSE_PACK_COMPRESSED_BLOB)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " TypeTree parse failed\n",
                bundle_name, object->path_id);
        return false;
    }
    bool success = false;
    if (stream.position != object_size || node_index != schema->node_count) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " TypeTree consumed bytes=%zu/%zu nodes=%d/%d\n",
                bundle_name, object->path_id, stream.position, object_size,
                node_index, schema->node_count);
        goto cleanup_value;
    }

    SerializedShaderSchemaProfile shader_profile;
    if (!serialized_shader_profile_from_unity_version(
            file->unity_version, &shader_profile)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " unsupported Unity version '%s'\n",
                bundle_name, object->path_id,
                file->unity_version ? file->unity_version : "<missing>");
        goto cleanup_value;
    }
    SerializedShader shader;
    serialized_shader_init(&shader);
    if (!serialized_shader_parse_with_profile(
            &shader, &shader_value, shader_profile)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s Shader path=%" PRId64
                " SerializedShader projection failed\n",
                bundle_name, object->path_id);
        goto cleanup_value;
    }
    /* Count successful TypeTree/SerializedShader projection independently
     * from the deeper archive gate, so a later blob failure cannot look like
     * a silently skipped Shader object. */
    if (!add_counter(&stats->shaders, 1) ||
        !add_counter(&stats->shader_object_bytes,
                     (uint64_t)object_size) ||
        !accumulate_shader_shape(&shader, stats) ||
        !validate_subprogram_platform_coverage(
            bundle_name, object->path_id, &shader)) {
        goto cleanup_shader;
    }
    for (int platform_index = 0;
         platform_index < shader.archive_platform_count; platform_index++) {
        if (!validate_blob_archive(
                bundle_name, object->path_id, &shader, &shader_value,
                shader.archive_platforms[platform_index], stats)) {
            goto cleanup_shader;
        }
    }
    success = true;

cleanup_shader:
    serialized_shader_free(&shader);
cleanup_value:
    typetree_free_value(&shader_value);
    return success;
}

static bool validate_serialized_member(
    const char* bundle_name, const char* member_name,
    const uint8_t* member_data, size_t member_size,
    TypeTreeSchemaRegistry* registry, CorpusStats* stats) {
    SerializedFile file;
    if (!serialized_file_open_with_schema_registry_ex(
            &file, member_data, member_size, registry,
            TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT)) {
        fprintf(stderr,
                "[CORPUS ERROR] %s member %s is flagged SerializedFile "
                "but strict parsing failed\n",
                bundle_name, member_name);
        return false;
    }
    bool success = false;
    if (!add_counter(&stats->serialized_files, 1) ||
        !add_counter(&stats->types, (uint64_t)file.type_count) ||
        !add_counter(&stats->objects, (uint64_t)file.object_count)) {
        fprintf(stderr, "[CORPUS ERROR] statistics overflow\n");
        goto cleanup;
    }
    if (file.type_tree_enabled) {
        if (!add_counter(&stats->typetree_enabled_files, 1)) goto cleanup;
    } else if (!add_counter(&stats->typetree_disabled_files, 1)) {
        goto cleanup;
    }
    for (int object_index = 0; object_index < file.object_count;
         object_index++) {
        const AssetObjectInfo* object = &file.objects[object_index];
        if (!add_taxon(stats->class_ids, &stats->class_id_count,
                       object->type_id, 1)) {
            fprintf(stderr, "[CORPUS ERROR] class taxonomy overflow\n");
            goto cleanup;
        }
        if (object->type_id == 48 &&
            !validate_shader_object(bundle_name, &file, object, stats)) {
            goto cleanup;
        }
    }
    success = true;

cleanup:
    serialized_file_close(&file);
    return success;
}

static bool validate_registry_roundtrip(
    const TypeTreeSchemaRegistry* registry) {
    uint8_t* first_bytes = NULL;
    uint8_t* second_bytes = NULL;
    size_t first_size = 0;
    size_t second_size = 0;
    TypeTreeSchemaRegistry loaded;
    typetree_schema_registry_init(&loaded);
    TypeTreeSchemaStatus status = typetree_schema_registry_serialize(
        registry, &first_bytes, &first_size);
    if (status == TYPETREE_SCHEMA_OK) {
        status = typetree_schema_registry_deserialize_replace(
            &loaded, first_bytes, first_size);
    }
    if (status == TYPETREE_SCHEMA_OK) {
        status = typetree_schema_registry_serialize(
            &loaded, &second_bytes, &second_size);
    }
    bool success = status == TYPETREE_SCHEMA_OK &&
                   typetree_schema_registry_count(&loaded) ==
                       typetree_schema_registry_count(registry) &&
                   first_size == second_size &&
                   (first_size == 0 ||
                    memcmp(first_bytes, second_bytes, first_size) == 0);
    if (!success) {
        fprintf(stderr,
                "[CORPUS ERROR] schema registry canonical roundtrip failed: "
                "%s\n",
                typetree_schema_status_name(status));
    }
    if (second_bytes) mem_free(second_bytes, second_size);
    if (first_bytes) mem_free(first_bytes, first_size);
    typetree_schema_registry_dispose(&loaded);
    return success;
}

static const KnownCorpus* find_known_corpus(const char* digest_hex) {
    for (size_t i = 0;
         i < sizeof(k_known_corpora) / sizeof(k_known_corpora[0]); i++) {
        if (strcmp(k_known_corpora[i].sha256, digest_hex) == 0) {
            return &k_known_corpora[i];
        }
    }
    return NULL;
}

static bool validate_known_counts(const KnownCorpus* known,
                                  const CorpusStats* stats) {
    if (!known || !stats) {
        fprintf(stderr,
                "[CORPUS ERROR] corpus digest is not pinned; refusing "
                "silent structural validation bypass\n");
        return false;
    }
    if (stats->bundle_members != known->members ||
        stats->serialized_files != known->serialized_files ||
        stats->objects != known->objects || stats->shaders != known->shaders ||
        stats->subprograms != known->subprograms ||
        stats->blob_entries != known->blob_entries ||
        stats->referenced_variant_entries != known->variant_entries ||
        stats->referenced_parameter_entries != known->parameter_entries ||
        stats->player_wrapper_headers != known->variant_entries ||
        stats->dxbc_documents != known->variant_entries ||
        stats->dxbc_exact_roundtrips != known->variant_entries ||
        stats->dxbc_semantic_projections != known->semantic_projections) {
        fprintf(stderr,
                "[CORPUS ERROR] pinned structure %s differs: "
                "members=%" PRIu64 "/%" PRIu64
                " serialized=%" PRIu64 "/%" PRIu64
                " objects=%" PRIu64 "/%" PRIu64
                " shaders=%" PRIu64 "/%" PRIu64
                " subprograms=%" PRIu64 "/%" PRIu64
                " entries=%" PRIu64 "/%" PRIu64
                " variants=%" PRIu64 "/%" PRIu64
                " parameters=%" PRIu64 "/%" PRIu64
                " dxbc=%" PRIu64 "/%" PRIu64
                " semantic=%" PRIu64 "/%" PRIu64 "\n",
                known->label, stats->bundle_members, known->members,
                stats->serialized_files, known->serialized_files,
                stats->objects, known->objects, stats->shaders,
                known->shaders, stats->subprograms, known->subprograms,
                stats->blob_entries, known->blob_entries,
                stats->referenced_variant_entries, known->variant_entries,
                stats->referenced_parameter_entries,
                known->parameter_entries, stats->dxbc_documents,
                known->variant_entries, stats->dxbc_semantic_projections,
                known->semantic_projections);
        return false;
    }
    if (stats->properties != known->properties ||
        stats->passes != known->passes ||
        stats->advertised_platforms != known->advertised_platforms ||
        stats->blob_segments != known->blob_segments ||
        stats->stage_counts_total != known->stage_counts_total ||
        stats->player_header_nonzero_words !=
            known->player_header_nonzero_words ||
        stats->player_source_maps_nonzero !=
            known->player_source_maps_nonzero) {
        fprintf(stderr,
                "[CORPUS ERROR] pinned ingestion totals %s differ: "
                "properties=%" PRIu64 "/%" PRIu64
                " passes=%" PRIu64 "/%" PRIu64
                " platforms=%" PRIu64 "/%" PRIu64
                " segments=%" PRIu64 "/%" PRIu64
                " stageCounts=%" PRIu64 "/%" PRIu64
                " header-words=%" PRIu64 "/%" PRIu64
                " source-maps=%" PRIu64 "/%" PRIu64 "\n",
                known->label, stats->properties, known->properties,
                stats->passes, known->passes,
                stats->advertised_platforms, known->advertised_platforms,
                stats->blob_segments, known->blob_segments,
                stats->stage_counts_total, known->stage_counts_total,
                stats->player_header_nonzero_words,
                known->player_header_nonzero_words,
                stats->player_source_maps_nonzero,
                known->player_source_maps_nonzero);
        return false;
    }
    for (size_t stage = 0; stage < UNITY_SERIALIZED_STAGE_COUNT; stage++) {
        if (stats->stage_subprograms[stage] !=
                known->stage_subprograms[stage] ||
            stats->dxbc_stage_contracts[stage] !=
                known->stage_contracts[stage]) {
            fprintf(stderr,
                    "[CORPUS ERROR] pinned stage %zu in %s differs: "
                    "records=%" PRIu64 "/%" PRIu64
                    " contracts=%" PRIu64 "/%" PRIu64 "\n",
                    stage, known->label,
                    stats->stage_subprograms[stage],
                    known->stage_subprograms[stage],
                    stats->dxbc_stage_contracts[stage],
                    known->stage_contracts[stage]);
            return false;
        }
    }
    return true;
}

static void print_taxonomy(const char* label, IntegerTaxon* taxa,
                           size_t count) {
    sort_taxa(taxa, count);
    printf("  %s:", label);
    for (size_t i = 0; i < count; i++) {
        printf(" %d=%" PRIu64, taxa[i].value, taxa[i].count);
    }
    printf("\n");
}

static void print_stats(const char* path, const char* digest_hex,
                        const KnownCorpus* known, CorpusStats* stats) {
    printf("[CORPUS] %s sha256=%s%s\n", path_basename(path), digest_hex,
           known ? " pinned" : " unpinned");
    printf("  members=%" PRIu64 " serialized=%" PRIu64
           " resources=%" PRIu64 " directories=%" PRIu64
           " deleted=%" PRIu64 "\n",
           stats->bundle_members, stats->serialized_members,
           stats->auxiliary_resources, stats->auxiliary_directories,
           stats->deleted_members);
    printf("  files=%" PRIu64 " typetree-enabled=%" PRIu64
           " typetree-disabled=%" PRIu64 " types=%" PRIu64
           " objects=%" PRIu64 " shaders=%" PRIu64 "\n",
           stats->serialized_files, stats->typetree_enabled_files,
           stats->typetree_disabled_files, stats->types, stats->objects,
           stats->shaders);
    printf("  shader-bytes=%" PRIu64 " properties=%" PRIu64
           " subshaders=%" PRIu64 " passes=%" PRIu64
           " subprograms=%" PRIu64 "\n",
           stats->shader_object_bytes, stats->properties,
           stats->subshaders, stats->passes, stats->subprograms);
    printf("  stages: vertex=%" PRIu64 " fragment=%" PRIu64
           " geometry=%" PRIu64 " hull=%" PRIu64
           " domain=%" PRIu64 " raytracing=%" PRIu64 "\n",
           stats->stage_subprograms[0], stats->stage_subprograms[1],
           stats->stage_subprograms[2], stats->stage_subprograms[3],
           stats->stage_subprograms[4], stats->stage_subprograms[5]);
    printf("  archives=%" PRIu64 " advertised-platforms=%" PRIu64
           " segments=%" PRIu64 " stage-count-total=%" PRIu64
           " entries=%" PRIu64
           " payload-bytes=%" PRIu64 "\n",
           stats->blob_archives, stats->advertised_platforms,
           stats->blob_segments, stats->stage_counts_total,
           stats->blob_entries,
           stats->blob_payload_bytes);
    printf("  entry-roles: variants=%" PRIu64 " parameters=%" PRIu64
           " dual=%" PRIu64 " unreferenced=%" PRIu64 "\n",
           stats->referenced_variant_entries,
           stats->referenced_parameter_entries,
           stats->dual_role_entries, stats->unreferenced_entries);
    printf("  player-wrapper: headers=%" PRIu64
           " nonzero-header-words=%" PRIu64
           " nonzero-source-maps=%" PRIu64 "\n",
           stats->player_wrapper_headers,
           stats->player_header_nonzero_words,
           stats->player_source_maps_nonzero);
    printf("  dxbc: documents=%" PRIu64 " exact-roundtrips=%" PRIu64
           " semantic-usil=%" PRIu64 "\n",
           stats->dxbc_documents, stats->dxbc_exact_roundtrips,
           stats->dxbc_semantic_projections);
    printf("  dxbc-contracts: vertex=%" PRIu64 " fragment=%" PRIu64
           " geometry=%" PRIu64 " hull=%" PRIu64
           " domain=%" PRIu64 " raytracing=%" PRIu64 "\n",
           stats->dxbc_stage_contracts[UNITY_SERIALIZED_STAGE_VERTEX],
           stats->dxbc_stage_contracts[UNITY_SERIALIZED_STAGE_FRAGMENT],
           stats->dxbc_stage_contracts[UNITY_SERIALIZED_STAGE_GEOMETRY],
           stats->dxbc_stage_contracts[UNITY_SERIALIZED_STAGE_HULL],
           stats->dxbc_stage_contracts[UNITY_SERIALIZED_STAGE_DOMAIN],
           stats->dxbc_stage_contracts[UNITY_SERIALIZED_STAGE_RAY_TRACING]);
    printf("  dxbc-operand-types:");
    for (int type = OPERAND_TYPE_TEMP;
         type <= OPERAND_TYPE_INNER_COVERAGE; ++type) {
        if (stats->dxbc_operand_types[type] != 0) {
            printf(" %d=%" PRIu64, type,
                   stats->dxbc_operand_types[type]);
        }
    }
    putchar('\n');
    print_taxonomy("ClassID", stats->class_ids, stats->class_id_count);
    print_taxonomy("platform", stats->platforms, stats->platform_count);
}

static bool validate_bundle(const char* path,
                            TypeTreeSchemaRegistry* registry) {
    CommonFileBytes input;
    CommonFileStatus file_status =
        common_file_read_regular(path, SIZE_MAX, &input);
    if (file_status != COMMON_FILE_OK) {
        fprintf(stderr, "[CORPUS ERROR] read %s: %s\n", path,
                common_file_status_name(file_status));
        return false;
    }
    const uint8_t* file_data = input.data;
    const size_t file_size = input.size;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char digest_hex[COMMON_SHA256_DIGEST_SIZE * 2u + 1u];
    common_sha256(file_data, file_size, digest);
    digest_to_hex(digest, digest_hex);
    const KnownCorpus* known = find_known_corpus(digest_hex);
    if (!known) {
        fprintf(stderr,
                "[CORPUS ERROR] %s sha256=%s is not in the pinned corpus\n",
                path_basename(path), digest_hex);
        common_file_bytes_dispose(&input);
        return false;
    }

    BundleArchive bundle;
    if (!bundle_open(&bundle, file_data, file_size)) {
        fprintf(stderr, "[CORPUS ERROR] %s is not a valid UnityFS bundle\n",
                path);
        common_file_bytes_dispose(&input);
        return false;
    }
    CorpusStats stats;
    memset(&stats, 0, sizeof(stats));
    bool success = true;
    const char* bundle_name = path_basename(path);
    for (int member_index = 0;
         member_index < bundle.directory_count; member_index++) {
        const BundleDirectoryInfo* member =
            &bundle.directories[member_index];
        if (!add_counter(&stats.bundle_members, 1)) {
            success = false;
            break;
        }
        if (!member->name || member->offset > bundle.payload_size ||
            member->decompressed_size >
                bundle.payload_size - (size_t)member->offset) {
            fprintf(stderr,
                    "[CORPUS ERROR] %s has an invalid member range\n",
                    bundle_name);
            success = false;
            break;
        }
        size_t member_size = (size_t)member->decompressed_size;
        const uint8_t* member_data = member_size == 0 ? NULL :
            bundle.payload + (size_t)member->offset;
        BundleMemberKind member_kind = bundle_member_classify(member);
        if (member_kind == BUNDLE_MEMBER_SERIALIZED_FILE) {
            if (!add_counter(&stats.serialized_members, 1) ||
                !validate_serialized_member(
                    bundle_name, member->name, member_data, member_size,
                    registry, &stats)) {
                success = false;
                break;
            }
        } else if (member_kind == BUNDLE_MEMBER_DIRECTORY) {
            if (member_size != 0 ||
                !add_counter(&stats.auxiliary_directories, 1)) {
                fprintf(stderr,
                        "[CORPUS ERROR] %s directory member %s has invalid "
                        "flags or data\n",
                        bundle_name, member->name);
                success = false;
                break;
            }
        } else if (member_kind == BUNDLE_MEMBER_DELETED) {
            if (member_size != 0 ||
                !add_counter(&stats.deleted_members, 1)) {
                fprintf(stderr,
                        "[CORPUS ERROR] %s deleted member %s is nonempty\n",
                        bundle_name, member->name);
                success = false;
                break;
            }
        } else if (member_kind == BUNDLE_MEMBER_RESOURCE) {
            if (looks_like_v22_serialized_file(member_data, member_size)) {
                fprintf(stderr,
                        "[CORPUS ERROR] %s member %s looks serialized but "
                        "lacks the SerializedFile flag\n",
                        bundle_name, member->name);
                success = false;
                break;
            }
            if (!add_counter(&stats.auxiliary_resources, 1)) {
                success = false;
                break;
            }
        } else {
            fprintf(stderr,
                    "[CORPUS ERROR] %s member %s flags=0x%x has no explicit "
                    "SerializedFile or auxiliary classification\n",
                    bundle_name, member->name, member->flags);
            success = false;
            break;
        }
    }
    if (success) success = validate_known_counts(known, &stats);
    print_stats(path, digest_hex, known, &stats);
    bundle_close(&bundle);
    common_file_bytes_dispose(&input);
    return success;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s bundle [bundle ...]\n", argv[0]);
        return 2;
    }
    size_t baseline_allocations = g_allocations_count;
    size_t baseline_bytes = g_allocated_bytes;
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    bool success = true;
    for (int argument = 1; argument < argc; argument++) {
        if (!validate_bundle(argv[argument], &registry)) success = false;
    }
    bool registry_roundtrip_ok = validate_registry_roundtrip(&registry);
    if (!registry_roundtrip_ok) success = false;
    size_t registry_count = typetree_schema_registry_count(&registry);
    typetree_schema_registry_dispose(&registry);
    if (g_allocations_count != baseline_allocations ||
        g_allocated_bytes != baseline_bytes) {
        fprintf(stderr,
                "[CORPUS ERROR] tracked allocations differ: blocks=%zu/%zu "
                "bytes=%zu/%zu\n",
                g_allocations_count, baseline_allocations,
                g_allocated_bytes, baseline_bytes);
        success = false;
    }
    printf("[CORPUS] registry-schemas=%zu canonical-roundtrip=%s\n",
           registry_count, registry_roundtrip_ok ? "yes" : "failed");
    printf("[CORPUS] result=%s\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
