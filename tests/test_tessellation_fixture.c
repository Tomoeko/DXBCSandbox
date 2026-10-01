// SPDX-License-Identifier: GPL-3.0-only

#include "test_tessellation_fixture.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"

#include <stdlib.h>
#include <string.h>

#define INSTRUCTION(opcode, length) \
    ((uint32_t)(opcode) | (uint32_t)(length) << 24)

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte)
        bytes[byte] = (uint8_t)(value >> (8 * byte));
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Authored token grammar and signatures, with no captured byte array. */
static size_t write_hull_signature(uint8_t *bytes, unsigned role, bool inner_first,
                                   const char *point_semantic, bool float3) {
    const bool patch = role == 2;
    const unsigned count = patch ? 4 : 1;
    const char *semantic = patch ? "SV_TessFactor" : point_semantic;
    const size_t name_offset = 8 + count * 24;
    const size_t size = name_offset + strlen(semantic) + 1 +
        (patch ? sizeof("SV_InsideTessFactor") : 0);
    memcpy(bytes, patch ? "PCSG" : role ? "OSGN" : "ISGN", 4);
    write_u32(bytes + 4, (uint32_t)size);
    bytes += 8;
    write_u32(bytes, count);
    write_u32(bytes + 4, 8);
    for (unsigned field = 0; field < count; ++field) {
        const unsigned logical = patch && inner_first ? (field ? field - 1 : 3) : field;
        uint8_t *element = bytes + 8 + 24 * field;
        write_u32(element, (uint32_t)(name_offset +
            (patch && logical == 3 ? strlen(semantic) + 1 : 0)));
        write_u32(element + 4, patch && logical < 3 ? logical : 0);
        write_u32(element + 8, patch ? (logical < 3 ? 13 : 14) : float3 ? 0 : 1);
        write_u32(element + 12, 3);
        write_u32(element + 16, patch ? field : 0);
        write_u32(element + 20, patch ? 0x0e01 : float3 ? (role ? 0x0807 : 0x0707) : role ? 15 : 0x0f0f);
    }
    memcpy(bytes + name_offset, semantic, strlen(semantic) + 1);
    if (patch)
        memcpy(bytes + name_offset + strlen(semantic) + 1,
               "SV_InsideTessFactor", sizeof("SV_InsideTessFactor"));
    return size + 8;
}

size_t test_tessellation_hull_signature(uint8_t *bytes, unsigned role, bool inner_first) {
    return write_hull_signature(bytes, role, inner_first, "SV_POSITION", false);
}

static uint8_t *make_hull_dxbc(uint32_t points, uint32_t output_points, uint8_t scenario,
                             const char *point_semantic, bool float3, size_t *size) {
    if (!point_semantic || strlen(point_semantic) > 128 || !size) return NULL;
    const uint32_t base_words[] = {
        INSTRUCTION(113, 1), INSTRUCTION(147, 1) | points << 11,
        INSTRUCTION(148, 1) | output_points << 11,
        INSTRUCTION(149, 1) | 2u << 11,
        INSTRUCTION(150, 1) | 1u << 11,
        INSTRUCTION(151, 1) | 3u << 11,
        INSTRUCTION(152, 2), 0x42000000,
        INSTRUCTION(106, 1) | 1u << 11,
        INSTRUCTION(115, 1), INSTRUCTION(153, 2), 3,
        INSTRUCTION(95, 2), 0x00017000,
        INSTRUCTION(103, 4), 0x00102012, 0, 17,
        INSTRUCTION(103, 4), 0x00102012, 1, 18,
        INSTRUCTION(103, 4), 0x00102012, 2, 19,
        INSTRUCTION(104, 2), 1,
        INSTRUCTION(91, 4), 0x00102012, 0, 3,
        INSTRUCTION(54, 4), 0x00100012, 0, 0x0001700a,
        INSTRUCTION(54, 6), 0x00902012, 0x0010000a, 0, 0x00004001, 0x40400000,
        INSTRUCTION(62, 1),
        INSTRUCTION(115, 1),
        INSTRUCTION(103, 4), 0x00102012, 3, 20,
        INSTRUCTION(54, 5), 0x00102012, 3, 0x00004001, 0x40800000,
        INSTRUCTION(62, 1)
    };
    uint32_t words[128];
    size_t word_count = 0;
    for (size_t word = 0; word < sizeof(base_words) / 4; ++word) {
        if ((scenario == 4 || scenario == 5) && word == 9) {
            const uint32_t cp_header[] = {INSTRUCTION(114, 1), INSTRUCTION(95, 2), 0x00016000,
                INSTRUCTION(95, 4), float3 ? 0x00201072 : 0x002010f2, points, 0,
                INSTRUCTION(101, 3), float3 ? 0x00102072 : 0x001020f2, 0,
                INSTRUCTION(104, 2), scenario == 5 ? 2 : 1,
                INSTRUCTION(54, 4), 0x00100012, 0, 0x00016001};
            memcpy(words + word_count, cp_header, sizeof(cp_header));
            word_count += sizeof(cp_header) / 4;
            if (scenario == 5) {
                const uint32_t chained[] = {INSTRUCTION(54, 5), 0x00100012, 1, 0x0010000a, 0};
                memcpy(words + word_count, chained, sizeof(chained));
                word_count += sizeof(chained) / 4;
            }
            const uint32_t cp_body[] = {INSTRUCTION(56, 12), float3 ? 0x00102072 : 0x001020f2, 0,
                0x00004002, 0x3fa00000, 0x3fa00000, 0x3fa00000, 0x3fa00000,
                0x00a01e46, 0x0010000a, scenario == 5 ? 1 : 0, 0, INSTRUCTION(62, 1)};
            memcpy(words + word_count, cp_body, sizeof(cp_body));
            word_count += sizeof(cp_body) / 4;
        }
        /* New authored arithmetic follows the instance-index transport.
         * The inner read control deliberately has no phase-local definition. */
        if ((scenario == 1 || scenario == 2) && word == 36) {
            const uint32_t product[] = {INSTRUCTION(56, 7), 0x00100012, 1,
                0x00004001, 0x3fc00000, 0x00004001, 0x40000000};
            memcpy(words + word_count, product, sizeof(product));
            word_count += sizeof(product) / 4;
        }
        if (scenario == 2 && base_words[word] == INSTRUCTION(54, 5) && base_words[word + 1] == 0x00102012) {
            words[word_count++] = INSTRUCTION(104, 2);
            words[word_count++] = 2;
        }
        words[word_count++] = base_words[word];
    }
    if (scenario == 1 || scenario == 2) {
        /* Locate authored DCL_TEMPS instead of depending on byte offsets. */
        for (size_t word = 0; word + 1 < word_count; ++word)
            if (words[word] == INSTRUCTION(104, 2)) words[word + 1] = 2;
        if (scenario == 1) {
            /* Replace the outer literal with the full scalar product use. */
            for (size_t word = 0; word + 5 < word_count; ++word)
                if (words[word] == INSTRUCTION(54, 6) && words[word + 1] == 0x00902012) {
                    words[word + 4] = 0x0010000a;
                    words[word + 5] = 1;
                }
        } else if (scenario == 2) {
            for (size_t word = 0; word + 4 < word_count; ++word)
                if (words[word] == INSTRUCTION(54, 5) && words[word + 1] == 0x00102012) {
                    words[word + 3] = 0x0010000a;
                    words[word + 4] = 1;
                }
        }
    }
    if (scenario == 3) {
        /* Author the same two independent phase shapes in inner/outer order,
         * with the indexed outer group beginning at register1. */
        size_t markers[2] = {0};
        unsigned marker_count = 0;
        for (size_t word = 0; word < word_count;) {
            const unsigned opcode = words[word] & 0x7ffu;
            const size_t length = words[word] >> 24;
            if (opcode == 115 && marker_count < 2) markers[marker_count++] = word;
            word += length;
        }
        if (marker_count != 2) return NULL;
        uint32_t reordered[128];
        size_t written = 0;
        const size_t first[] = {0, markers[1], markers[0]};
        const size_t end[] = {markers[0], word_count, markers[1]};
        for (unsigned section = 0; section < 3; ++section) {
            for (size_t word = first[section]; word < end[section];) {
                const unsigned opcode = words[word] & 0x7ffu;
                const size_t length = words[word] >> 24;
                if (opcode == 54 && words[word + 1] == 0x00902012) {
                    reordered[written++] = INSTRUCTION(54, 7);
                    reordered[written++] = 0x00d02012;
                    reordered[written++] = 1;
                    memcpy(reordered + written, words + word + 2, 4 * (length - 2));
                    written += length - 2;
                } else {
                    memcpy(reordered + written, words + word, 4 * length);
                    if (opcode == 103) reordered[written + 2] = words[word + 3] == 20 ? 0 : words[word + 2] + 1;
                    if (opcode == 91) reordered[written + 2] = 1;
                    if (opcode == 54 && words[word + 1] == 0x00102012) reordered[written + 2] = 0;
                    written += length;
                }
                word += length;
            }
        }
        memcpy(words, reordered, 4 * written);
        word_count = written;
    }
    const size_t word_bytes = word_count * 4;
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    write_u32(bytes + 20, 1);
    write_u32(bytes + 28, 4);
    size_t offset = 48;
    for (unsigned role = 0; role < 3; ++role) {
        write_u32(bytes + 32 + 4 * role, (uint32_t)offset);
        offset += write_hull_signature(bytes + offset, role, scenario == 3, point_semantic, float3);
        offset = (offset + 3) & ~(size_t)3;
    }
    write_u32(bytes + 44, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    write_u32(bytes + offset + 4, (uint32_t)word_bytes + 8);
    write_u32(bytes + offset + 8, 0x00030050);
    write_u32(bytes + offset + 12, (uint32_t)word_count + 2);
    for (unsigned word = 0; word < word_count; ++word)
        write_u32(bytes + offset + 16 + 4 * word, words[word]);
    *size = offset + 16 + word_bytes;
    write_u32(bytes + 24, (uint32_t)*size);
    if (!dxbc_compute_hash(bytes, *size, bytes + 4)) return NULL;
    uint8_t *result = malloc(*size);
    if (result) memcpy(result, bytes, *size);
    return result;
}

uint8_t *test_tessellation_hull_dxbc(uint32_t points, uint32_t output_points, uint8_t scenario, size_t *size) {
    return make_hull_dxbc(points, output_points, scenario, "SV_POSITION", false, size);
}

uint8_t *test_tessellation_hull_float3_dxbc(uint32_t points, uint32_t output_points,
    uint8_t scenario, const char *semantic, size_t *size) {
    return make_hull_dxbc(points, output_points, scenario, semantic, true, size);
}

/* Independently authored quad/isoline token grammars exercise the generic
 * descriptor producer, including distinct raw per-edge names. */
static size_t shape_signature(uint8_t *bytes, unsigned role, bool isoline) {
    if (role != 2) return test_tessellation_hull_signature(bytes, role, false);
    const unsigned count = isoline ? 2 : 6;
    const size_t names = 8 + 24 * count;
    const size_t size = names + sizeof("SV_TessFactor") + (isoline ? 0 : sizeof("SV_InsideTessFactor"));
    memcpy(bytes, "PCSG", 4);
    write_u32(bytes + 4, (uint32_t)size);
    bytes += 8;
    write_u32(bytes, count);
    write_u32(bytes + 4, 8);
    for (unsigned field = 0; field < count; ++field) {
        const bool inner = !isoline && field >= 4;
        uint8_t *element = bytes + 8 + 24 * field;
        write_u32(element, (uint32_t)(names + (inner ? sizeof("SV_TessFactor") : 0)));
        write_u32(element + 4, inner ? field - 4 : field);
        write_u32(element + 8, isoline ? (field ? 15 : 16) : inner ? 12 : 11);
        write_u32(element + 12, 3);
        write_u32(element + 16, field);
        write_u32(element + 20, 0x0e01);
    }
    memcpy(bytes + names, "SV_TessFactor", sizeof("SV_TessFactor"));
    if (!isoline) memcpy(bytes + names + sizeof("SV_TessFactor"),
        "SV_InsideTessFactor", sizeof("SV_InsideTessFactor"));
    return size + 8;
}

uint8_t *test_tessellation_hull_shape_dxbc(bool isoline, size_t *size) {
    uint32_t words[128];
    size_t count = 0;
#define WORD(value) words[count++] = (uint32_t)(value)
    const unsigned points = isoline ? 2 : 4;
    WORD(INSTRUCTION(113, 1));
    WORD(INSTRUCTION(147, 1) | points << 11);
    WORD(INSTRUCTION(148, 1) | points << 11);
    WORD(INSTRUCTION(149, 1) | (isoline ? 1u : 3u) << 11);
    WORD(INSTRUCTION(150, 1) | 1u << 11);
    WORD(INSTRUCTION(151, 1) | (isoline ? 2u : 3u) << 11);
    WORD(INSTRUCTION(152, 2)); WORD(0x42000000);
    WORD(INSTRUCTION(106, 1) | 1u << 11);
    for (unsigned phase = 0; phase < (isoline ? 1u : 2u); ++phase) {
        const unsigned factors = isoline ? 2 : phase ? 2 : 4;
        const unsigned base = phase ? 4 : 0;
        WORD(INSTRUCTION(115, 1));
        WORD(INSTRUCTION(153, 2)); WORD(factors);
        WORD(INSTRUCTION(95, 2)); WORD(0x00017000);
        for (unsigned factor = 0; factor < factors; ++factor) {
            const unsigned raw_name = isoline ? (factor ? 21 : 22) : phase ? 15 + factor : 11 + factor;
            WORD(INSTRUCTION(103, 4)); WORD(0x00102012); WORD(base + factor); WORD(raw_name);
        }
        WORD(INSTRUCTION(104, 2)); WORD(1);
        WORD(INSTRUCTION(91, 4)); WORD(0x00102012); WORD(base); WORD(factors);
        WORD(INSTRUCTION(54, 4)); WORD(0x00100012); WORD(0); WORD(0x0001700a);
        WORD(INSTRUCTION(54, base ? 7 : 6)); WORD(base ? 0x00d02012 : 0x00902012);
        if (base) WORD(base);
        WORD(0x0010000a); WORD(0); WORD(0x00004001); WORD(phase ? 0x40400000 : 0x40000000);
        WORD(INSTRUCTION(62, 1));
    }
#undef WORD
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    write_u32(bytes + 20, 1);
    write_u32(bytes + 28, 4);
    size_t offset = 48;
    for (unsigned role = 0; role < 3; ++role) {
        write_u32(bytes + 32 + 4 * role, (uint32_t)offset);
        offset += shape_signature(bytes + offset, role, isoline);
        offset = (offset + 3) & ~(size_t)3;
    }
    write_u32(bytes + 44, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    write_u32(bytes + offset + 4, (uint32_t)(4 * count + 8));
    write_u32(bytes + offset + 8, 0x00030050);
    write_u32(bytes + offset + 12, (uint32_t)(count + 2));
    for (size_t word = 0; word < count; ++word) write_u32(bytes + offset + 16 + 4 * word, words[word]);
    *size = offset + 16 + 4 * count;
    write_u32(bytes + 24, (uint32_t)*size);
    if (!dxbc_compute_hash(bytes, *size, bytes + 4)) return NULL;
    uint8_t *result = malloc(*size);
    if (result) memcpy(result, bytes, *size);
    return result;
}

/* Reuse the authored signatures and instruction grammar above. The lossless
 * decoder supplies bounded instruction coordinates; this existing transform
 * adds one scalar buffer and two final clamps without another token parser. */
uint8_t *test_tessellation_hull_scalar_cbuffer_dxbc(uint32_t input_points,
    uint32_t output_points, bool float3, const char *semantic, size_t *size) {
    if (!size) return NULL;
    size_t original_size = 0;
    uint8_t *bytes = float3
        ? test_tessellation_hull_float3_dxbc(input_points, output_points, 0, semantic, &original_size)
        : test_tessellation_hull_dxbc(input_points, output_points, 0, &original_size);
    if (!bytes) return NULL;
    DXBCDocument document;
    DXBCDocumentDiagnostic diagnostic;
    dxbc_document_init(&document);
    uint8_t *authored = NULL;
    if (!dxbc_document_parse(&document, bytes, original_size, &diagnostic) || !document.instruction_count) goto fail;
    const DXBCDocumentChunk *chunk = &document.chunks[document.instructions[0].chunk_index];
    if (chunk->kind != DXBC_DOCUMENT_CHUNK_EXECUTABLE || chunk->offset + chunk->raw_size != original_size) goto fail;
    uint32_t words[128];
    size_t count = 0;
    bool declared = false;
    unsigned clamps = 0;
    for (size_t index = 0; index < document.instruction_count; ++index) {
        const DXBCDocumentInstruction *instruction = &document.instructions[index];
        if (instruction->chunk_index != document.instructions[0].chunk_index ||
            instruction->token_count >= 32 || count + instruction->token_count + 8 >= 128) goto fail;
        if (instruction->opcode == 115 && !declared) {
            words[count++] = INSTRUCTION(89, 4);
            words[count++] = UINT32_C(0x00208000);
            words[count++] = 0;
            words[count++] = 1;
            declared = true;
        }
        const uint32_t destination = instruction->token_count > 1 ? read_u32(instruction->raw_bytes + 4) : 0;
        if (instruction->opcode == 54 &&
            (destination == UINT32_C(0x00902012) || destination == UINT32_C(0x00102012))) {
            if (instruction->token_count != (destination == UINT32_C(0x00902012) ? 6u : 5u)) goto fail;
            words[count++] = INSTRUCTION(51, instruction->token_count + 3);
            for (uint32_t word = 1; word < instruction->token_count - 2; ++word)
                words[count++] = read_u32(instruction->raw_bytes + word * 4);
            words[count++] = UINT32_C(0x0020800a);
            words[count++] = 0;
            words[count++] = 0;
            words[count++] = UINT32_C(0x00004001);
            words[count++] = UINT32_C(0x42000000);
            ++clamps;
        } else {
            for (uint32_t word = 0; word < instruction->token_count; ++word)
                words[count++] = read_u32(instruction->raw_bytes + word * 4);
        }
    }
    if (!declared || clamps != 2) goto fail;
    const size_t instruction_offset = (size_t)chunk->offset + 16;
    const size_t authored_size = instruction_offset + count * 4;
    authored = calloc(authored_size, 1);
    if (!authored) goto fail;
    memcpy(authored, bytes, instruction_offset);
    write_u32(authored + 24, (uint32_t)authored_size);
    write_u32(authored + chunk->offset + 4, (uint32_t)(count * 4 + 8));
    write_u32(authored + chunk->offset + 12, (uint32_t)(count + 2));
    for (size_t word = 0; word < count; ++word)
        write_u32(authored + instruction_offset + word * 4, words[word]);
    if (!dxbc_compute_hash(authored, authored_size, authored + 4)) goto fail;
    dxbc_document_free(&document);
    free(bytes);
    *size = authored_size;
    return authored;
fail:
    dxbc_document_free(&document);
    free(bytes);
    free(authored);
    return NULL;
}


/* Transform only this file's controlled grammar. DXBCDocument supplies every
 * instruction boundary; the selected fixed operands are authored coordinates,
 * not a second parser for arbitrary DXBC. */
uint8_t *test_tessellation_hull_icb_dxbc(unsigned rows, unsigned column,
    unsigned chain_length, unsigned transport_lane, bool zero_base,
    bool scalar, bool float3, const uint32_t *values, size_t *size) {
    if (!size) return NULL;
    *size = 0;
    if (rows < 2 || rows > 4 || column > 3 || chain_length > 3 ||
        transport_lane > 3 || !values || ((scalar || float3) && rows != 3)) return NULL;
    size_t original_size = 0;
    uint8_t *bytes = rows != 3 ? test_tessellation_hull_shape_dxbc(rows == 2, &original_size)
        : scalar ? test_tessellation_hull_scalar_cbuffer_dxbc(3, 3, float3, "POINTVALUE", &original_size)
        : float3 ? test_tessellation_hull_float3_dxbc(3, 3, 0, "POINTVALUE", &original_size)
        : test_tessellation_hull_dxbc(3, 3, 0, &original_size);
    if (!bytes) return NULL;
    DXBCDocument document;
    dxbc_document_init(&document);
    uint8_t *authored = NULL;
    if (!dxbc_document_parse(&document, bytes, original_size, NULL) || !document.instruction_count) goto fail;
    const DXBCDocumentChunk *chunk = &document.chunks[document.instructions[0].chunk_index];
    if (chunk->kind != DXBC_DOCUMENT_CHUNK_EXECUTABLE || chunk->offset + chunk->raw_size != original_size) goto fail;
    uint32_t words[256];
    size_t count = 0;
    unsigned phase_count = 0, consumers = 0;
    bool declared = false;
#define ICB_WORD(value) do { if (count == 256) goto fail; words[count++] = (uint32_t)(value); } while (0)
    for (size_t index = 0; index < document.instruction_count; ++index) {
        const DXBCDocumentInstruction *instruction = &document.instructions[index];
        if (instruction->chunk_index != document.instructions[0].chunk_index || instruction->token_count > 32) goto fail;
        if (instruction->opcode == 115) ++phase_count;
        const uint32_t destination = instruction->token_count > 1 ? read_u32(instruction->raw_bytes + 4) : 0;
        if (phase_count == 1 && (instruction->opcode == 54 || instruction->opcode == 51) &&
            destination == UINT32_C(0x00902012)) {
            const unsigned source_words = scalar ? 3 : 2;
            if (instruction->token_count != (scalar ? 9u : 6u)) goto fail;
            for (unsigned edge = 0; edge < chain_length; ++edge) {
                const unsigned lane = (transport_lane + edge) % 4;
                ICB_WORD(INSTRUCTION(54, edge ? 5 : 4));
                ICB_WORD(UINT32_C(0x00100002) | (1u << lane) << 4);
                ICB_WORD(edge + 1);
                if (edge) {
                    ICB_WORD(UINT32_C(0x0010000a) | ((transport_lane + edge - 1) % 4) << 4);
                    ICB_WORD(edge);
                } else ICB_WORD(UINT32_C(0x0001700a));
            }
            const unsigned operand_words = (zero_base ? 1 : 0) + (chain_length ? 3 : 2);
            ICB_WORD(INSTRUCTION(instruction->opcode, instruction->token_count - source_words + operand_words));
            for (unsigned word = 1; word < 4; ++word)
                ICB_WORD(read_u32(instruction->raw_bytes + word * 4));
            ICB_WORD((zero_base ? UINT32_C(0x00d0900a) : UINT32_C(0x0090900a)) | column << 4);
            if (zero_base) ICB_WORD(0);
            if (chain_length) {
                ICB_WORD(UINT32_C(0x0010000a) | ((transport_lane + chain_length - 1) % 4) << 4);
                ICB_WORD(chain_length);
            } else ICB_WORD(UINT32_C(0x0001700a));
            for (unsigned word = 4 + source_words; word < instruction->token_count; ++word)
                ICB_WORD(read_u32(instruction->raw_bytes + word * 4));
            ++consumers;
        } else if (phase_count == 1 && instruction->opcode == 104) {
            if (instruction->token_count != 2) goto fail;
            ICB_WORD(INSTRUCTION(104, 2)); ICB_WORD(chain_length + 1);
        } else {
            for (uint32_t word = 0; word < instruction->token_count; ++word)
                ICB_WORD(read_u32(instruction->raw_bytes + word * 4));
        }
        if (!declared && instruction->opcode == 113) {
            ICB_WORD(53u | 3u << 11); ICB_WORD(rows * 4 + 2);
            for (unsigned row = 0; row < rows; ++row)
                for (unsigned component = 0; component < 4; ++component)
                    ICB_WORD(component == column ? values[row] : 0);
            declared = true;
        }
    }
#undef ICB_WORD
    if (!declared || consumers != 1) goto fail;
    const size_t instruction_offset = (size_t)chunk->offset + 16;
    const size_t authored_size = instruction_offset + count * 4;
    authored = calloc(authored_size, 1);
    if (!authored) goto fail;
    memcpy(authored, bytes, instruction_offset);
    write_u32(authored + 24, (uint32_t)authored_size);
    write_u32(authored + chunk->offset + 4, (uint32_t)(count * 4 + 8));
    write_u32(authored + chunk->offset + 12, (uint32_t)(count + 2));
    for (size_t word = 0; word < count; ++word) write_u32(authored + instruction_offset + word * 4, words[word]);
    if (!dxbc_compute_hash(authored, authored_size, authored + 4)) goto fail;
    dxbc_document_free(&document);
    free(bytes);
    *size = authored_size;
    return authored;
fail:
    dxbc_document_free(&document);
    free(bytes);
    free(authored);
    return NULL;
}

/* Authored token grammar and signatures, with no captured byte array. */
static size_t write_domain_signature(uint8_t *bytes, unsigned role, unsigned domain) {
    const bool patch = role == 2;
    const unsigned outer = domain == 2 ? 3 : domain == 3 ? 4 : 2;
    const unsigned inner = domain == 2 ? 1 : domain == 3 ? 2 : 0;
    const unsigned count = patch ? outer + inner : 1;
    const char *semantic = patch ? "SV_TessFactor" : "SV_POSITION";
    const size_t name_offset = 8 + count * 24;
    const size_t size = name_offset + strlen(semantic) + 1 +
        (patch ? sizeof("SV_InsideTessFactor") : 0);
    memcpy(bytes, patch ? "PCSG" : role ? "OSGN" : "ISGN", 4);
    write_u32(bytes + 4, (uint32_t)size);
    bytes += 8;
    write_u32(bytes, count);
    write_u32(bytes + 4, 8);
    for (unsigned field = 0; field < count; ++field) {
        uint8_t *element = bytes + 8 + 24 * field;
        write_u32(element, (uint32_t)(name_offset +
            (patch && field >= outer ? strlen(semantic) + 1 : 0)));
        write_u32(element + 4, patch ? (field < outer ? field : field - outer) : 0);
        const unsigned system = domain == 2 ? (field < outer ? 13 : 14) :
            domain == 3 ? (field < outer ? 11 : 12) : (field ? 15 : 16);
        write_u32(element + 8, patch ? system : 1);
        write_u32(element + 12, 3);
        write_u32(element + 16, patch ? field : 0);
        write_u32(element + 20, patch ? 1 : role ? 15 : 0x0f0f);
    }
    memcpy(bytes + name_offset, semantic, strlen(semantic) + 1);
    if (patch)
        memcpy(bytes + name_offset + strlen(semantic) + 1,
               "SV_InsideTessFactor", sizeof("SV_InsideTessFactor"));
    return size + 8;
}

uint8_t *test_tessellation_domain_dxbc(unsigned domain, uint32_t points, uint8_t location_mask,
                                     size_t *size) {
    const unsigned coordinate_y = domain == 1 ? 0 : 1;
    const unsigned coordinate_z = domain == 2 ? 2 : 0;
    const uint32_t words[] = {
        INSTRUCTION(147, 1) | (points << 11),
        INSTRUCTION(149, 1) | ((uint32_t)domain << 11),
        INSTRUCTION(106, 1) | (1u << 11),
        INSTRUCTION(95, 2), 0x0001c002u | (uint32_t)location_mask << 4,
        INSTRUCTION(95, 4), 0x002190f2, points, 0,
        INSTRUCTION(103, 4), 0x001020f2, 0, 1,
        INSTRUCTION(104, 2), 1,
        INSTRUCTION(56, 7), 0x001000f2, 0, 0x0001c006u | ((uint32_t)coordinate_y * 0x55u << 4),
            0x00219e46, 1, 0,
        INSTRUCTION(50, 9), 0x001000f2, 0, 0x00219e46, 0, 0,
            0x0001c006, 0x00100e46, 0,
        INSTRUCTION(50, 9), 0x001020f2, 0, 0x00219e46, points == 2 ? 1u : 2u, 0,
            0x0001c006u | ((uint32_t)coordinate_z * 0x55u << 4), 0x00100e46, 0,
        INSTRUCTION(62, 1)
    };
    uint8_t bytes[1024] = {0};
    memcpy(bytes, "DXBC", 4);
    write_u32(bytes + 20, 1);
    write_u32(bytes + 28, 4);
    size_t offset = 48;
    for (unsigned role = 0; role < 3; ++role) {
        write_u32(bytes + 32 + 4 * role, (uint32_t)offset);
        offset += write_domain_signature(bytes + offset, role, domain);
        offset = (offset + 3) & ~(size_t)3;
    }
    write_u32(bytes + 44, (uint32_t)offset);
    memcpy(bytes + offset, "SHEX", 4);
    write_u32(bytes + offset + 4, sizeof(words) + 8);
    write_u32(bytes + offset + 8, 0x00040050);
    write_u32(bytes + offset + 12, sizeof(words) / 4 + 2);
    for (unsigned word = 0; word < sizeof(words) / 4; ++word)
        write_u32(bytes + offset + 16 + 4 * word, words[word]);
    *size = offset + 16 + sizeof(words);
    write_u32(bytes + 24, (uint32_t)*size);
    if (!dxbc_compute_hash(bytes, *size, bytes + 4)) return NULL;
    uint8_t *result = malloc(*size);
    if (result) memcpy(result, bytes, *size);
    return result;
}
