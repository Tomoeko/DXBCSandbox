// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_parser_internal.h"
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* dxbc_reg_name(uint32_t type) {
    switch (type) {
        case OPERAND_TYPE_TEMP: return "r";
        case OPERAND_TYPE_INPUT: return "v";
        case OPERAND_TYPE_OUTPUT: return "o";
        case OPERAND_TYPE_INDEXABLE_TEMP: return "x";
        case OPERAND_TYPE_SAMPLER: return "s";
        case OPERAND_TYPE_RESOURCE: return "t";
        case OPERAND_TYPE_CONSTANT_BUFFER: return "cb";
        case OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER: return "icb";
        case OPERAND_TYPE_LABEL: return "l";
        case OPERAND_TYPE_INPUT_PRIMITIVE_ID: return "vPrim";
        case OPERAND_TYPE_OUTPUT_DEPTH: return "oDepth";
        case OPERAND_TYPE_NULL: return "null";
        case OPERAND_TYPE_RASTERIZER: return "rasterizer";
        case OPERAND_TYPE_OUTPUT_COVERAGE_MASK: return "oMask";
        case OPERAND_TYPE_STREAM: return "m";
        case OPERAND_TYPE_FUNCTION_BODY: return "fb";
        case OPERAND_TYPE_FUNCTION_TABLE: return "ft";
        case OPERAND_TYPE_INTERFACE: return "fp";
        case OPERAND_TYPE_FUNCTION_INPUT: return "fi";
        case OPERAND_TYPE_FUNCTION_OUTPUT: return "fo";
        case OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID:
            return "vOutputControlPointID";
        case OPERAND_TYPE_FORK_INSTANCE_ID: return "vForkInstanceID";
        case OPERAND_TYPE_JOIN_INSTANCE_ID: return "vJoinInstanceID";
        case OPERAND_TYPE_INPUT_CONTROL_POINT: return "vicp";
        case OPERAND_TYPE_OUTPUT_CONTROL_POINT: return "vocp";
        case OPERAND_TYPE_INPUT_PATCH_CONSTANT: return "vpc";
        case OPERAND_TYPE_DOMAIN_LOCATION: return "vDomain";
        case OPERAND_TYPE_THIS_POINTER: return "this";
        case OPERAND_TYPE_UAV: return "u";
        case OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY: return "g";
        case OPERAND_TYPE_INPUT_THREAD_ID: return "vThreadID";
        case OPERAND_TYPE_INPUT_THREAD_GROUP_ID: return "vThreadGroupID";
        case OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP:
            return "vThreadIDInGroup";
        case OPERAND_TYPE_INPUT_COVERAGE_MASK: return "vCoverage";
        case OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED:
            return "vThreadIDInGroupFlattened";
        case OPERAND_TYPE_INPUT_GS_INSTANCE_ID: return "vGSInstanceID";
        case OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL: return "oDepthGE";
        case OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL: return "oDepthLE";
        case OPERAND_TYPE_CYCLE_COUNTER: return "vCycleCounter";
        case OPERAND_TYPE_OUTPUT_STENCIL_REF: return "oStencilRef";
        case OPERAND_TYPE_INNER_COVERAGE: return "vInnerCoverage";
        default: return "r";
    }
}

static bool is_float_only_opcode(uint32_t opcode) {
    switch (opcode) {
        case 0:   /* ADD */
        case 11:  /* DERIV_RTX */
        case 12:  /* DERIV_RTY */
        case 14:  /* DIV */
        case 15:  /* DP2 */
        case 16:  /* DP3 */
        case 17:  /* DP4 */
        case 24:  /* EQ */
        case 25:  /* EXP */
        case 29:  /* GE */
        case 47:  /* LOG */
        case 49:  /* LT */
        case 50:  /* MAD */
        case 51:  /* MIN */
        case 52:  /* MAX */
        case 56:  /* MUL */
        case 57:  /* NE */
        case 64:  /* ROUND_NE */
        case 65:  /* ROUND_NI */
        case 66:  /* ROUND_PI */
        case 67:  /* ROUND_Z */
        case 68:  /* RSQ */
        case 69:  /* SAMPLE */
        case 70:  /* SAMPLE_C */
        case 71:  /* SAMPLE_C_LZ */
        case 72:  /* SAMPLE_L */
        case 73:  /* SAMPLE_D */
        case 74:  /* SAMPLE_B */
        case 75:  /* SQRT */
        case 77:  /* SINCOS */
        case 109: /* GATHER4 */
        case 110: /* SAMPLE_POS */
        case 111: /* SAMPLEINFO */
        case 122: /* DERIV_RTX_COARSE */
        case 123: /* DERIV_RTX_FINE */
        case 124: /* DERIV_RTY_COARSE */
        case 125: /* DERIV_RTY_FINE */
            return true;
        default:
            return false;
    }
}

static bool is_integer_immediate_opcode(uint32_t opcode) {
    switch (opcode) {
        case 1:  /* AND */
        case 6:  /* CASE */
        case 30: /* IADD */
        case 32: /* IEQ */
        case 33: /* IGE */
        case 34: /* ILT */
        case 35: /* IMAD */
        case 36: /* IMAX */
        case 37: /* IMIN */
        case 38: /* IMUL */
        case 39: /* INE */
        case 40: /* INEG */
        case 41: /* ISHL */
        case 42: /* ISHR */
        case 43: /* ITOF */
        case 59: /* NOT */
        case 60: /* OR */
        case 78: /* UDIV */
        case 79: /* ULT */
        case 80: /* UGE */
        case 81: /* UMUL */
        case 82: /* UMAD */
        case 83: /* UMAX */
        case 84: /* UMIN */
        case 85: /* USHR */
        case 86: /* UTOF */
        case 87: /* XOR */
            return true;
        default:
            return false;
    }
}

void dxbc_format_immediate_diagnostic(uint32_t u, uint32_t opcode,
                                      bool isInt, char* out_buf,
                                      size_t out_sz) {
    bool is_float = false;
    
    if (isInt) {
        is_float = false;
    } else if (is_float_only_opcode(opcode)) {
        is_float = true;
    } else {
        uint32_t exp = (u >> 23) & 0xFF;
        if (u == 0x80000000) {
            is_float = true;
        } else if (exp == 0 || exp == 0xFF) {
            is_float = false;
        } else {
            is_float = true;
        }
    }
    
    if (is_float) {
        float f;
        memcpy(&f, &u, sizeof(f));
        if (f == 0.0f) {
            if (u & 0x80000000) {
                snprintf(out_buf, out_sz, "-0.000000");
            } else {
                snprintf(out_buf, out_sz, "0.000000");
            }
        } else {
            double val = f;
            if (val > -9e12 && val < 9e12) {
                if (val >= 0.0) {
                    val = (double)((long long)(val * 1000000.0 + 0.5)) / 1000000.0;
                } else {
                    val = (double)((long long)(val * 1000000.0 - 0.5)) / 1000000.0;
                }
            }
            snprintf(out_buf, out_sz, "%f", val);
        }
    } else {
        bool is_bitwise = opcode == 1u || opcode == 59u || opcode == 60u ||
                          opcode == 87u;
        if (is_bitwise) {
            if (u > 65535 || (u & 0x80000000) != 0) {
                snprintf(out_buf, out_sz, "0x%08x", u);
            } else {
                snprintf(out_buf, out_sz, "%d", (int)u);
            }
        } else {
            int s_val = (int)u;
            if (s_val < -65535 || s_val > 65535) {
                snprintf(out_buf, out_sz, "0x%08x", u);
            } else {
                snprintf(out_buf, out_sz, "%d", s_val);
            }
        }
    }
}

static bool operand_read_u32(ByteStream* stream, size_t limit, uint32_t* value) {
    return stream && stream->position <= limit &&
           limit - stream->position >= sizeof(uint32_t) &&
           stream_read_uint32(stream, value);
}

static bool append_extended_token(DXBCOperand* op, uint32_t token) {
    if (op->extended_token_count == SIZE_MAX / sizeof(uint32_t)) return false;
    size_t old_size = op->extended_token_count * sizeof(uint32_t);
    size_t new_count = op->extended_token_count + 1;
    uint32_t* tokens = (uint32_t*)mem_realloc(
        op->extended_tokens, old_size, new_count * sizeof(uint32_t));
    if (!tokens) return false;
    op->extended_tokens = tokens;
    op->extended_tokens[op->extended_token_count] = token;
    op->extended_token_count = new_count;
    return true;
}

static DXBCOperand** relative_operand_slot(DXBCOperand* op, int dimension) {
    if (dimension == 0) return &op->rel_op0;
    if (dimension == 1) return &op->rel_op1;
    return &op->rel_op2;
}

static void sync_legacy_index(DXBCOperand* op, int dimension) {
    uint64_t value = op->index_values[dimension];
    int legacy_value = value <= (uint64_t)INT_MAX ? (int)value : -1;
    op->index_value_exceeds_int[dimension] = value > (uint64_t)INT_MAX;
    if (dimension == 0) {
        op->register_index = legacy_value;
        op->rel_offset1 = legacy_value;
    } else if (dimension == 1) {
        op->rel_offset0 = legacy_value;
    } else {
        op->rel_offset2 = legacy_value;
    }
}

static DXBCOperand* operand_parse_failed(ByteStream* stream, size_t limit,
                                         DXBCOperand* op) {
    /* The caller must never reinterpret a partial operand as another operand.
     * Advancing to the operand boundary makes that property explicit even for
     * malformed nested relative-address operands. */
    if (stream && limit <= stream->size) stream->position = limit;
    if (op) {
        free_operand(op);
        mem_free(op, sizeof(DXBCOperand));
    }
    return NULL;
}

static void format_operand_text(char* output, size_t output_size,
                                const char* format, ...) {
    if (!output || output_size == 0U || !format) return;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(output, output_size, format, arguments);
    va_end(arguments);
    if (written < 0) {
        output[0] = '\0';
        return;
    }
    if ((size_t)written >= output_size) {
        /* Presentation is deliberately bounded and best-effort.  Mark a
         * shortened diagnostic, but never turn a valid token stream into a
         * grammar failure because a human-readable rendering is longer than
         * the legacy inline field. */
        if (output_size >= 4u) {
            memcpy(output + output_size - 4u, "...", 4u);
        } else {
            output[output_size - 1u] = '\0';
        }
    }
}

static void format_index_component(const DXBCOperand* op, int dimension,
                                   char* output, size_t output_size) {
    const DXBCOperand* relative = dimension == 0 ? op->rel_op0 :
                                  dimension == 1 ? op->rel_op1 : op->rel_op2;
    if (relative && op->index_has_immediate[dimension]) {
        format_operand_text(
            output, output_size, "%s + %" PRIu64, relative->text,
            op->index_values[dimension]);
    } else if (relative) {
        format_operand_text(output, output_size, "%s", relative->text);
    } else {
        format_operand_text(output, output_size, "%" PRIu64,
                            op->index_values[dimension]);
    }
}

static DXBCOperand* parse_operand_recursive_impl(
    ByteStream* stream, size_t limit, DXBCOperandParseContext context,
    uint32_t opcode, size_t* operand_token_budget) {
    if (!stream || !operand_token_budget || *operand_token_budget == 0u ||
        limit > stream->size || stream->position > limit ||
        limit - stream->position < sizeof(uint32_t)) {
        if (stream && limit <= stream->size) stream->position = limit;
        return NULL;
    }
    --*operand_token_budget;

    uint32_t token0;
    if (!operand_read_u32(stream, limit, &token0)) return NULL;

    DXBCOperand* op = (DXBCOperand*)mem_alloc(sizeof(DXBCOperand));
    if (!op) {
        stream->position = limit;
        return NULL;
    }
    memset(op, 0, sizeof(DXBCOperand));
    op->raw_token = token0;

    int modifier = 0;
    bool saw_modifier = false;
    bool has_more_extensions = (token0 & 0x80000000u) != 0;
    while (has_more_extensions) {
        uint32_t ext;
        if (!operand_read_u32(stream, limit, &ext) ||
            !append_extended_token(op, ext)) {
            return operand_parse_failed(stream, limit, op);
        }
        uint32_t extension_type = ext & 0x3fu;
        if (extension_type == 1) {
            uint32_t decoded_modifier = (ext >> 6) & 0xffu;
            uint32_t min_precision = (ext >> 14) & 0x7u;
            if (saw_modifier || decoded_modifier > 3u ||
                (min_precision != 0u && min_precision != 1u &&
                 min_precision != 2u && min_precision != 4u &&
                 min_precision != 5u)) {
                return operand_parse_failed(stream, limit, op);
            }
            saw_modifier = true;
            modifier = (int)decoded_modifier;
            op->min_precision = (uint8_t)min_precision;
        } else if (extension_type != 0) {
            /* Unknown extension semantics cannot be translated faithfully.
             * The raw token is retained for diagnostics before failing. */
            return operand_parse_failed(stream, limit, op);
        }
        has_more_extensions = (ext & 0x80000000u) != 0;
    }

    uint32_t type = (token0 >> 12) & 0xffu;
    uint32_t component_count_code = token0 & 0x3u;
    int index_dim = (int)((token0 >> 20) & 0x3u);
    uint32_t selection_mode = (token0 >> 2) & 0x3u;

    if (component_count_code == 3u ||
        (component_count_code == 2u && selection_mode == 3u)) {
        return operand_parse_failed(stream, limit, op);
    }

    op->type = (DXBCOperandType)type;
    op->register_index_dim = index_dim;
    op->has_neg = (modifier & 0x1) != 0;
    op->has_abs = (modifier & 0x2) != 0;
    op->swizzle_mode = (uint8_t)selection_mode;
    op->swizzle[0] = 0;
    op->swizzle[1] = 1;
    op->swizzle[2] = 2;
    op->swizzle[3] = 3;
    if (selection_mode == 0 && type != OPERAND_TYPE_IMMEDIATE32 &&
        type != OPERAND_TYPE_IMMEDIATE64) {
        op->destination_mask = token0 & 0xf0u;
    }

    char reg[128] = "";
    if (type == OPERAND_TYPE_IMMEDIATE32 ||
        type == OPERAND_TYPE_IMMEDIATE64) {
        if (component_count_code != 1u && component_count_code != 2u) {
            return operand_parse_failed(stream, limit, op);
        }
        int encoded_component_count = component_count_code == 1u ? 1 : 4;
        int words_per_component = type == OPERAND_TYPE_IMMEDIATE64 ? 2 : 1;
        int word_count = encoded_component_count * words_per_component;
        op->immediate_word_count = word_count;
        for (int word = 0; word < word_count; ++word) {
            if (!operand_read_u32(stream, limit, &op->immediate_words[word])) {
                return operand_parse_failed(stream, limit, op);
            }
        }

        if (type == OPERAND_TYPE_IMMEDIATE32) {
            uint32_t raw_values[4] = {0};
            for (int component = 0; component < encoded_component_count;
                 ++component) {
                raw_values[component] = op->immediate_words[component];
            }
            if (encoded_component_count == 1) {
                op->imm_value_count = 1;
                for (int component = 0; component < 4; ++component)
                    op->imm_values[component] = raw_values[0];
            } else if (selection_mode == 1u) {
                op->imm_value_count = 4;
                for (int component = 0; component < 4; ++component) {
                    int selected = (int)((token0 >> (4 + 2 * component)) & 3u);
                    op->imm_values[component] = raw_values[selected];
                }
            } else if (selection_mode == 2u) {
                int selected = (int)((token0 >> 4) & 3u);
                op->imm_value_count = 1;
                for (int component = 0; component < 4; ++component)
                    op->imm_values[component] = raw_values[selected];
            } else {
                op->imm_value_count = 4;
                memcpy(op->imm_values, raw_values, sizeof(raw_values));
            }

            bool is_int = is_integer_immediate_opcode(opcode);
            if (op->imm_value_count == 1) {
                char value[64];
                dxbc_format_immediate_diagnostic(
                    op->imm_values[0], opcode, is_int, value,
                    sizeof(value));
                format_operand_text(reg, sizeof(reg), "l(%s)", value);
            } else {
                char values[4][64];
                for (int component = 0; component < 4; ++component) {
                    dxbc_format_immediate_diagnostic(
                        op->imm_values[component], opcode, is_int,
                        values[component], sizeof(values[component]));
                }
                const char* separator =
                    opcode == 54u || opcode == 55u ? "," : ", ";
                format_operand_text(reg, sizeof(reg),
                                    "l(%s%s%s%s%s%s%s)", values[0],
                                    separator, values[1], separator,
                                    values[2], separator, values[3]);
            }
        } else {
            uint64_t raw_values[4] = {0};
            for (int component = 0; component < encoded_component_count;
                 ++component) {
                uint32_t lo = op->immediate_words[component * 2];
                uint32_t hi = op->immediate_words[component * 2 + 1];
                raw_values[component] = (uint64_t)lo | ((uint64_t)hi << 32);
            }
            if (encoded_component_count == 1) {
                op->imm_value_count = 1;
                for (int component = 0; component < 4; ++component)
                    op->imm64_values[component] = raw_values[0];
            } else if (selection_mode == 1u) {
                op->imm_value_count = 4;
                for (int component = 0; component < 4; ++component) {
                    int selected = (int)((token0 >> (4 + 2 * component)) & 3u);
                    op->imm64_values[component] = raw_values[selected];
                }
            } else if (selection_mode == 2u) {
                int selected = (int)((token0 >> 4) & 3u);
                op->imm_value_count = 1;
                for (int component = 0; component < 4; ++component)
                    op->imm64_values[component] = raw_values[selected];
            } else {
                op->imm_value_count = 4;
                memcpy(op->imm64_values, raw_values, sizeof(raw_values));
            }
            if (op->imm_value_count == 1) {
                format_operand_text(reg, sizeof(reg),
                                    "d(0x%016" PRIx64 ")",
                                    op->imm64_values[0]);
            } else {
                format_operand_text(
                    reg, sizeof(reg),
                    "d(0x%016" PRIx64 ", 0x%016" PRIx64
                    ", 0x%016" PRIx64 ", 0x%016" PRIx64 ")",
                    op->imm64_values[0], op->imm64_values[1],
                    op->imm64_values[2], op->imm64_values[3]);
            }
        }
    } else {
        if (type == OPERAND_TYPE_NULL) {
            strcpy(reg, index_dim == 0 ? "null" : "u");
        } else {
            strncpy(reg, dxbc_reg_name(type), sizeof(reg) - 1);
            reg[sizeof(reg) - 1] = '\0';
        }
        if (context == DXBC_OPERAND_CONTEXT_CONSTANT_BUFFER_DECLARATION ||
            opcode == 89u) {
            strcpy(reg, "CB");
        }
    }

    for (int dimension = 0; dimension < index_dim; ++dimension) {
        uint32_t representation =
            (token0 >> (22 + 3 * dimension)) & 0x7u;
        op->index_representations[dimension] = (uint8_t)representation;
        DXBCOperand** relative = relative_operand_slot(op, dimension);
        uint32_t lo = 0;
        uint32_t hi = 0;

        switch (representation) {
            case 0: /* immediate32 */
                if (!operand_read_u32(stream, limit, &lo))
                    return operand_parse_failed(stream, limit, op);
                op->index_values[dimension] = lo;
                op->index_has_immediate[dimension] = true;
                break;
            case 1: /* immediate64, low DWORD followed by high DWORD */
                if (!operand_read_u32(stream, limit, &lo) ||
                    !operand_read_u32(stream, limit, &hi))
                    return operand_parse_failed(stream, limit, op);
                op->index_values[dimension] =
                    (uint64_t)lo | ((uint64_t)hi << 32);
                op->index_has_immediate[dimension] = true;
                break;
            case 2: /* relative operand */
                *relative = parse_operand_recursive_impl(
                    stream, limit, DXBC_OPERAND_CONTEXT_RELATIVE_INDEX,
                    opcode, operand_token_budget);
                if (!*relative)
                    return operand_parse_failed(stream, limit, op);
                break;
            case 3: /* immediate32 plus relative operand */
                if (!operand_read_u32(stream, limit, &lo))
                    return operand_parse_failed(stream, limit, op);
                op->index_values[dimension] = lo;
                op->index_has_immediate[dimension] = true;
                *relative = parse_operand_recursive_impl(
                    stream, limit, DXBC_OPERAND_CONTEXT_RELATIVE_INDEX,
                    opcode, operand_token_budget);
                if (!*relative)
                    return operand_parse_failed(stream, limit, op);
                break;
            case 4: /* immediate64 plus relative operand */
                if (!operand_read_u32(stream, limit, &lo) ||
                    !operand_read_u32(stream, limit, &hi))
                    return operand_parse_failed(stream, limit, op);
                op->index_values[dimension] =
                    (uint64_t)lo | ((uint64_t)hi << 32);
                op->index_has_immediate[dimension] = true;
                *relative = parse_operand_recursive_impl(
                    stream, limit, DXBC_OPERAND_CONTEXT_RELATIVE_INDEX,
                    opcode, operand_token_budget);
                if (!*relative)
                    return operand_parse_failed(stream, limit, op);
                break;
            default:
                return operand_parse_failed(stream, limit, op);
        }
        sync_legacy_index(op, dimension);
    }

    char idx[128] = "";
    if (!(type == OPERAND_TYPE_NULL && index_dim == 0) && index_dim > 0) {
        char components[3][160];
        for (int dimension = 0; dimension < index_dim; ++dimension) {
            format_index_component(op, dimension, components[dimension],
                                   sizeof(components[dimension]));
        }
        bool embeds_first_index =
            !op->rel_op0 &&
            (type <= OPERAND_TYPE_INDEXABLE_TEMP ||
             type == OPERAND_TYPE_SAMPLER ||
             type == OPERAND_TYPE_RESOURCE || type == OPERAND_TYPE_NULL ||
             type == OPERAND_TYPE_STREAM || type == OPERAND_TYPE_UAV ||
             type == OPERAND_TYPE_CONSTANT_BUFFER);
        if (embeds_first_index) {
            size_t used = strlen(reg);
            if (used < sizeof(reg))
                format_operand_text(reg + used, sizeof(reg) - used, "%s",
                                    components[0]);
            for (int dimension = 1; dimension < index_dim; ++dimension) {
                size_t idx_used = strlen(idx);
                if (idx_used < sizeof(idx))
                    format_operand_text(idx + idx_used,
                                        sizeof(idx) - idx_used, "[%s]",
                                        components[dimension]);
            }
        } else {
            for (int dimension = 0; dimension < index_dim; ++dimension) {
                size_t idx_used = strlen(idx);
                if (idx_used < sizeof(idx))
                    format_operand_text(idx + idx_used,
                                        sizeof(idx) - idx_used, "[%s]",
                                        components[dimension]);
            }
        }
    }
    
    char swiz[16] = "";
    if (type != OPERAND_TYPE_IMMEDIATE32 &&
        type != OPERAND_TYPE_IMMEDIATE64) {
        if (selection_mode == 0) {
            char s[8] = ".";
            int s_idx = 1;
            if (token0 & 16) s[s_idx++] = 'x';
            if (token0 & 32) s[s_idx++] = 'y';
            if (token0 & 64) s[s_idx++] = 'z';
            if (token0 & 128) s[s_idx++] = 'w';
            s[s_idx] = '\0';
            
            if (strcmp(s, ".xyzw") == 0) {
                if (type == 0 || type == 1 || type == 2 || type == 3 || type == 7 || type == 8 || type == 9 || type == 25 || type == 26 || type == 30) {
                    if (type == OPERAND_TYPE_CONSTANT_BUFFER &&
                        (context ==
                             DXBC_OPERAND_CONTEXT_CONSTANT_BUFFER_DECLARATION ||
                         opcode == 89u)) {
                        // skip
                    } else {
                        strncpy(swiz, s, sizeof(swiz) - 1);
                        swiz[sizeof(swiz) - 1] = '\0';
                    }
                } else if (context == DXBC_OPERAND_CONTEXT_DECLARATION ||
                           context ==
                               DXBC_OPERAND_CONTEXT_CONSTANT_BUFFER_DECLARATION) {
                    strncpy(swiz, s, sizeof(swiz) - 1);
                    swiz[sizeof(swiz) - 1] = '\0';
                }
            } else if (strcmp(s, ".") != 0) {
                strncpy(swiz, s, sizeof(swiz) - 1);
                swiz[sizeof(swiz) - 1] = '\0';
            }
        } else if (selection_mode == 1) {
            uint8_t x = (token0 >> 4) & 3;
            uint8_t y = (token0 >> 6) & 3;
            uint8_t z = (token0 >> 8) & 3;
            uint8_t w = (token0 >> 10) & 3;
            op->swizzle[0] = x; op->swizzle[1] = y; op->swizzle[2] = z; op->swizzle[3] = w;
            if (!(x == 0 && y == 1 && z == 2 && w == 3)) {
                snprintf(swiz, sizeof(swiz), ".%c%c%c%c", "xyzw"[x], "xyzw"[y], "xyzw"[z], "xyzw"[w]);
            } else {
                if (type == 0 || type == 1 || type == 2 || type == 3 || type == 7 || type == 8 || type == 9 || type == 25 || type == 26 || type == 30) {
                    if (type == OPERAND_TYPE_CONSTANT_BUFFER &&
                        (context ==
                             DXBC_OPERAND_CONTEXT_CONSTANT_BUFFER_DECLARATION ||
                         opcode == 89u)) {
                        // skip
                    } else {
                        strncpy(swiz, ".xyzw", sizeof(swiz) - 1);
                        swiz[sizeof(swiz) - 1] = '\0';
                    }
                }
            }
        } else if (selection_mode == 2) {
            uint8_t sel = (token0 >> 4) & 3;
            op->swizzle[0] = sel; op->swizzle[1] = sel; op->swizzle[2] = sel; op->swizzle[3] = sel;
            snprintf(swiz, sizeof(swiz), ".%c", "xyzw"[sel]);
        }
    }
    
    char full_text[128];
    format_operand_text(full_text, sizeof(full_text), "%s%s%s", reg, idx,
                        swiz);
    
    if (op->has_abs && op->has_neg) {
        format_operand_text(op->text, sizeof(op->text), "-|%s|", full_text);
    } else if (op->has_abs) {
        format_operand_text(op->text, sizeof(op->text), "|%s|", full_text);
    } else if (op->has_neg) {
        format_operand_text(op->text, sizeof(op->text), "-%s", full_text);
    } else {
        memcpy(op->text, full_text, strlen(full_text) + 1U);
    }
    
    return op;
}

DXBCOperand* parse_operand_recursive(ByteStream* stream, size_t limit,
                                     DXBCOperandParseContext context,
                                     uint32_t opcode,
                                     DXBCOperand* destOp) {
    (void)destOp;
    size_t operand_token_budget = 0u;
    if (stream && limit <= stream->size && stream->position <= limit) {
        operand_token_budget =
            (limit - stream->position) / sizeof(uint32_t);
        if (operand_token_budget > DXBC_MAX_NESTED_OPERAND_TOKENS) {
            operand_token_budget = DXBC_MAX_NESTED_OPERAND_TOKENS;
        }
    }
    return parse_operand_recursive_impl(stream, limit, context, opcode,
                                        &operand_token_budget);
}

void free_operand(DXBCOperand* op) {
    if (op->rel_op0) {
        free_operand(op->rel_op0);
        mem_free(op->rel_op0, sizeof(DXBCOperand));
        op->rel_op0 = NULL;
    }
    if (op->rel_op1) {
        free_operand(op->rel_op1);
        mem_free(op->rel_op1, sizeof(DXBCOperand));
        op->rel_op1 = NULL;
    }
    if (op->rel_op2) {
        free_operand(op->rel_op2);
        mem_free(op->rel_op2, sizeof(DXBCOperand));
        op->rel_op2 = NULL;
    }
    if (op->extended_tokens) {
        mem_free(op->extended_tokens,
                 op->extended_token_count * sizeof(uint32_t));
        op->extended_tokens = NULL;
        op->extended_token_count = 0;
    }
}
