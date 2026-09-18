#include "dxbc/dxbc_parser.h"
#include "dxbc/dxbc_parser_internal.h"
#include "dxbc/dxbc_decoder.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_emitter_ops_internal.h"
#include "translation/usil.h"
#include "test_fixture.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint32_t read_le_u32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_le_u32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static uint8_t* read_fixture(size_t* out_size) {
    FILE* file = fopen(DXBC_TEST_FIXTURE, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) return NULL;
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    uint8_t* data = (uint8_t*)malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

static int verify_golden_corpus(void) {
    static const char* cases[] = {
        "dxbctests_1_propertymap", "dxbctests_2_texture",
        "dxbctests_3_math", "dxbctests_4_controlflow",
        "dxbctests_5_keywords", "fx_flare", "gui_text_shader",
        "hidden_blitcopy", "hidden_blitcopydepth", "hidden_checkerboard",
        "hidden_internal-colored", "hidden_internal-flare",
        "hidden_preview_2d_texture_array",
        "hidden_shader_graph_fallbackerror", "hidden_showshadowcascadesplits",
        "hidden_vr_blittexarrayslice", "hidden_vr_internal-vrdistortion",
        "sprites_default", "unlit_color", "unlit_texture",
        "unlit_transparent", "unlit_transparent_cutout",
        "vr_spatialmapping_occlusion"
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/target.bin", DXBC_TEST_GOLDEN_DIR,
                 cases[i]);
        FILE* file = fopen(path, "rb");
        if (!file || fseek(file, 0, SEEK_END) != 0) return 0;
        long length = ftell(file);
        if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            return 0;
        }
        uint8_t* bytes = (uint8_t*)malloc((size_t)length);
        if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
            free(bytes);
            fclose(file);
            return 0;
        }
        fclose(file);
        DXBCContainer container;
        const bool parsed = dxbc_parse(&container, bytes, (size_t)length);
        free(bytes);
        if (!parsed) return 0;
        USILProgram program;
        if (!usil_translate(&program, &container)) {
            fprintf(stderr, "golden USIL failure: %s\n", cases[i]);
            dxbc_free(&container);
            return 0;
        }
        StringBuilder hlsl;
        sb_init(&hlsl);
        const bool emitted = hlsl_emit(&program, &hlsl, NULL, NULL, NULL);
        sb_free(&hlsl);
        usil_free(&program);
        dxbc_free(&container);
        if (!emitted) {
            fprintf(stderr, "golden HLSL failure: %s\n", cases[i]);
            return 0;
        }
    }
    return 1;
}

static size_t first_usbd_payload_offset(const uint8_t* data, size_t size) {
    if (size < 16 || memcmp(data, "USBD", 4) != 0) return 0;
    uint32_t name_length = read_le_u32(data + 8);
    size_t payload_offset = 12u + name_length + 4u;
    return payload_offset <= size ? payload_offset : 0;
}

static uint32_t operand_token(uint32_t type, uint32_t component_count,
                              uint32_t index_dimensions,
                              uint32_t index0_representation,
                              uint32_t index1_representation,
                              uint32_t index2_representation) {
    return component_count | (type << 12) | (index_dimensions << 20) |
           (index0_representation << 22) |
           (index1_representation << 25) |
           (index2_representation << 28);
}

static DXBCOperand* parse_operand_words(const uint32_t* words,
                                        size_t word_count,
                                        ByteStream* out_stream,
                                        uint8_t* storage,
                                        size_t storage_size) {
    if (word_count > storage_size / sizeof(uint32_t)) return NULL;
    for (size_t i = 0; i < word_count; ++i) {
        write_le_u32(storage + i * sizeof(uint32_t), words[i]);
    }
    stream_init(out_stream, storage, word_count * sizeof(uint32_t));
    return parse_operand_recursive(out_stream, out_stream->size,
                                   DXBC_OPERAND_CONTEXT_EXECUTABLE, 54u,
                                   NULL);
}

static void destroy_test_operand(DXBCOperand* operand) {
    if (!operand) return;
    free_operand(operand);
    mem_free(operand, sizeof(DXBCOperand));
}

static bool operand_words_rejected(const uint32_t* words, size_t word_count) {
    uint8_t storage[128] = {0};
    ByteStream stream;
    size_t allocation_count = g_allocations_count;
    size_t allocated_bytes = g_allocated_bytes;
    DXBCOperand* operand = parse_operand_words(
        words, word_count, &stream, storage, sizeof(storage));
    bool rejected = operand == NULL && stream.position == stream.size &&
                    g_allocations_count == allocation_count &&
                    g_allocated_bytes == allocated_bytes;
    destroy_test_operand(operand);
    return rejected;
}

static int verify_operand_decoder(void) {
    uint8_t storage[256] = {0};
    ByteStream stream;
    DXBCOperand* operand;
    const uint32_t relative = operand_token(OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0);

    const uint32_t immediate32_index[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0), 7
    };
    operand = parse_operand_words(immediate32_index, 2, &stream, storage,
                                  sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->index_representations[0] == 0);
    CHECK(operand->index_has_immediate[0]);
    CHECK(operand->index_values[0] == 7 && operand->register_index == 7);
    CHECK(!operand->rel_op0);
    destroy_test_operand(operand);

    const uint32_t immediate64_index[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 1, 0, 0),
        0x89abcdefu, 0x00000001u
    };
    operand = parse_operand_words(immediate64_index, 3, &stream, storage,
                                  sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->index_representations[0] == 1);
    CHECK(operand->index_values[0] == UINT64_C(0x0000000189abcdef));
    CHECK(operand->index_value_exceeds_int[0] && operand->register_index == -1);
    destroy_test_operand(operand);

    const uint32_t relative_index[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 2, 0, 0), relative, 9
    };
    operand = parse_operand_words(relative_index, 3, &stream, storage,
                                  sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->index_representations[0] == 2);
    CHECK(!operand->index_has_immediate[0] && operand->rel_op0 != NULL);
    CHECK(operand->rel_op0->register_index == 9);
    destroy_test_operand(operand);

    const uint32_t immediate32_relative_index[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 3, 0, 0), 5, relative, 9
    };
    operand = parse_operand_words(immediate32_relative_index, 4, &stream,
                                  storage, sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->index_representations[0] == 3);
    CHECK(operand->index_has_immediate[0] && operand->index_values[0] == 5);
    CHECK(operand->rel_op0 && operand->rel_op0->register_index == 9);
    destroy_test_operand(operand);

    const uint32_t immediate64_relative_index[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 4, 0, 0),
        0x76543210u, 0x00000001u, relative, 9
    };
    operand = parse_operand_words(immediate64_relative_index, 5, &stream,
                                  storage, sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->index_representations[0] == 4);
    CHECK(operand->index_values[0] == UINT64_C(0x0000000176543210));
    CHECK(operand->rel_op0 && operand->rel_op0->register_index == 9);
    destroy_test_operand(operand);

    const uint32_t three_dimensions[] = {
        operand_token(19, 1, 3, 0, 3, 4),
        11,
        22, relative, 3,
        33, 1, relative, 4
    };
    operand = parse_operand_words(three_dimensions, 9, &stream, storage,
                                  sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->register_index_dim == 3);
    CHECK(operand->index_representations[0] == 0);
    CHECK(operand->index_representations[1] == 3);
    CHECK(operand->index_representations[2] == 4);
    CHECK(operand->index_values[0] == 11 && operand->index_values[1] == 22);
    CHECK(operand->index_values[2] == UINT64_C(0x0000000100000021));
    CHECK(operand->rel_op1 && operand->rel_op1->register_index == 3);
    CHECK(operand->rel_op2 && operand->rel_op2->register_index == 4);
    destroy_test_operand(operand);

    const uint32_t immediate32_selected[] = {
        2u | (2u << 2) | (2u << 4) |
            ((uint32_t)OPERAND_TYPE_IMMEDIATE32 << 12),
        10, 20, 30, 40
    };
    operand = parse_operand_words(immediate32_selected, 5, &stream, storage,
                                  sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->immediate_word_count == 4);
    CHECK(operand->immediate_words[0] == 10 &&
          operand->immediate_words[3] == 40);
    CHECK(operand->imm_value_count == 1 && operand->imm_values[0] == 30);
    destroy_test_operand(operand);

    const uint32_t immediate64_scalar[] = {
        operand_token(OPERAND_TYPE_IMMEDIATE64, 1, 0, 0, 0, 0),
        0, 0x3ff00000u
    };
    operand = parse_operand_words(immediate64_scalar, 3, &stream, storage,
                                  sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->type == OPERAND_TYPE_IMMEDIATE64);
    CHECK(operand->immediate_word_count == 2 && operand->imm_value_count == 1);
    CHECK(operand->imm64_values[0] == UINT64_C(0x3ff0000000000000));
    destroy_test_operand(operand);

    const uint32_t immediate64_vector[] = {
        operand_token(OPERAND_TYPE_IMMEDIATE64, 2, 0, 0, 0, 0) |
            (1u << 2) | (3u << 4) | (2u << 6) | (1u << 8),
        1, 0, 2, 0, 3, 0, 4, 0
    };
    operand = parse_operand_words(immediate64_vector, 9, &stream, storage,
                                  sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->immediate_word_count == 8 && operand->imm_value_count == 4);
    CHECK(operand->imm64_values[0] == 4 && operand->imm64_values[1] == 3 &&
          operand->imm64_values[2] == 2 && operand->imm64_values[3] == 1);
    destroy_test_operand(operand);

    const uint32_t chained_extensions[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0) | 0x80000000u,
        0x80000000u | 1u | (3u << 6) | (5u << 14),
        0,
        12
    };
    operand = parse_operand_words(chained_extensions, 4, &stream, storage,
                                  sizeof(storage));
    CHECK(operand != NULL && stream.position == stream.size);
    CHECK(operand->raw_token == chained_extensions[0]);
    CHECK(operand->extended_token_count == 2);
    CHECK(operand->extended_tokens[0] == chained_extensions[1] &&
          operand->extended_tokens[1] == chained_extensions[2]);
    CHECK(operand->has_abs && operand->has_neg && operand->min_precision == 5);
    destroy_test_operand(operand);

    const uint32_t truncated_extension[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 0, 0, 0, 0) | 0x80000000u
    };
    const uint32_t truncated_extension_chain[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 0, 0, 0, 0) | 0x80000000u,
        0x80000000u
    };
    const uint32_t truncated_immediate32[] = {
        operand_token(OPERAND_TYPE_IMMEDIATE32, 1, 0, 0, 0, 0)
    };
    const uint32_t truncated_immediate64[] = {
        operand_token(OPERAND_TYPE_IMMEDIATE64, 1, 0, 0, 0, 0), 1
    };
    const uint32_t truncated_index32[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0)
    };
    const uint32_t truncated_index64[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 1, 0, 0), 1
    };
    const uint32_t truncated_relative[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 2, 0, 0)
    };
    const uint32_t truncated_relative_body[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 2, 0, 0), relative
    };
    const uint32_t truncated_index32_relative[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 3, 0, 0), 1
    };
    const uint32_t truncated_index64_relative[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 4, 0, 0), 1, 0
    };
    const uint32_t reserved_index_representation[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 5, 0, 0)
    };
    const uint32_t unsupported_component_count[] = {
        operand_token(OPERAND_TYPE_TEMP, 3, 0, 0, 0, 0)
    };
    const uint32_t unsupported_selection_mode[] = {
        operand_token(OPERAND_TYPE_TEMP, 2, 0, 0, 0, 0) | (3u << 2)
    };
    const uint32_t reserved_min_precision[] = {
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0) | 0x80000000u,
        1u | (3u << 14), 0u
    };
    CHECK(operand_words_rejected(truncated_extension, 1));
    CHECK(operand_words_rejected(truncated_extension_chain, 2));
    CHECK(operand_words_rejected(truncated_immediate32, 1));
    CHECK(operand_words_rejected(truncated_immediate64, 2));
    CHECK(operand_words_rejected(truncated_index32, 1));
    CHECK(operand_words_rejected(truncated_index64, 2));
    CHECK(operand_words_rejected(truncated_relative, 1));
    CHECK(operand_words_rejected(truncated_relative_body, 2));
    CHECK(operand_words_rejected(truncated_index32_relative, 2));
    CHECK(operand_words_rejected(truncated_index64_relative, 3));
    CHECK(operand_words_rejected(reserved_index_representation, 1));
    CHECK(operand_words_rejected(unsupported_component_count, 1));
    CHECK(operand_words_rejected(unsupported_selection_mode, 1));
    CHECK(operand_words_rejected(reserved_min_precision, 3));

    /* The seven-bit instruction length admits 126 operand-token DWORDs after
     * the opcode.  Exercise that exact structural maximum: the former depth
     * heuristic rejected this valid finite chain at operand 65. */
    uint32_t maximum_relative_chain[DXBC_MAX_NESTED_OPERAND_TOKENS];
    for (size_t index = 0;
         index + 1u < DXBC_MAX_NESTED_OPERAND_TOKENS; ++index) {
        maximum_relative_chain[index] =
            operand_token(OPERAND_TYPE_TEMP, 1, 1, 2, 0, 0);
    }
    maximum_relative_chain[DXBC_MAX_NESTED_OPERAND_TOKENS - 1u] =
        operand_token(OPERAND_TYPE_TEMP, 1, 0, 0, 0, 0);
    uint8_t maximum_chain_storage[sizeof(maximum_relative_chain)] = {0};
    const size_t chain_allocation_count = g_allocations_count;
    const size_t chain_allocated_bytes = g_allocated_bytes;
    operand = parse_operand_words(
        maximum_relative_chain, DXBC_MAX_NESTED_OPERAND_TOKENS, &stream,
        maximum_chain_storage, sizeof(maximum_chain_storage));
    CHECK(operand != NULL && stream.position == stream.size);
    size_t parsed_operand_tokens = 0u;
    for (const DXBCOperand* nested = operand; nested;
         nested = nested->rel_op0) {
        CHECK(nested->rel_op1 == NULL && nested->rel_op2 == NULL);
        ++parsed_operand_tokens;
    }
    CHECK(parsed_operand_tokens == DXBC_MAX_NESTED_OPERAND_TOKENS);
    destroy_test_operand(operand);
    CHECK(g_allocations_count == chain_allocation_count);
    CHECK(g_allocated_bytes == chain_allocated_bytes);

    /* One more operand token would require an unrepresentable 128-DWORD
     * ordinary instruction.  Reject it by the grammar budget and unwind the
     * entire partially owned chain. */
    uint32_t over_budget_chain[DXBC_MAX_NESTED_OPERAND_TOKENS + 1u];
    for (size_t index = 0; index < DXBC_MAX_NESTED_OPERAND_TOKENS;
         ++index) {
        over_budget_chain[index] =
            operand_token(OPERAND_TYPE_TEMP, 1, 1, 2, 0, 0);
    }
    over_budget_chain[DXBC_MAX_NESTED_OPERAND_TOKENS] =
        operand_token(OPERAND_TYPE_TEMP, 1, 0, 0, 0, 0);
    uint8_t over_budget_storage[sizeof(over_budget_chain)] = {0};
    operand = parse_operand_words(
        over_budget_chain, DXBC_MAX_NESTED_OPERAND_TOKENS + 1u, &stream,
        over_budget_storage, sizeof(over_budget_storage));
    CHECK(operand == NULL && stream.position == stream.size);
    CHECK(g_allocations_count == chain_allocation_count);
    CHECK(g_allocated_bytes == chain_allocated_bytes);
    return 0;
}

static uint32_t instruction_token(uint32_t opcode, uint32_t length) {
    return opcode | (length << 24);
}

static size_t build_test_shader_container(
    const uint32_t* instruction_words, size_t instruction_word_count,
    uint32_t declared_program_word_count, uint8_t* output,
    size_t output_capacity) {
    const size_t payload_word_count = 2 + instruction_word_count;
    const size_t payload_size = payload_word_count * sizeof(uint32_t);
    const size_t total_size = 44 + payload_size;
    if (!instruction_words || !output || total_size > output_capacity ||
        total_size > UINT32_MAX) {
        return 0;
    }

    memset(output, 0, total_size);
    memcpy(output, "DXBC", 4);
    write_le_u32(output + 20, 1);
    write_le_u32(output + 24, (uint32_t)total_size);
    write_le_u32(output + 28, 1);
    write_le_u32(output + 32, 36);
    memcpy(output + 36, "SHDR", 4);
    write_le_u32(output + 40, (uint32_t)payload_size);
    write_le_u32(output + 44, 0x00000050u); /* ps_5_0 */
    write_le_u32(output + 48, declared_program_word_count);
    for (size_t word = 0; word < instruction_word_count; ++word) {
        write_le_u32(output + 52 + word * sizeof(uint32_t),
                     instruction_words[word]);
    }
    uint8_t hash[16];
    if (!dxbc_compute_hash(output, total_size, hash)) return 0;
    memcpy(output + 4, hash, sizeof(hash));
    return total_size;
}

static size_t build_signature_capacity_container(
    uint32_t signature_count, uint32_t padding_chunk_count,
    const char* semantic_name, uint8_t* output, size_t output_capacity) {
    const size_t semantic_name_size =
        semantic_name ? strlen(semantic_name) + 1u : 0u;
    const uint32_t chunk_count = padding_chunk_count + 2u;
    const uint32_t declaration_count =
        signature_count < 16u ? signature_count : 16u;
    const size_t table_size = (size_t)chunk_count * sizeof(uint32_t);
    const size_t records_size = (size_t)signature_count * 24u;
    const size_t semantic_offset = 8u + records_size;
    const size_t unaligned_signature_size =
        semantic_offset + semantic_name_size;
    const size_t signature_size = (unaligned_signature_size + 3u) & ~(size_t)3u;
    const size_t total_size = 32u + table_size +
                              (size_t)padding_chunk_count * 8u +
                              8u + signature_size + 8u +
                              (size_t)(3u + declaration_count * 3u) * 4u;
    if (!output || !semantic_name || semantic_name_size == 0u ||
        chunk_count < padding_chunk_count ||
        signature_count > INT_MAX || total_size > output_capacity ||
        total_size > UINT32_MAX || semantic_offset > UINT32_MAX ||
        signature_size > UINT32_MAX) {
        return 0;
    }

    memset(output, 0, total_size);
    memcpy(output, "DXBC", 4);
    write_le_u32(output + 20, 1);
    write_le_u32(output + 24, (uint32_t)total_size);
    write_le_u32(output + 28, chunk_count);

    size_t chunk_offset = 32u + table_size;
    for (uint32_t chunk = 0; chunk < padding_chunk_count; ++chunk) {
        write_le_u32(output + 32u + (size_t)chunk * 4u,
                     (uint32_t)chunk_offset);
        memcpy(output + chunk_offset, "STAT", 4);
        write_le_u32(output + chunk_offset + 4u, 0);
        chunk_offset += 8u;
    }

    write_le_u32(output + 32u + (size_t)padding_chunk_count * 4u,
                 (uint32_t)chunk_offset);
    memcpy(output + chunk_offset, "ISGN", 4);
    write_le_u32(output + chunk_offset + 4u, (uint32_t)signature_size);
    const size_t signature_start = chunk_offset + 8u;
    write_le_u32(output + signature_start, signature_count);
    write_le_u32(output + signature_start + 4u, 8u);
    for (uint32_t element = 0; element < signature_count; ++element) {
        const size_t record = signature_start + 8u + (size_t)element * 24u;
        write_le_u32(output + record, (uint32_t)semantic_offset);
        write_le_u32(output + record + 4u, element);
        write_le_u32(output + record + 8u, 0);
        write_le_u32(output + record + 12u, 3);
        write_le_u32(output + record + 16u, element % 16u);
        write_le_u32(output + record + 20u, 0x00000f0fu);
    }
    memcpy(output + signature_start + semantic_offset, semantic_name,
           semantic_name_size);
    chunk_offset += 8u + signature_size;

    write_le_u32(output + 32u + (size_t)(chunk_count - 1u) * 4u,
                 (uint32_t)chunk_offset);
    memcpy(output + chunk_offset, "SHDR", 4);
    const uint32_t shader_word_count = 3u + declaration_count * 3u;
    write_le_u32(output + chunk_offset + 4u,
                 shader_word_count * 4u);
    write_le_u32(output + chunk_offset + 8u, 0x00000050u);
    write_le_u32(output + chunk_offset + 12u, shader_word_count);
    size_t shader_word_offset = chunk_offset + 16u;
    for (uint32_t declaration = 0u;
         declaration < declaration_count; ++declaration) {
        write_le_u32(output + shader_word_offset,
                     instruction_token(95u, 3u));
        write_le_u32(
            output + shader_word_offset + 4u,
            operand_token(OPERAND_TYPE_INPUT, 2u, 1u, 0u, 0u, 0u) |
                UINT32_C(0x000000f0));
        write_le_u32(output + shader_word_offset + 8u, declaration);
        shader_word_offset += 12u;
    }
    write_le_u32(output + shader_word_offset, instruction_token(62, 1));

    uint8_t hash[16];
    if (!dxbc_compute_hash(output, total_size, hash)) return 0;
    memcpy(output + 4, hash, sizeof(hash));
    return total_size;
}

static bool test_shader_parse_result(const uint32_t* instruction_words,
                                     size_t instruction_word_count,
                                     uint32_t declared_program_word_count,
                                     bool expected_result,
                                     int* out_instruction_count) {
    uint8_t bytes[512];
    size_t byte_count = build_test_shader_container(
        instruction_words, instruction_word_count,
        declared_program_word_count, bytes, sizeof(bytes));
    if (byte_count == 0) return false;

    size_t allocation_count = g_allocations_count;
    size_t allocated_bytes = g_allocated_bytes;
    DXBCContainer container;
    bool result = dxbc_parse(&container, bytes, byte_count);
    if (out_instruction_count)
        *out_instruction_count = result ? container.instruction_count : 0;
    if (result) dxbc_free(&container);
    return result == expected_result &&
           g_allocations_count == allocation_count &&
           g_allocated_bytes == allocated_bytes;
}

static int verify_precision_and_instruction_controls(void) {
    const uint32_t temp =
        operand_token(OPERAND_TYPE_TEMP, 2, 1, 0, 0, 0) | 0xf0u;
    const uint32_t immediate =
        operand_token(OPERAND_TYPE_IMMEDIATE32, 1, 0, 0, 0, 0);
    const uint32_t resource =
        operand_token(OPERAND_TYPE_RESOURCE, 1, 1, 0, 0, 0);

    const uint32_t precise_program[] = {
        instruction_token(54, 5) | (1u << 19),
        temp, 0u, immediate, UINT32_C(0x3f800000),
        instruction_token(62, 1)
    };
    uint8_t bytes[512];
    size_t byte_count = build_test_shader_container(
        precise_program,
        sizeof(precise_program) / sizeof(precise_program[0]),
        2u + (uint32_t)(sizeof(precise_program) /
                        sizeof(precise_program[0])),
        bytes, sizeof(bytes));
    CHECK(byte_count > 0u);
    DXBCContainer container;
    CHECK(dxbc_parse(&container, bytes, byte_count));
    CHECK(container.instruction_count == 2);
    CHECK(container.instructions[0].precise_mask == 1u);
    USILProgram program;
    CHECK(usil_translate(&program, &container));
    CHECK(program.instructions[0].precise_mask == 1u);
    StringBuilder hlsl;
    sb_init(&hlsl);
    CHECK(!hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    sb_free(&hlsl);

    program.instructions[0].precise_mask = 0u;
    /* The compact parser fixture omits DCL_TEMPS so it can isolate opcode
     * controls.  Supply the declaration projection before asking the HLSL
     * boundary to validate the otherwise well-formed r0 destination. */
    program.temp_count = 1;
    sb_init(&hlsl);
    CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    sb_free(&hlsl);
    program.instructions[0].operands[1].min_precision = 1u;
    sb_init(&hlsl);
    CHECK(!hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    sb_free(&hlsl);
    program.instructions[0].operands[1].min_precision = 0u;

    DXBCSignatureElement input;
    memset(&input, 0, sizeof(input));
    snprintf(input.semantic_name, sizeof(input.semantic_name), "TEXCOORD");
    input.register_id = 0u;
    input.mask = 0x0fu;
    input.component_type = 3u;
    input.min_precision = 1u;
    program.inputs = &input;
    program.input_count = 1;
    program.input_alloc = 1;
    sb_init(&hlsl);
    CHECK(!hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    sb_free(&hlsl);
    program.inputs = NULL;
    program.input_count = 0;
    program.input_alloc = 0;
    usil_free(&program);
    dxbc_free(&container);

    const uint32_t ordinary_zero_length[] = {54u, 3u, 0u};
    CHECK(test_shader_parse_result(
        ordinary_zero_length,
        sizeof(ordinary_zero_length) /
            sizeof(ordinary_zero_length[0]),
        5u, false, NULL));

    const uint32_t sampleinfo_float[] = {
        instruction_token(111, 5), temp, 0u, resource, 0u,
        instruction_token(62, 1)
    };
    byte_count = build_test_shader_container(
        sampleinfo_float,
        sizeof(sampleinfo_float) / sizeof(sampleinfo_float[0]),
        2u + (uint32_t)(sizeof(sampleinfo_float) /
                        sizeof(sampleinfo_float[0])),
        bytes, sizeof(bytes));
    CHECK(byte_count > 0u && dxbc_parse(&container, bytes, byte_count));
    CHECK(container.instructions[0].sample_info_return_type == 0u);
    CHECK(strstr(container.instructions[0].formatted_asm, "_uint") == NULL);
    dxbc_free(&container);

    uint32_t sampleinfo_uint[
        sizeof(sampleinfo_float) / sizeof(sampleinfo_float[0])];
    memcpy(sampleinfo_uint, sampleinfo_float, sizeof(sampleinfo_uint));
    sampleinfo_uint[0] |= 1u << 11;
    byte_count = build_test_shader_container(
        sampleinfo_uint,
        sizeof(sampleinfo_uint) / sizeof(sampleinfo_uint[0]),
        2u + (uint32_t)(sizeof(sampleinfo_uint) /
                        sizeof(sampleinfo_uint[0])),
        bytes, sizeof(bytes));
    CHECK(byte_count > 0u && dxbc_parse(&container, bytes, byte_count));
    CHECK(container.instructions[0].sample_info_return_type == 1u);
    CHECK(strstr(container.instructions[0].formatted_asm,
                 "sampleinfo_uint") != NULL);
    dxbc_free(&container);

    sampleinfo_uint[0] = sampleinfo_float[0] | (2u << 11);
    CHECK(test_shader_parse_result(
        sampleinfo_uint,
        sizeof(sampleinfo_uint) / sizeof(sampleinfo_uint[0]),
        2u + (uint32_t)(sizeof(sampleinfo_uint) /
                        sizeof(sampleinfo_uint[0])),
        false, NULL));
    return 0;
}

static int verify_signature_parser_authority(void) {
    uint8_t bytes[1024];
    size_t byte_count = build_signature_capacity_container(
        1u, 0u, "TEXCOORD", bytes, sizeof(bytes));
    CHECK(byte_count > 0u);
    const uint32_t chunk = read_le_u32(bytes + 32u);
    const size_t record = (size_t)chunk + 16u;
    uint8_t hash[16];
    DXBCContainer container;

#define REBUILD_SIGNATURE()                                                    \
    do {                                                                       \
        byte_count = build_signature_capacity_container(                      \
            1u, 0u, "TEXCOORD", bytes, sizeof(bytes));                       \
        CHECK(byte_count > 0u);                                                \
    } while (0)
#define REHASH_SIGNATURE()                                                     \
    do {                                                                       \
        CHECK(dxbc_compute_hash(bytes, byte_count, hash));                     \
        memcpy(bytes + 4u, hash, sizeof(hash));                                \
    } while (0)

    write_le_u32(bytes + record + 12u, 0u); /* component type */
    REHASH_SIGNATURE();
    CHECK(!dxbc_parse(&container, bytes, byte_count));

    DXBCSignatureElement passthrough;
    memset(&passthrough, 0, sizeof(passthrough));
    snprintf(passthrough.semantic_name, sizeof(passthrough.semantic_name),
             "SV_InstanceID");
    passthrough.semantic_name_length = strlen(passthrough.semantic_name);
    passthrough.component_type = 2u;
    passthrough.mask = 1u;
    passthrough.rw_mask = 1u;
    CHECK(dxbc_signature_element_is_valid(
        &passthrough, DXBC_SIGNATURE_ROLE_INPUT));
    passthrough.rw_mask = 0u;
    CHECK(dxbc_signature_element_is_valid(
        &passthrough, DXBC_SIGNATURE_ROLE_OUTPUT));
    snprintf(passthrough.semantic_name, sizeof(passthrough.semantic_name),
             "SV_NotARealSystemValue");
    passthrough.semantic_name_length = strlen(passthrough.semantic_name);
    CHECK(!dxbc_signature_element_is_valid(
        &passthrough, DXBC_SIGNATURE_ROLE_OUTPUT));

    REBUILD_SIGNATURE();
    write_le_u32(bytes + record + 8u, 17u); /* sparse D3D_NAME hole */
    REHASH_SIGNATURE();
    CHECK(!dxbc_parse(&container, bytes, byte_count));

    REBUILD_SIGNATURE();
    write_le_u32(bytes + record + 20u, UINT32_C(0x0000100f));
    REHASH_SIGNATURE();
    CHECK(!dxbc_parse(&container, bytes, byte_count));

    REBUILD_SIGNATURE();
    write_le_u32(bytes + record + 20u, UINT32_C(0x0001000f));
    REHASH_SIGNATURE();
    CHECK(!dxbc_parse(&container, bytes, byte_count));

    byte_count = build_signature_capacity_container(
        1u, 0u, "SV_Target", bytes, sizeof(bytes));
    CHECK(byte_count > 0u);
    CHECK(!dxbc_parse(&container, bytes, byte_count));

#undef REHASH_SIGNATURE
#undef REBUILD_SIGNATURE

    const uint32_t input =
        operand_token(OPERAND_TYPE_INPUT, 2u, 1u, 0u, 0u, 0u) |
        UINT32_C(0x00000010);
    const uint32_t typed_siv[] = {
        instruction_token(100u, 4u) | (4u << 11u), input, 0u, 1u,
        instruction_token(62u, 1u)
    };
    byte_count = build_test_shader_container(
        typed_siv, sizeof(typed_siv) / sizeof(typed_siv[0]), 7u,
        bytes, sizeof(bytes));
    CHECK(byte_count > 0u && dxbc_parse(&container, bytes, byte_count));
    CHECK(container.instruction_count == 2);
    CHECK(container.instructions[0].has_declaration_system_value);
    CHECK(container.instructions[0].declaration_system_value == 1u);
    CHECK(container.instructions[0].has_declaration_interpolation);
    CHECK(container.instructions[0].declaration_interpolation == 4u);
    dxbc_free(&container);

    uint32_t malformed_siv[
        sizeof(typed_siv) / sizeof(typed_siv[0])];
    memcpy(malformed_siv, typed_siv, sizeof(malformed_siv));
    malformed_siv[0] = instruction_token(99u, 4u) | (16u << 11u);
    CHECK(test_shader_parse_result(
        malformed_siv,
        sizeof(malformed_siv) / sizeof(malformed_siv[0]), 7u, false,
        NULL));
    memcpy(malformed_siv, typed_siv, sizeof(malformed_siv));
    malformed_siv[3] = UINT32_C(0x00010001);
    CHECK(test_shader_parse_result(
        malformed_siv,
        sizeof(malformed_siv) / sizeof(malformed_siv[0]), 7u, false,
        NULL));
    memcpy(malformed_siv, typed_siv, sizeof(malformed_siv));
    malformed_siv[0] = instruction_token(100u, 4u) | (8u << 11u);
    CHECK(test_shader_parse_result(
        malformed_siv,
        sizeof(malformed_siv) / sizeof(malformed_siv[0]), 7u, false,
        NULL));
    return 0;
}

static int verify_dynamic_capacity_boundaries(void) {
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;

    uint8_t signature_container[4096];
    const size_t signature_size = build_signature_capacity_container(
        65, 33, "TEXCOORD", signature_container,
        sizeof(signature_container));
    CHECK(signature_size > 0);
    DXBCContainer container;
    CHECK(dxbc_parse(&container, signature_container, signature_size));
    CHECK(container.chunk_count == 35);
    CHECK(container.chunk_alloc >= container.chunk_count);
    CHECK(container.input_signature_count == 65);
    CHECK(container.input_signature_alloc >=
          container.input_signature_count);
    CHECK(strcmp(container.chunks[34].name, "SHDR") == 0);
    CHECK(container.has_executable_program);
    CHECK(container.program_type == DXBC_PROGRAM_TYPE_PIXEL);
    CHECK(container.major_version == 5 && container.minor_version == 0);
    USILProgram program;
    CHECK(usil_translate(&program, &container));
    CHECK(program.input_count == 65);
    CHECK(program.input_alloc >= program.input_count);
    usil_free(&program);
    dxbc_free(&container);

    const uint32_t signature_offset =
        read_le_u32(signature_container + 32u + 33u * sizeof(uint32_t));
    write_le_u32(signature_container + signature_offset + 4u, 4u);
    uint8_t mutated_hash[16];
    CHECK(dxbc_compute_hash(signature_container, signature_size,
                            mutated_hash));
    memcpy(signature_container + 4u, mutated_hash, sizeof(mutated_hash));
    CHECK(!dxbc_parse(&container, signature_container, signature_size));

    const size_t large_instruction_count = 4097;
    uint32_t* instruction_words =
        (uint32_t*)calloc(large_instruction_count, sizeof(uint32_t));
    CHECK(instruction_words != NULL);
    for (size_t instruction = 0; instruction < large_instruction_count;
         ++instruction) {
        instruction_words[instruction] = instruction_token(58, 1);
    }
    instruction_words[large_instruction_count - 1] =
        instruction_token(62, 1);
    const size_t large_shader_capacity =
        44u + (2u + large_instruction_count) * sizeof(uint32_t);
    uint8_t* large_shader = (uint8_t*)malloc(large_shader_capacity);
    CHECK(large_shader != NULL);
    const size_t large_shader_size = build_test_shader_container(
        instruction_words, large_instruction_count,
        (uint32_t)(large_instruction_count + 2u), large_shader,
        large_shader_capacity);
    CHECK(large_shader_size == large_shader_capacity);
    CHECK(dxbc_parse(&container, large_shader, large_shader_size));
    CHECK(container.instruction_count == (int)large_instruction_count);
    CHECK(container.instruction_alloc >= container.instruction_count);
    CHECK(usil_translate(&program, &container));
    CHECK(program.instruction_count == (int)large_instruction_count);
    CHECK(program.instruction_alloc >= program.instruction_count);
    usil_free(&program);
    dxbc_free(&container);
    free(large_shader);
    free(instruction_words);

    const size_t icb_dword_count = 1028;
    const size_t icb_instruction_word_count = 2u + icb_dword_count + 1u;
    uint32_t* icb_words =
        (uint32_t*)calloc(icb_instruction_word_count, sizeof(uint32_t));
    CHECK(icb_words != NULL);
    icb_words[0] = instruction_token(53, 0) | (3u << 11);
    icb_words[1] = (uint32_t)(2u + icb_dword_count);
    for (size_t value = 0; value < icb_dword_count; ++value) {
        icb_words[2u + value] = (uint32_t)value;
    }
    icb_words[2u + icb_dword_count] = instruction_token(62, 1);
    const size_t icb_shader_capacity =
        44u + (2u + icb_instruction_word_count) * sizeof(uint32_t);
    uint8_t* icb_shader = (uint8_t*)malloc(icb_shader_capacity);
    CHECK(icb_shader != NULL);
    const size_t icb_shader_size = build_test_shader_container(
        icb_words, icb_instruction_word_count,
        (uint32_t)(2u + icb_instruction_word_count), icb_shader,
        icb_shader_capacity);
    CHECK(icb_shader_size == icb_shader_capacity);
    CHECK(dxbc_parse(&container, icb_shader, icb_shader_size));
    CHECK(container.icb_value_count == (int)icb_dword_count);
    CHECK(container.icb_value_alloc >= container.icb_value_count);
    CHECK(container.icb_values[1027] == 1027u);
    CHECK(usil_translate(&program, &container));
    CHECK(program.icb_value_count == (int)icb_dword_count);
    CHECK(program.icb_values[1027] == 1027u);
    StringBuilder icb_hlsl;
    sb_init(&icb_hlsl);
    CHECK(hlsl_emit(&program, &icb_hlsl, NULL, NULL, NULL));
    CHECK(strstr(icb_hlsl.buf, "static const float4 unk0_arr[]") != NULL);
    CHECK(strstr(icb_hlsl.buf, "asfloat(0x00000403u)") != NULL);
    sb_free(&icb_hlsl);
    usil_free(&program);
    dxbc_free(&container);
    free(icb_shader);
    free(icb_words);

    const uint32_t resource_operand =
        operand_token(OPERAND_TYPE_RESOURCE, 1, 1, 0, 0, 0);
    const uint32_t uav_operand =
        operand_token(OPERAND_TYPE_UAV, 1, 1, 0, 0, 0);
    const uint32_t sparse_resource[] = {
        instruction_token(88, 4) | (3u << 11),
        resource_operand, 4096u, 0x5555u,
        instruction_token(157, 3), uav_operand, 8192u,
        instruction_token(88, 4) | (3u << 11),
        resource_operand, 1u, 0x5555u
    };
    uint8_t resource_shader[512];
    const size_t resource_shader_size = build_test_shader_container(
        sparse_resource,
        sizeof(sparse_resource) / sizeof(sparse_resource[0]), 13,
        resource_shader, sizeof(resource_shader));
    CHECK(resource_shader_size > 0);
    CHECK(dxbc_parse(&container, resource_shader, resource_shader_size));
    CHECK(container.resource_count == 2);
    CHECK(container.resources[0].register_index == 1);
    CHECK(container.resources[1].register_index == 4096);
    const DXBCResourceDecl* sparse_declaration =
        dxbc_find_resource_declaration(&container, 4096, false);
    const DXBCResourceDecl* sparse_uav =
        dxbc_find_resource_declaration(&container, 8192, true);
    CHECK(sparse_declaration && sparse_declaration->declared);
    CHECK(sparse_uav && sparse_uav->declared);
    CHECK(usil_translate(&program, &container));
    CHECK(program.texture_count == 2);
    CHECK(program.textures[0].reg_idx == 1);
    CHECK(program.textures[1].reg_idx == 4096);
    CHECK(program.uav_count == 1);
    CHECK(program.uavs[0].reg_idx == 8192);
    StringBuilder rejected_hlsl;
    sb_init(&rejected_hlsl);
    CHECK(!hlsl_emit(&program, &rejected_hlsl, NULL, NULL, NULL));
    CHECK(rejected_hlsl.failed);
    sb_free(&rejected_hlsl);
    usil_free(&program);
    dxbc_free(&container);

    const uint32_t representable_temp[] = {
        instruction_token(104, 2), (uint32_t)INT_MAX
    };
    uint8_t temp_shader[512];
    const size_t temp_shader_size = build_test_shader_container(
        representable_temp, 2, 4, temp_shader, sizeof(temp_shader));
    CHECK(temp_shader_size > 0);
    CHECK(dxbc_parse(&container, temp_shader, temp_shader_size));
    CHECK(usil_translate(&program, &container));
    CHECK(program.temp_count == INT_MAX);
    sb_init(&rejected_hlsl);
    CHECK(!hlsl_emit(&program, &rejected_hlsl, NULL, NULL, NULL));
    CHECK(rejected_hlsl.failed);
    sb_free(&rejected_hlsl);
    usil_free(&program);
    dxbc_free(&container);

    const uint32_t narrowing_temp[] = {
        instruction_token(104, 2), UINT32_MAX
    };
    CHECK(test_shader_parse_result(narrowing_temp, 2, 4, false, NULL));
    const uint32_t narrowing_indexable_temp[] = {
        instruction_token(105, 4), UINT32_MAX, 1, 4
    };
    CHECK(test_shader_parse_result(narrowing_indexable_temp, 4, 6, false,
                                   NULL));

    DXBCContainer malformed;
    memset(&malformed, 0, sizeof(malformed));
    malformed.input_signature_count = 1;
    malformed.input_signature_alloc = 1;
    CHECK(!usil_translate(&program, &malformed));

    DXBCResourceDecl unsorted_resources[2];
    memset(unsorted_resources, 0, sizeof(unsorted_resources));
    unsorted_resources[0].declared = true;
    unsorted_resources[0].register_index = 2;
    unsorted_resources[1].declared = true;
    unsorted_resources[1].register_index = 1;
    memset(&malformed, 0, sizeof(malformed));
    malformed.resources = unsorted_resources;
    malformed.resource_count = 2;
    malformed.resource_alloc = 2;
    CHECK(!usil_translate(&program, &malformed));

    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int verify_dynamic_signature_semantics(void) {
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    static const size_t lengths[] = {63u, 64u, 65u, 130u};

    for (size_t length_index = 0;
         length_index < sizeof(lengths) / sizeof(lengths[0]);
         ++length_index) {
        const size_t length = lengths[length_index];
        char semantic[131];
        CHECK(length < sizeof(semantic));
        semantic[0] = 'S';
        for (size_t index = 1; index < length; ++index) {
            semantic[index] = (char)('A' + index % 26u);
        }
        semantic[length] = '\0';

        uint8_t bytes[1024];
        const size_t byte_count = build_signature_capacity_container(
            1u, 0u, semantic, bytes, sizeof(bytes));
        CHECK(byte_count > 0u);

        DXBCContainer container;
        CHECK(dxbc_parse(&container, bytes, byte_count));
        CHECK(container.input_signature_count == 1);
        const DXBCSignatureElement* parsed =
            &container.input_signature[0];
        CHECK(parsed->semantic_name_length == length);
        CHECK((parsed->semantic_name_extended != NULL) == (length >= 64u));
        CHECK(strcmp(dxbc_signature_semantic_name(parsed), semantic) == 0);

        USILProgram program;
        CHECK(usil_translate(&program, &container));
        CHECK(program.input_count == 1);
        CHECK(strcmp(dxbc_signature_semantic_name(&program.inputs[0]),
                     semantic) == 0);
        if (length >= 64u) {
            CHECK(program.inputs[0].semantic_name_extended !=
                  parsed->semantic_name_extended);
        }

        /* The semantic remains owned by USIL after its source container is
         * released and the exact emitter consumes the complete spill name. */
        dxbc_free(&container);
        CHECK(strcmp(dxbc_signature_semantic_name(&program.inputs[0]),
                     semantic) == 0);
        StringBuilder hlsl;
        sb_init(&hlsl);
        CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
        CHECK(hlsl.buf != NULL && strstr(hlsl.buf, semantic) != NULL);
        sb_free(&hlsl);
        usil_free(&program);
    }

    uint8_t malformed[1024];
    const size_t malformed_size = build_signature_capacity_container(
        1u, 0u, "TEXCOORD", malformed, sizeof(malformed));
    CHECK(malformed_size > 0u);
    const uint32_t signature_chunk = read_le_u32(malformed + 32u);
    write_le_u32(malformed + signature_chunk + 12u, 4u);
    uint8_t hash[16];
    CHECK(dxbc_compute_hash(malformed, malformed_size, hash));
    memcpy(malformed + 4u, hash, sizeof(hash));
    DXBCContainer rejected;
    CHECK(!dxbc_parse(&rejected, malformed, malformed_size));

    /* A failed deep clone must roll back only initialized elements; reserved
     * tail storage is intentionally uninitialized. Exercise each signature
     * table ownership boundary with a malformed second spill descriptor. */
    for (int table = 0; table < 3; ++table) {
        DXBCSignatureElement signatures[2];
        memset(signatures, 0, sizeof(signatures));
        char first_name[65];
        char second_name[65];
        memset(first_name, 'A', sizeof(first_name));
        memset(second_name, 'B', sizeof(second_name));
        first_name[64] = '\0';
        second_name[64] = 'X';
        signatures[0].semantic_name_extended = first_name;
        signatures[0].semantic_name_length = 64u;
        signatures[1].semantic_name_extended = second_name;
        signatures[1].semantic_name_length = 64u;

        DXBCContainer source;
        memset(&source, 0, sizeof(source));
        snprintf(source.shader_type_model,
                 sizeof(source.shader_type_model), "ps_5_0");
        if (table == 0) {
            source.input_signature = signatures;
            source.input_signature_count = 2;
            source.input_signature_alloc = 2;
        } else if (table == 1) {
            source.output_signature = signatures;
            source.output_signature_count = 2;
            source.output_signature_alloc = 2;
        } else {
            source.patch_constant_signature = signatures;
            source.patch_constant_signature_count = 2;
            source.patch_constant_signature_alloc = 2;
        }
        USILProgram failed;
        CHECK(!usil_translate(&failed, &source));
    }

    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int verify_instruction_fail_closed(void) {
    const uint32_t valid[] = {
        instruction_token(104, 2), 2,
        instruction_token(62, 1)
    };
    int instruction_count = 0;
    CHECK(test_shader_parse_result(valid, 3, 5, true, &instruction_count));
    CHECK(instruction_count == 2);

    /* Version-token program type is 16 bits and bits 15:8 are reserved.  The
     * semantic parser must not truncate malformed types into a valid stage or
     * accept an "unk" stage merely because its diagnostic string is nonempty. */
    uint8_t invalid_version_shader[512];
    const size_t invalid_version_size = build_test_shader_container(
        valid, sizeof(valid) / sizeof(valid[0]), 5,
        invalid_version_shader, sizeof(invalid_version_shader));
    CHECK(invalid_version_size > 0);
    write_le_u32(invalid_version_shader + 44u, UINT32_C(0x01000050));
    uint8_t invalid_version_hash[16];
    CHECK(dxbc_compute_hash(invalid_version_shader, invalid_version_size,
                            invalid_version_hash));
    memcpy(invalid_version_shader + 4u, invalid_version_hash,
           sizeof(invalid_version_hash));
    DXBCContainer invalid_version_container;
    CHECK(!dxbc_parse(&invalid_version_container, invalid_version_shader,
                      invalid_version_size));

    /* A valid first operand owns an extension array and a nested relative
     * operand.  The truncated second operand proves rejection unwinds all of
     * that state even though the current instruction was never committed. */
    const uint32_t relative_with_extension =
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0) | 0x80000000u;
    const uint32_t truncated_after_owned_operand[] = {
        instruction_token(54, 7),
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 2, 0, 0),
        relative_with_extension,
        1u | (1u << 6),
        7,
        operand_token(OPERAND_TYPE_IMMEDIATE64, 1, 0, 0, 0, 0),
        0
    };
    CHECK(test_shader_parse_result(truncated_after_owned_operand, 7, 9,
                                   false, NULL));

    const uint32_t truncated_temps[] = {instruction_token(104, 1)};
    const uint32_t truncated_indexable_temp[] = {
        instruction_token(105, 3), 0, 4
    };
    const uint32_t truncated_max_output[] = {instruction_token(94, 1)};
    const uint32_t resource_operand =
        operand_token(OPERAND_TYPE_RESOURCE, 1, 1, 0, 0, 0);
    const uint32_t truncated_resource_return_type[] = {
        instruction_token(88, 3), resource_operand, 0
    };
    const uint32_t truncated_structured_stride[] = {
        instruction_token(162, 3), resource_operand, 0
    };
    const uint32_t input_operand =
        operand_token(OPERAND_TYPE_INPUT, 1, 1, 0, 0, 0);
    const uint32_t truncated_system_value[] = {
        instruction_token(96, 3), input_operand, 0
    };
    const uint32_t truncated_input_ps[] = {instruction_token(98, 1)};
    const uint32_t truncated_constant_buffer[] = {instruction_token(89, 1)};
    const uint32_t partial_immediate_constant_buffer[] = {
        instruction_token(53, 2) | (3u << 11), 0
    };
    /* CUSTOMDATA class zero is a comment payload.  The lossless document can
     * preserve it, but the semantic parser must not silently reinterpret its
     * DWORDs as an immediate constant buffer. */
    const uint32_t comment_custom_data[] = {
        instruction_token(53, 0), 3, UINT32_C(0x746e656d)
    };
    const uint32_t missing_opcode_extension[] = {
        instruction_token(62, 1) | 0x80000000u
    };
    const uint32_t truncated_opcode_extension_chain[] = {
        instruction_token(62, 2) | 0x80000000u, 0x80000000u
    };

    CHECK(test_shader_parse_result(truncated_temps, 1, 3, false, NULL));
    CHECK(test_shader_parse_result(truncated_indexable_temp, 3, 5, false,
                                   NULL));
    CHECK(test_shader_parse_result(truncated_max_output, 1, 3, false, NULL));
    CHECK(test_shader_parse_result(truncated_resource_return_type, 3, 5,
                                   false, NULL));
    CHECK(test_shader_parse_result(truncated_structured_stride, 3, 5, false,
                                   NULL));
    CHECK(test_shader_parse_result(truncated_system_value, 3, 5, false,
                                   NULL));
    CHECK(test_shader_parse_result(truncated_input_ps, 1, 3, false, NULL));
    CHECK(test_shader_parse_result(truncated_constant_buffer, 1, 3, false,
                                   NULL));
    CHECK(test_shader_parse_result(partial_immediate_constant_buffer, 2, 4,
                                   false, NULL));
    CHECK(test_shader_parse_result(comment_custom_data, 3, 5, false, NULL));
    CHECK(test_shader_parse_result(missing_opcode_extension, 1, 3, false,
                                   NULL));
    CHECK(test_shader_parse_result(truncated_opcode_extension_chain, 2, 4,
                                   false, NULL));

    /* A legal, deeply nested diagnostic operand can leave no room for the
     * declaration suffix in the legacy inline presentation field. Parsing is
     * governed by typed tokens, so a safely shortened diagnostic must not
     * reject the shader. */
    enum { NESTED_RELATIVE_OPERANDS = 15 };
    uint32_t long_sampler_declaration[64];
    size_t long_sampler_word_count = 0;
    long_sampler_declaration[long_sampler_word_count++] = 0;
    long_sampler_declaration[long_sampler_word_count++] =
        operand_token(OPERAND_TYPE_SAMPLER, 1, 1, 3, 0, 0);
    long_sampler_declaration[long_sampler_word_count++] = 0;
    for (int nested = 0; nested < NESTED_RELATIVE_OPERANDS; ++nested) {
        long_sampler_declaration[long_sampler_word_count++] =
            operand_token(OPERAND_TYPE_TEMP, 1, 1, 3, 0, 0);
        long_sampler_declaration[long_sampler_word_count++] = 0;
    }
    long_sampler_declaration[long_sampler_word_count++] =
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0);
    long_sampler_declaration[long_sampler_word_count++] = 0;
    CHECK(long_sampler_word_count > 1 && long_sampler_word_count < 64);
    long_sampler_declaration[0] =
        instruction_token(90, (uint32_t)long_sampler_word_count) |
        (1u << 11);

    uint8_t long_operand_storage[512] = {0};
    ByteStream long_operand_stream;
    DXBCOperand* long_operand = parse_operand_words(
        long_sampler_declaration + 1, long_sampler_word_count - 1,
        &long_operand_stream, long_operand_storage,
        sizeof(long_operand_storage));
    CHECK(long_operand != NULL &&
          long_operand_stream.position == long_operand_stream.size);
    CHECK(strlen(long_operand->text) < sizeof(long_operand->text));
    CHECK(strlen(long_operand->text) + strlen(", mode_comparison") >=
          sizeof(long_operand->text));
    destroy_test_operand(long_operand);

    long_sampler_declaration[long_sampler_word_count++] =
        instruction_token(62, 1);
    int long_sampler_instruction_count = 0;
    CHECK(test_shader_parse_result(
        long_sampler_declaration, long_sampler_word_count,
        (uint32_t)(long_sampler_word_count + 2u), true,
        &long_sampler_instruction_count));
    CHECK(long_sampler_instruction_count == 2);

    /* Each operand below fits its checked diagnostic field, but the complete
     * six-operand SAMPLE_D line does not fit DXBCInstruction::formatted_asm.
     * That annotation is presentation only; token parsing and typed semantic
     * state remain authoritative. */
    enum {
        DIAGNOSTIC_RELATIVE_DEPTH = 4,
        SAMPLE_D_OPERAND_COUNT = 6
    };
    uint32_t long_assembly[128];
    size_t long_assembly_word_count = 1;
    for (int operand_index = 0; operand_index < SAMPLE_D_OPERAND_COUNT;
         ++operand_index) {
        const size_t operand_start = long_assembly_word_count;
        for (int depth = 0; depth < DIAGNOSTIC_RELATIVE_DEPTH; ++depth) {
            long_assembly[long_assembly_word_count++] =
                operand_token(OPERAND_TYPE_TEMP, 1, 1, 4, 0, 0);
            long_assembly[long_assembly_word_count++] = UINT32_MAX;
            long_assembly[long_assembly_word_count++] = UINT32_MAX;
        }
        long_assembly[long_assembly_word_count++] =
            operand_token(OPERAND_TYPE_TEMP, 1, 1, 1, 0, 0);
        long_assembly[long_assembly_word_count++] = UINT32_MAX;
        long_assembly[long_assembly_word_count++] = UINT32_MAX;

        uint8_t operand_storage[256] = {0};
        ByteStream operand_stream;
        DXBCOperand* diagnostic_operand = parse_operand_words(
            long_assembly + operand_start,
            long_assembly_word_count - operand_start, &operand_stream,
            operand_storage, sizeof(operand_storage));
        CHECK(diagnostic_operand != NULL &&
              operand_stream.position == operand_stream.size);
        CHECK(strlen(diagnostic_operand->text) <
              sizeof(diagnostic_operand->text));
        destroy_test_operand(diagnostic_operand);
    }
    CHECK(long_assembly_word_count < 128);
    long_assembly[0] = instruction_token(
        73, (uint32_t)long_assembly_word_count); /* SAMPLE_D */
    long_assembly[long_assembly_word_count++] = instruction_token(62, 1);
    int long_assembly_instruction_count = 0;
    CHECK(test_shader_parse_result(
        long_assembly, long_assembly_word_count,
        (uint32_t)(long_assembly_word_count + 2u), true,
        &long_assembly_instruction_count));
    CHECK(long_assembly_instruction_count == 2);

    /* The SHDR/SHEX program length must describe the complete chunk. */
    CHECK(test_shader_parse_result(valid, 3, 4, false, NULL));
    return 0;
}

static int verify_condition_test_authority(void) {
    const uint32_t temp_scalar =
        operand_token(OPERAND_TYPE_TEMP, 1, 1, 0, 0, 0);
    uint8_t bytes[512];

    for (int nonzero = 0; nonzero <= 1; ++nonzero) {
        const uint32_t instructions[] = {
            instruction_token(31, 3) | (nonzero ? 0x40000u : 0u),
            temp_scalar, 0,
            instruction_token(21, 1),
            instruction_token(62, 1)
        };
        size_t byte_count = build_test_shader_container(
            instructions, sizeof(instructions) / sizeof(instructions[0]), 7,
            bytes, sizeof(bytes));
        CHECK(byte_count > 0);

        DXBCContainer container;
        CHECK(dxbc_parse(&container, bytes, byte_count));
        CHECK(container.instruction_count == 3);
        CHECK(container.instructions[0].condition_test ==
              (nonzero ? DXBC_INSTRUCTION_TEST_NONZERO
                       : DXBC_INSTRUCTION_TEST_ZERO));
        CHECK(strstr(container.instructions[0].formatted_asm,
                     nonzero ? "if_nz" : "if_z") != NULL);

        USILProgram program;
        CHECK(usil_translate(&program, &container));
        CHECK(program.instructions[0].condition_test ==
              container.instructions[0].condition_test);
        /* Diagnostic text is deliberately contradictory. Exact emission must
         * use the raw-token-derived enum and remain unchanged. */
        snprintf(program.instructions[0].original_asm,
                 sizeof(program.instructions[0].original_asm), "%s r0.x",
                 nonzero ? "if_z" : "if_nz");
        StringBuilder output;
        sb_init(&output);
        CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
        CHECK(strstr(output.buf,
                     nonzero ? "[branch] if (asuint("
                             : "[branch] if (!asuint(") != NULL);
        sb_free(&output);

        program.instructions[0].condition_test =
            DXBC_INSTRUCTION_TEST_NONE;
        sb_init(&output);
        CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
        CHECK(output.failed);
        sb_free(&output);
        usil_free(&program);
        dxbc_free(&container);
    }

    /* DISCARD uses the same token bit. The old decoder hard-coded _nz, which
     * inverted legal discard_z programs before HLSL emission. */
    const uint32_t discard_zero[] = {
        instruction_token(13, 3), temp_scalar, 0,
        instruction_token(62, 1)
    };
    size_t byte_count = build_test_shader_container(
        discard_zero, sizeof(discard_zero) / sizeof(discard_zero[0]), 6,
        bytes, sizeof(bytes));
    CHECK(byte_count > 0);
    DXBCContainer container;
    CHECK(dxbc_parse(&container, bytes, byte_count));
    CHECK(container.instructions[0].condition_test ==
          DXBC_INSTRUCTION_TEST_ZERO);
    CHECK(strstr(container.instructions[0].formatted_asm, "discard_z") !=
          NULL);
    USILProgram program;
    CHECK(usil_translate(&program, &container));
    snprintf(program.instructions[0].original_asm,
             sizeof(program.instructions[0].original_asm),
             "discard_nz r0.x");
    StringBuilder output;
    sb_init(&output);
    CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(strstr(output.buf, "clip(asuint(") != NULL);
    CHECK(strstr(output.buf, "? 1.0f : -1.0f);") != NULL);
    sb_free(&output);
    usil_free(&program);
    dxbc_free(&container);
    return 0;
}

static int verify_resource_declaration_authority(void) {
    const uint32_t resource_operand =
        operand_token(OPERAND_TYPE_RESOURCE, 1, 1, 0, 0, 0);
    const uint32_t declaration[] = {
        instruction_token(88, 4) | (3u << 11),
        resource_operand,
        0,
        0x5433u
    };
    uint8_t bytes[512];
    const size_t byte_count = build_test_shader_container(
        declaration, sizeof(declaration) / sizeof(declaration[0]), 6,
        bytes, sizeof(bytes));
    CHECK(byte_count > 0);

    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    DXBCContainer container;
    CHECK(dxbc_parse(&container, bytes, byte_count));
    const DXBCResourceDecl* resource =
        dxbc_find_resource_declaration(&container, 0, false);
    CHECK(resource != NULL && resource->declared);
    CHECK(resource->dimension == 3);
    CHECK(strcmp(resource->dim_name, "texture2d") == 0);
    CHECK(resource->return_types[0] == 3);
    CHECK(resource->return_types[1] == 3);
    CHECK(resource->return_types[2] == 4);
    CHECK(resource->return_types[3] == 5);
    CHECK(strcmp(resource->ret_types,
                 "int,int,uint,float") == 0);

    USILProgram program;
    CHECK(usil_translate(&program, &container));
    CHECK(program.texture_count == 1);
    CHECK(strcmp(program.textures[0].dimension, "2d") == 0);
    CHECK(memcmp(program.textures[0].return_types,
                 resource->return_types, 4) == 0);
    usil_free(&program);
    dxbc_free(&container);
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);

    const uint32_t unspecialized_ms[] = {
        instruction_token(88, 4) | (4u << 11),
        resource_operand, 0, 0x5555u,
        instruction_token(88, 4) | (9u << 11),
        resource_operand, 1, 0x5555u
    };
    const size_t unspecialized_ms_size = build_test_shader_container(
        unspecialized_ms,
        sizeof(unspecialized_ms) / sizeof(unspecialized_ms[0]), 10,
        bytes, sizeof(bytes));
    CHECK(unspecialized_ms_size > 0);
    CHECK(dxbc_parse(&container, bytes, unspecialized_ms_size));
    const DXBCResourceDecl* resource0 =
        dxbc_find_resource_declaration(&container, 0, false);
    const DXBCResourceDecl* resource1 =
        dxbc_find_resource_declaration(&container, 1, false);
    CHECK(resource0 && resource0->dimension == 4 &&
          resource0->sample_count == 0);
    CHECK(resource1 && resource1->dimension == 9 &&
          resource1->sample_count == 0);
    dxbc_free(&container);

    const uint32_t reserved_ms_sample_bit[] = {
        instruction_token(88, 4) | (4u << 11) | 0x00800000u,
        resource_operand, 0, 0x5555u
    };
    CHECK(test_shader_parse_result(reserved_ms_sample_bit, 4, 6, false,
                                   NULL));

    const uint32_t uav_operand =
        operand_token(OPERAND_TYPE_UAV, 1, 1, 0, 0, 0);
    const uint32_t resource_forms[] = {
        instruction_token(156, 4) | (3u << 11) | 0x00010000u,
        uav_operand, 0, 0x4444u,
        instruction_token(157, 3) | 0x00020000u,
        uav_operand, 1,
        instruction_token(158, 4) | 0x00010000u | 0x00800000u,
        uav_operand, 2, 20,
        instruction_token(161, 3), resource_operand, 3
    };
    const size_t forms_byte_count = build_test_shader_container(
        resource_forms,
        sizeof(resource_forms) / sizeof(resource_forms[0]), 16,
        bytes, sizeof(bytes));
    CHECK(forms_byte_count > 0);
    CHECK(dxbc_parse(&container, bytes, forms_byte_count));
    const DXBCResourceDecl* uav0 =
        dxbc_find_resource_declaration(&container, 0, true);
    const DXBCResourceDecl* uav1 =
        dxbc_find_resource_declaration(&container, 1, true);
    const DXBCResourceDecl* uav2 =
        dxbc_find_resource_declaration(&container, 2, true);
    const DXBCResourceDecl* resource3 =
        dxbc_find_resource_declaration(&container, 3, false);
    CHECK(uav0 && uav0->declared && uav0->dimension == 3);
    CHECK(uav0->globally_coherent);
    CHECK(!uav0->rasterizer_ordered);
    CHECK(uav1 && uav1->declared && uav1->dimension == 11);
    CHECK(uav1->rasterizer_ordered);
    CHECK(uav2 && uav2->declared && uav2->dimension == 12);
    CHECK(uav2->stride == 20);
    CHECK(uav2->globally_coherent);
    CHECK(uav2->has_order_preserving_counter);
    CHECK(resource3 && resource3->declared && resource3->dimension == 11);
    CHECK(usil_translate(&program, &container));
    CHECK(program.uav_count == 3 && program.texture_count == 1);
    CHECK(program.uavs[0].globally_coherent);
    CHECK(program.uavs[1].rasterizer_ordered);
    CHECK(program.uavs[2].has_order_preserving_counter);
    CHECK(strcmp(program.textures[0].dimension, "raw") == 0);
    usil_free(&program);
    dxbc_free(&container);
    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);

    /* Return types occupy four nibbles. Reserved high bits and undefined
     * nibble values are malformed instead of being silently interpreted as
     * float, which was the legacy byte-oriented parser behavior. */
    uint32_t malformed[4];
    memcpy(malformed, declaration, sizeof(malformed));
    malformed[3] = 0x00010000u | 0x5555u;
    CHECK(test_shader_parse_result(malformed, 4, 6, false, NULL));
    malformed[3] = 0xaaaau;
    CHECK(test_shader_parse_result(malformed, 4, 6, false, NULL));
    memcpy(malformed, declaration, sizeof(malformed));
    malformed[0] = instruction_token(88, 4) | (3u << 11) | 0x00010000u;
    CHECK(test_shader_parse_result(malformed, 4, 6, false, NULL));

    const uint32_t invalid_uav_dimension[] = {
        instruction_token(156, 4) | (6u << 11),
        uav_operand, 0, 0x4444u
    };
    CHECK(test_shader_parse_result(invalid_uav_dimension, 4, 6, false,
                                   NULL));
    return 0;
}

static int verify_resource_declaration_emission(void) {
    USILTexture textures[4];
    USILUav uavs[3];
    memset(textures, 0, sizeof(textures));
    memset(uavs, 0, sizeof(uavs));

    textures[0].reg_idx = 0;
    snprintf(textures[0].dimension, sizeof(textures[0].dimension),
             "1darray");
    memset(textures[0].return_types, 5, 4);
    textures[1].reg_idx = 1;
    snprintf(textures[1].dimension, sizeof(textures[1].dimension), "2dms");
    textures[1].return_types[0] = 3;
    textures[1].return_types[1] = 3;
    textures[1].return_types[2] = 9;
    textures[1].return_types[3] = 9;
    textures[1].sample_count = 4;
    textures[2].reg_idx = 2;
    snprintf(textures[2].dimension, sizeof(textures[2].dimension),
             "cubearray");
    memset(textures[2].return_types, 4, 4);
    textures[3].reg_idx = 3;
    snprintf(textures[3].dimension, sizeof(textures[3].dimension), "raw");

    uavs[0].reg_idx = 0;
    snprintf(uavs[0].dimension, sizeof(uavs[0].dimension), "buffer");
    uavs[0].return_types[0] = 5;
    uavs[0].return_types[1] = 9;
    uavs[0].return_types[2] = 9;
    uavs[0].return_types[3] = 9;
    uavs[0].globally_coherent = true;
    uavs[1].reg_idx = 1;
    snprintf(uavs[1].dimension, sizeof(uavs[1].dimension), "2darray");
    memset(uavs[1].return_types, 4, 4);
    uavs[1].rasterizer_ordered = true;
    uavs[2].reg_idx = 2;
    snprintf(uavs[2].dimension, sizeof(uavs[2].dimension), "structured");
    uavs[2].stride = 20;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.textures = textures;
    program.texture_count = 4;
    program.texture_alloc = 4;
    program.uavs = uavs;
    program.uav_count = 3;
    program.uav_alloc = 3;

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "Texture1DArray<float4> t0") != NULL);
    CHECK(strstr(builder.buf, "Texture2DMS<int2, 4> t1") != NULL);
    CHECK(strstr(builder.buf, "TextureCubeArray<uint4> t2") != NULL);
    CHECK(strstr(builder.buf, "ByteAddressBuffer t3") != NULL);
    CHECK(strstr(builder.buf,
                 "globallycoherent RWBuffer<float> u0") != NULL);
    CHECK(strstr(builder.buf,
                 "RasterizerOrderedTexture2DArray<uint4> u1") != NULL);
    CHECK(strstr(builder.buf, "struct dxbc_struct_u2") != NULL);
    CHECK(strstr(builder.buf, "    float m16;") != NULL);
    CHECK(strstr(builder.buf,
                 "RWStructuredBuffer<dxbc_struct_u2> u2") != NULL);
    sb_free(&builder);

    textures[1].sample_count = 0;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "Texture2DMS<int2> t1") != NULL);
    CHECK(strstr(builder.buf, "Texture2DMS<int2, 0>") == NULL);
    sb_free(&builder);
    textures[1].sample_count = 32;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "Texture2DMS<int2, 32> t1") != NULL);
    sb_free(&builder);
    textures[1].sample_count = 33;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    textures[1].sample_count = 4;

    /* A mixed token has been preserved correctly, but this HLSL witness
     * layer cannot express it as one resource template type. Fail closed. */
    textures[0].return_types[1] = 4;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    textures[0].return_types[1] = 5;

    uavs[2].has_order_preserving_counter = true;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);

    /* Shader Model 5 exposes exactly s0..s15.  The old shared 128-slot
     * binding ceiling incorrectly accepted s16+ and could emit bytecode that
     * D3D11 cannot bind. */
    USILSampler samplers[16];
    memset(samplers, 0, sizeof(samplers));
    for (int sampler = 0; sampler < 16; ++sampler)
        samplers[sampler].reg_idx = sampler;
    USILProgram sampler_program;
    memset(&sampler_program, 0, sizeof(sampler_program));
    snprintf(sampler_program.shader_type_model,
             sizeof(sampler_program.shader_type_model), "ps_5_0");
    sampler_program.samplers = samplers;
    sampler_program.sampler_count = 16;
    sampler_program.sampler_alloc = 16;
    sb_init(&builder);
    CHECK(hlsl_emit(&sampler_program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "SamplerState s15 : register(s15);") != NULL);
    sb_free(&builder);
    samplers[15].reg_idx = 16;
    sb_init(&builder);
    CHECK(!hlsl_emit(&sampler_program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);

    /* Metadata can resolve different registers to the same source name.
     * Emitting only the first declaration would silently bind later uses to
     * the wrong register; declaration emission must reject that collision. */
    samplers[0].reg_idx = 0;
    samplers[1].reg_idx = 1;
    sampler_program.sampler_count = 2;
    sampler_program.sampler_alloc = 16;
    HLSLEmitterContext sampler_context;
    memset(&sampler_context, 0, sizeof(sampler_context));
    sampler_context.program = &sampler_program;
    char shared_name_a[] = "samplerShared";
    char shared_name_b[] = "samplerShared";
    sampler_context.sampler_names[0] = shared_name_a;
    sampler_context.sampler_names[1] = shared_name_b;
    sb_init(&builder);
    sampler_context.sb = &builder;
    emit_resources(&sampler_context);
    CHECK(builder.failed);
    sb_free(&builder);

    /* Serialized texture identifiers can exceed every former scratch buffer.
     * The sampler prefix and declaration must retain the exact spelling. */
    char long_texture_name[601];
    long_texture_name[0] = '_';
    for (size_t index = 1; index + 1u < sizeof(long_texture_name); ++index)
        long_texture_name[index] = (char)('A' + index % 26u);
    long_texture_name[sizeof(long_texture_name) - 1u] = '\0';
    SerializedResourceParam long_resource;
    memset(&long_resource, 0, sizeof(long_resource));
    long_resource.name = long_texture_name;
    long_resource.bind_type = SERIALIZED_RESOURCE_TEXTURE;
    long_resource.bind_index = 3;
    long_resource.sampler_index = 7;
    SerializedProgramParameters long_params;
    serialized_program_parameters_init(&long_params);
    long_params.resources = &long_resource;
    long_params.res_count = 1;
    samplers[0].reg_idx = 7;
    sampler_program.sampler_count = 1;
    HLSLEmitterContext long_context;
    memset(&long_context, 0, sizeof(long_context));
    long_context.program = &sampler_program;
    long_context.params = &long_params;
    sb_init(&builder);
    long_context.sb = &builder;
    CHECK(build_sampler_name_map(&long_context));
    CHECK(strlen(long_context.sampler_names[7]) ==
          strlen("sampler") + strlen(long_texture_name));
    emit_resources(&long_context);
    CHECK(sb_ok(&builder));
    CHECK(strstr(builder.buf, long_context.sampler_names[7]) != NULL);
    free_sampler_name_map(&long_context);
    sb_free(&builder);

    SerializedResourceParam colliding_resources[2];
    memset(colliding_resources, 0, sizeof(colliding_resources));
    for (int index = 0; index < 2; ++index) {
        colliding_resources[index].name = "_Shared";
        colliding_resources[index].bind_type = SERIALIZED_RESOURCE_TEXTURE;
        colliding_resources[index].bind_index = (uint32_t)index;
        colliding_resources[index].sampler_index = (uint32_t)index;
        samplers[index].reg_idx = index;
    }
    SerializedProgramParameters colliding_params;
    serialized_program_parameters_init(&colliding_params);
    colliding_params.resources = colliding_resources;
    colliding_params.res_count = 2;
    sampler_program.sampler_count = 2;
    HLSLEmitterContext colliding_context;
    memset(&colliding_context, 0, sizeof(colliding_context));
    colliding_context.program = &sampler_program;
    colliding_context.params = &colliding_params;
    sb_init(&builder);
    colliding_context.sb = &builder;
    CHECK(build_sampler_name_map(&colliding_context));
    CHECK(strcmp(colliding_context.sampler_names[0],
                 "sampler_Shared") == 0);
    CHECK(strcmp(colliding_context.sampler_names[1],
                 "sampler_Shared_dxbc_s1") == 0);
    emit_resources(&colliding_context);
    CHECK(sb_ok(&builder));
    CHECK(strstr(builder.buf,
                 "SamplerState sampler_Shared : register(s0);") != NULL);
    CHECK(strstr(builder.buf,
                 "SamplerState sampler_Shared_dxbc_s1 : register(s1);") !=
          NULL);
    free_sampler_name_map(&colliding_context);
    sb_free(&builder);

    /* The declaration token, not sampled-opcode heuristics, owns sampler
     * state type.  A comparison declaration emits exactly one comparison
     * object; a different ordinary sampler may legitimately retain a source
     * identifier ending in `_cmp`. */
    colliding_resources[0].name = "_Base";
    colliding_resources[1].name = "_Base_cmp";
    USILInstruction sampler_uses[2];
    memset(sampler_uses, 0, sizeof(sampler_uses));
    sampler_uses[0].opcode = USIL_OP_SAMPLE_C;
    sampler_uses[1].opcode = USIL_OP_SAMPLE;
    for (int index = 0; index < 2; ++index) {
        sampler_uses[index].operand_count = 1;
        sampler_uses[index].operands[0].type = OPERAND_TYPE_SAMPLER;
        sampler_uses[index].operands[0].register_index = index;
    }
    samplers[0].mode = 1;
    samplers[1].mode = 0;
    sampler_program.instructions = sampler_uses;
    sampler_program.instruction_count = 2;
    sampler_program.instruction_alloc = 2;
    memset(&colliding_context, 0, sizeof(colliding_context));
    colliding_context.program = &sampler_program;
    colliding_context.params = &colliding_params;
    sb_init(&builder);
    colliding_context.sb = &builder;
    CHECK(build_sampler_name_map(&colliding_context));
    CHECK(strcmp(colliding_context.sampler_names[0],
                 "sampler_Base") == 0);
    CHECK(strcmp(colliding_context.sampler_names[1],
                 "sampler_Base_cmp") == 0);
    emit_resources(&colliding_context);
    CHECK(sb_ok(&builder));
    CHECK(strstr(builder.buf,
                 "SamplerComparisonState sampler_Base : register(s0);") !=
          NULL);
    CHECK(strstr(builder.buf,
                 "SamplerState sampler_Base_cmp : register(s1);") !=
          NULL);
    free_sampler_name_map(&colliding_context);
    sb_free(&builder);

    /* Serialized inline sampler state is binding authority, not a source
     * name.  Its deterministic inverse must outrank the texture association
     * and reproduce a name accepted by Unity's inline-sampler parser. */
    SerializedResourceParam inline_resources[2];
    memset(inline_resources, 0, sizeof(inline_resources));
    inline_resources[0].name = "_MainTex";
    inline_resources[0].bind_type = SERIALIZED_RESOURCE_TEXTURE;
    inline_resources[0].bind_index = 0;
    inline_resources[0].sampler_index = 1;
    inline_resources[1].name = "";
    inline_resources[1].bind_type = SERIALIZED_RESOURCE_SAMPLER;
    inline_resources[1].bind_index = 1;
    inline_resources[1].sampler_state = 0x54u;
    SerializedProgramParameters inline_params;
    serialized_program_parameters_init(&inline_params);
    inline_params.resources = inline_resources;
    inline_params.res_count = 2;
    samplers[0].reg_idx = 1;
    samplers[0].mode = 0;
    sampler_program.samplers = samplers;
    sampler_program.sampler_count = 1;
    sampler_program.sampler_alloc = 16;
    sampler_program.instructions = NULL;
    sampler_program.instruction_count = 0;
    sampler_program.instruction_alloc = 0;
    HLSLEmitterContext inline_context;
    memset(&inline_context, 0, sizeof(inline_context));
    inline_context.program = &sampler_program;
    inline_context.params = &inline_params;
    sb_init(&builder);
    inline_context.sb = &builder;
    CHECK(build_sampler_name_map(&inline_context));
    CHECK(strcmp(inline_context.sampler_names[1],
                 "sampler_dxbc_s1_point_clampu_clampv_clampw") == 0);
    emit_resources(&inline_context);
    CHECK(sb_ok(&builder));
    CHECK(strstr(builder.buf,
                 "SamplerState sampler_dxbc_s1_point_clampu_clampv_clampw "
                 ": register(s1);") != NULL);
    CHECK(strstr(builder.buf, "sampler_MainTex") == NULL);
    free_sampler_name_map(&inline_context);
    sb_free(&builder);

    /* dcl_sampler mode and the serialized comparison bit are two encodings
     * of the same binding.  Inconsistent authority fails closed. */
    inline_resources[1].sampler_state = 0x154u;
    sb_init(&builder);
    inline_context.sb = &builder;
    CHECK(!build_sampler_name_map(&inline_context));
    CHECK(builder.failed);
    sb_free(&builder);

    samplers[0].mode = 1;
    sb_init(&builder);
    inline_context.sb = &builder;
    CHECK(build_sampler_name_map(&inline_context));
    CHECK(strcmp(inline_context.sampler_names[1],
                 "sampler_dxbc_s1_point_clampu_clampv_clampw_compare") ==
          0);
    emit_resources(&inline_context);
    CHECK(sb_ok(&builder));
    CHECK(strstr(
              builder.buf,
              "SamplerComparisonState sampler_dxbc_s1_point_clampu_clampv_"
              "clampw_compare : register(s1);") != NULL);
    free_sampler_name_map(&inline_context);
    sb_free(&builder);

    inline_resources[1].sampler_state = 0x1000u;
    sb_init(&builder);
    inline_context.sb = &builder;
    CHECK(!build_sampler_name_map(&inline_context));
    CHECK(builder.failed);
    sb_free(&builder);

    /* Serialized sampler records are architectural bindings.  Reject an
     * out-of-range s# even when the DXBC program never references it; silently
     * ignoring malformed metadata would let a lower-authority texture name
     * stand in for the serialized state. */
    inline_resources[1].sampler_state = 0x54u;
    inline_resources[1].bind_index = HLSL_SM5_SAMPLER_REGISTER_COUNT;
    sb_init(&builder);
    inline_context.sb = &builder;
    CHECK(!build_sampler_name_map(&inline_context));
    CHECK(builder.failed);
    sb_free(&builder);

    /* A binding has exactly one serialized state record.  Even identical
     * duplicates are malformed rather than corroborating authority. */
    inline_resources[1].bind_index = 1;
    SerializedResourceParam duplicate_inline_resources[3];
    memcpy(duplicate_inline_resources, inline_resources,
           sizeof(inline_resources));
    duplicate_inline_resources[2] = inline_resources[1];
    inline_params.resources = duplicate_inline_resources;
    inline_params.res_count = 3;
    sb_init(&builder);
    inline_context.sb = &builder;
    CHECK(!build_sampler_name_map(&inline_context));
    CHECK(builder.failed);
    sb_free(&builder);

    inline_params.resources = inline_resources;
    inline_params.res_count = 2;
    return 0;
}

static int verify_expression_scratch_overflow_fails_closed(void) {
    char long_operand[192];
    memset(long_operand, 'x', sizeof(long_operand) - 1u);
    long_operand[sizeof(long_operand) - 1u] = '\0';
    char line[16];
    char rhs[16];
    bool custom = false;
    bool wrap = false;
    USILInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.operand_count = 3;
    instruction.operands[0].destination_mask = 0xf0;

    HLSLEmitterContext context;
    memset(&context, 0, sizeof(context));
    StringBuilder output;
    sb_init(&output);
    context.sb = &output;

    instruction.opcode = USIL_OP_SUB;
    emit_arithmetic_op(&context, &instruction, false, false, 0, 0, 0, 0,
                       "destination", long_operand, long_operand, "", "", "",
                       line, sizeof(line), rhs, sizeof(rhs), &custom, &wrap);
    CHECK(output.failed);
    CHECK(rhs[0] == '\0');
    sb_free(&output);

    memset(line, 0, sizeof(line));
    memset(rhs, 0, sizeof(rhs));
    sb_init(&output);
    context.sb = &output;
    instruction.opcode = USIL_OP_OR;
    emit_bitwise_op(&context, &instruction, false, false, 0, 0, 0, 0,
                    "destination", long_operand, long_operand, "", "", "",
                    line, sizeof(line), rhs, sizeof(rhs), &custom, &wrap);
    CHECK(output.failed);
    CHECK(rhs[0] == '\0');
    sb_free(&output);

    memset(line, 0, sizeof(line));
    memset(rhs, 0, sizeof(rhs));
    sb_init(&output);
    context.sb = &output;
    instruction.opcode = USIL_OP_ITOF;
    emit_conversion_op(&context, &instruction, false, false, 0, 0, 0, 0,
                       "destination", long_operand, "", "", "", "", line,
                       sizeof(line), rhs, sizeof(rhs), &custom, &wrap);
    CHECK(output.failed);
    CHECK(rhs[0] == '\0');
    sb_free(&output);

    memset(line, 0, sizeof(line));
    memset(rhs, 0, sizeof(rhs));
    sb_init(&output);
    context.sb = &output;
    instruction.opcode = USIL_OP_DERIV_RTX;
    emit_texture_op(&context, &instruction, false, false, 0, 0, 0, 0,
                    "destination", long_operand, "", "", "", "", line,
                    sizeof(line), rhs, sizeof(rhs), &custom, &wrap);
    CHECK(output.failed);
    CHECK(line[0] == '\0');
    sb_free(&output);
    return 0;
}

static void init_dxbc_instruction(DXBCInstruction* instruction,
                                  uint32_t opcode,
                                  const char* opcode_string,
                                  bool is_declaration) {
    memset(instruction, 0, sizeof(*instruction));
    instruction->opcode = opcode;
    instruction->is_decl = is_declaration;
    snprintf(instruction->opcode_str, sizeof(instruction->opcode_str), "%s",
             opcode_string);
    snprintf(instruction->formatted_asm, sizeof(instruction->formatted_asm),
             "%s", opcode_string);
}

static int verify_usil_rejects_silent_opcode_loss(void) {
    DXBCContainer container;
    USILProgram program;
    DXBCInstruction instruction;
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;

    memset(&container, 0, sizeof(container));
    snprintf(container.shader_type_model, sizeof(container.shader_type_model),
             "ps_5_0");
    container.instructions = &instruction;
    container.instruction_count = 1;
    container.instruction_alloc = 1;

    /* Opcode 58 is the genuine tokenized-program NOP. */
    CHECK(strcmp(dxbc_opcode_name(58), "NOP") == 0);
    /* D3D_NAME values are sparse.  In particular, pixel-output values begin
     * at 64; treating the DXIL-era 23..25 additions as target/depth/coverage
     * corrupts declaration diagnostics and any downstream inspection. */
    CHECK(strcmp(dxbc_sys_value_name(2), "clip_distance") == 0);
    CHECK(strcmp(dxbc_sys_value_name(3), "cull_distance") == 0);
    CHECK(strcmp(dxbc_sys_value_name(23), "barycentrics") == 0);
    CHECK(strcmp(dxbc_sys_value_name(25), "cull_primitive") == 0);
    CHECK(strcmp(dxbc_sys_value_name(64), "target") == 0);
    CHECK(strcmp(dxbc_sys_value_name(65), "depth") == 0);
    CHECK(strcmp(dxbc_sys_value_name(70), "inner_coverage") == 0);
    CHECK(strcmp(dxbc_sys_value_name(17), "unknown_siv") == 0);
    CHECK(strcmp(dxbc_opcode_name(4), "CALL") == 0);
    CHECK(strcmp(dxbc_opcode_name(5), "CALLC") == 0);
    CHECK(strcmp(dxbc_opcode_name(20), "EMITTHENCUT") == 0);
    CHECK(strcmp(dxbc_opcode_name(102), "DCL_OUTPUT_SGV") == 0);
    CHECK(strcmp(dxbc_opcode_name(163), "LD_UAV_TYPED") == 0);
    CHECK(strcmp(dxbc_opcode_name(208), "DEBUG_BREAK") == 0);
    CHECK(strcmp(dxbc_opcode_name(209), "UNKNOWN_OP") == 0);
    CHECK(dxbc_opcode_is_known(208));
    CHECK(!dxbc_opcode_is_known(209));
    CHECK(dxbc_opcode_is_known(234));
    CHECK(!dxbc_opcode_is_known(235));
    init_dxbc_instruction(&instruction, 58, "nop", false);
    CHECK(usil_translate(&program, &container));
    CHECK(program.instruction_count == 1);
    CHECK(program.instructions[0].opcode == USIL_OP_NOP);
    usil_free(&program);

    /* A caller-supplied declaration flag cannot make an unknown raw opcode
     * skippable.  Only tokenized declaration opcodes are metadata. */
    init_dxbc_instruction(&instruction, 2047, "dcl_future_metadata", true);
    CHECK(!usil_translate(&program, &container));
    CHECK(program.instructions == NULL && program.instruction_count == 0);

    /* Both unknown tokens and known-but-unimplemented executable operations
     * must fail instead of being silently rewritten to NOP. */
    init_dxbc_instruction(&instruction, 2047, "op_2047", false);
    CHECK(!usil_translate(&program, &container));
    CHECK(program.instructions == NULL && program.instruction_count == 0);

    init_dxbc_instruction(
        &instruction, 109,
        "gather4_indexable(texture2d)(float,float,float,float)", false);
    CHECK(!usil_translate(&program, &container));
    CHECK(program.instructions == NULL && program.instruction_count == 0);

    init_dxbc_instruction(
        &instruction, 168,
        "store_structured_indexable(structured_buffer, stride=16)", false);
    CHECK(!usil_translate(&program, &container));
    CHECK(program.instructions == NULL && program.instruction_count == 0);

    /* Declaration extraction follows the raw DCL opcode even when every
     * cached diagnostic field claims that this is executable arithmetic. */
    init_dxbc_instruction(&instruction, 89, "mul", false);
    snprintf(instruction.formatted_asm, sizeof(instruction.formatted_asm),
             "mul r0, r1, r2");
    instruction.operand_count = 1;
    instruction.operands[0].type = OPERAND_TYPE_CONSTANT_BUFFER;
    instruction.operands[0].register_index = 3;
    instruction.operands[0].rel_offset0 = 12;
    CHECK(usil_translate(&program, &container));
    CHECK(program.instruction_count == 0);
    CHECK(program.cbuffer_count == 1);
    CHECK(program.cbuffers[0].reg_idx == 3);
    CHECK(program.cbuffers[0].size == 12);
    usil_free(&program);

    /* Conversely, neither an is_decl bit nor a declaration-looking label can
     * hide an executable raw opcode.  formatted_asm remains diagnostic text. */
    init_dxbc_instruction(&instruction, 54, "dcl_sampler", true);
    snprintf(instruction.formatted_asm, sizeof(instruction.formatted_asm),
             "mul r9, r8, r7");
    CHECK(usil_translate(&program, &container));
    CHECK(program.instruction_count == 1);
    CHECK(program.instructions[0].opcode == USIL_OP_MOV);
    CHECK(strcmp(program.instructions[0].original_asm,
                 "mul r9, r8, r7") == 0);
    usil_free(&program);

    /* Canonical presentation and translation both recognize UMIN's raw D3D
     * opcode; USIL must remain independent of mutable diagnostic strings. */
    CHECK(strcmp(dxbc_opcode_name(84), "UMIN") == 0);
    init_dxbc_instruction(&instruction, 84, "definitely_not_umin", false);
    snprintf(instruction.formatted_asm, sizeof(instruction.formatted_asm),
             "add r0, r1, r2");
    CHECK(usil_translate(&program, &container));
    CHECK(program.instruction_count == 1);
    CHECK(program.instructions[0].opcode == USIL_OP_UMIN);
    CHECK(strcmp(program.instructions[0].original_asm,
                 "add r0, r1, r2") == 0);
    usil_free(&program);

    init_dxbc_instruction(&instruction, 54, "mov", false);
    instruction.operand_count = DXBC_MAX_OPERANDS + 1;
    CHECK(!usil_translate(&program, &container));
    CHECK(program.instructions == NULL && program.instruction_count == 0);

    /* A mapped executable opcode still translates normally. */
    uint32_t extended_tokens[2] = {0x80000001u, 0x00000000u};
    DXBCOperand relative_operand;
    memset(&relative_operand, 0, sizeof(relative_operand));
    relative_operand.type = OPERAND_TYPE_TEMP;
    relative_operand.register_index = 7;
    init_dxbc_instruction(&instruction, 54, "mov", false);
    instruction.operand_count = 1;
    instruction.operands[0].type = OPERAND_TYPE_TEMP;
    instruction.operands[0].extended_tokens = extended_tokens;
    instruction.operands[0].extended_token_count = 2;
    instruction.operands[0].rel_op2 = &relative_operand;
    CHECK(usil_translate(&program, &container));
    CHECK(program.instruction_count == 1);
    CHECK(program.instructions[0].opcode == USIL_OP_MOV);
    CHECK(program.instructions[0].operands[0].extended_tokens !=
          extended_tokens);
    CHECK(program.instructions[0].operands[0].extended_token_count == 2);
    CHECK(program.instructions[0].operands[0].extended_tokens[0] ==
          extended_tokens[0]);
    CHECK(program.instructions[0].operands[0].rel_op2 != &relative_operand);
    CHECK(program.instructions[0].operands[0].rel_op2->register_index == 7);
    CHECK(program.temp_count == 8);
    usil_free(&program);

    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static void init_register_operand(DXBCOperand* operand,
                                  DXBCOperandType type, int register_index) {
    memset(operand, 0, sizeof(*operand));
    operand->type = type;
    operand->register_index = register_index;
    operand->swizzle_mode = 1;
    operand->swizzle[0] = 0;
    operand->swizzle[1] = 1;
    operand->swizzle[2] = 2;
    operand->swizzle[3] = 3;
    if (type != OPERAND_TYPE_OUTPUT_DEPTH) {
        operand->register_index_dim = 1;
        operand->index_has_immediate[0] = true;
        operand->index_values[0] = (uint32_t)register_index;
    }
}

static void init_cbuffer_operand(DXBCOperand* operand, int buffer_register,
                                 int row) {
    init_register_operand(operand, OPERAND_TYPE_CONSTANT_BUFFER,
                          buffer_register);
    operand->register_index_dim = 2;
    operand->index_has_immediate[1] = true;
    operand->index_values[1] = (uint32_t)row;
    operand->rel_offset0 = row;
}

static void init_immediate_operand(DXBCOperand* operand, uint32_t value) {
    memset(operand, 0, sizeof(*operand));
    operand->type = OPERAND_TYPE_IMMEDIATE32;
    operand->swizzle_mode = 2;
    operand->imm_value_count = 1;
    operand->immediate_word_count = 1;
    operand->imm_values[0] = value;
    operand->immediate_words[0] = value;
}

static void init_immediate_vector_operand(DXBCOperand* operand,
                                          uint32_t x, uint32_t y,
                                          uint32_t z, uint32_t w) {
    memset(operand, 0, sizeof(*operand));
    operand->type = OPERAND_TYPE_IMMEDIATE32;
    operand->imm_value_count = 4;
    operand->immediate_word_count = 4;
    operand->imm_values[0] = x;
    operand->imm_values[1] = y;
    operand->imm_values[2] = z;
    operand->imm_values[3] = w;
    operand->immediate_words[0] = x;
    operand->immediate_words[1] = y;
    operand->immediate_words[2] = z;
    operand->immediate_words[3] = w;
}

static int verify_exact_operand_width_emission(void) {
    StringBuilder builder;

    /* Real corpus form: dp3 r0.x, r1.xyz, r2.zzzz.  The replicate must be
     * preserved at the three-lane dot width, not widened to float4. */
    {
        USILInstruction instructions[2];
        USILProgram program;
        memset(instructions, 0, sizeof(instructions));
        memset(&program, 0, sizeof(program));
        instructions[0].opcode = USIL_OP_DP3;
        instructions[0].operand_count = 3;
        snprintf(instructions[0].original_asm,
                 sizeof(instructions[0].original_asm),
                 "dp3 r0.x, r1.xyz, r2.zzzz");
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        init_register_operand(&instructions[0].operands[2],
                              OPERAND_TYPE_TEMP, 2);
        memset(instructions[0].operands[2].swizzle, 2,
               sizeof(instructions[0].operands[2].swizzle));
        instructions[1].opcode = USIL_OP_RET;
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 3;
        program.instructions = instructions;
        program.instruction_count = 2;
        program.instruction_alloc = 2;
        sb_init(&builder);
        CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(strstr(builder.buf, ".zzz)") != NULL);
        CHECK(strstr(builder.buf, ".zzzz)") == NULL);
        sb_free(&builder);
    }

    /* Comparison sampling returns one replicated scalar.  A resource
     * operand's canonical .xxxx must therefore not turn the HLSL intrinsic
     * into a float4 expression for a scalar destination. */
    {
        USILTexture texture;
        USILSampler sampler;
        USILInstruction instructions[2];
        USILProgram program;
        memset(&texture, 0, sizeof(texture));
        memset(&sampler, 0, sizeof(sampler));
        memset(instructions, 0, sizeof(instructions));
        memset(&program, 0, sizeof(program));
        texture.reg_idx = 0;
        snprintf(texture.dimension, sizeof(texture.dimension), "2d");
        memset(texture.return_types, 5, sizeof(texture.return_types));
        sampler.reg_idx = 0;
        sampler.mode = 1;
        instructions[0].opcode = USIL_OP_SAMPLE_C_LZ;
        instructions[0].operand_count = 5;
        snprintf(instructions[0].resource_dimension,
                 sizeof(instructions[0].resource_dimension), "2d");
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        init_register_operand(&instructions[0].operands[2],
                              OPERAND_TYPE_RESOURCE, 0);
        memset(instructions[0].operands[2].swizzle, 0,
               sizeof(instructions[0].operands[2].swizzle));
        init_register_operand(&instructions[0].operands[3],
                              OPERAND_TYPE_SAMPLER, 0);
        init_register_operand(&instructions[0].operands[4],
                              OPERAND_TYPE_TEMP, 2);
        instructions[0].operands[4].swizzle_mode = 2;
        instructions[0].operands[4].swizzle[0] = 2;
        instructions[1].opcode = USIL_OP_RET;
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 3;
        program.textures = &texture;
        program.texture_count = 1;
        program.texture_alloc = 1;
        program.samplers = &sampler;
        program.sampler_count = 1;
        program.sampler_alloc = 1;
        program.instructions = instructions;
        program.instruction_count = 2;
        program.instruction_alloc = 2;
        sb_init(&builder);
        CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(strstr(builder.buf, "SampleCmpLevelZero") != NULL);
        CHECK(strstr(builder.buf, "SampleCmpLevelZero") != NULL &&
              strstr(builder.buf, ").xxxx") == NULL);
        sb_free(&builder);
    }

    /* Ordinary samples project the resource result onto the destination
     * mask, and SampleGrad projects derivative operands onto the resource
     * coordinate dimension. */
    {
        USILTexture texture;
        USILSampler sampler;
        USILInstruction instructions[3];
        USILProgram program;
        memset(&texture, 0, sizeof(texture));
        memset(&sampler, 0, sizeof(sampler));
        memset(instructions, 0, sizeof(instructions));
        memset(&program, 0, sizeof(program));
        texture.reg_idx = 0;
        snprintf(texture.dimension, sizeof(texture.dimension), "2d");
        memset(texture.return_types, 5, sizeof(texture.return_types));
        sampler.reg_idx = 0;
        instructions[0].opcode = USIL_OP_SAMPLE;
        instructions[0].operand_count = 4;
        snprintf(instructions[0].resource_dimension,
                 sizeof(instructions[0].resource_dimension), "2d");
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x70;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        init_register_operand(&instructions[0].operands[2],
                              OPERAND_TYPE_RESOURCE, 0);
        init_register_operand(&instructions[0].operands[3],
                              OPERAND_TYPE_SAMPLER, 0);
        instructions[1].opcode = USIL_OP_SAMPLE_D;
        instructions[1].operand_count = 6;
        snprintf(instructions[1].resource_dimension,
                 sizeof(instructions[1].resource_dimension), "2d");
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 2);
        instructions[1].operands[0].destination_mask = 0xf0;
        init_register_operand(&instructions[1].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        init_register_operand(&instructions[1].operands[2],
                              OPERAND_TYPE_RESOURCE, 0);
        init_register_operand(&instructions[1].operands[3],
                              OPERAND_TYPE_SAMPLER, 0);
        init_immediate_vector_operand(&instructions[1].operands[4],
                                      0x3dcccccdu, 0, 0, 0);
        init_immediate_vector_operand(&instructions[1].operands[5],
                                      0, 0x3dcccccdu, 0, 0);
        instructions[2].opcode = USIL_OP_RET;
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 3;
        program.textures = &texture;
        program.texture_count = 1;
        program.texture_alloc = 1;
        program.samplers = &sampler;
        program.sampler_count = 1;
        program.sampler_alloc = 1;
        program.instructions = instructions;
        program.instruction_count = 3;
        program.instruction_alloc = 3;
        sb_init(&builder);
        CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(strstr(builder.buf, ").xyz;") != NULL);
        CHECK(strstr(builder.buf, "SampleGrad") != NULL);
        CHECK(strstr(builder.buf, "float2(0.100000001f, 0.0f)") != NULL);
        CHECK(strstr(builder.buf, "float4(0.100000001f") == NULL);
        sb_free(&builder);

        /* A float2 resource cannot prove a z result lane. */
        texture.return_types[2] = 9;
        texture.return_types[3] = 9;
        instructions[0].operands[0].destination_mask = 0x40;
        program.instruction_count = 1;
        sb_init(&builder);
        CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(builder.failed);
        sb_free(&builder);
    }

    /* The corpus vector-literal comparison must select one source lane for
     * each scalar destination assignment. */
    {
        USILInstruction instructions[2];
        USILProgram program;
        memset(instructions, 0, sizeof(instructions));
        memset(&program, 0, sizeof(program));
        instructions[0].opcode = USIL_OP_EQ;
        instructions[0].operand_count = 3;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x70;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[0].operands[1].has_abs = true;
        init_immediate_vector_operand(&instructions[0].operands[2],
                                      0x3f800000u, 0x3f800000u,
                                      0x3f800000u, 0);
        instructions[1].opcode = USIL_OP_RET;
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 2;
        program.instructions = instructions;
        program.instruction_count = 2;
        program.instruction_alloc = 2;
        sb_init(&builder);
        CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(strstr(builder.buf, "abs(r1.x)") != NULL);
        CHECK(strstr(builder.buf, "abs(r1.y)") != NULL);
        CHECK(strstr(builder.buf, "abs(r1.z)") != NULL);
        CHECK(strstr(builder.buf, "abs(r1.xyz)") == NULL);
        sb_free(&builder);
    }

    /* IMUL's operand zero is the optional high destination.  When it is
     * null, source width comes from the low destination in operand one. */
    {
        USILInstruction instructions[2];
        USILProgram program;
        memset(instructions, 0, sizeof(instructions));
        memset(&program, 0, sizeof(program));
        instructions[0].opcode = USIL_OP_IMUL;
        instructions[0].operand_count = 4;
        instructions[0].operands[0].type = OPERAND_TYPE_NULL;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[1].destination_mask = 0x70;
        init_register_operand(&instructions[0].operands[2],
                              OPERAND_TYPE_TEMP, 1);
        instructions[0].operands[2].swizzle[3] = 0;
        init_immediate_vector_operand(&instructions[0].operands[3],
                                      1103515245u, 1103515245u,
                                      1103515245u, 0);
        instructions[1].opcode = USIL_OP_RET;
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 2;
        program.instructions = instructions;
        program.instruction_count = 2;
        program.instruction_alloc = 2;
        sb_init(&builder);
        CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(strstr(builder.buf, "asint(r1.xyz)") != NULL);
        CHECK(strstr(builder.buf, "int3(1103515245") != NULL);
        CHECK(strstr(builder.buf, "r1.xyzx") == NULL);
        sb_free(&builder);
    }

    /* ISHL operates on unsigned lane bits with a five-bit shift count. */
    {
        USILInstruction instructions[2];
        USILProgram program;
        memset(instructions, 0, sizeof(instructions));
        memset(&program, 0, sizeof(program));
        instructions[0].opcode = USIL_OP_ISHL;
        instructions[0].operand_count = 3;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x10;
        init_immediate_operand(&instructions[0].operands[1], 1);
        init_register_operand(&instructions[0].operands[2],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[2].swizzle_mode = 2;
        instructions[0].operands[2].swizzle[0] = 0;
        instructions[1].opcode = USIL_OP_RET;
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 1;
        program.instructions = instructions;
        program.instruction_count = 2;
        program.instruction_alloc = 2;
        sb_init(&builder);
        CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(strstr(builder.buf, "asuint(1) <<") != NULL);
        CHECK(strstr(builder.buf, "& 31u") != NULL);
        sb_free(&builder);
    }

    /* Multisample coordinates are dimension-sized signed integers, and a
     * partial RESINFO result must be projected before assignment. */
    {
        USILTexture texture;
        USILInstruction instructions[3];
        USILProgram program;
        memset(&texture, 0, sizeof(texture));
        memset(instructions, 0, sizeof(instructions));
        memset(&program, 0, sizeof(program));
        texture.reg_idx = 0;
        snprintf(texture.dimension, sizeof(texture.dimension), "2dms");
        memset(texture.return_types, 5, sizeof(texture.return_types));
        instructions[0].opcode = USIL_OP_LD_MS;
        instructions[0].operand_count = 4;
        snprintf(instructions[0].resource_dimension,
                 sizeof(instructions[0].resource_dimension), "2dms");
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 2);
        instructions[0].operands[0].destination_mask = 0xf0;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[0].operands[1].swizzle[2] = 3;
        instructions[0].operands[1].swizzle[3] = 3;
        init_register_operand(&instructions[0].operands[2],
                              OPERAND_TYPE_RESOURCE, 0);
        init_immediate_operand(&instructions[0].operands[3], 0);
        instructions[1].opcode = USIL_OP_RESINFO;
        instructions[1].operand_count = 3;
        instructions[1].resource_info_return_type = 2;
        snprintf(instructions[1].resource_dimension,
                 sizeof(instructions[1].resource_dimension), "2dms");
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[1].operands[0].destination_mask = 0x30;
        init_immediate_operand(&instructions[1].operands[1], 0);
        init_register_operand(&instructions[1].operands[2],
                              OPERAND_TYPE_RESOURCE, 0);
        instructions[2].opcode = USIL_OP_RET;
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 3;
        program.textures = &texture;
        program.texture_count = 1;
        program.texture_alloc = 1;
        program.instructions = instructions;
        program.instruction_count = 3;
        program.instruction_alloc = 3;
        sb_init(&builder);
        CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(strstr(builder.buf, ".Load(asint(r1.xy), 0)") != NULL);
        CHECK(strstr(builder.buf, "asint(asint") == NULL);
        CHECK(strstr(builder.buf, "uint4(w, h, 0u, levels).xy") != NULL);
        sb_free(&builder);
    }

    /* Structured atomics name the proven byte-offset field and give only
     * that field integer storage.  A non-constant offset has no exact typed
     * HLSL member and must fail closed. */
    {
        USILUav uav;
        USILInstruction instructions[2];
        USILProgram program;
        memset(&uav, 0, sizeof(uav));
        memset(instructions, 0, sizeof(instructions));
        memset(&program, 0, sizeof(program));
        uav.reg_idx = 1;
        uav.stride = 8;
        snprintf(uav.dimension, sizeof(uav.dimension), "structured");
        instructions[0].opcode = USIL_OP_IMM_ATOMIC_IADD;
        instructions[0].operand_count = 4;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_UAV, 1);
        init_immediate_vector_operand(&instructions[0].operands[2],
                                      0, 0, 0, 0);
        init_immediate_operand(&instructions[0].operands[3], 1);
        instructions[1].opcode = USIL_OP_RET;
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 1;
        program.uavs = &uav;
        program.uav_count = 1;
        program.uav_alloc = 1;
        program.instructions = instructions;
        program.instruction_count = 2;
        program.instruction_alloc = 2;
        sb_init(&builder);
        CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(strstr(builder.buf, "uint m0;") != NULL);
        CHECK(strstr(builder.buf, "float m4;") != NULL);
        CHECK(strstr(builder.buf,
                     "InterlockedAdd(u1[0u].m0, 1u, atomic_temp)") != NULL);
        sb_free(&builder);

        instructions[0].operands[2].imm_value_count = 1;
        instructions[0].operands[2].immediate_word_count = 1;
        sb_init(&builder);
        CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
        CHECK(builder.failed);
        sb_free(&builder);
    }

    return 0;
}

static int verify_integer_multi_output_emission(void) {
    USILInstruction instruction;
    USILProgram program;
    StringBuilder builder;

    /* A live IMUL high result has no Shader Model 5 HLSL source intrinsic.
     * In particular, the GLSL-only `imulExtended` spelling must never leak
     * into generated ShaderLab. */
    memset(&instruction, 0, sizeof(instruction));
    memset(&program, 0, sizeof(program));
    instruction.opcode = USIL_OP_IMUL;
    instruction.operand_count = 4;
    snprintf(instruction.original_asm, sizeof(instruction.original_asm),
             "imul r0.xy, r2.xy, r0.yx, r1.xy");
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0x30;
    init_register_operand(&instruction.operands[1], OPERAND_TYPE_TEMP, 2);
    instruction.operands[1].destination_mask = 0x30;
    init_register_operand(&instruction.operands[2], OPERAND_TYPE_TEMP, 0);
    instruction.operands[2].swizzle[0] = 1;
    instruction.operands[2].swizzle[1] = 0;
    init_register_operand(&instruction.operands[3], OPERAND_TYPE_TEMP, 1);
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.temp_count = 3;
    program.instructions = &instruction;
    program.instruction_count = 1;
    program.instruction_alloc = 1;

    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    CHECK(builder.buf == NULL || strstr(builder.buf, "imulExtended") == NULL);
    sb_free(&builder);

    /* Low-only IMUL is an ordinary simultaneous vector assignment. The RHS
     * consumes the old r0.yx tuple before r0.xy is committed, so source/output
     * aliasing does not lose a lane and needs no scalar staging. */
    memset(&instruction.operands[0], 0, sizeof(instruction.operands[0]));
    instruction.operands[0].type = OPERAND_TYPE_NULL;
    init_register_operand(&instruction.operands[1], OPERAND_TYPE_TEMP, 0);
    instruction.operands[1].destination_mask = 0x30;
    snprintf(instruction.original_asm, sizeof(instruction.original_asm),
             "imul null, r0.xy, r0.yx, r1.xy");
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf,
                 "r0.xy = asfloat(asint(r0.yx) * asint(r1.xy));") != NULL);
    CHECK(strstr(builder.buf, "imulExtended") == NULL);
    sb_free(&builder);

    /* UDIV has quotient and remainder destinations. The union .xw width is
     * uint2, and r0.wx must be captured before quotient r0.x is committed so
     * the remainder lane still observes the original r0.x. */
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_UDIV;
    instruction.operand_count = 4;
    snprintf(instruction.original_asm, sizeof(instruction.original_asm),
             "udiv r0.x, r2.w, r0.wxxx, r1.xyzw");
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0x10;
    init_register_operand(&instruction.operands[1], OPERAND_TYPE_TEMP, 2);
    instruction.operands[1].destination_mask = 0x80;
    init_register_operand(&instruction.operands[2], OPERAND_TYPE_TEMP, 0);
    instruction.operands[2].swizzle[0] = 3;
    instruction.operands[2].swizzle[3] = 0;
    init_register_operand(&instruction.operands[3], OPERAND_TYPE_TEMP, 1);

    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    const char* udiv_capture =
        strstr(builder.buf, "uint2 udiv_dividend_0 = asuint(r0.wx);");
    const char* udiv_quotient = strstr(
        builder.buf,
        "uint2 udiv_quotient_0 = udiv_dividend_0 / udiv_divisor_0;");
    const char* udiv_remainder = strstr(
        builder.buf,
        "uint2 udiv_remainder_0 = udiv_dividend_0 % udiv_divisor_0;");
    const char* udiv_first_write =
        strstr(builder.buf, "r0.x = asfloat(udiv_quotient_0.x);");
    CHECK(udiv_capture != NULL && udiv_quotient != NULL &&
          udiv_remainder != NULL && udiv_first_write != NULL);
    CHECK(udiv_capture < udiv_quotient && udiv_quotient < udiv_remainder &&
          udiv_remainder < udiv_first_write);
    CHECK(strstr(builder.buf,
                 "r2.w = asfloat(udiv_remainder_0.y);") != NULL);
    sb_free(&builder);

    instruction.operands[1] = instruction.operands[0];
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    return 0;
}

static int verify_sincos_lane_emission(void) {
    USILInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_SINCOS;
    instruction.operand_count = 3;
    snprintf(instruction.original_asm, sizeof(instruction.original_asm),
             "sincos r0.zw, null, r1.zzzw");
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0xc0;
    instruction.operands[1].type = OPERAND_TYPE_NULL;
    init_register_operand(&instruction.operands[2], OPERAND_TYPE_TEMP, 1);
    instruction.operands[2].swizzle[0] = 2;
    instruction.operands[2].swizzle[1] = 2;
    instruction.operands[2].swizzle[2] = 2;
    instruction.operands[2].swizzle[3] = 3;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "vs_5_0");
    program.temp_count = 2;
    program.instructions = &instruction;
    program.instruction_count = 1;
    program.instruction_alloc = 1;

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "r0.zw = sin(r1.zw);") != NULL);
    CHECK(strstr(builder.buf, "sincos(") == NULL);
    CHECK(strstr(builder.buf, "sin(r1.zzzw") == NULL);
    sb_free(&builder);

    HLSLEmitterContext analysis;
    memset(&analysis, 0, sizeof(analysis));
    analysis.program = &program;
    CHECK(analyze_lane_value_types(&analysis));
    CHECK((get_operand_value_facts(&analysis, 0, 2, 2) &
           HLSL_VALUE_FLOAT) != 0);
    CHECK(get_operand_value_facts(&analysis, 0, 1, 2) ==
          HLSL_VALUE_UNKNOWN);
    free_lane_value_types(&analysis);

    /* Replicated DXBC sources remain vector-valued when the destination has
     * multiple lanes; HLSL sincos requires all three arguments to agree. */
    instruction.operands[2].swizzle_mode = 2;
    instruction.operands[2].swizzle[0] = 2;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf,
                 "r0.zw = sin((float2)(r1.z));") != NULL);
    sb_free(&builder);

    /* A null sine destination is the exact unary cosine contract. */
    instruction.operands[1] = instruction.operands[0];
    memset(&instruction.operands[0], 0, sizeof(instruction.operands[0]));
    instruction.operands[0].type = OPERAND_TYPE_NULL;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf,
                 "r0.zw = cos((float2)(r1.z));") != NULL);
    CHECK(strstr(builder.buf, "sincos(") == NULL);
    sb_free(&builder);

    /* The USIL lane authority permits independent destination masks. Pack
     * the union once, then scatter each result back to its logical lanes. */
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0x10;
    init_register_operand(&instruction.operands[1], OPERAND_TYPE_TEMP, 1);
    instruction.operands[1].destination_mask = 0x80;
    init_register_operand(&instruction.operands[2], OPERAND_TYPE_TEMP, 2);
    program.temp_count = 3;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf,
                 "sincos(r2.xw, sincos_sin_temp_0, "
                 "sincos_cos_temp_0);") != NULL);
    CHECK(strstr(builder.buf, "r0.x = sincos_sin_temp_0.x;") != NULL);
    CHECK(strstr(builder.buf, "r1.w = sincos_cos_temp_0.y;") != NULL);
    sb_free(&builder);

    /* A later raw-bits operation selects uint register storage for the whole
     * program. The trigonometric call still operates on float2 values and
     * only the writes are bitcast back to register storage. */
    USILInstruction raw_instructions[2];
    memset(raw_instructions, 0, sizeof(raw_instructions));
    raw_instructions[0] = instruction;
    raw_instructions[1].opcode = USIL_OP_AND;
    raw_instructions[1].operand_count = 3;
    init_register_operand(&raw_instructions[1].operands[0],
                          OPERAND_TYPE_TEMP, 2);
    raw_instructions[1].operands[0].destination_mask = 0x10;
    init_register_operand(&raw_instructions[1].operands[1],
                          OPERAND_TYPE_TEMP, 2);
    raw_instructions[1].operands[1].swizzle_mode = 2;
    raw_instructions[1].operands[1].swizzle[0] = 0;
    init_immediate_operand(&raw_instructions[1].operands[2], 1u);
    program.instructions = raw_instructions;
    program.instruction_count = 2;
    program.instruction_alloc = 2;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "float2 sincos_sin_temp_0;") != NULL);
    CHECK(strstr(builder.buf, "float2 sincos_cos_temp_0;") != NULL);
    CHECK(strstr(builder.buf,
                 "r0.x = asuint(sincos_sin_temp_0.x);") != NULL);
    CHECK(strstr(builder.buf,
                 "r1.w = asuint(sincos_cos_temp_0.y);") != NULL);
    sb_free(&builder);

    /* HLSL evaluates the input argument before the intrinsic call and copies
     * `out` cxvalues back afterward, so a decoded source may alias an output
     * without losing a lane. Equal float2 masks take the direct call path. */
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_SINCOS;
    instruction.operand_count = 3;
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0x30;
    instruction.operands[0].swizzle_mode = 0;
    init_register_operand(&instruction.operands[1], OPERAND_TYPE_TEMP, 1);
    instruction.operands[1].destination_mask = 0x30;
    instruction.operands[1].swizzle_mode = 0;
    init_register_operand(&instruction.operands[2], OPERAND_TYPE_TEMP, 0);
    program.instructions = &instruction;
    program.instruction_count = 1;
    program.instruction_alloc = 1;
    program.temp_count = 2;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "sincos(r0.xy, r0.xy, r1.xy);") != NULL);
    sb_free(&builder);

    /* Aliased output lanes have no decoded copy-out ordering and therefore
     * fail closed instead of silently selecting sine or cosine. */
    instruction.operands[1] = instruction.operands[0];
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    return 0;
}

static int verify_masked_vector_dependency_emission(void) {
    USILInstruction instructions[6];
    memset(instructions, 0, sizeof(instructions));

    /* These two writes are one packed two-lane expression chain.  Splitting
     * either .xw write into scalar temporaries exposes its w lane to LICM and
     * lets D3DCompiler fuse the otherwise independent scalar sine calls. */
    instructions[0].opcode = USIL_OP_ADD;
    instructions[0].operand_count = 3;
    snprintf(instructions[0].original_asm,
             sizeof(instructions[0].original_asm),
             "add r2.xw, r0.yyyz, -r2.xxxy");
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 2);
    instructions[0].operands[0].destination_mask = 0x90;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[1].swizzle[0] = 1;
    instructions[0].operands[1].swizzle[1] = 1;
    instructions[0].operands[1].swizzle[2] = 1;
    instructions[0].operands[1].swizzle[3] = 2;
    init_register_operand(&instructions[0].operands[2], OPERAND_TYPE_TEMP, 2);
    instructions[0].operands[2].swizzle[0] = 0;
    instructions[0].operands[2].swizzle[1] = 0;
    instructions[0].operands[2].swizzle[2] = 0;
    instructions[0].operands[2].swizzle[3] = 1;
    instructions[0].operands[2].has_neg = true;

    instructions[1].opcode = USIL_OP_ADD;
    instructions[1].operand_count = 3;
    snprintf(instructions[1].original_asm,
             sizeof(instructions[1].original_asm),
             "add r2.xw, -r3.xxxy, r2.xxxw");
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 2);
    instructions[1].operands[0].destination_mask = 0x90;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP, 3);
    instructions[1].operands[1].swizzle[0] = 0;
    instructions[1].operands[1].swizzle[1] = 0;
    instructions[1].operands[1].swizzle[2] = 0;
    instructions[1].operands[1].swizzle[3] = 1;
    instructions[1].operands[1].has_neg = true;
    init_register_operand(&instructions[1].operands[2], OPERAND_TYPE_TEMP, 2);
    instructions[1].operands[2].swizzle[0] = 0;
    instructions[1].operands[2].swizzle[1] = 0;
    instructions[1].operands[2].swizzle[2] = 0;
    instructions[1].operands[2].swizzle[3] = 3;

    for (int sine = 0; sine < 2; ++sine) {
        USILInstruction *instruction = &instructions[2 + sine];
        instruction->opcode = USIL_OP_SINCOS;
        instruction->operand_count = 3;
        snprintf(instruction->original_asm,
                 sizeof(instruction->original_asm),
                 "sincos r4.%c, null, r%d.%c", "xy"[sine],
                 sine == 0 ? 2 : 3, sine == 0 ? 'w' : 'x');
        init_register_operand(&instruction->operands[0], OPERAND_TYPE_TEMP, 4);
        instruction->operands[0].destination_mask = 16 << sine;
        instruction->operands[1].type = OPERAND_TYPE_NULL;
        init_register_operand(&instruction->operands[2], OPERAND_TYPE_TEMP,
                              sine == 0 ? 2 : 3);
        instruction->operands[2].swizzle_mode = 2;
        instruction->operands[2].swizzle[0] = sine == 0 ? 3 : 0;
    }

    /* The cubic multiply consumes the lane written by the immediately prior
     * square.  That dependency takes precedence over same-register component
     * canonicalization in the compiler-inverse source order. */
    instructions[4].opcode = USIL_OP_MUL;
    instructions[4].operand_count = 3;
    snprintf(instructions[4].original_asm,
             sizeof(instructions[4].original_asm),
             "mul r0.z, r0.x, r0.x");
    init_register_operand(&instructions[4].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[4].operands[0].destination_mask = 0x40;
    init_register_operand(&instructions[4].operands[1], OPERAND_TYPE_TEMP, 0);
    instructions[4].operands[1].swizzle_mode = 2;
    instructions[4].operands[1].swizzle[0] = 0;
    instructions[4].operands[2] = instructions[4].operands[1];

    instructions[5].opcode = USIL_OP_MUL;
    instructions[5].operand_count = 3;
    snprintf(instructions[5].original_asm,
             sizeof(instructions[5].original_asm),
             "mul r0.x, r0.x, r0.z");
    init_register_operand(&instructions[5].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[5].operands[0].destination_mask = 0x10;
    init_register_operand(&instructions[5].operands[1], OPERAND_TYPE_TEMP, 0);
    instructions[5].operands[1].swizzle_mode = 2;
    instructions[5].operands[1].swizzle[0] = 0;
    init_register_operand(&instructions[5].operands[2], OPERAND_TYPE_TEMP, 0);
    instructions[5].operands[2].swizzle_mode = 2;
    instructions[5].operands[2].swizzle[0] = 2;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.temp_count = 5;
    program.instructions = instructions;
    program.instruction_count = 6;
    program.instruction_alloc = 6;

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "r2.xw = -r2.xy + r0.yz;") != NULL);
    CHECK(strstr(builder.buf, "r2.xw = r2.xw - r3.xy;") != NULL);
    CHECK(strstr(builder.buf, "r4.x = sin(r2.w);") != NULL);
    CHECK(strstr(builder.buf, "r4.y = sin(r3.x);") != NULL);
    CHECK(strstr(builder.buf, "r0.x = r0.z * r0.x;") != NULL);
    CHECK(strstr(builder.buf, "u_xlat_temp_x = -r2.x") == NULL);
    CHECK(strstr(builder.buf, "u_xlat_temp_w = -r2.y") == NULL);
    CHECK(strstr(builder.buf, "r2.x = u_xlat_temp_x") == NULL);
    CHECK(strstr(builder.buf, "r2.w = u_xlat_temp_w") == NULL);
    sb_free(&builder);

    /* Without an immediate definition of exactly one multiplicand lane, the
     * established same-register component-order rule remains authoritative. */
    instructions[4].operands[0].register_index = 1;
    instructions[4].operands[0].index_values[0] = 1;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "r0.x = r0.x * r0.z;") != NULL);
    sb_free(&builder);
    return 0;
}

static int verify_declaration_control_contracts(void) {
    const uint32_t sampler_operand =
        operand_token(OPERAND_TYPE_SAMPLER, 1u, 1u, 0u, 0u, 0u);
    const uint32_t mono_sampler[] = {
        instruction_token(90u, 3u) | (2u << 11u),
        sampler_operand, 0u, instruction_token(62u, 1u)
    };
    const uint32_t reserved_sampler[] = {
        instruction_token(90u, 3u) | (3u << 11u),
        sampler_operand, 0u, instruction_token(62u, 1u)
    };
    CHECK(test_shader_parse_result(
        reserved_sampler,
        sizeof(reserved_sampler) / sizeof(reserved_sampler[0]), 6u, false,
        NULL));

    uint8_t bytes[512];
    size_t byte_count = build_test_shader_container(
        mono_sampler, sizeof(mono_sampler) / sizeof(mono_sampler[0]), 6u,
        bytes, sizeof(bytes));
    DXBCContainer container;
    CHECK(byte_count > 0u && dxbc_parse(&container, bytes, byte_count));
    CHECK(container.instruction_count == 2);
    CHECK(((container.instructions[0].token >> 11u) & 0x0fu) == 2u);
    USILProgram parsed_program;
    CHECK(usil_translate(&parsed_program, &container));
    CHECK(parsed_program.sampler_count == 1);
    CHECK(parsed_program.samplers[0].mode == 2u);
    StringBuilder output;
    sb_init(&output);
    CHECK(!hlsl_emit(&parsed_program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    sb_free(&output);
    usil_free(&parsed_program);
    dxbc_free(&container);

    USILTexture texture;
    memset(&texture, 0, sizeof(texture));
    texture.reg_idx = 0;
    snprintf(texture.dimension, sizeof(texture.dimension), "2d");
    memset(texture.return_types, 5, sizeof(texture.return_types));
    USILSampler sampler = {0, 1u};
    DXBCSignatureElement target;
    memset(&target, 0, sizeof(target));
    snprintf(target.semantic_name, sizeof(target.semantic_name),
             "SV_Target");
    target.semantic_name_length = strlen(target.semantic_name);
    target.system_value = 64u;
    target.component_type = 3u;
    target.mask = 0x0fu;

    USILInstruction sample[2];
    memset(sample, 0, sizeof(sample));
    sample[0].opcode = USIL_OP_SAMPLE_C;
    sample[0].operand_count = 5;
    snprintf(sample[0].resource_dimension,
             sizeof(sample[0].resource_dimension), "2d");
    init_register_operand(&sample[0].operands[0], OPERAND_TYPE_OUTPUT, 0);
    sample[0].operands[0].destination_mask = 0xf0u;
    init_immediate_vector_operand(&sample[0].operands[1], 0u, 0u, 0u, 0u);
    init_register_operand(&sample[0].operands[2], OPERAND_TYPE_RESOURCE, 0);
    init_register_operand(&sample[0].operands[3], OPERAND_TYPE_SAMPLER, 0);
    init_immediate_operand(&sample[0].operands[4], 0u);
    sample[1].opcode = USIL_OP_RET;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.outputs = &target;
    program.output_count = 1;
    program.output_alloc = 1;
    program.textures = &texture;
    program.texture_count = 1;
    program.texture_alloc = 1;
    program.samplers = &sampler;
    program.sampler_count = 1;
    program.sampler_alloc = 1;
    program.instructions = sample;
    program.instruction_count = 2;
    program.instruction_alloc = 2;

    sb_init(&output);
    CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
    sb_free(&output);
    sampler.mode = 0u;
    sb_init(&output);
    CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    sb_free(&output);
    sampler.mode = 1u;
    sample[0].opcode = USIL_OP_SAMPLE;
    sample[0].operand_count = 4;
    sb_init(&output);
    CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    sb_free(&output);
    sample[1] = sample[0];
    sample[0].opcode = USIL_OP_SAMPLE_C;
    sample[0].operand_count = 5;
    sb_init(&output);
    CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    sb_free(&output);
    sample[0].opcode = USIL_OP_RET;
    sample[0].operand_count = 0;
    program.instruction_count = 1;
    sb_init(&output);
    CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    sb_free(&output);

    const uint32_t cbuffer_operand =
        operand_token(OPERAND_TYPE_CONSTANT_BUFFER, 2u, 2u, 0u, 0u, 0u) |
        UINT32_C(0x000000f0);
    uint32_t cbuffer_declaration[] = {
        instruction_token(89u, 4u), cbuffer_operand, 0u, 4u,
        instruction_token(62u, 1u)
    };
    for (uint32_t dynamic = 0u; dynamic <= 1u; ++dynamic) {
        cbuffer_declaration[0] =
            instruction_token(89u, 4u) | (dynamic << 11u);
        byte_count = build_test_shader_container(
            cbuffer_declaration,
            sizeof(cbuffer_declaration) / sizeof(cbuffer_declaration[0]),
            7u, bytes, sizeof(bytes));
        CHECK(byte_count > 0u && dxbc_parse(&container, bytes, byte_count));
        CHECK(usil_translate(&parsed_program, &container));
        CHECK(parsed_program.cbuffer_count == 1);
        CHECK(parsed_program.cbuffers[0].dynamic_indexed == (dynamic != 0u));
        sb_init(&output);
        CHECK(hlsl_emit(&parsed_program, &output, NULL, NULL, NULL) ==
              (dynamic == 0u));
        CHECK((dynamic == 0u) != output.failed);
        sb_free(&output);
        usil_free(&parsed_program);
        dxbc_free(&container);
    }

    USILConstantBuffer cbuffer = {0, 4, false};
    DXBCOperand relative;
    init_register_operand(&relative, OPERAND_TYPE_TEMP, 0);
    USILInstruction dynamic_access;
    memset(&dynamic_access, 0, sizeof(dynamic_access));
    dynamic_access.opcode = USIL_OP_MOV;
    dynamic_access.operand_count = 2;
    init_register_operand(&dynamic_access.operands[0], OPERAND_TYPE_OUTPUT,
                          0);
    dynamic_access.operands[0].destination_mask = 0x10u;
    DXBCOperand* cbuffer_source = &dynamic_access.operands[1];
    memset(cbuffer_source, 0, sizeof(*cbuffer_source));
    cbuffer_source->type = OPERAND_TYPE_CONSTANT_BUFFER;
    cbuffer_source->register_index = 0;
    cbuffer_source->register_index_dim = 2;
    cbuffer_source->index_has_immediate[0] = true;
    cbuffer_source->index_values[0] = 0u;
    cbuffer_source->index_representations[1] = 2u;
    cbuffer_source->rel_op1 = &relative;
    cbuffer_source->swizzle_mode = 1u;
    cbuffer_source->swizzle[0] = 0u;
    cbuffer_source->swizzle[1] = 1u;
    cbuffer_source->swizzle[2] = 2u;
    cbuffer_source->swizzle[3] = 3u;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.outputs = &target;
    program.output_count = 1;
    program.output_alloc = 1;
    program.cbuffers = &cbuffer;
    program.cbuffer_count = 1;
    program.cbuffer_alloc = 1;
    program.temp_count = 1;
    program.instructions = &dynamic_access;
    program.instruction_count = 1;
    program.instruction_alloc = 1;
    sb_init(&output);
    CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    sb_free(&output);
    return 0;
}

static int verify_signature_declaration_contract(void) {
    DXBCSignatureElement input;
    memset(&input, 0, sizeof(input));
    snprintf(input.semantic_name, sizeof(input.semantic_name),
             "SV_Position");
    input.semantic_name_length = strlen(input.semantic_name);
    input.system_value = 1u;
    input.component_type = 3u;
    input.register_id = 0u;
    input.mask = 0x0fu;
    input.rw_mask = 0x01u;

    DXBCInstruction instructions[2];
    memset(instructions, 0, sizeof(instructions));
    instructions[0].opcode = 100u;
    instructions[0].is_decl = true;
    instructions[0].operand_count = 2;
    instructions[0].has_declaration_system_value = true;
    instructions[0].declaration_system_value = 1u;
    instructions[0].has_declaration_interpolation = true;
    instructions[0].declaration_interpolation = 4u;
    DXBCOperand* declaration_operand = &instructions[0].operands[0];
    memset(declaration_operand, 0, sizeof(*declaration_operand));
    declaration_operand->type = OPERAND_TYPE_INPUT;
    declaration_operand->register_index = 0;
    declaration_operand->register_index_dim = 1;
    declaration_operand->index_has_immediate[0] = true;
    declaration_operand->index_values[0] = 0u;
    declaration_operand->destination_mask = 0x10u;
    declaration_operand->swizzle_mode = 0u;
    instructions[1].opcode = 62u;

    DXBCContainer container;
    memset(&container, 0, sizeof(container));
    snprintf(container.shader_type_model,
             sizeof(container.shader_type_model), "ps_5_0");
    container.major_version = 5u;
    container.input_signature = &input;
    container.input_signature_count = 1;
    container.input_signature_alloc = 1;
    container.instructions = instructions;
    container.instruction_count = 2;
    container.instruction_alloc = 2;
    container.parsed_signature_authority = true;

    USILProgram program;
    CHECK(usil_translate(&program, &container));
    CHECK(program.has_parsed_signature_authority);
    CHECK(program.signature_declaration_count == 1);
    CHECK(program.signature_declarations[0].kind ==
          USIL_SIGNATURE_DECL_INPUT_PS_SIV);
    CHECK(program.signature_declarations[0].system_value_name == 1u);
    CHECK(program.signature_declarations[0].interpolation_mode == 4u);
    CHECK(program.signature_declarations[0].mask == 1u);
    CHECK(program.inputs[0].interpolation_mode == 4u);
    CHECK(usil_signature_authority_is_valid(&program));

    StringBuilder hlsl;
    sb_init(&hlsl);
    CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    CHECK(hlsl.buf != NULL &&
          strstr(hlsl.buf, "noperspective float4 v0 : SV_Position") != NULL);
    sb_free(&hlsl);

    program.inputs[0].component_type = 0u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.inputs[0].component_type = 3u;
    program.inputs[0].mask = 0u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.inputs[0].mask = 0x0fu;
    program.inputs[0].rw_mask = 0x10u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.inputs[0].rw_mask = 0x01u;
    program.inputs[0].min_precision = 3u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.inputs[0].min_precision = 0u;
    program.inputs[0].interpolation_mode = 8u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.inputs[0].interpolation_mode = 4u;
    program.inputs[0].stream_index = 1u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.inputs[0].stream_index = 0u;
    program.inputs[0].system_value = 2u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.inputs[0].system_value = 1u;
    program.signature_declarations[0].system_value_name = 2u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.signature_declarations[0].system_value_name = 1u;
    program.signature_declarations[0].kind =
        USIL_SIGNATURE_DECL_INPUT_SGV;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.signature_declarations[0].kind =
        USIL_SIGNATURE_DECL_INPUT_PS_SIV;
    program.signature_declarations[0].interpolation_mode = 8u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.signature_declarations[0].interpolation_mode = 4u;
    program.signature_declarations[0].stream_index = 1u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.signature_declarations[0].stream_index = 0u;
    program.signature_declarations[0].mask = 0x10u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.signature_declarations[0].mask = 8u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.signature_declarations[0].mask = 1u;
    CHECK(usil_signature_authority_is_valid(&program));
    usil_free(&program);

    instructions[0].declaration_system_value = 2u;
    CHECK(!usil_translate(&program, &container));
    instructions[0].declaration_system_value = 1u;
    instructions[0].has_declaration_system_value = false;
    CHECK(!usil_translate(&program, &container));

    USILSignatureDeclaration intrinsic;
    memset(&intrinsic, 0, sizeof(intrinsic));
    intrinsic.kind = USIL_SIGNATURE_DECL_INPUT;
    intrinsic.operand_type = OPERAND_TYPE_DOMAIN_LOCATION;
    intrinsic.register_id = UINT32_MAX;
    intrinsic.mask = 7u;
    intrinsic.source_instruction_index = 1u;
    memset(&program, 0, sizeof(program));
    program.program_type = DXBC_PROGRAM_TYPE_DOMAIN;
    program.tessellation.valid = true;
    program.tessellation.domain = DXBC_TESSELLATOR_DOMAIN_TRIANGLE;
    program.signature_declarations = &intrinsic;
    program.signature_declaration_count = 1;
    program.signature_declaration_alloc = 1;
    CHECK(usil_signature_authority_is_valid(&program));
    intrinsic.mask = 3u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.tessellation.domain = DXBC_TESSELLATOR_DOMAIN_QUAD;
    CHECK(usil_signature_authority_is_valid(&program));
    intrinsic.mask = 0u;
    CHECK(!usil_signature_authority_is_valid(&program));

    DXBCSignatureElement control_point;
    memset(&control_point, 0, sizeof(control_point));
    snprintf(control_point.semantic_name,
             sizeof(control_point.semantic_name), "NORMAL");
    control_point.semantic_name_length =
        strlen(control_point.semantic_name);
    control_point.component_type = 3u;
    control_point.mask = 7u;
    control_point.rw_mask = 7u;
    memset(&intrinsic, 0, sizeof(intrinsic));
    intrinsic.kind = USIL_SIGNATURE_DECL_INPUT;
    intrinsic.operand_type = OPERAND_TYPE_INPUT_CONTROL_POINT;
    intrinsic.has_signature_register = true;
    intrinsic.register_id = 0u;
    intrinsic.mask = 7u;
    intrinsic.has_array_element_count = true;
    intrinsic.array_element_count = 3u;
    intrinsic.source_instruction_index = 1u;
    memset(&program, 0, sizeof(program));
    program.program_type = DXBC_PROGRAM_TYPE_DOMAIN;
    program.tessellation.valid = true;
    program.tessellation.input_control_point_count = 3u;
    program.inputs = &control_point;
    program.input_count = 1;
    program.input_alloc = 1;
    program.signature_declarations = &intrinsic;
    program.signature_declaration_count = 1;
    program.signature_declaration_alloc = 1;
    program.has_parsed_signature_authority = true;
    CHECK(usil_signature_authority_is_valid(&program));
    intrinsic.array_element_count = 2u;
    CHECK(!usil_signature_authority_is_valid(&program));
    intrinsic.array_element_count = 3u;
    intrinsic.register_id = 1u;
    CHECK(!usil_signature_authority_is_valid(&program));

    DXBCSignatureElement depth;
    memset(&depth, 0, sizeof(depth));
    snprintf(depth.semantic_name, sizeof(depth.semantic_name), "SV_Depth");
    depth.semantic_name_length = strlen(depth.semantic_name);
    depth.component_type = 3u;
    depth.register_id = UINT32_MAX;
    depth.mask = 1u;
    depth.rw_mask = 0x0eu;
    memset(&intrinsic, 0, sizeof(intrinsic));
    intrinsic.kind = USIL_SIGNATURE_DECL_OUTPUT;
    intrinsic.operand_type = OPERAND_TYPE_OUTPUT_DEPTH;
    intrinsic.register_id = UINT32_MAX;
    intrinsic.source_instruction_index = 1u;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.program_type = DXBC_PROGRAM_TYPE_PIXEL;
    program.outputs = &depth;
    program.output_count = 1;
    program.output_alloc = 1;
    program.signature_declarations = &intrinsic;
    program.signature_declaration_count = 1;
    program.signature_declaration_alloc = 1;
    program.has_parsed_signature_authority = true;
    CHECK(usil_signature_authority_is_valid(&program));
    sb_init(&hlsl);
    CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    sb_free(&hlsl);
    depth.register_id = 0u;
    CHECK(!usil_signature_authority_is_valid(&program));
    depth.register_id = UINT32_MAX;
    intrinsic.operand_type = OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL;
    CHECK(!usil_signature_authority_is_valid(&program));
    sb_init(&hlsl);
    CHECK(!hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    CHECK(hlsl.failed);
    sb_free(&hlsl);

    memset(&intrinsic, 0, sizeof(intrinsic));
    intrinsic.kind = USIL_SIGNATURE_DECL_INPUT;
    intrinsic.operand_type = OPERAND_TYPE_INPUT_THREAD_ID;
    intrinsic.register_id = UINT32_MAX;
    intrinsic.mask = 7u;
    intrinsic.source_instruction_index = 1u;
    memset(&program, 0, sizeof(program));
    program.program_type = DXBC_PROGRAM_TYPE_COMPUTE;
    program.signature_declarations = &intrinsic;
    program.signature_declaration_count = 1;
    program.signature_declaration_alloc = 1;
    CHECK(usil_signature_authority_is_valid(&program));
    intrinsic.mask = 3u;
    CHECK(!usil_signature_authority_is_valid(&program));
    intrinsic.mask = 7u;
    program.program_type = DXBC_PROGRAM_TYPE_PIXEL;
    CHECK(!usil_signature_authority_is_valid(&program));
    return 0;
}

static int verify_structural_vector_output_mad_lowering(void) {
    USILInstruction instructions[2];
    memset(instructions, 0, sizeof(instructions));
    instructions[0].opcode = USIL_OP_MAD;
    instructions[0].operand_count = 4;
    snprintf(instructions[0].original_asm,
             sizeof(instructions[0].original_asm),
             "mad o0.xyz, cb0[45].xyzx, r0.wwww, r0.xyzx");
    init_register_operand(&instructions[0].operands[0],
                          OPERAND_TYPE_OUTPUT, 0);
    instructions[0].operands[0].destination_mask = 0x70;
    init_cbuffer_operand(&instructions[0].operands[1], 0, 45);
    init_register_operand(&instructions[0].operands[2],
                          OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[2].swizzle_mode = 2;
    instructions[0].operands[2].swizzle[0] = 3;
    init_register_operand(&instructions[0].operands[3],
                          OPERAND_TYPE_TEMP, 0);
    instructions[1].opcode = USIL_OP_RET;

    DXBCSignatureElement output_signature;
    memset(&output_signature, 0, sizeof(output_signature));
    snprintf(output_signature.semantic_name,
             sizeof(output_signature.semantic_name), "TEXCOORD");
    output_signature.semantic_name_length =
        strlen(output_signature.semantic_name);
    output_signature.component_type = 3u;
    output_signature.register_id = 0u;
    output_signature.mask = 0x07u;

    USILConstantBuffer cbuffer = {0, 46, false};
    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "vs_4_0");
    program.program_type = DXBC_PROGRAM_TYPE_VERTEX;
    program.temp_count = 1;
    program.outputs = &output_signature;
    program.output_count = 1;
    program.output_alloc = 1;
    program.cbuffers = &cbuffer;
    program.cbuffer_count = 1;
    program.cbuffer_alloc = 1;
    program.instructions = instructions;
    program.instruction_count = 2;
    program.instruction_alloc = 2;

    StringBuilder output;
    sb_init(&output);
    CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.buf != NULL);
    CHECK(strstr(output.buf,
                 "o0.xyz = get_cb0(45).xyz * r0.w + r0.xyz;") != NULL);
    CHECK(strstr(output.buf,
                 "u_xlat_temp_x = get_cb0(45).x * r0.w + r0.x;") == NULL);
    sb_free(&output);

    /* The recompile decision is exact and mutation-safe: changing the
     * accumulator register breaks the decoded vector-MAD contract, so the
     * ordinary three-lane output lowering remains scalar. */
    instructions[0].operands[3].register_index = 1;
    instructions[0].operands[3].index_values[0] = 1u;
    program.temp_count = 2;
    sb_init(&output);
    CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(strstr(output.buf,
                 "u_xlat_temp_x = get_cb0(45).x * r0.w + r1.x;") != NULL);
    CHECK(strstr(output.buf,
                 "o0.xyz = get_cb0(45).xyz * r0.w + r1.xyz;") == NULL);
    sb_free(&output);
    return 0;
}

static int verify_never_written_output_lanes_are_not_fabricated(void) {
    DXBCSignatureElement outputs[3];
    memset(outputs, 0, sizeof(outputs));

    snprintf(outputs[0].semantic_name, sizeof(outputs[0].semantic_name),
             "SV_Position");
    outputs[0].semantic_name_length = strlen(outputs[0].semantic_name);
    outputs[0].system_value = 1u;
    outputs[0].component_type = 3u;
    outputs[0].register_id = 0u;
    outputs[0].mask = 0x0fu;

    snprintf(outputs[1].semantic_name, sizeof(outputs[1].semantic_name),
             "TEXCOORD");
    outputs[1].semantic_name_length = strlen(outputs[1].semantic_name);
    outputs[1].semantic_index = 1u;
    outputs[1].component_type = 3u;
    outputs[1].register_id = 1u;
    outputs[1].mask = 0x07u;

    snprintf(outputs[2].semantic_name, sizeof(outputs[2].semantic_name),
             "TEXCOORD");
    outputs[2].semantic_name_length = strlen(outputs[2].semantic_name);
    outputs[2].semantic_index = 2u;
    outputs[2].component_type = 3u;
    outputs[2].register_id = 1u;
    outputs[2].mask = 0x08u;
    outputs[2].rw_mask = 0x08u; /* OSGN: lane is never written. */

    USILInstruction ret;
    memset(&ret, 0, sizeof(ret));
    ret.opcode = USIL_OP_RET;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "vs_4_0");
    program.program_type = DXBC_PROGRAM_TYPE_VERTEX;
    program.outputs = outputs;
    program.output_count = 3;
    program.output_alloc = 3;
    USILSignatureDeclaration declarations[3];
    memset(declarations, 0, sizeof(declarations));
    declarations[0].kind = USIL_SIGNATURE_DECL_OUTPUT_SIV;
    declarations[0].operand_type = OPERAND_TYPE_OUTPUT;
    declarations[0].has_signature_register = true;
    declarations[0].register_id = 0u;
    declarations[0].mask = 0x0fu;
    declarations[0].has_system_value = true;
    declarations[0].system_value_name = 1u;
    declarations[0].source_instruction_index = 0u;
    declarations[1].kind = USIL_SIGNATURE_DECL_OUTPUT;
    declarations[1].operand_type = OPERAND_TYPE_OUTPUT;
    declarations[1].has_signature_register = true;
    declarations[1].register_id = 1u;
    declarations[1].mask = 0x07u;
    declarations[1].source_instruction_index = 1u;
    declarations[2].kind = USIL_SIGNATURE_DECL_OUTPUT;
    declarations[2].operand_type = OPERAND_TYPE_OUTPUT;
    declarations[2].has_signature_register = true;
    declarations[2].register_id = 1u;
    declarations[2].mask = 0x08u;
    declarations[2].source_instruction_index = 2u;
    program.signature_declarations = declarations;
    program.signature_declaration_count = 2;
    program.signature_declaration_alloc = 3;
    program.has_parsed_signature_authority = true;
    program.instructions = &ret;
    program.instruction_count = 1;
    program.instruction_alloc = 1;

    StringBuilder hlsl;
    sb_init(&hlsl);
    CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    CHECK(strstr(hlsl.buf, "float3 o1 : TEXCOORD1;") != NULL);
    /* Preserve the complete OSGN contract in the return type so recompilation
     * reproduces the container signature, but do not fabricate an assignment
     * for a lane the signature proves the executable never writes. */
    CHECK(strstr(hlsl.buf, "float o_TEXCOORD2 : TEXCOORD2;") != NULL);
    CHECK(strstr(hlsl.buf, "output.o_TEXCOORD2 = o1.w;") == NULL);
    CHECK(strstr(hlsl.buf, "output.o1 = o1.xyz;") != NULL);
    sb_free(&hlsl);

    /* Near miss: clearing the authoritative never-written bit makes w a real
     * output again, so the distinct semantic and copy-back must reappear. */
    outputs[2].rw_mask = 0u;
    program.signature_declaration_count = 3;
    sb_init(&hlsl);
    CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
    CHECK(strstr(hlsl.buf, "float o_TEXCOORD2 : TEXCOORD2;") != NULL);
    CHECK(strstr(hlsl.buf, "output.o_TEXCOORD2 = o1.w;") != NULL);
    sb_free(&hlsl);
    return 0;
}

static int verify_hlsl_sm5_register_boundaries(void) {
    const size_t allocation_count = g_allocations_count;
    const size_t allocated_bytes = g_allocated_bytes;
    USILInstruction instructions[3];
    memset(instructions, 0, sizeof(instructions));
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP,
                          4095);
    instructions[0].operands[0].destination_mask = 0x10;
    init_immediate_operand(&instructions[0].operands[1], 0x3f800000u);
    instructions[1].opcode = USIL_OP_MOV;
    instructions[1].operand_count = 2;
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_OUTPUT,
                          31);
    instructions[1].operands[0].destination_mask = 0x10;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP,
                          4095);
    instructions[1].operands[1].swizzle_mode = 2;
    instructions[1].operands[1].swizzle[0] = 0;
    instructions[2].opcode = USIL_OP_RET;

    DXBCSignatureElement output;
    memset(&output, 0, sizeof(output));
    snprintf(output.semantic_name, sizeof(output.semantic_name), "TEXCOORD");
    output.semantic_index = 31;
    output.register_id = 31;
    output.mask = 1;
    output.component_type = 3;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "vs_5_0");
    program.outputs = &output;
    program.output_count = 1;
    program.output_alloc = 1;
    program.temp_count = HLSL_SM5_TEMP_REGISTER_COUNT;
    program.instructions = instructions;
    program.instruction_count = 3;
    program.instruction_alloc = 3;

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "float4 r4095 = (float4)0;") != NULL);
    CHECK(strstr(builder.buf, "float4 o31 = (float4)0;") != NULL);
    sb_free(&builder);

    program.temp_count = HLSL_SM5_TEMP_REGISTER_COUNT + 1;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    program.temp_count = HLSL_SM5_TEMP_REGISTER_COUNT;

    output.register_id = HLSL_SM5_IO_REGISTER_COUNT;
    instructions[1].operands[0].register_index =
        HLSL_SM5_IO_REGISTER_COUNT;
    instructions[1].operands[0].index_values[0] =
        HLSL_SM5_IO_REGISTER_COUNT;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);

    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    snprintf(output.semantic_name, sizeof(output.semantic_name), "SV_Target");
    output.system_value = 64u;
    output.semantic_index = 7;
    output.register_id = 7;
    instructions[1].operands[0].register_index = 7;
    instructions[1].operands[0].index_values[0] = 7;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    sb_free(&builder);
    output.semantic_index = 8;
    output.register_id = HLSL_SM5_PIXEL_OUTPUT_REGISTER_COUNT;
    instructions[1].operands[0].register_index =
        HLSL_SM5_PIXEL_OUTPUT_REGISTER_COUNT;
    instructions[1].operands[0].index_values[0] =
        HLSL_SM5_PIXEL_OUTPUT_REGISTER_COUNT;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);

    CHECK(g_allocations_count == allocation_count);
    CHECK(g_allocated_bytes == allocated_bytes);
    return 0;
}

static int generation_allocator_call_count;
static int generation_allocator_fail_call;

static void* generation_test_calloc(size_t count, size_t element_size) {
    generation_allocator_call_count++;
    if (generation_allocator_call_count == generation_allocator_fail_call)
        return NULL;
    return calloc(count, element_size);
}

static int verify_dynamic_generation_state(void) {
    USILProgram program;
    memset(&program, 0, sizeof(program));
    program.instruction_count = 4;
    program.temp_count = 3;

    HLSLEmitterContext context;
    memset(&context, 0, sizeof(context));
    context.program = &program;

    /* Fail after one successful matrix allocation. The helper must release
     * that partial state and leave a reusable, zeroed context. */
    generation_allocator_call_count = 0;
    generation_allocator_fail_call = 2;
    CHECK(!allocate_hlsl_generation_state(&context,
                                           generation_test_calloc));
    CHECK(context.inst_src_gen == NULL);
    CHECK(context.inst_dest_gen == NULL);
    CHECK(context.reg_max_gen == NULL);
    CHECK(context.generation_instruction_count == 0);
    CHECK(context.generation_register_count == 0);

    /* Also cover failure of the final per-register allocation. */
    generation_allocator_call_count = 0;
    generation_allocator_fail_call = 3;
    CHECK(!allocate_hlsl_generation_state(&context,
                                           generation_test_calloc));
    CHECK(context.inst_src_gen == NULL);
    CHECK(context.inst_dest_gen == NULL);
    CHECK(context.reg_max_gen == NULL);

    generation_allocator_call_count = 0;
    generation_allocator_fail_call = 0;
    CHECK(allocate_hlsl_generation_state(&context, generation_test_calloc));
    CHECK(context.generation_instruction_count == 4);
    CHECK(context.generation_register_count == 3);
    context.inst_src_gen[1 * 3 + 2] = 7;
    context.inst_dest_gen[2 * 3 + 1] = 9;
    context.reg_max_gen[2] = 7;
    CHECK(hlsl_instruction_generation(&context, false, 1, 2) == 7);
    CHECK(hlsl_instruction_generation(&context, true, 2, 1) == 9);
    CHECK(hlsl_register_max_generation(&context, 2) == 7);
    free_hlsl_generation_state(&context);
    CHECK(context.inst_src_gen == NULL);
    CHECK(context.inst_dest_gen == NULL);
    CHECK(context.reg_max_gen == NULL);
    return 0;
}

static int verify_typed_emitter_storage_decisions(void) {
    HLSLEmitterContext context;
    memset(&context, 0, sizeof(context));
    StringBuilder diagnostics;
    sb_init(&diagnostics);
    context.sb = &diagnostics;
    context.use_uint_temps = true;
    HLSLTempLaneNames write_redirects[4];
    HLSLTempLaneFlags has_write_redirect[4];
    HLSLTempLaneStorage write_redirect_storage[4];
    HLSLTempLaneFlags has_int_temp[4];
    HLSLTempLaneFlags has_ftoi_temp[4];
    HLSLTempLaneFlags has_deferred_float[4];
    memset(write_redirects, 0, sizeof(write_redirects));
    memset(has_write_redirect, 0, sizeof(has_write_redirect));
    memset(write_redirect_storage, 0, sizeof(write_redirect_storage));
    memset(has_int_temp, 0, sizeof(has_int_temp));
    memset(has_ftoi_temp, 0, sizeof(has_ftoi_temp));
    memset(has_deferred_float, 0, sizeof(has_deferred_float));
    context.temp_state_count = 4;
    context.write_redirects = write_redirects;
    context.has_write_redirect = has_write_redirect;
    context.write_redirect_storage = write_redirect_storage;
    context.has_int_temp = has_int_temp;
    context.has_ftoi_temp = has_ftoi_temp;
    context.has_deferred_float = has_deferred_float;

    DXBCOperand operand;
    init_register_operand(&operand, OPERAND_TYPE_TEMP, 3);
    operand.destination_mask = 0x10;
    context.has_write_redirect[3][0] = true;

    /* A float redirect whose identifier happens to contain the legacy marker
     * remains float storage. */
    snprintf(context.write_redirects[3][0],
             sizeof(context.write_redirects[3][0]),
             "renamed_u_xlati_but_float");
    context.write_redirect_storage[3][0] = HLSL_BACKING_STORAGE_FLOAT;
    CHECK(hlsl_operand_backing_storage(&context, &operand, 0x10, true) ==
          HLSL_BACKING_STORAGE_FLOAT);

    /* Conversely, a signed redirect with an unrelated spelling remains
     * signed storage. */
    snprintf(context.write_redirects[3][0],
             sizeof(context.write_redirects[3][0]), "plain_lane_name");
    context.write_redirect_storage[3][0] = HLSL_BACKING_STORAGE_SINT;
    CHECK(hlsl_operand_backing_storage(&context, &operand, 0x10, true) ==
          HLSL_BACKING_STORAGE_SINT);

    static const char nested_inner[] =
        "asint(alpha) + helper((asint(beta + (gamma))))";
    char expression[256];
    snprintf(expression, sizeof(expression), "asfloat(%s)", nested_inner);
    CHECK(hlsl_retarget_expression_for_storage(
        &context, HLSL_EXPRESSION_INTEGER_BITS_AS_FLOAT,
        HLSL_BACKING_STORAGE_SINT, expression));
    CHECK(strcmp(expression, nested_inner) == 0);

    /* The same typed conversion works for identifiers containing the old
     * magic spelling, without rewriting nested asint calls. */
    static const char marker_inner[] =
        "asint(value_u_xlati_named) + asint(other)";
    snprintf(expression, sizeof(expression), "asfloat(%s)", marker_inner);
    CHECK(hlsl_retarget_expression_for_storage(
        &context, HLSL_EXPRESSION_INTEGER_BITS_AS_FLOAT,
        HLSL_BACKING_STORAGE_SINT, expression));
    CHECK(strcmp(expression, marker_inner) == 0);

    char malformed[] = "asfloat(asint(alpha)) + trailing";
    CHECK(!hlsl_retarget_expression_for_storage(
        &context, HLSL_EXPRESSION_INTEGER_BITS_AS_FLOAT,
        HLSL_BACKING_STORAGE_SINT, malformed));
    CHECK(diagnostics.failed);
    CHECK(malformed[0] == '\0');
    sb_free(&diagnostics);
    return 0;
}

static int verify_unsupported_hlsl_opcode_fails_closed(void) {
    USILInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_NOP;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.instructions = &instruction;
    program.instruction_count = 1;
    program.instruction_alloc = 1;

    StringBuilder output;
    sb_init(&output);
    CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    CHECK(!output.buf || strstr(output.buf, "Unhandled USIL") == NULL);
    sb_free(&output);

    instruction.opcode = (USILOpcode)999;
    sb_init(&output);
    CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    CHECK(!output.buf || strstr(output.buf, "Unhandled USIL") == NULL);
    sb_free(&output);

    /* Unsupported architectural operands must never be represented by the
     * historical uninitialized `unkN` register fallback. */
    instruction.opcode = USIL_OP_MOV;
    instruction.operand_count = 2;
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0x10;
    memset(&instruction.operands[1], 0, sizeof(instruction.operands[1]));
    instruction.operands[1].type = OPERAND_TYPE_INPUT_COVERAGE_MASK;
    instruction.operands[1].swizzle_mode = 2;
    program.temp_count = 1;
    sb_init(&output);
    CHECK(!hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.failed);
    CHECK(!output.buf || strstr(output.buf, "unk") == NULL);
    sb_free(&output);
    return 0;
}

static int verify_typed_decomposition_emission(void) {
    USILInstruction instructions[5];
    memset(instructions, 0, sizeof(instructions));
    const USILOpcode writers[3] = {USIL_OP_IADD, USIL_OP_ADD, USIL_OP_ADD};
    for (int component = 0; component < 3; ++component) {
        instructions[component].opcode = writers[component];
        instructions[component].operand_count = 3;
        init_register_operand(&instructions[component].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[component].operands[0].destination_mask =
            16 << component;
        init_register_operand(&instructions[component].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[component].operands[1].swizzle_mode = 2;
        instructions[component].operands[1].swizzle[0] = (uint8_t)component;
        init_register_operand(&instructions[component].operands[2],
                              OPERAND_TYPE_TEMP, 2);
        instructions[component].operands[2].swizzle_mode = 2;
        instructions[component].operands[2].swizzle[0] = (uint8_t)component;
        snprintf(instructions[component].original_asm,
                 sizeof(instructions[component].original_asm),
                 "typed_write_%d", component);
    }

    instructions[3].opcode = USIL_OP_MOV;
    instructions[3].operand_count = 2;
    init_register_operand(&instructions[3].operands[0], OPERAND_TYPE_TEMP, 3);
    instructions[3].operands[0].destination_mask = 0x70;
    init_register_operand(&instructions[3].operands[1], OPERAND_TYPE_TEMP, 0);
    instructions[3].operands[1].swizzle[0] = 1;
    instructions[3].operands[1].swizzle[1] = 0;
    instructions[3].operands[1].swizzle[2] = 2;
    snprintf(instructions[3].original_asm,
             sizeof(instructions[3].original_asm), "typed_swizzle_move");
    instructions[4].opcode = USIL_OP_RET;
    snprintf(instructions[4].original_asm,
             sizeof(instructions[4].original_asm), "typed_return");

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.temp_count = 4;
    program.instructions = instructions;
    program.instruction_count = 5;
    program.instruction_alloc = 5;

    StringBuilder output;
    sb_init(&output);
    CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.buf != NULL);
    CHECK(strstr(output.buf, "int u_xlati3_y = 0;") == NULL);
    CHECK(strstr(output.buf, "skipped decomposed") == NULL);
    CHECK(strstr(output.buf, "// typed_write_0") != NULL);
    CHECK(strstr(output.buf, "// typed_write_1") != NULL);
    CHECK(strstr(output.buf, "// typed_write_2") != NULL);
    CHECK(strstr(output.buf, "// typed_swizzle_move") != NULL);
    sb_free(&output);

    /* Typed decomposition is retained only as an explicitly requested source
     * presentation.  It is not evidence that the inferred locals preserve
     * DXBC register lifetime, lane aliasing, or control-flow semantics. */
    const HLSLEmitOptions readable_options =
        HLSL_EMIT_READABLE_OPTIONS_INIT;
    sb_init(&output);
    CHECK(hlsl_emit_with_options(&program, &output, NULL, NULL, NULL,
                                 &readable_options));
    CHECK(strstr(output.buf, "int u_xlati3_y = 0;") != NULL);
    const char* declaration = strstr(output.buf, "u_xlati3_y = ");
    CHECK(declaration != NULL);
    const char* assignment = strstr(declaration + 1, "u_xlati3_y = ");
    CHECK(assignment != NULL);
    const char* assignment_end = strchr(assignment, ';');
    CHECK(assignment_end != NULL);
    const char* left_cast = strstr(assignment, "asint(r1.x)");
    const char* right_cast = strstr(assignment, "asint(r2.x)");
    CHECK(left_cast != NULL && left_cast < assignment_end);
    CHECK(right_cast != NULL && right_cast < assignment_end);
    sb_free(&output);
    return 0;
}

static int verify_typed_move_modifier_emission(void) {
    USILInstruction instructions[7];
    memset(instructions, 0, sizeof(instructions));

    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[0].destination_mask = 0x30;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 1);
    instructions[0].operands[1].has_neg = true;
    snprintf(instructions[0].original_asm,
             sizeof(instructions[0].original_asm), "float_negated_move");

    instructions[1].opcode = USIL_OP_MOV;
    instructions[1].operand_count = 2;
    instructions[1].saturate = true;
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[1].operands[0].destination_mask = 0x40;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP, 1);
    instructions[1].operands[1].swizzle_mode = 2;
    instructions[1].operands[1].swizzle[0] = 2;
    instructions[1].operands[1].has_neg = true;
    snprintf(instructions[1].original_asm,
             sizeof(instructions[1].original_asm), "saturated_negated_move");

    instructions[2].opcode = USIL_OP_MOV;
    instructions[2].operand_count = 2;
    init_register_operand(&instructions[2].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[2].operands[0].destination_mask = 0x80;
    init_register_operand(&instructions[2].operands[1], OPERAND_TYPE_TEMP, 1);
    instructions[2].operands[1].swizzle_mode = 2;
    instructions[2].operands[1].swizzle[0] = 3;
    instructions[2].operands[1].has_abs = true;
    snprintf(instructions[2].original_asm,
             sizeof(instructions[2].original_asm), "absolute_move");

    instructions[3].opcode = USIL_OP_MOVC;
    instructions[3].operand_count = 4;
    init_register_operand(&instructions[3].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[3].operands[0].destination_mask = 0x10;
    init_register_operand(&instructions[3].operands[1], OPERAND_TYPE_TEMP, 2);
    instructions[3].operands[1].swizzle_mode = 2;
    instructions[3].operands[1].swizzle[0] = 0;
    init_register_operand(&instructions[3].operands[2], OPERAND_TYPE_TEMP, 1);
    instructions[3].operands[2].swizzle_mode = 2;
    instructions[3].operands[2].swizzle[0] = 0;
    instructions[3].operands[2].has_neg = true;
    init_register_operand(&instructions[3].operands[3], OPERAND_TYPE_TEMP, 3);
    instructions[3].operands[3].swizzle_mode = 2;
    instructions[3].operands[3].swizzle[0] = 0;
    instructions[3].operands[3].has_abs = true;
    snprintf(instructions[3].original_asm,
             sizeof(instructions[3].original_asm), "modified_movc_values");

    instructions[4].opcode = USIL_OP_MOVC;
    instructions[4].operand_count = 4;
    init_register_operand(&instructions[4].operands[0], OPERAND_TYPE_TEMP, 0);
    instructions[4].operands[0].destination_mask = 0x20;
    init_register_operand(&instructions[4].operands[1], OPERAND_TYPE_TEMP, 2);
    instructions[4].operands[1].swizzle_mode = 2;
    instructions[4].operands[1].swizzle[0] = 1;
    instructions[4].operands[1].has_neg = true;
    init_register_operand(&instructions[4].operands[2], OPERAND_TYPE_TEMP, 1);
    instructions[4].operands[2].swizzle_mode = 2;
    instructions[4].operands[2].swizzle[0] = 1;
    init_register_operand(&instructions[4].operands[3], OPERAND_TYPE_TEMP, 3);
    instructions[4].operands[3].swizzle_mode = 2;
    instructions[4].operands[3].swizzle[0] = 1;
    snprintf(instructions[4].original_asm,
             sizeof(instructions[4].original_asm), "modified_movc_condition");
    /* One actual integer instruction selects the exact emitter's raw uint
     * temporary ABI, matching mixed float/bitwise production shaders. */
    instructions[5].opcode = USIL_OP_AND;
    instructions[5].operand_count = 3;
    init_register_operand(&instructions[5].operands[0], OPERAND_TYPE_TEMP, 3);
    instructions[5].operands[0].destination_mask = 0x40;
    init_register_operand(&instructions[5].operands[1], OPERAND_TYPE_TEMP, 3);
    instructions[5].operands[1].swizzle_mode = 2;
    instructions[5].operands[1].swizzle[0] = 2;
    init_immediate_operand(&instructions[5].operands[2], UINT32_MAX);
    snprintf(instructions[5].original_asm,
             sizeof(instructions[5].original_asm), "force_raw_temp_storage");
    instructions[6].opcode = USIL_OP_RET;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.temp_count = 4;
    program.instructions = instructions;
    program.instruction_count = 7;
    program.instruction_alloc = 7;

    StringBuilder output;
    sb_init(&output);
    CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
    CHECK(output.buf != NULL);
    CHECK(strstr(output.buf, "asuint(-asfloat(r1.xy))") != NULL);
    CHECK(strstr(output.buf,
                 "asuint(saturate(-asfloat(r1.z)))") != NULL);
    CHECK(strstr(output.buf, "asuint(abs(asfloat(r1.w)))") != NULL);
    CHECK(strstr(output.buf,
                 "asuint(r2.x) ? -asfloat(r1.x) : abs(asfloat(r3.x))") !=
          NULL);
    CHECK(strstr(output.buf,
                 "asuint(-asfloat(r2.y)) ? r1.y : r3.y") != NULL);
    CHECK(strstr(output.buf, "saturate(-r1") == NULL);
    CHECK(strstr(output.buf, " ? -r1") == NULL);
    sb_free(&output);
    return 0;
}

static int verify_recompile_transform_safety_gate(void) {
    const HLSLEmitOptions readable_options =
        HLSL_EMIT_READABLE_OPTIONS_INIT;

    /* A repeated-lane permutation followed by a multiwrite satisfies the old
     * decomposition recognizer, yet aliasing two destinations to the same
     * source lane makes the inferred write redirects non-bijective. */
    {
        USILInstruction instructions[6];
        memset(instructions, 0, sizeof(instructions));
        const USILOpcode writers[3] = {USIL_OP_IADD, USIL_OP_ADD,
                                       USIL_OP_ADD};
        for (int lane = 0; lane < 3; ++lane) {
            instructions[lane].opcode = writers[lane];
            instructions[lane].operand_count = 3;
            init_register_operand(&instructions[lane].operands[0],
                                  OPERAND_TYPE_TEMP, 0);
            instructions[lane].operands[0].destination_mask = 16 << lane;
            init_register_operand(&instructions[lane].operands[1],
                                  OPERAND_TYPE_TEMP, 1);
            instructions[lane].operands[1].swizzle_mode = 2;
            instructions[lane].operands[1].swizzle[0] = (uint8_t)lane;
            init_register_operand(&instructions[lane].operands[2],
                                  OPERAND_TYPE_TEMP, 2);
            instructions[lane].operands[2].swizzle_mode = 2;
            instructions[lane].operands[2].swizzle[0] = (uint8_t)lane;
            snprintf(instructions[lane].original_asm,
                     sizeof(instructions[lane].original_asm),
                     "repeat_writer_%d", lane);
        }
        instructions[3].opcode = USIL_OP_MOV;
        instructions[3].operand_count = 2;
        init_register_operand(&instructions[3].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[3].operands[0].destination_mask = 0x70;
        init_register_operand(&instructions[3].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[3].operands[1].swizzle[0] = 1;
        instructions[3].operands[1].swizzle[1] = 1;
        instructions[3].operands[1].swizzle[2] = 0;
        snprintf(instructions[3].original_asm,
                 sizeof(instructions[3].original_asm),
                 "repeat_lane_swizzle");
        instructions[4].opcode = USIL_OP_ADD;
        instructions[4].operand_count = 3;
        init_register_operand(&instructions[4].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[4].operands[0].destination_mask = 0x30;
        init_register_operand(&instructions[4].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        init_register_operand(&instructions[4].operands[2],
                              OPERAND_TYPE_TEMP, 2);
        snprintf(instructions[4].original_asm,
                 sizeof(instructions[4].original_asm),
                 "decomposed_multiwrite");
        instructions[5].opcode = USIL_OP_RET;
        snprintf(instructions[5].original_asm,
                 sizeof(instructions[5].original_asm), "repeat_return");

        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 4;
        program.instructions = instructions;
        program.instruction_count = 6;
        program.instruction_alloc = 6;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf, "// repeat_lane_swizzle") != NULL);
        CHECK(strstr(exact.buf, "// decomposed_multiwrite") != NULL);
        CHECK(strstr(exact.buf, "skipped decomposed") == NULL);
        CHECK(strstr(exact.buf, "u_xlati3_") == NULL);
        sb_free(&exact);
    }

    /* MOVC sources may describe different lane mappings.  A single inferred
     * permutation cannot represent both arms, so exact mode keeps all three
     * decoded swizzles physical and local to the instruction. */
    {
        USILInstruction instructions[4];
        memset(instructions, 0, sizeof(instructions));
        instructions[0].opcode = USIL_OP_MOV;
        instructions[0].operand_count = 2;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0xf0;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[0].operands[1].swizzle[0] = 1;
        instructions[0].operands[1].swizzle[1] = 0;
        snprintf(instructions[0].original_asm,
                 sizeof(instructions[0].original_asm), "scramble_seed");

        instructions[1].opcode = USIL_OP_MOVC;
        instructions[1].operand_count = 4;
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 2);
        instructions[1].operands[0].destination_mask = 0xf0;
        init_register_operand(&instructions[1].operands[1],
                              OPERAND_TYPE_TEMP, 6);
        instructions[1].operands[1].swizzle_mode = 2;
        instructions[1].operands[1].swizzle[0] = 0;
        init_register_operand(&instructions[1].operands[2],
                              OPERAND_TYPE_TEMP, 0);
        init_register_operand(&instructions[1].operands[3],
                              OPERAND_TYPE_TEMP, 3);
        instructions[1].operands[3].swizzle[0] = 2;
        instructions[1].operands[3].swizzle[1] = 3;
        instructions[1].operands[3].swizzle[2] = 0;
        instructions[1].operands[3].swizzle[3] = 1;
        snprintf(instructions[1].original_asm,
                 sizeof(instructions[1].original_asm),
                 "divergent_movc_mapping");

        instructions[2].opcode = USIL_OP_ADD;
        instructions[2].operand_count = 3;
        init_register_operand(&instructions[2].operands[0],
                              OPERAND_TYPE_TEMP, 4);
        instructions[2].operands[0].destination_mask = 0xf0;
        init_register_operand(&instructions[2].operands[1],
                              OPERAND_TYPE_TEMP, 2);
        init_register_operand(&instructions[2].operands[2],
                              OPERAND_TYPE_TEMP, 5);
        snprintf(instructions[2].original_asm,
                 sizeof(instructions[2].original_asm),
                 "consume_divergent_movc");
        instructions[3].opcode = USIL_OP_RET;

        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 7;
        program.instructions = instructions;
        program.instruction_count = 4;
        program.instruction_alloc = 4;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf, "// scramble_seed") != NULL);
        CHECK(strstr(exact.buf, "// divergent_movc_mapping") != NULL);
        CHECK(strstr(exact.buf, "// consume_divergent_movc") != NULL);
        CHECK(strstr(exact.buf, "r1.yxzw") != NULL);
        CHECK(strstr(exact.buf, "r3.zwxy") != NULL);
        sb_free(&exact);
    }

    /* The cross recognizer historically ignored destination widths and most
     * operand flags.  This scalar/two-lane near-match must remain two decoded
     * instructions in exact mode. */
    {
        USILInstruction instructions[3];
        memset(instructions, 0, sizeof(instructions));
        instructions[0].opcode = USIL_OP_MUL;
        instructions[0].operand_count = 3;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[0].operands[1].swizzle[0] = 2;
        instructions[0].operands[1].swizzle[1] = 0;
        instructions[0].operands[1].swizzle[2] = 1;
        init_register_operand(&instructions[0].operands[2],
                              OPERAND_TYPE_TEMP, 2);
        snprintf(instructions[0].original_asm,
                 sizeof(instructions[0].original_asm), "near_cross_mul");
        instructions[1].opcode = USIL_OP_MAD;
        instructions[1].operand_count = 4;
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[1].operands[0].destination_mask = 0x30;
        init_register_operand(&instructions[1].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[1].operands[1].swizzle[0] = 1;
        instructions[1].operands[1].swizzle[1] = 2;
        instructions[1].operands[1].swizzle[2] = 0;
        init_register_operand(&instructions[1].operands[2],
                              OPERAND_TYPE_TEMP, 2);
        instructions[1].operands[2].swizzle[0] = 1;
        instructions[1].operands[2].swizzle[1] = 2;
        instructions[1].operands[2].swizzle[2] = 0;
        init_register_operand(&instructions[1].operands[3],
                              OPERAND_TYPE_TEMP, 0);
        instructions[1].operands[3].swizzle_mode = 2;
        instructions[1].operands[3].swizzle[0] = 0;
        instructions[1].operands[3].has_neg = true;
        snprintf(instructions[1].original_asm,
                 sizeof(instructions[1].original_asm), "near_cross_mad");
        instructions[2].opcode = USIL_OP_RET;

        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 4;
        program.instructions = instructions;
        program.instruction_count = 3;
        program.instruction_alloc = 3;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf, "// near_cross_mul") != NULL);
        CHECK(strstr(exact.buf, "// near_cross_mad") != NULL);
        CHECK(strstr(exact.buf, "merged into cross") == NULL);
        CHECK(strstr(exact.buf, "cross(") == NULL);
        sb_free(&exact);

        StringBuilder readable;
        sb_init(&readable);
        CHECK(hlsl_emit_with_options(&program, &readable, NULL, NULL, NULL,
                                     &readable_options));
        CHECK(strstr(readable.buf, "merged into cross") != NULL);
        CHECK(strstr(readable.buf, "cross(") != NULL);
        sb_free(&readable);
    }

    /* An increment under a conditional cannot be moved into a for-loop
     * header.  Exact mode reconstructs the proven loop-prefix condition but
     * emits the conditional increment at its decoded program point. */
    {
        USILInstruction instructions[8];
        memset(instructions, 0, sizeof(instructions));
        instructions[0].opcode = USIL_OP_LOOP;
        snprintf(instructions[0].original_asm,
                 sizeof(instructions[0].original_asm), "conditional_loop");
        instructions[1].opcode = USIL_OP_IGE;
        instructions[1].operand_count = 3;
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[1].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[1].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[1].operands[1].swizzle_mode = 2;
        instructions[1].operands[1].swizzle[0] = 0;
        init_register_operand(&instructions[1].operands[2],
                              OPERAND_TYPE_TEMP, 2);
        instructions[1].operands[2].swizzle_mode = 2;
        instructions[1].operands[2].swizzle[0] = 0;
        snprintf(instructions[1].original_asm,
                 sizeof(instructions[1].original_asm), "loop_compare");
        instructions[2].opcode = USIL_OP_BREAKC;
        instructions[2].operand_count = 1;
        instructions[2].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
        init_register_operand(&instructions[2].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[2].operands[0].swizzle_mode = 2;
        instructions[2].operands[0].swizzle[0] = 0;
        snprintf(instructions[2].original_asm,
                 sizeof(instructions[2].original_asm), "loop_break");
        instructions[3].opcode = USIL_OP_IF;
        instructions[3].operand_count = 1;
        instructions[3].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
        init_register_operand(&instructions[3].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[3].operands[0].swizzle_mode = 2;
        instructions[3].operands[0].swizzle[0] = 0;
        snprintf(instructions[3].original_asm,
                 sizeof(instructions[3].original_asm), "increment_guard");
        instructions[4].opcode = USIL_OP_IADD;
        instructions[4].operand_count = 3;
        init_register_operand(&instructions[4].operands[0],
                              OPERAND_TYPE_TEMP, 1);
        instructions[4].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[4].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[4].operands[1].swizzle_mode = 2;
        instructions[4].operands[1].swizzle[0] = 0;
        init_immediate_operand(&instructions[4].operands[2], 1u);
        snprintf(instructions[4].original_asm,
                 sizeof(instructions[4].original_asm),
                 "conditional_increment");
        instructions[5].opcode = USIL_OP_ENDIF;
        instructions[6].opcode = USIL_OP_ENDLOOP;
        instructions[7].opcode = USIL_OP_RET;

        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 4;
        program.instructions = instructions;
        program.instruction_count = 8;
        program.instruction_alloc = 8;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf, "[loop] while (") != NULL);
        CHECK(strstr(exact.buf, "[loop] while (true)") == NULL);
        CHECK(strstr(exact.buf, "[loop] for (") == NULL);
        CHECK(strstr(exact.buf, "// loop_compare") != NULL);
        CHECK(strstr(exact.buf, "// loop_break") != NULL);
        CHECK(strstr(exact.buf, "// conditional_increment") != NULL);
        sb_free(&exact);

        StringBuilder readable;
        sb_init(&readable);
        CHECK(hlsl_emit_with_options(&program, &readable, NULL, NULL, NULL,
                                     &readable_options));
        CHECK(strstr(readable.buf, "[loop] for (") != NULL);
        sb_free(&readable);
    }

    /* A loop-prefix comparison that remains live in the body is assigned in
     * the source while-condition.  The compiler inverse therefore retains
     * the decoded comparison value as well as the structured BREAKC. */
    {
        USILInstruction instructions[6];
        memset(instructions, 0, sizeof(instructions));
        instructions[0].opcode = USIL_OP_LOOP;
        instructions[1].opcode = USIL_OP_ILT;
        instructions[1].operand_count = 3;
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[1].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[1].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[1].operands[1].swizzle_mode = 2;
        instructions[1].operands[1].swizzle[0] = 0;
        init_register_operand(&instructions[1].operands[2],
                              OPERAND_TYPE_TEMP, 2);
        instructions[1].operands[2].swizzle_mode = 2;
        instructions[1].operands[2].swizzle[0] = 0;
        snprintf(instructions[1].original_asm,
                 sizeof(instructions[1].original_asm),
                 "live_loop_compare");
        instructions[2].opcode = USIL_OP_BREAKC;
        instructions[2].operand_count = 1;
        instructions[2].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
        init_register_operand(&instructions[2].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[2].operands[0].swizzle_mode = 2;
        instructions[2].operands[0].swizzle[0] = 0;
        snprintf(instructions[2].original_asm,
                 sizeof(instructions[2].original_asm),
                 "live_loop_break");
        instructions[3].opcode = USIL_OP_MOV;
        instructions[3].operand_count = 2;
        init_register_operand(&instructions[3].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[3].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[3].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[3].operands[1].swizzle_mode = 2;
        instructions[3].operands[1].swizzle[0] = 0;
        snprintf(instructions[3].original_asm,
                 sizeof(instructions[3].original_asm),
                 "consume_loop_compare");
        instructions[4].opcode = USIL_OP_ENDLOOP;
        instructions[5].opcode = USIL_OP_RET;

        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 4;
        program.instructions = instructions;
        program.instruction_count = 6;
        program.instruction_alloc = 6;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf, "[loop] while (true)") == NULL);
        CHECK(strstr(exact.buf,
                     "asuint(r0.x = asfloat((asint(r1.x) < "
                     "asint(r2.x)) ? 0xFFFFFFFFu : 0u))") != NULL);
        CHECK(strstr(exact.buf, "// live_loop_compare") != NULL);
        CHECK(strstr(exact.buf, "// live_loop_break") != NULL);
        CHECK(strstr(exact.buf, "// consume_loop_compare") != NULL);
        CHECK(strstr(exact.buf, "if (asuint(r0.x)) break;") == NULL);
        sb_free(&exact);

        instructions[2].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
        StringBuilder zero_test;
        sb_init(&zero_test);
        CHECK(hlsl_emit(&program, &zero_test, NULL, NULL, NULL));
        CHECK(strstr(zero_test.buf, "[loop] while (asuint(r0.x =") != NULL);
        CHECK(strstr(zero_test.buf, "[loop] while (!asuint(r0.x =") == NULL);
        sb_free(&zero_test);

        /* Raw temporary storage uses the same assignment-preserving inverse
         * without an asfloat/asuint round trip. */
        instructions[2].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
        instructions[3].opcode = USIL_OP_AND;
        instructions[3].operand_count = 3;
        init_immediate_operand(&instructions[3].operands[2], 1u);
        StringBuilder raw_storage;
        sb_init(&raw_storage);
        CHECK(hlsl_emit(&program, &raw_storage, NULL, NULL, NULL));
        CHECK(strstr(raw_storage.buf,
                     "[loop] while (!(r0.x = ((asint(r1.x) < ") != NULL);
        CHECK(strstr(raw_storage.buf, "asuint(r0.x = asfloat(") == NULL);
        sb_free(&raw_storage);
    }

    /* Unity's FXC-to-HLSLcc Metal path can mis-simulate this exact decoded
     * integer loop bound when it remains encoded in float temporary bits.
     * The recompile inverse keeps the physical writes but carries the proven
     * FTOI/IMAX/IMIN value through one typed alias.  It is admitted only when
     * the comparison result is overwritten on loop entry and dead on exit. */
    {
        USILInstruction instructions[16];
        memset(instructions, 0, sizeof(instructions));

        instructions[0].opcode = USIL_OP_FTOI;
        instructions[0].operand_count = 2;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x80;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[1].swizzle_mode = 2;
        instructions[0].operands[1].swizzle[0] = 3;
        snprintf(instructions[0].original_asm,
                 sizeof(instructions[0].original_asm), "typed_bound_ftoi");

        instructions[1].opcode = USIL_OP_IMAX;
        instructions[1].operand_count = 3;
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[1].operands[0].destination_mask = 0x80;
        init_register_operand(&instructions[1].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[1].operands[1].swizzle_mode = 2;
        instructions[1].operands[1].swizzle[0] = 3;
        init_immediate_operand(&instructions[1].operands[2], 3u);
        snprintf(instructions[1].original_asm,
                 sizeof(instructions[1].original_asm), "typed_bound_imax");

        instructions[2].opcode = USIL_OP_IMIN;
        instructions[2].operand_count = 3;
        init_register_operand(&instructions[2].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[2].operands[0].destination_mask = 0x80;
        init_register_operand(&instructions[2].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[2].operands[1].swizzle_mode = 2;
        instructions[2].operands[1].swizzle[0] = 3;
        init_immediate_operand(&instructions[2].operands[2], 16u);
        snprintf(instructions[2].original_asm,
                 sizeof(instructions[2].original_asm), "typed_bound_imin");

        instructions[3].opcode = USIL_OP_ITOF;
        instructions[3].operand_count = 2;
        init_register_operand(&instructions[3].operands[0],
                              OPERAND_TYPE_TEMP, 1);
        instructions[3].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[3].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[3].operands[1].swizzle_mode = 2;
        instructions[3].operands[1].swizzle[0] = 3;
        snprintf(instructions[3].original_asm,
                 sizeof(instructions[3].original_asm), "typed_bound_itof");

        instructions[4].opcode = USIL_OP_DIV;
        instructions[4].operand_count = 3;
        init_register_operand(&instructions[4].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[4].operands[0].destination_mask = 0x10;
        init_register_operand(&instructions[4].operands[1],
                              OPERAND_TYPE_TEMP, 4);
        instructions[4].operands[1].swizzle_mode = 2;
        instructions[4].operands[1].swizzle[0] = 0;
        init_register_operand(&instructions[4].operands[2],
                              OPERAND_TYPE_TEMP, 1);
        instructions[4].operands[2].swizzle_mode = 2;
        instructions[4].operands[2].swizzle[0] = 0;
        snprintf(instructions[4].original_asm,
                 sizeof(instructions[4].original_asm), "typed_bound_div");

        instructions[5].opcode = USIL_OP_MOV;
        instructions[5].operand_count = 2;
        init_register_operand(&instructions[5].operands[0],
                              OPERAND_TYPE_TEMP, 1);
        instructions[5].operands[0].destination_mask = 0x80;
        init_immediate_operand(&instructions[5].operands[1], 0u);
        snprintf(instructions[5].original_asm,
                 sizeof(instructions[5].original_asm), "counter_zero");

        instructions[6].opcode = USIL_OP_LOOP;
        snprintf(instructions[6].original_asm,
                 sizeof(instructions[6].original_asm), "typed_bound_loop");

        instructions[7].opcode = USIL_OP_IGE;
        instructions[7].operand_count = 3;
        init_register_operand(&instructions[7].operands[0],
                              OPERAND_TYPE_TEMP, 2);
        instructions[7].operands[0].destination_mask = 0x40;
        init_register_operand(&instructions[7].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[7].operands[1].swizzle_mode = 2;
        instructions[7].operands[1].swizzle[0] = 3;
        init_register_operand(&instructions[7].operands[2],
                              OPERAND_TYPE_TEMP, 0);
        instructions[7].operands[2].swizzle_mode = 2;
        instructions[7].operands[2].swizzle[0] = 3;
        snprintf(instructions[7].original_asm,
                 sizeof(instructions[7].original_asm),
                 "typed_bound_compare");

        instructions[8].opcode = USIL_OP_BREAKC;
        instructions[8].operand_count = 1;
        instructions[8].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
        init_register_operand(&instructions[8].operands[0],
                              OPERAND_TYPE_TEMP, 2);
        instructions[8].operands[0].swizzle_mode = 2;
        instructions[8].operands[0].swizzle[0] = 2;
        snprintf(instructions[8].original_asm,
                 sizeof(instructions[8].original_asm),
                 "typed_bound_break");

        instructions[9].opcode = USIL_OP_ITOF;
        instructions[9].operand_count = 2;
        init_register_operand(&instructions[9].operands[0],
                              OPERAND_TYPE_TEMP, 2);
        instructions[9].operands[0].destination_mask = 0x40;
        init_register_operand(&instructions[9].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[9].operands[1].swizzle_mode = 2;
        instructions[9].operands[1].swizzle[0] = 3;
        snprintf(instructions[9].original_asm,
                 sizeof(instructions[9].original_asm),
                 "overwrite_loop_predicate");

        instructions[10].opcode = USIL_OP_DIV;
        instructions[10].operand_count = 3;
        init_register_operand(&instructions[10].operands[0],
                              OPERAND_TYPE_TEMP, 3);
        instructions[10].operands[0].destination_mask = 0x20;
        init_register_operand(&instructions[10].operands[1],
                              OPERAND_TYPE_TEMP, 2);
        instructions[10].operands[1].swizzle_mode = 2;
        instructions[10].operands[1].swizzle[0] = 2;
        init_register_operand(&instructions[10].operands[2],
                              OPERAND_TYPE_TEMP, 1);
        instructions[10].operands[2].swizzle_mode = 2;
        instructions[10].operands[2].swizzle[0] = 0;
        snprintf(instructions[10].original_asm,
                 sizeof(instructions[10].original_asm), "body_div");

        instructions[11].opcode = USIL_OP_IADD;
        instructions[11].operand_count = 3;
        init_register_operand(&instructions[11].operands[0],
                              OPERAND_TYPE_TEMP, 1);
        instructions[11].operands[0].destination_mask = 0x80;
        init_register_operand(&instructions[11].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[11].operands[1].swizzle_mode = 2;
        instructions[11].operands[1].swizzle[0] = 3;
        init_immediate_operand(&instructions[11].operands[2], 1u);
        snprintf(instructions[11].original_asm,
                 sizeof(instructions[11].original_asm), "counter_increment");
        instructions[12].opcode = USIL_OP_ENDLOOP;
        instructions[13].opcode = USIL_OP_RET;
        instructions[14].opcode = USIL_OP_RET;
        instructions[15].opcode = USIL_OP_RET;

        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 5;
        program.instructions = instructions;
        program.instruction_count = 14;
        program.instruction_alloc = 16;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf,
                     "int dxbc_loop_bound_0 = (int)(r0.w);") != NULL);
        CHECK(strstr(exact.buf, "#ifdef dxbc_loop_bound_0") != NULL);
        CHECK(strstr(
                  exact.buf,
                  "#error DXBCSandbox_typed_loop_witness_macro_collision_"
                  "dxbc_loop_bound_0") != NULL);
        CHECK(strstr(exact.buf,
                     "dxbc_loop_bound_0 = max(dxbc_loop_bound_0, 3);") !=
              NULL);
        CHECK(strstr(exact.buf,
                     "dxbc_loop_bound_0 = min(dxbc_loop_bound_0, 16);") !=
              NULL);
        CHECK(strstr(exact.buf,
                     "r1.x = (float)(dxbc_loop_bound_0);") != NULL);
        CHECK(strstr(exact.buf,
                     "[loop] while (asint(r1.w) < dxbc_loop_bound_0) {") !=
              NULL);
        CHECK(strstr(exact.buf, "asuint(r2.z = asfloat") == NULL);
        CHECK(strstr(exact.buf, "// typed_bound_compare") != NULL);
        CHECK(strstr(exact.buf, "// typed_bound_break") != NULL);
        CHECK(strstr(exact.buf, "r1.w = asfloat(asint(r1.w) + 1);") != NULL);
        sb_free(&exact);

        /* The wrapper's complete keyword universe participates in local
         * reservation. A collision deterministically selects a suffix; an
         * unrelated keyword leaves the canonical spelling unchanged. */
        const char *reserved_collision[] = {"dxbc_loop_bound_0"};
        HLSLEmitOptions reserved_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
        reserved_options.reserved_preprocessor_identifiers =
            reserved_collision;
        reserved_options.reserved_preprocessor_identifier_count = 1;
        StringBuilder reserved_name;
        sb_init(&reserved_name);
        CHECK(hlsl_emit_with_options(&program, &reserved_name, NULL, NULL,
                                     NULL, &reserved_options));
        CHECK(strstr(reserved_name.buf,
                     "int dxbc_loop_bound_0_dxbc_1 = (int)(r0.w);") !=
              NULL);
        CHECK(strstr(reserved_name.buf,
                     "#ifdef dxbc_loop_bound_0_dxbc_1") != NULL);
        CHECK(strstr(reserved_name.buf,
                     "[loop] while (asint(r1.w) < "
                     "dxbc_loop_bound_0_dxbc_1) {") != NULL);
        sb_free(&reserved_name);

        const char *unrelated_reserved[] = {"UNRELATED_VARIANT_KEYWORD"};
        reserved_options.reserved_preprocessor_identifiers =
            unrelated_reserved;
        StringBuilder unrelated_name;
        sb_init(&unrelated_name);
        CHECK(hlsl_emit_with_options(&program, &unrelated_name, NULL, NULL,
                                     NULL, &reserved_options));
        CHECK(strstr(unrelated_name.buf,
                     "int dxbc_loop_bound_0 = (int)(r0.w);") != NULL);
        sb_free(&unrelated_name);

        /* A serialized global with the canonical witness spelling remains a
         * distinct runtime parameter. Reserve a suffixed local and prove a
         * later cbuffer read still names the serialized parameter. */
        USILConstantBuffer collision_cbuffer = {0, 1, false};
        SerializedVariable collision_variable;
        memset(&collision_variable, 0, sizeof(collision_variable));
        collision_variable.name = "dxbc_loop_bound_0";
        collision_variable.layout[0] = 0;
        collision_variable.layout[3] = 1;
        SerializedConstantBuffer collision_globals;
        memset(&collision_globals, 0, sizeof(collision_globals));
        collision_globals.name = "$Globals";
        collision_globals.size = 16;
        collision_globals.variables = &collision_variable;
        collision_globals.var_count = 1;
        SerializedResourceParam collision_binding;
        memset(&collision_binding, 0, sizeof(collision_binding));
        collision_binding.name = "$Globals";
        collision_binding.bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER;
        collision_binding.bind_index = 0;
        SerializedProgramParameters collision_parameters;
        memset(&collision_parameters, 0, sizeof(collision_parameters));
        collision_parameters.constant_buffers = &collision_globals;
        collision_parameters.cb_count = 1;
        collision_parameters.resources = &collision_binding;
        collision_parameters.res_count = 1;
        instructions[13].opcode = USIL_OP_MOV;
        instructions[13].operand_count = 2;
        init_register_operand(&instructions[13].operands[0],
                              OPERAND_TYPE_TEMP, 4);
        instructions[13].operands[0].destination_mask = 0x10;
        init_cbuffer_operand(&instructions[13].operands[1], 0, 0);
        instructions[13].operands[1].swizzle_mode = 2;
        instructions[13].operands[1].swizzle[0] = 0;
        instructions[14].opcode = USIL_OP_RET;
        program.cbuffers = &collision_cbuffer;
        program.cbuffer_count = 1;
        program.cbuffer_alloc = 1;
        program.instruction_count = 15;
        StringBuilder metadata_collision;
        sb_init(&metadata_collision);
        CHECK(hlsl_emit(&program, &metadata_collision,
                        &collision_parameters, NULL, NULL));
        CHECK(strstr(metadata_collision.buf,
                     "float dxbc_loop_bound_0") != NULL);
        CHECK(strstr(metadata_collision.buf,
                     "int dxbc_loop_bound_0_dxbc_1 = (int)(r0.w);") !=
              NULL);
        CHECK(strstr(metadata_collision.buf,
                     "r4.x = dxbc_loop_bound_0;") != NULL);
        CHECK(strstr(metadata_collision.buf,
                     "int dxbc_loop_bound_0 =") == NULL);
        sb_free(&metadata_collision);
        program.cbuffers = NULL;
        program.cbuffer_count = 0;
        program.cbuffer_alloc = 0;
        memset(&instructions[13], 0, sizeof(instructions[13]));
        instructions[13].opcode = USIL_OP_RET;
        memset(&instructions[14], 0, sizeof(instructions[14]));
        instructions[14].opcode = USIL_OP_RET;
        program.instruction_count = 14;

        StringBuilder readable;
        sb_init(&readable);
        CHECK(hlsl_emit_with_options(&program, &readable, NULL, NULL, NULL,
                                     &readable_options));
        CHECK(strstr(readable.buf, "dxbc_loop_bound_") == NULL);
        sb_free(&readable);

        /* An unrelated post-loop branch is safe when the predicate lane is
         * never read on any subsequent path. */
        instructions[13].opcode = USIL_OP_IF;
        instructions[13].operand_count = 1;
        instructions[13].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
        init_register_operand(&instructions[13].operands[0],
                              OPERAND_TYPE_TEMP, 4);
        instructions[13].operands[0].swizzle_mode = 2;
        instructions[13].operands[0].swizzle[0] = 0;
        instructions[14].opcode = USIL_OP_ENDIF;
        program.instruction_count = 16;
        StringBuilder never_read_after_branch;
        sb_init(&never_read_after_branch);
        CHECK(hlsl_emit(&program, &never_read_after_branch,
                        NULL, NULL, NULL));
        CHECK(strstr(never_read_after_branch.buf,
                     "dxbc_loop_bound_0") != NULL);
        sb_free(&never_read_after_branch);

        /* Control-flow predicates are operand-zero sources even in mask
         * selection mode. Reading r2.z after ENDLOOP makes the comparison
         * assignment observable and must reject the inverse. */
        init_register_operand(&instructions[13].operands[0],
                              OPERAND_TYPE_TEMP, 2);
        instructions[13].operands[0].swizzle_mode = 0;
        instructions[13].operands[0].destination_mask = 0x40;
        StringBuilder masked_control_read;
        sb_init(&masked_control_read);
        CHECK(hlsl_emit(&program, &masked_control_read, NULL, NULL, NULL));
        CHECK(strstr(masked_control_read.buf, "dxbc_loop_bound_") == NULL);
        CHECK(strstr(masked_control_read.buf,
                     "asuint(r2.z = asfloat") != NULL);
        sb_free(&masked_control_read);

        memset(&instructions[13], 0, sizeof(instructions[13]));
        instructions[13].opcode = USIL_OP_RET;
        memset(&instructions[14], 0, sizeof(instructions[14]));
        instructions[14].opcode = USIL_OP_RET;
        program.instruction_count = 14;

        /* A comparison lane not overwritten by the first body instruction
         * keeps the generic assignment-preserving exact loop. */
        instructions[9].operands[0].destination_mask = 0x20;
        StringBuilder live_on_entry;
        sb_init(&live_on_entry);
        CHECK(hlsl_emit(&program, &live_on_entry, NULL, NULL, NULL));
        CHECK(strstr(live_on_entry.buf, "dxbc_loop_bound_") == NULL);
        CHECK(strstr(live_on_entry.buf, "asuint(r2.z = asfloat") != NULL);
        sb_free(&live_on_entry);
        instructions[9].operands[0].destination_mask = 0x40;

        /* Rewriting the physical bound in the body invalidates the alias. */
        instructions[10].operands[0].register_index = 0;
        instructions[10].operands[0].index_values[0] = 0u;
        instructions[10].operands[0].destination_mask = 0x80;
        StringBuilder rewritten_bound;
        sb_init(&rewritten_bound);
        CHECK(hlsl_emit(&program, &rewritten_bound, NULL, NULL, NULL));
        CHECK(strstr(rewritten_bound.buf, "dxbc_loop_bound_") == NULL);
        CHECK(strstr(rewritten_bound.buf, "asuint(r2.z = asfloat") != NULL);
        sb_free(&rewritten_bound);
        instructions[10].operands[0].register_index = 3;
        instructions[10].operands[0].index_values[0] = 3u;
        instructions[10].operands[0].destination_mask = 0x20;

        /* IMUL and UDIV have two architectural destinations. A bound write
         * through operand one invalidates the alias both between the clamp
         * and LOOP and inside the loop body. */
        const int multi_dest_sites[] = {4, 10};
        const USILOpcode multi_dest_opcodes[] = {
            USIL_OP_IMUL, USIL_OP_UDIV};
        for (size_t site_index = 0;
             site_index < sizeof(multi_dest_sites) /
                              sizeof(multi_dest_sites[0]);
             ++site_index) {
            const int site = multi_dest_sites[site_index];
            const USILInstruction saved = instructions[site];
            for (size_t opcode_index = 0;
                 opcode_index < sizeof(multi_dest_opcodes) /
                                    sizeof(multi_dest_opcodes[0]);
                 ++opcode_index) {
                memset(&instructions[site], 0, sizeof(instructions[site]));
                instructions[site].opcode = multi_dest_opcodes[opcode_index];
                instructions[site].operand_count = 4;
                instructions[site].operands[0].type = OPERAND_TYPE_NULL;
                init_register_operand(&instructions[site].operands[1],
                                      OPERAND_TYPE_TEMP, 0);
                instructions[site].operands[1].destination_mask = 0x80;
                init_register_operand(&instructions[site].operands[2],
                                      OPERAND_TYPE_TEMP, 4);
                instructions[site].operands[2].swizzle_mode = 2;
                instructions[site].operands[2].swizzle[0] = 3;
                init_immediate_operand(&instructions[site].operands[3], 1u);
                snprintf(instructions[site].original_asm,
                         sizeof(instructions[site].original_asm),
                         "operand1_bound_write_%d_%d", site,
                         (int)multi_dest_opcodes[opcode_index]);
                StringBuilder operand1_bound_write;
                sb_init(&operand1_bound_write);
                CHECK(hlsl_emit(&program, &operand1_bound_write,
                                NULL, NULL, NULL));
                CHECK(strstr(operand1_bound_write.buf,
                             "dxbc_loop_bound_") == NULL);
                CHECK(strstr(operand1_bound_write.buf,
                             "asuint(r2.z = asfloat") != NULL);
                sb_free(&operand1_bound_write);
            }
            instructions[site] = saved;
        }

        /* Reading the comparison result after ENDLOOP likewise keeps the
         * assignment in the exact source condition. */
        instructions[13].opcode = USIL_OP_MOV;
        instructions[13].operand_count = 2;
        init_register_operand(&instructions[13].operands[0],
                              OPERAND_TYPE_TEMP, 4);
        instructions[13].operands[0].destination_mask = 0x40;
        init_register_operand(&instructions[13].operands[1],
                              OPERAND_TYPE_TEMP, 2);
        instructions[13].operands[1].swizzle_mode = 2;
        instructions[13].operands[1].swizzle[0] = 2;
        program.instruction_count = 15;
        StringBuilder live_on_exit;
        sb_init(&live_on_exit);
        CHECK(hlsl_emit(&program, &live_on_exit, NULL, NULL, NULL));
        CHECK(strstr(live_on_exit.buf, "dxbc_loop_bound_") == NULL);
        CHECK(strstr(live_on_exit.buf, "asuint(r2.z = asfloat") != NULL);
        sb_free(&live_on_exit);

        /* The non-zero lower clamp is a proof obligation, not a heuristic. */
        instructions[13].opcode = USIL_OP_RET;
        instructions[13].operand_count = 0;
        program.instruction_count = 14;
        instructions[1].operands[2].imm_values[0] = 0u;
        instructions[1].operands[2].immediate_words[0] = 0u;
        StringBuilder zero_lower_bound;
        sb_init(&zero_lower_bound);
        CHECK(hlsl_emit(&program, &zero_lower_bound, NULL, NULL, NULL));
        CHECK(strstr(zero_lower_bound.buf, "dxbc_loop_bound_") == NULL);
        CHECK(strstr(zero_lower_bound.buf, "asuint(r2.z = asfloat") != NULL);
        sb_free(&zero_lower_bound);
    }

    /* The modulo recognizer accepted floating MAX and multi-lane writes even
     * though its replacement emitted one scalar signed remainder. */
    {
        USILInstruction instructions[6];
        memset(instructions, 0, sizeof(instructions));
        instructions[0].opcode = USIL_OP_AND;
        instructions[0].operand_count = 3;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x30;
        init_register_operand(&instructions[0].operands[1],
                              OPERAND_TYPE_TEMP, 1);
        instructions[0].operands[1].swizzle_mode = 2;
        instructions[0].operands[1].swizzle[0] = 0;
        init_immediate_operand(&instructions[0].operands[2], 0x80000000u);
        snprintf(instructions[0].original_asm,
                 sizeof(instructions[0].original_asm), "mod_sign_bits");
        instructions[1].opcode = USIL_OP_MAX;
        instructions[1].operand_count = 3;
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[1].operands[0].destination_mask = 0x60;
        instructions[1].operands[1] = instructions[0].operands[1];
        instructions[1].operands[2] = instructions[0].operands[1];
        instructions[1].operands[2].has_neg = true;
        snprintf(instructions[1].original_asm,
                 sizeof(instructions[1].original_asm), "mod_float_abs");
        instructions[2].opcode = USIL_OP_AND;
        instructions[2].operand_count = 3;
        init_register_operand(&instructions[2].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[2].operands[0].destination_mask = 0x60;
        init_register_operand(&instructions[2].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[2].operands[1].swizzle_mode = 2;
        instructions[2].operands[1].swizzle[0] = 1;
        init_immediate_operand(&instructions[2].operands[2], 3u);
        snprintf(instructions[2].original_asm,
                 sizeof(instructions[2].original_asm), "mod_mask");
        instructions[3].opcode = USIL_OP_INEG;
        instructions[3].operand_count = 2;
        init_register_operand(&instructions[3].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[3].operands[0].destination_mask = 0xc0;
        init_register_operand(&instructions[3].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[3].operands[1].swizzle_mode = 2;
        instructions[3].operands[1].swizzle[0] = 1;
        snprintf(instructions[3].original_asm,
                 sizeof(instructions[3].original_asm), "mod_negate");
        instructions[4].opcode = USIL_OP_MOVC;
        instructions[4].operand_count = 4;
        init_register_operand(&instructions[4].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[4].operands[0].destination_mask = 0x30;
        init_register_operand(&instructions[4].operands[1],
                              OPERAND_TYPE_TEMP, 0);
        instructions[4].operands[1].swizzle_mode = 2;
        instructions[4].operands[1].swizzle[0] = 0;
        init_register_operand(&instructions[4].operands[2],
                              OPERAND_TYPE_TEMP, 0);
        instructions[4].operands[2].swizzle_mode = 2;
        instructions[4].operands[2].swizzle[0] = 2;
        init_register_operand(&instructions[4].operands[3],
                              OPERAND_TYPE_TEMP, 0);
        instructions[4].operands[3].swizzle_mode = 2;
        instructions[4].operands[3].swizzle[0] = 1;
        snprintf(instructions[4].original_asm,
                 sizeof(instructions[4].original_asm), "mod_select");
        instructions[5].opcode = USIL_OP_RET;

        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 2;
        program.instructions = instructions;
        program.instruction_count = 6;
        program.instruction_alloc = 6;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf, " % 4") == NULL);
        CHECK(strstr(exact.buf, "// mod_float_abs") != NULL);
        CHECK(strstr(exact.buf, "// mod_mask") != NULL);
        CHECK(strstr(exact.buf, "// mod_negate") != NULL);
        CHECK(strstr(exact.buf, "// mod_select") != NULL);
        sb_free(&exact);

        StringBuilder readable;
        sb_init(&readable);
        CHECK(hlsl_emit_with_options(&program, &readable, NULL, NULL, NULL,
                                     &readable_options));
        CHECK(strstr(readable.buf, " % 4") != NULL);
        sb_free(&readable);
    }

    /* Exact mode may name a reused product only for the compiler-model form
     * proven by the SeparableBlur recurrence: fixed cbuffer operands, reverse
     * product order in at least two straight-line MAD updates, identical
     * widths, and an accumulator whose reaching definition is the updated
     * destination itself. */
    {
        USILInstruction instructions[4];
        memset(instructions, 0, sizeof(instructions));
        instructions[0].opcode = USIL_OP_MUL;
        instructions[0].operand_count = 3;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[0].operands[0].destination_mask = 0x30;
        init_cbuffer_operand(&instructions[0].operands[1], 0, 2);
        instructions[0].operands[1].swizzle[0] = 0;
        instructions[0].operands[1].swizzle[1] = 1;
        instructions[0].operands[1].swizzle[2] = 0;
        instructions[0].operands[1].swizzle[3] = 0;
        init_cbuffer_operand(&instructions[0].operands[2], 0, 3);
        instructions[0].operands[2].swizzle[0] = 0;
        instructions[0].operands[2].swizzle[1] = 1;
        instructions[0].operands[2].swizzle[2] = 0;
        instructions[0].operands[2].swizzle[3] = 0;
        snprintf(instructions[0].original_asm,
                 sizeof(instructions[0].original_asm),
                 "saved_product_definition");
        for (int index = 1; index <= 2; index++) {
            instructions[index].opcode = USIL_OP_MAD;
            instructions[index].operand_count = 4;
            init_register_operand(&instructions[index].operands[0],
                                  OPERAND_TYPE_TEMP, 0);
            instructions[index].operands[0].destination_mask = 0x30;
            instructions[index].operands[1] =
                instructions[0].operands[2];
            instructions[index].operands[2] =
                instructions[0].operands[1];
            init_register_operand(&instructions[index].operands[3],
                                  OPERAND_TYPE_TEMP, 0);
            instructions[index].operands[3].swizzle[0] = 0;
            instructions[index].operands[3].swizzle[1] = 1;
            instructions[index].operands[3].swizzle[2] = 0;
            instructions[index].operands[3].swizzle[3] = 0;
            snprintf(instructions[index].original_asm,
                     sizeof(instructions[index].original_asm),
                     "saved_product_recurrence_%d", index);
        }
        instructions[3].opcode = USIL_OP_RET;

        USILConstantBuffer cbuffer = {0, 4, false};
        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 1;
        program.cbuffers = &cbuffer;
        program.cbuffer_count = 1;
        program.cbuffer_alloc = 1;
        program.instructions = instructions;
        program.instruction_count = 4;
        program.instruction_alloc = 4;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf,
                     "float2 dxbc_saved_mul0 = (float2)0;") != NULL);
        CHECK(strstr(exact.buf,
                     "dxbc_saved_mul0 = get_cb0(3).xy * get_cb0(2).xy;") !=
              NULL);
        CHECK(strstr(exact.buf, "r0.xy = dxbc_saved_mul0;") != NULL);
        const char *first_reuse =
            strstr(exact.buf, "r0.xy = dxbc_saved_mul0 + r0.xy;");
        CHECK(first_reuse != NULL);
        CHECK(strstr(first_reuse + 1,
                     "r0.xy = dxbc_saved_mul0 + r0.xy;") != NULL);
        sb_free(&exact);

        /* One reverse MAD is insufficient evidence for the recovered local
         * product family, so the recompile emitter remains instruction-local. */
        program.instruction_count = 3;
        instructions[2] = instructions[3];
        StringBuilder single_reuse;
        sb_init(&single_reuse);
        CHECK(hlsl_emit(&program, &single_reuse, NULL, NULL, NULL));
        CHECK(strstr(single_reuse.buf, "dxbc_saved_mul") == NULL);
        sb_free(&single_reuse);

        /* Same-order MADs are not the observed inverse and must not be
         * promoted merely because multiplication is commutative. */
        instructions[2] = instructions[1];
        instructions[3].opcode = USIL_OP_RET;
        instructions[3].operand_count = 0;
        program.instruction_count = 4;
        for (int index = 1; index <= 2; index++) {
            instructions[index].operands[1] =
                instructions[0].operands[1];
            instructions[index].operands[2] =
                instructions[0].operands[2];
        }
        StringBuilder same_order;
        sb_init(&same_order);
        CHECK(hlsl_emit(&program, &same_order, NULL, NULL, NULL));
        CHECK(strstr(same_order.buf, "dxbc_saved_mul") == NULL);
        sb_free(&same_order);
    }

    /* The complete exact shape is still rejected when definition and uses
     * occupy opposite branches.  A function-scoped declaration does not make
     * an unexecuted product definition available in the ELSE path. */
    {
        USILInstruction instructions[7];
        memset(instructions, 0, sizeof(instructions));
        instructions[0].opcode = USIL_OP_IF;
        instructions[0].operand_count = 1;
        instructions[0].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
        init_register_operand(&instructions[0].operands[0],
                              OPERAND_TYPE_TEMP, 1);
        instructions[0].operands[0].swizzle_mode = 2;
        instructions[0].operands[0].swizzle[0] = 0;
        instructions[1].opcode = USIL_OP_MUL;
        instructions[1].operand_count = 3;
        init_register_operand(&instructions[1].operands[0],
                              OPERAND_TYPE_TEMP, 0);
        instructions[1].operands[0].destination_mask = 0x30;
        init_cbuffer_operand(&instructions[1].operands[1], 0, 2);
        instructions[1].operands[1].swizzle[2] = 0;
        instructions[1].operands[1].swizzle[3] = 0;
        init_cbuffer_operand(&instructions[1].operands[2], 0, 3);
        instructions[1].operands[2].swizzle[2] = 0;
        instructions[1].operands[2].swizzle[3] = 0;
        snprintf(instructions[1].original_asm,
                 sizeof(instructions[1].original_asm), "branch_mul");
        instructions[2].opcode = USIL_OP_ELSE;
        for (int index = 3; index <= 4; index++) {
            instructions[index].opcode = USIL_OP_MAD;
            instructions[index].operand_count = 4;
            init_register_operand(&instructions[index].operands[0],
                                  OPERAND_TYPE_TEMP, 0);
            instructions[index].operands[0].destination_mask = 0x30;
            instructions[index].operands[1] =
                instructions[1].operands[2];
            instructions[index].operands[2] =
                instructions[1].operands[1];
            init_register_operand(&instructions[index].operands[3],
                                  OPERAND_TYPE_TEMP, 0);
            instructions[index].operands[3].swizzle[2] = 0;
            instructions[index].operands[3].swizzle[3] = 0;
            snprintf(instructions[index].original_asm,
                     sizeof(instructions[index].original_asm),
                     "branch_recurrence_%d", index);
        }
        instructions[5].opcode = USIL_OP_ENDIF;
        instructions[6].opcode = USIL_OP_RET;

        USILConstantBuffer cbuffer = {0, 4, false};
        USILProgram program;
        memset(&program, 0, sizeof(program));
        snprintf(program.shader_type_model, sizeof(program.shader_type_model),
                 "ps_5_0");
        program.temp_count = 2;
        program.cbuffers = &cbuffer;
        program.cbuffer_count = 1;
        program.cbuffer_alloc = 1;
        program.instructions = instructions;
        program.instruction_count = 7;
        program.instruction_alloc = 7;

        StringBuilder exact;
        sb_init(&exact);
        CHECK(hlsl_emit(&program, &exact, NULL, NULL, NULL));
        CHECK(strstr(exact.buf, "// branch_mul") != NULL);
        CHECK(strstr(exact.buf, "// branch_recurrence_3") != NULL);
        CHECK(strstr(exact.buf, "// branch_recurrence_4") != NULL);
        CHECK(strstr(exact.buf, "dxbc_saved_mul") == NULL);

        const HLSLEmitOptions production_options =
            HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
        CHECK(production_options.mode == HLSL_EMIT_MODE_RECOMPILE);
        StringBuilder explicit_exact;
        sb_init(&explicit_exact);
        CHECK(hlsl_emit_with_options(&program, &explicit_exact, NULL, NULL,
                                     NULL, &production_options));
        CHECK(strcmp(exact.buf, explicit_exact.buf) == 0);
        sb_free(&explicit_exact);
        sb_free(&exact);

        StringBuilder readable;
        sb_init(&readable);
        CHECK(hlsl_emit_with_options(&program, &readable, NULL, NULL, NULL,
                                     &readable_options));
        CHECK(strstr(readable.buf, "dxbc_saved_mul") == NULL);
        sb_free(&readable);
    }

    return 0;
}

static int verify_instruction_resource_binding_authority(void) {
    DXBCContainer container;
    DXBCInstruction instruction;
    DXBCResourceDecl resource;
    memset(&container, 0, sizeof(container));
    memset(&instruction, 0, sizeof(instruction));
    memset(&resource, 0, sizeof(resource));
    snprintf(container.shader_type_model, sizeof(container.shader_type_model),
             "ps_5_0");
    resource.register_index = 0;
    resource.declared = true;
    resource.dimension = 3;
    snprintf(resource.dim_name, sizeof(resource.dim_name), "texture2d");
    container.resources = &resource;
    container.resource_count = 1;
    container.resource_alloc = 1;
    instruction.opcode = 69;
    snprintf(instruction.opcode_str, sizeof(instruction.opcode_str), "sample");
    snprintf(instruction.formatted_asm, sizeof(instruction.formatted_asm),
             "sample_indexable(texture3d)(float,float,float,float)");
    instruction.operand_count = 4;
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    init_register_operand(&instruction.operands[1], OPERAND_TYPE_TEMP, 1);
    init_register_operand(&instruction.operands[2], OPERAND_TYPE_RESOURCE, 0);
    init_register_operand(&instruction.operands[3], OPERAND_TYPE_SAMPLER, 0);
    container.instructions = &instruction;
    container.instruction_count = 1;
    container.instruction_alloc = 1;

    USILProgram program;
    CHECK(usil_translate(&program, &container));
    CHECK(program.instruction_count == 1);
    CHECK(strcmp(program.instructions[0].resource_dimension, "2d") == 0);
    CHECK(strstr(program.instructions[0].original_asm, "texture3d") != NULL);
    usil_free(&program);
    return 0;
}

static int verify_resinfo_authority_and_dimensions(void) {
    USILTexture texture;
    memset(&texture, 0, sizeof(texture));
    texture.reg_idx = 0;
    snprintf(texture.dimension, sizeof(texture.dimension), "2darray");
    memset(texture.return_types, 5, sizeof(texture.return_types));

    USILInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_RESINFO;
    instruction.resource_info_return_type = 1;
    snprintf(instruction.resource_dimension,
             sizeof(instruction.resource_dimension), "2darray");
    instruction.operand_count = 3;
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0xf0;
    init_immediate_operand(&instruction.operands[1], 1);
    init_register_operand(&instruction.operands[2], OPERAND_TYPE_RESOURCE, 0);
    instruction.operands[2].swizzle[0] = 2;
    instruction.operands[2].swizzle[1] = 0;
    instruction.operands[2].swizzle[2] = 1;
    instruction.operands[2].swizzle[3] = 3;

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.temp_count = 1;
    program.textures = &texture;
    program.texture_count = 1;
    program.texture_alloc = 1;
    program.instructions = &instruction;
    program.instruction_count = 1;
    program.instruction_alloc = 1;

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf,
                 "GetDimensions(asuint(") != NULL);
    CHECK(strstr(builder.buf, "w, h, elements, levels") != NULL);
    CHECK(strstr(builder.buf, "1.0f / (float)w") != NULL);
    CHECK(strstr(builder.buf, "1.0f / (float)h") != NULL);
    CHECK(strstr(builder.buf, "(float)elements") != NULL);
    CHECK(strstr(builder.buf, ").zxyw") != NULL);
    sb_free(&builder);

    instruction.resource_info_return_type = 2;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf,
                 "uint4(w, h, elements, levels).zxyw") != NULL);
    sb_free(&builder);
    return 0;
}

static int verify_sampleinfo_return_authority(void) {
    USILTexture texture;
    memset(&texture, 0, sizeof(texture));
    texture.reg_idx = 0;
    snprintf(texture.dimension, sizeof(texture.dimension), "2dms");
    memset(texture.return_types, 5, sizeof(texture.return_types));

    USILInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_SAMPLEINFO;
    snprintf(instruction.resource_dimension,
             sizeof(instruction.resource_dimension), "2dms");
    instruction.operand_count = 2;
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0x10;
    init_register_operand(&instruction.operands[1], OPERAND_TYPE_RESOURCE, 0);

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.temp_count = 1;
    program.textures = &texture;
    program.texture_count = 1;
    program.texture_alloc = 1;
    program.instructions = &instruction;
    program.instruction_count = 1;
    program.instruction_alloc = 1;

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "(float)samples") != NULL);
    sb_free(&builder);

    instruction.sample_info_return_type = 1u;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "= samples;") != NULL);
    CHECK(strstr(builder.buf, "(float)samples") == NULL);
    sb_free(&builder);

    instruction.sample_info_return_type = 2u;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    sb_free(&builder);
    return 0;
}

static int verify_untyped_resource_return_authority(void) {
    USILTexture texture;
    memset(&texture, 0, sizeof(texture));
    texture.reg_idx = 0;
    texture.stride = 16;
    snprintf(texture.dimension, sizeof(texture.dimension), "structured");

    USILInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = USIL_OP_LD_STRUCTURED;
    instruction.operand_count = 4;
    instruction.has_resource_dimension = true;
    instruction.resource_stride = 16;
    snprintf(instruction.resource_dimension,
             sizeof(instruction.resource_dimension), "structured");
    instruction.has_resource_return_types = true;
    memset(instruction.resource_return_types, 6,
           sizeof(instruction.resource_return_types));
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0xf0;
    init_immediate_operand(&instruction.operands[1], 0);
    init_immediate_operand(&instruction.operands[2], 0);
    init_register_operand(&instruction.operands[3], OPERAND_TYPE_RESOURCE, 0);

    USILProgram program;
    memset(&program, 0, sizeof(program));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.temp_count = 1;
    program.textures = &texture;
    program.texture_count = 1;
    program.texture_alloc = 1;
    program.instructions = &instruction;
    program.instruction_count = 1;
    program.instruction_alloc = 1;

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "StructuredBuffer<dxbc_struct_t0> t0") != NULL);
    CHECK(strstr(builder.buf, "t0[asuint(") != NULL);
    sb_free(&builder);

    /* Unity serializes structured SRVs through bufferbind rather than
     * texbind. Preserve that binding name: a synthetic t0 declaration has
     * identical executable DXBC but cannot receive the original runtime
     * property binding. */
    SerializedResourceParam srv_bindings[3];
    memset(srv_bindings, 0, sizeof(srv_bindings));
    /* A typed texture at the same t register is a distinct Unity reflection
     * record.  Place it first to prove structured declaration and operand
     * spelling do not depend on serialized record order. */
    srv_bindings[0].name = "_MainTex";
    srv_bindings[0].bind_type = SERIALIZED_RESOURCE_TEXTURE;
    srv_bindings[0].bind_index = 0;
    srv_bindings[1].name = "_WaveformBuffer";
    srv_bindings[1].bind_type = SERIALIZED_RESOURCE_BUFFER;
    srv_bindings[1].bind_index = 0;
    srv_bindings[1].array_size = 1;
    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    parameters.resources = srv_bindings;
    parameters.res_count = 2;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, &parameters, NULL, NULL));
    CHECK(strstr(builder.buf,
                 "StructuredBuffer<dxbc_struct_t0> _WaveformBuffer") !=
          NULL);
    CHECK(strstr(builder.buf, "_WaveformBuffer[asuint(") != NULL);
    CHECK(strstr(builder.buf, "StructuredBuffer<dxbc_struct_t0> t0") ==
          NULL);
    sb_free(&builder);

    const char *texture_name = resolve_texture_name(&parameters, 0);
    CHECK(texture_name && strcmp(texture_name, "_MainTex") == 0);

    /* A second bufferbind for one architectural binding is contradictory,
     * even when a valid texbind also occupies t0.  Do not silently choose the
     * first buffer or fall back to a synthetic identifier. */
    srv_bindings[2] = srv_bindings[1];
    srv_bindings[2].name = "_OtherBuffer";
    parameters.res_count = 3;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, &parameters, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    parameters.res_count = 2;

    /* Typed resources do carry return-type authority, so a mismatch there
     * remains a hard failure. */
    snprintf(texture.dimension, sizeof(texture.dimension), "2d");
    memset(texture.return_types, 5, sizeof(texture.return_types));
    snprintf(instruction.resource_dimension,
             sizeof(instruction.resource_dimension), "2d");
    instruction.resource_stride = 0;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    return 0;
}

static int verify_ubfe_translation_and_emission(void) {
    DXBCInstruction instruction;
    init_dxbc_instruction(&instruction, 138, "ubfe", false);
    instruction.operand_count = 4;
    snprintf(instruction.formatted_asm, sizeof(instruction.formatted_asm),
             "ubfe r0.yz, l(0,8,8,0), l(0,8,16,0), r1.wwww");

    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0x60;
    init_immediate_vector_operand(&instruction.operands[1], 0, 8, 8, 0);
    init_immediate_vector_operand(&instruction.operands[2], 0, 8, 16, 0);
    init_register_operand(&instruction.operands[3], OPERAND_TYPE_TEMP, 1);
    instruction.operands[3].swizzle_mode = 2;
    instruction.operands[3].swizzle[0] = 3;

    DXBCContainer container;
    memset(&container, 0, sizeof(container));
    snprintf(container.shader_type_model, sizeof(container.shader_type_model),
             "ps_5_0");
    container.instructions = &instruction;
    container.instruction_count = 1;
    container.instruction_alloc = 1;

    USILProgram program;
    CHECK(usil_translate(&program, &container));
    CHECK(program.instruction_count == 1);
    CHECK(program.instructions[0].opcode == USIL_OP_UBFE);
    CHECK(program.instructions[0].operand_count == 4);
    /* DXBC operand order is destination, width, offset, value. */
    CHECK(program.instructions[0].operands[1].imm_values[1] == 8);
    CHECK(program.instructions[0].operands[2].imm_values[2] == 16);
    CHECK(program.instructions[0].operands[3].type == OPERAND_TYPE_TEMP);
    CHECK(program.instructions[0].operands[3].register_index == 1);

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.buf != NULL);
    CHECK(strstr(builder.buf, "uint4 r0 = (uint4)0;") != NULL);
    CHECK(strstr(builder.buf, "uint4 r1 = (uint4)0;") != NULL);
    CHECK(strstr(builder.buf,
                 "r0.yz = (r1.w >> (uint2(8u, 16u) & 31u)) & "
                 "((1u << (uint2(8u, 8u) & 31u)) - 1u);") != NULL);
    CHECK(strstr(builder.buf, "asint(r1.w)") == NULL);
    sb_free(&builder);
    usil_free(&program);

    /* A zero width produces a zero mask: (1 << 0) - 1.  Keep the value and
     * shift operands unsigned even when the high bit of the value is set. */
    init_dxbc_instruction(&instruction, 138, "ubfe", false);
    instruction.operand_count = 4;
    snprintf(instruction.formatted_asm, sizeof(instruction.formatted_asm),
             "ubfe r0.x, l(0), l(13), l(0x80000000)");
    init_register_operand(&instruction.operands[0], OPERAND_TYPE_TEMP, 0);
    instruction.operands[0].destination_mask = 0x10;
    init_immediate_operand(&instruction.operands[1], 0);
    init_immediate_operand(&instruction.operands[2], 13);
    init_immediate_operand(&instruction.operands[3], 0x80000000u);

    CHECK(usil_translate(&program, &container));
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf,
                 "(2147483648u >> (13u & 31u)) & "
                 "((1u << (0u & 31u)) - 1u)") != NULL);
    CHECK(strstr(builder.buf, "-2147483648") == NULL);
    CHECK(strstr(builder.buf, "asint(") == NULL);
    sb_free(&builder);
    usil_free(&program);
    return 0;
}

static int verify_depth_output_and_register_bounds(void) {
    USILInstruction instructions[3];
    memset(instructions, 0, sizeof(instructions));

    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_OUTPUT,
                          0);
    instructions[0].operands[0].destination_mask = 0xf0;
    init_immediate_operand(&instructions[0].operands[1], 0x3f800000u);

    instructions[1].opcode = USIL_OP_MOV;
    instructions[1].operand_count = 2;
    init_register_operand(&instructions[1].operands[0],
                          OPERAND_TYPE_OUTPUT_DEPTH, 0);
    instructions[1].operands[0].destination_mask = 0x10;
    init_immediate_operand(&instructions[1].operands[1], 0x3f000000u);
    instructions[2].opcode = USIL_OP_RET;

    USILProgram program;
    DXBCSignatureElement outputs[2];
    memset(&program, 0, sizeof(program));
    memset(outputs, 0, sizeof(outputs));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "ps_5_0");
    program.outputs = outputs;
    program.output_count = 2;
    program.output_alloc = 2;
    snprintf(program.outputs[0].semantic_name,
             sizeof(program.outputs[0].semantic_name), "SV_Target");
    program.outputs[0].system_value = 64u;
    program.outputs[0].register_id = 0;
    program.outputs[0].mask = 0xf;
    program.outputs[0].component_type = 3;
    snprintf(program.outputs[1].semantic_name,
             sizeof(program.outputs[1].semantic_name), "SV_Depth");
    program.outputs[1].system_value = 65u;
    program.outputs[1].register_id = UINT32_MAX;
    program.outputs[1].mask = 1;
    program.outputs[1].component_type = 3;
    program.instructions = instructions;
    program.instruction_count = 3;
    program.instruction_alloc = 3;

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.buf != NULL);
    CHECK(strstr(builder.buf, "out float oDepth : SV_Depth") != NULL);
    CHECK(strstr(builder.buf, "o-1") == NULL);
    CHECK(strstr(builder.buf, "o4294967295") == NULL);
    sb_free(&builder);

    USILInstruction depth_only_instructions[2] = {
        instructions[1], instructions[2]
    };
    DXBCSignatureElement depth_only_output = program.outputs[1];
    USILProgram depth_only = program;
    depth_only.outputs = &depth_only_output;
    depth_only.output_count = 1;
    depth_only.output_alloc = 1;
    depth_only.instructions = depth_only_instructions;
    depth_only.instruction_count = 2;
    sb_init(&builder);
    CHECK(hlsl_emit(&depth_only, &builder, NULL, NULL, NULL));
    CHECK(strstr(builder.buf, "float oDepth = 0.0f;") != NULL);
    CHECK(strstr(builder.buf, "return oDepth;") != NULL);
    CHECK(strstr(builder.buf, "o4294967295") == NULL);
    sb_free(&builder);

    /* UINT32_MAX is a valid signature sentinel for special system outputs.
     * Other values outside the emitter's fixed I/O register model must fail
     * before a declaration bitmap or analysis register file is indexed. */
    program.outputs[0].register_id = 16;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    program.outputs[0].register_id = 0;

    instructions[0].operands[0].register_index = -1;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    instructions[0].operands[0].register_index = 0;

    instructions[0].operands[0].type = OPERAND_TYPE_INPUT_CONTROL_POINT;
    sb_init(&builder);
    CHECK(!hlsl_emit(&program, &builder, NULL, NULL, NULL));
    CHECK(builder.failed);
    sb_free(&builder);
    return 1;
}

static int verify_screen_position_is_readable_only(void) {
    USILInstruction instructions[3];
    memset(instructions, 0, sizeof(instructions));

    /* This is the legacy detector's compact screen-position shape:
     *   mul r1.xzw, r0.xxwy, l(0.5, 0, 0.5, 0.5)
     *   mov o0.zw, r0.zw
     *   add o0.xy, r1.zzzz, r1.xwxx
     * It intentionally matches the heuristic so this test guards the mode
     * boundary rather than merely testing a non-match. */
    instructions[0].opcode = USIL_OP_MUL;
    instructions[0].operand_count = 3;
    snprintf(instructions[0].original_asm,
             sizeof(instructions[0].original_asm),
             "mul r1.xzw, r0.xxwy, l(0.5,0,0.5,0.5)");
    init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 1);
    instructions[0].operands[0].destination_mask = 0xd0;
    init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 0);
    instructions[0].operands[1].swizzle[0] = 0;
    instructions[0].operands[1].swizzle[1] = 0;
    instructions[0].operands[1].swizzle[2] = 3;
    instructions[0].operands[1].swizzle[3] = 1;
    init_immediate_vector_operand(&instructions[0].operands[2],
                                  0x3f000000u, 0, 0x3f000000u,
                                  0x3f000000u);

    instructions[1].opcode = USIL_OP_MOV;
    instructions[1].operand_count = 2;
    snprintf(instructions[1].original_asm,
             sizeof(instructions[1].original_asm), "mov o0.zw, r0.zw");
    init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_OUTPUT,
                          0);
    instructions[1].operands[0].destination_mask = 0xc0;
    init_register_operand(&instructions[1].operands[1], OPERAND_TYPE_TEMP, 0);

    instructions[2].opcode = USIL_OP_ADD;
    instructions[2].operand_count = 3;
    snprintf(instructions[2].original_asm,
             sizeof(instructions[2].original_asm),
             "add o0.xy, r1.zzzz, r1.xwxx");
    init_register_operand(&instructions[2].operands[0], OPERAND_TYPE_OUTPUT,
                          0);
    instructions[2].operands[0].destination_mask = 0x30;
    init_register_operand(&instructions[2].operands[1], OPERAND_TYPE_TEMP, 1);
    instructions[2].operands[1].swizzle_mode = 2;
    instructions[2].operands[1].swizzle[0] = 2;
    init_register_operand(&instructions[2].operands[2], OPERAND_TYPE_TEMP, 1);
    instructions[2].operands[2].swizzle[0] = 0;
    instructions[2].operands[2].swizzle[1] = 3;
    instructions[2].operands[2].swizzle[2] = 0;
    instructions[2].operands[2].swizzle[3] = 0;

    USILProgram program;
    DXBCSignatureElement output;
    memset(&program, 0, sizeof(program));
    memset(&output, 0, sizeof(output));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "vs_5_0");
    program.temp_count = 2;
    program.outputs = &output;
    program.output_count = 1;
    program.output_alloc = 1;
    snprintf(program.outputs[0].semantic_name,
             sizeof(program.outputs[0].semantic_name), "TEXCOORD");
    program.outputs[0].register_id = 0;
    program.outputs[0].mask = 0xf;
    program.outputs[0].component_type = 3;
    program.instructions = instructions;
    program.instruction_count = 3;
    program.instruction_alloc = 3;

    const HLSLEmitOptions recompile_options = {
        HLSL_EMIT_MODE_RECOMPILE, "MustNeverAppearInRecompileMode", false,
        NULL, 0};
    StringBuilder recompile;
    sb_init(&recompile);
    CHECK(hlsl_emit_with_options(&program, &recompile, NULL, NULL, NULL,
                                 &recompile_options));
    CHECK(recompile.buf != NULL);
    CHECK(strstr(recompile.buf, "MustNeverAppearInRecompileMode") == NULL);
    CHECK(strstr(recompile.buf, "ComputeNonStereoScreenPos") == NULL);
    CHECK(strstr(recompile.buf, "Optimized ComputeScreenPos") == NULL);
    CHECK(strstr(recompile.buf, "r1.xzw") != NULL);
    CHECK(strstr(recompile.buf, "o0.zw") != NULL);
    CHECK(strstr(recompile.buf, "o0.xy") != NULL);

    /* The compatibility API has the same safe default and therefore cannot
     * silently opt verification into readable reconstruction. */
    StringBuilder compatibility;
    sb_init(&compatibility);
    CHECK(hlsl_emit(&program, &compatibility, NULL, NULL, NULL));
    CHECK(strcmp(recompile.buf, compatibility.buf) == 0);

    HLSLEmitOptions readable_options = HLSL_EMIT_READABLE_OPTIONS_INIT;
    readable_options.readable_screen_pos_helper = "ReadableScreenPosition";
    StringBuilder readable;
    sb_init(&readable);
    CHECK(hlsl_emit_with_options(&program, &readable, NULL, NULL, NULL,
                                 &readable_options));
    CHECK(readable.buf != NULL);
    CHECK(strstr(readable.buf, "float4 ReadableScreenPosition(") != NULL);
    CHECK(strstr(readable.buf, "Optimized ComputeScreenPos") != NULL);
    CHECK(strcmp(recompile.buf, readable.buf) != 0);

    /* Readable-only names are caller supplied.  Preserve the complete valid
     * identifier instead of silently cutting the helper declaration at the
     * old 256-byte formatting boundary. */
    char long_helper[513];
    memset(long_helper, 'H', sizeof(long_helper) - 1);
    long_helper[0] = 'h';
    long_helper[sizeof(long_helper) - 1] = '\0';
    readable_options.readable_screen_pos_helper = long_helper;
    StringBuilder long_readable;
    sb_init(&long_readable);
    CHECK(hlsl_emit_with_options(&program, &long_readable, NULL, NULL, NULL,
                                 &readable_options));
    CHECK(strstr(long_readable.buf, long_helper) != NULL);
    sb_free(&long_readable);

    HLSLEmitOptions invalid_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
    invalid_options.mode = (HLSLEmitMode)99;
    StringBuilder invalid;
    sb_init(&invalid);
    CHECK(!hlsl_emit_with_options(&program, &invalid, NULL, NULL, NULL,
                                  &invalid_options));
    CHECK(invalid.failed);

    sb_free(&invalid);
    sb_free(&readable);
    sb_free(&compatibility);
    sb_free(&recompile);
    return 1;
}

static int verify_large_emission_fragments_are_lossless(void) {
    USILInstruction ret;
    memset(&ret, 0, sizeof(ret));
    ret.opcode = USIL_OP_RET;

    USILProgram program;
    DXBCSignatureElement outputs[64];
    memset(&program, 0, sizeof(program));
    memset(outputs, 0, sizeof(outputs));
    snprintf(program.shader_type_model, sizeof(program.shader_type_model),
             "vs_5_0");
    program.outputs = outputs;
    program.output_count = (int)(sizeof(outputs) / sizeof(outputs[0]));
    program.output_alloc = program.output_count;
    for (int output = 0; output < program.output_count; ++output) {
        DXBCSignatureElement *element = &program.outputs[output];
        snprintf(element->semantic_name, sizeof(element->semantic_name),
                 "SEMANTIC_IDENTIFIER_PADDED_FOR_RETURN_BLOCK_TEST");
        element->semantic_index = output;
        element->register_id = (uint32_t)(output % 16);
        element->mask = 0xf;
        element->component_type = 3;
    }
    program.instructions = &ret;
    program.instruction_count = 1;
    program.instruction_alloc = 1;

    char entry_point[513];
    char input_struct[513];
    char output_struct[513];
    memset(entry_point, 'E', sizeof(entry_point) - 1);
    memset(input_struct, 'I', sizeof(input_struct) - 1);
    memset(output_struct, 'O', sizeof(output_struct) - 1);
    entry_point[0] = 'e';
    input_struct[0] = 'i';
    output_struct[0] = 'o';
    entry_point[sizeof(entry_point) - 1] = '\0';
    input_struct[sizeof(input_struct) - 1] = '\0';
    output_struct[sizeof(output_struct) - 1] = '\0';
    HLSLEmitNames names = {entry_point, input_struct, output_struct};

    StringBuilder builder;
    sb_init(&builder);
    CHECK(hlsl_emit(&program, &builder, NULL, NULL, &names));
    CHECK(builder.buf != NULL);
    CHECK(strstr(builder.buf, entry_point) != NULL);
    CHECK(strstr(builder.buf, input_struct) != NULL);
    CHECK(strstr(builder.buf, output_struct) != NULL);
    CHECK(strstr(builder.buf,
                 "output.o_SEMANTIC_IDENTIFIER_PADDED_FOR_RETURN_BLOCK_TEST63 = o15;")
          != NULL);
    CHECK(builder.len > 8192);
    sb_free(&builder);
    return 1;
}

static int verify_ray_box_inverse_is_structural_and_fail_closed(void) {
    uint8_t* bytes = NULL;
    size_t byte_count = 0;
    CHECK(test_fixture_decode_base64(DXBC_PREVIEW3D_SLICED_FIXTURE,
                                     &bytes, &byte_count));

    DXBCDocument document;
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCContainer container;
    DXBCStageContract contract;
    DXBCStageContractDiagnostic contract_diagnostic;
    dxbc_document_init(&document);
    memset(&container, 0, sizeof(container));
    dxbc_stage_contract_init(&contract);
    CHECK(dxbc_document_parse(&document, bytes, byte_count,
                              &document_diagnostic));
    CHECK(dxbc_document_decode_semantic(&document, &container));
    CHECK(dxbc_stage_contract_decode(&document, &container, &contract,
                                     &contract_diagnostic));
    USILProgram program;
    CHECK(usil_translate_with_stage_contract(&program, &container,
                                              &contract));
    CHECK(hlsl_ray_box_intersection_lift_matches(&program));
    CHECK(hlsl_volume_slice_sampling_lift_matches(&program));

    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit(&program, &source, NULL, NULL, NULL));
    CHECK(source.buf != NULL);
    CHECK(strstr(source.buf,
                 "float2 dxbc_ray_box_intersection(float3 ro, float3 rd,")
          != NULL);
    CHECK(strstr(source.buf,
                 "float2 dxbc_intersection3 = dxbc_ray_box_intersection(")
          != NULL);
    CHECK(strstr(source.buf,
                 "dxbc_intersection3.y < 0.0f) {") != NULL);
    CHECK(strstr(source.buf,
                 "float3 dxbc_unpack_normal(float4 packednormal)") != NULL);
    sb_free(&source);

    SerializedVariable globals_variables[7];
    memset(globals_variables, 0, sizeof(globals_variables));
    globals_variables[0].name = "_VoxelSize";
    globals_variables[0].layout[0] = 48;
    globals_variables[0].layout[3] = 3;
    globals_variables[1].name = "_InvScale";
    globals_variables[1].layout[0] = 64;
    globals_variables[1].layout[3] = 4;
    globals_variables[2].name = "_IsNormalMap";
    globals_variables[2].layout[0] = 80;
    globals_variables[2].layout[2] = 1;
    globals_variables[2].layout[3] = 1;
    globals_variables[3].name = "_Positions";
    globals_variables[3].layout[0] = 84;
    globals_variables[3].layout[3] = 3;
    globals_variables[4].name = "_InvChannels";
    globals_variables[4].layout[0] = 96;
    globals_variables[4].layout[3] = 3;
    globals_variables[5].name = "_Ramp";
    globals_variables[5].layout[0] = 108;
    globals_variables[5].layout[3] = 1;
    globals_variables[6].name = "_FilterMode";
    globals_variables[6].layout[0] = 112;
    globals_variables[6].layout[3] = 1;

    SerializedConstantBuffer globals;
    memset(&globals, 0, sizeof(globals));
    globals.name = "$Globals";
    globals.size = 128;
    globals.variables = globals_variables;
    globals.var_count = 7;

    SerializedResourceParam resources[6];
    memset(resources, 0, sizeof(resources));
    resources[0].name = "$Globals";
    resources[0].bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER;
    resources[0].bind_index = 0;
    resources[1].name = "_MainTex";
    resources[1].bind_type = SERIALIZED_RESOURCE_TEXTURE;
    resources[1].bind_index = 1;
    resources[2].name = "";
    resources[2].bind_type = SERIALIZED_RESOURCE_SAMPLER;
    resources[2].bind_index = 0;
    resources[2].sampler_state = 0x54u;
    resources[3].name = "";
    resources[3].bind_type = SERIALIZED_RESOURCE_SAMPLER;
    resources[3].bind_index = 1;
    resources[3].sampler_state = 0x55u;
    resources[4].name = "";
    resources[4].bind_type = SERIALIZED_RESOURCE_SAMPLER;
    resources[4].bind_index = 2;
    resources[4].sampler_state = 0x56u;
    resources[5].name = "_ColorRamp";
    resources[5].bind_type = SERIALIZED_RESOURCE_TEXTURE;
    resources[5].bind_index = 0;
    resources[5].sampler_index = 3;
    SerializedProgramParameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.constant_buffers = &globals;
    parameters.cb_count = 1;
    parameters.resources = resources;
    parameters.res_count = 6;

    sb_init(&source);
    CHECK(hlsl_emit(&program, &source, &parameters, NULL, NULL));
    CHECK(source.buf != NULL);
    CHECK(strstr(source.buf,
                 "float3 dxbc_sample_position1 = asfloat(r2.xyz) * "
                 "_InvScale.xyz;") != NULL);
    CHECK(strstr(source.buf, "float4 color1;") != NULL);
    CHECK(strstr(source.buf,
                 "color1 = _MainTex.Sample("
                 "sampler_dxbc_s0_point_clampu_clampv_clampw, "
                 "dxbc_sample_position1 + float3(0.500001013f,") != NULL);
    CHECK(strstr(source.buf,
                 "if (_IsNormalMap) {") != NULL);
    CHECK(strstr(source.buf, "//   92: sample") == NULL);
    sb_free(&source);

    /* Every part of the volume inverse is structural.  A resource swizzle,
     * sample-coordinate literal, or final normal/color join near miss must
     * retain the raw instruction representation. */
    CHECK(program.instruction_count > 150);
    DXBCOperand* sample_resource = &program.instructions[92].operands[2];
    CHECK(sample_resource->swizzle_mode == 1 &&
          sample_resource->swizzle[0] == 2);
    sample_resource->swizzle[0] = 0;
    CHECK(!hlsl_volume_slice_sampling_lift_matches(&program));
    CHECK(hlsl_ray_box_intersection_lift_matches(&program));
    sample_resource->swizzle[0] = 2;
    CHECK(hlsl_volume_slice_sampling_lift_matches(&program));

    DXBCOperand* sample_literal = &program.instructions[91].operands[3];
    CHECK(sample_literal->immediate_word_count == 4);
    sample_literal->immediate_words[0] ^= 1u;
    CHECK(!hlsl_volume_slice_sampling_lift_matches(&program));
    CHECK(hlsl_ray_box_intersection_lift_matches(&program));
    sample_literal->immediate_words[0] ^= 1u;
    CHECK(hlsl_volume_slice_sampling_lift_matches(&program));

    DXBCOperand* color_join = &program.instructions[150].operands[3];
    CHECK(color_join->swizzle_mode == 1 && color_join->swizzle[0] == 1);
    color_join->swizzle[0] = 0;
    CHECK(!hlsl_volume_slice_sampling_lift_matches(&program));
    CHECK(hlsl_ray_box_intersection_lift_matches(&program));
    color_join->swizzle[0] = 1;
    CHECK(hlsl_volume_slice_sampling_lift_matches(&program));

    /* A one-bit literal near miss must not select the source-level inverse. */
    CHECK(program.instruction_count > 47);
    DXBCOperand* box_literal = &program.instructions[47].operands[2];
    CHECK(box_literal->immediate_word_count == 4);
    box_literal->immediate_words[0] ^= 1u;
    CHECK(!hlsl_ray_box_intersection_lift_matches(&program));
    box_literal->immediate_words[0] ^= 1u;
    CHECK(hlsl_ray_box_intersection_lift_matches(&program));

    /* Likewise, a condition encoding change is not treated as equivalent. */
    DXBCOperand* movc_condition = &program.instructions[57].operands[1];
    CHECK(movc_condition->swizzle_mode == 1);
    movc_condition->swizzle[0] = 2;
    CHECK(!hlsl_ray_box_intersection_lift_matches(&program));
    movc_condition->swizzle[0] = 3;
    CHECK(hlsl_ray_box_intersection_lift_matches(&program));

    usil_free(&program);
    dxbc_free(&container);
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    mem_free(bytes, byte_count);
    return 0;
}

int main(void) {
    CHECK(dxbc_tokenized_instruction_length(0x7f000032u) == 127u);
    CHECK(dxbc_tokenized_instruction_length(0x20000032u) == 32u);
    CHECK(dxbc_tokenized_instruction_length(0x80000032u) == 0u);
    CHECK(verify_operand_decoder() == 0);
    CHECK(verify_precision_and_instruction_controls() == 0);
    CHECK(verify_signature_parser_authority() == 0);
    CHECK(verify_dynamic_capacity_boundaries() == 0);
    CHECK(verify_dynamic_signature_semantics() == 0);
    CHECK(verify_instruction_fail_closed() == 0);
    CHECK(verify_condition_test_authority() == 0);
    CHECK(verify_resource_declaration_authority() == 0);
    CHECK(verify_resource_declaration_emission() == 0);
    CHECK(verify_expression_scratch_overflow_fails_closed() == 0);
    CHECK(verify_instruction_resource_binding_authority() == 0);
    CHECK(verify_sincos_lane_emission() == 0);
    CHECK(verify_integer_multi_output_emission() == 0);
    CHECK(verify_masked_vector_dependency_emission() == 0);
    CHECK(verify_exact_operand_width_emission() == 0);
    CHECK(verify_resinfo_authority_and_dimensions() == 0);
    CHECK(verify_sampleinfo_return_authority() == 0);
    CHECK(verify_untyped_resource_return_authority() == 0);
    CHECK(verify_dynamic_generation_state() == 0);
    CHECK(verify_declaration_control_contracts() == 0);
    CHECK(verify_signature_declaration_contract() == 0);
    CHECK(verify_structural_vector_output_mad_lowering() == 0);
    CHECK(verify_never_written_output_lanes_are_not_fabricated() == 0);
    CHECK(verify_hlsl_sm5_register_boundaries() == 0);
    CHECK(verify_typed_emitter_storage_decisions() == 0);
    CHECK(verify_typed_move_modifier_emission() == 0);
    CHECK(verify_typed_decomposition_emission() == 0);
    CHECK(verify_recompile_transform_safety_gate() == 0);
    CHECK(verify_unsupported_hlsl_opcode_fails_closed() == 0);
    CHECK(verify_usil_rejects_silent_opcode_loss() == 0);
    CHECK(verify_ubfe_translation_and_emission() == 0);
    CHECK(verify_depth_output_and_register_bounds());
    CHECK(verify_screen_position_is_readable_only());
    CHECK(verify_large_emission_fragments_are_lossless());
    CHECK(verify_ray_box_inverse_is_structural_and_fail_closed() == 0);
    CHECK(verify_golden_corpus());
    size_t size = 0;
    uint8_t* data = read_fixture(&size);
    CHECK(data != NULL);

    const size_t raw_offset = first_usbd_payload_offset(data, size);
    CHECK(raw_offset != 0);
    const uint32_t raw_size = read_le_u32(data + raw_offset - 4);
    CHECK(raw_size <= size - raw_offset);

    DXBCContainerView view;
    CHECK(dxbc_container_view_first(data, size, &view));
    CHECK(view.data == data + raw_offset);
    CHECK(view.size == raw_size);
    CHECK(dxbc_container_view_first(data + raw_offset, raw_size, &view));
    CHECK(view.data == data + raw_offset);
    CHECK(view.size == raw_size);

    DXBCContainer wrapped;
    DXBCContainer raw;
    CHECK(dxbc_parse(&wrapped, data, size));
    CHECK(dxbc_parse(&raw, data + raw_offset, raw_size));
    CHECK(strcmp(wrapped.shader_type_model, raw.shader_type_model) == 0);
    CHECK(wrapped.input_signature_count == raw.input_signature_count);
    CHECK(wrapped.output_signature_count == raw.output_signature_count);
    CHECK(wrapped.instruction_count == raw.instruction_count);

    uint8_t* prefixed = (uint8_t*)malloc(raw_size + 1);
    CHECK(prefixed != NULL);
    prefixed[0] = 0xcc;
    memcpy(prefixed + 1, data + raw_offset, raw_size);
    DXBCContainer rejected;
    CHECK(!dxbc_container_view_first(prefixed, raw_size + 1, &view));
    CHECK(!dxbc_parse(&rejected, prefixed, raw_size + 1));
    CHECK(!dxbc_parse(&rejected, data + raw_offset, raw_size - 1));
    CHECK(!dxbc_parse(&rejected, data, size - 1));

    const uint32_t many_record_count = 186;
    const size_t many_record_size = 9u + raw_size;
    const size_t many_usbd_size = 8u + many_record_count * many_record_size;
    uint8_t* many_usbd = (uint8_t*)calloc(1, many_usbd_size);
    CHECK(many_usbd != NULL);
    memcpy(many_usbd, "USBD", 4);
    write_le_u32(many_usbd + 4, many_record_count);
    size_t many_offset = 8;
    for (uint32_t i = 0; i < many_record_count; ++i) {
        write_le_u32(many_usbd + many_offset, 1);
        many_offset += 4;
        many_usbd[many_offset++] = 'v';
        write_le_u32(many_usbd + many_offset, raw_size);
        many_offset += 4;
        memcpy(many_usbd + many_offset, data + raw_offset, raw_size);
        many_offset += raw_size;
    }
    CHECK(many_offset == many_usbd_size);
    CHECK(dxbc_container_view_first(many_usbd, many_usbd_size, &view));
    CHECK(view.data == many_usbd + 17);
    CHECK(view.size == raw_size);
    many_usbd[many_usbd_size - raw_size] = 'X';
    CHECK(!dxbc_container_view_first(many_usbd, many_usbd_size, &view));
    free(many_usbd);

    uint8_t* unity_wrapped = (uint8_t*)calloc(1, raw_size + 38);
    CHECK(unity_wrapped != NULL);
    unity_wrapped[0] = 2;
    unity_wrapped[2] = 3;
    memcpy(unity_wrapped + 38, data + raw_offset, raw_size);
    CHECK(dxbc_container_view_first(unity_wrapped, raw_size + 38, &view));
    CHECK(view.data == unity_wrapped + 38);
    CHECK(view.size == raw_size);
    DXBCContainer unity_container;
    CHECK(dxbc_parse(&unity_container, unity_wrapped, raw_size + 38));
    CHECK(unity_container.instruction_count == raw.instruction_count);
    dxbc_free(&unity_container);
    unity_wrapped[10] = 1;
    CHECK(dxbc_parse(&unity_container, unity_wrapped, raw_size + 38));
    dxbc_free(&unity_container);
    unity_wrapped[0] = 3;
    CHECK(!dxbc_parse(&rejected, unity_wrapped, raw_size + 38));

    USILProgram program_a;
    USILProgram program_b;
    CHECK(usil_translate(&program_a, &wrapped));
    CHECK(usil_translate(&program_b, &raw));
    CHECK(program_a.instruction_count == program_b.instruction_count);

    StringBuilder hlsl_a;
    StringBuilder hlsl_b;
    sb_init(&hlsl_a);
    sb_init(&hlsl_b);
    CHECK(hlsl_emit(&program_a, &hlsl_a, NULL, NULL, NULL));
    CHECK(hlsl_emit(&program_b, &hlsl_b, NULL, NULL, NULL));
    CHECK(hlsl_a.buf != NULL && hlsl_b.buf != NULL);
    CHECK(strcmp(hlsl_a.buf, hlsl_b.buf) == 0);

    sb_free(&hlsl_a);
    sb_free(&hlsl_b);
    usil_free(&program_a);
    usil_free(&program_b);
    dxbc_free(&wrapped);
    dxbc_free(&raw);
    free(prefixed);
    free(unity_wrapped);
    free(data);
    CHECK(g_allocations_count == 0);
    CHECK(g_allocated_bytes == 0);
    return 0;
}
