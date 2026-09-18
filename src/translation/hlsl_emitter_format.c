// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static bool operand_fits_packed_variable(
    const DXBCOperand *operand, int write_mask,
    const DecodedVariableLayout *layout);

void sb_append_spaces(StringBuilder* sb, int count) {
    for (int i = 0; i < count; i++) {
        sb_append(sb, " ");
    }
}

static bool hlsl_builder_failed(HLSLEmitterContext *ctx,
                                StringBuilder *builder) {
    if (builder) builder->failed = true;
    if (ctx && ctx->sb) ctx->sb->failed = true;
    return false;
}

static const char *hlsl_builder_text(const StringBuilder *builder) {
    return builder && builder->buf ? builder->buf : "";
}

static bool srv_kind_for_program_register(
    const USILProgram *program, int register_index,
    SerializedResourceType *out_kind) {
    if (!program || !out_kind) return false;
    const USILTexture *match = NULL;
    for (int index = 0; index < program->texture_count; ++index) {
        if (program->textures[index].reg_idx != register_index) continue;
        if (match) return false;
        match = &program->textures[index];
    }
    if (!match) return false;
    *out_kind = strcmp(match->dimension, "structured") == 0 ||
                        strcmp(match->dimension, "raw") == 0
                    ? SERIALIZED_RESOURCE_BUFFER
                    : SERIALIZED_RESOURCE_TEXTURE;
    return true;
}

static bool hlsl_builder_copy_checked(HLSLEmitterContext *ctx,
                                      StringBuilder *builder,
                                      const char *value) {
    if (!builder || !value) return hlsl_builder_failed(ctx, builder);
    sb_clear(builder);
    sb_append(builder, value);
    return sb_ok(builder) || hlsl_builder_failed(ctx, builder);
}

static bool hlsl_builder_format_checked(HLSLEmitterContext *ctx,
                                        StringBuilder *builder,
                                        const char *format, ...) {
    if (!builder || !format) return hlsl_builder_failed(ctx, builder);
    sb_clear(builder);
    va_list args;
    va_start(args, format);
    sb_appendfv(builder, format, args);
    va_end(args);
    return sb_ok(builder) || hlsl_builder_failed(ctx, builder);
}

static bool wrap_operand_modifiers(HLSLEmitterContext* ctx,
                                   StringBuilder *dest, const char* expr,
                                   bool has_abs, bool has_neg) {
    if (has_abs && has_neg) {
        return hlsl_builder_format_checked(ctx, dest, "-abs(%s)", expr);
    } else if (has_abs) {
        return hlsl_builder_format_checked(ctx, dest, "abs(%s)", expr);
    } else if (has_neg) {
        return hlsl_builder_format_checked(ctx, dest, "-%s", expr);
    }
    return hlsl_builder_copy_checked(ctx, dest, expr);
}

static bool wrap_operand_cast(HLSLEmitterContext* ctx, StringBuilder *dest,
                              const char* expr,
                              const DXBCOperand* op, bool is_int,
                              bool is_uint, bool is_declared_int,
                              bool is_declared_bool) {
    if ((is_int || is_uint) && op->type != OPERAND_TYPE_SAMPLER && op->type != OPERAND_TYPE_RESOURCE && op->type != OPERAND_TYPE_UAV) {
        if (is_declared_bool) {
            return hlsl_builder_format_checked(ctx, dest,
                                       is_uint ? "((%s) ? 1u : 0u)"
                                               : "((%s) ? 1 : 0)",
                                       expr);
        }
        if (is_uint) {
            if (ctx->use_uint_temps && (op->type == OPERAND_TYPE_TEMP || op->type == OPERAND_TYPE_INDEXABLE_TEMP)) {
                return hlsl_builder_copy_checked(ctx, dest, expr);
            } else {
                return hlsl_builder_format_checked(ctx, dest, "asuint(%s)", expr);
            }
        } else {
            if (is_declared_int) {
                return hlsl_builder_copy_checked(ctx, dest, expr);
            } else {
                return hlsl_builder_format_checked(ctx, dest, "asint(%s)", expr);
            }
        }
    } else {
        if (is_declared_bool) {
            return hlsl_builder_format_checked(ctx, dest,
                                       "((%s) ? 1.0f : 0.0f)", expr);
        }
        if (ctx->use_uint_temps && (op->type == OPERAND_TYPE_TEMP || op->type == OPERAND_TYPE_INDEXABLE_TEMP)) {
            return hlsl_builder_format_checked(ctx, dest, "asfloat(%s)", expr);
        }
        return hlsl_builder_copy_checked(ctx, dest, expr);
    }
}

bool format_float_bits_hlsl(uint32_t bits, char* buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return false;
    buf[0] = '\0';

    uint32_t exponent = (bits >> 23) & UINT32_C(0xff);
    uint32_t mantissa = bits & UINT32_C(0x7fffff);
    if (exponent == UINT32_C(0xff) ||
        (exponent == 0 && mantissa != 0)) {
        int written = snprintf(buf, buf_sz, "asfloat(0x%08Xu)", bits);
        if (written < 0 || (size_t)written >= buf_sz) {
            buf[0] = '\0';
            return false;
        }
        return true;
    }

    if ((bits & UINT32_C(0x7fffffff)) == 0) {
        const char* zero = (bits & UINT32_C(0x80000000)) ? "-0.0f" : "0.0f";
        size_t length = strlen(zero);
        if (length >= buf_sz) return false;
        memcpy(buf, zero, length + 1);
        return true;
    }

    float value;
    memcpy(&value, &bits, sizeof(value));
    char digits[64];
    int digit_count = snprintf(digits, sizeof(digits), "%.9g", value);
    if (digit_count < 0 || (size_t)digit_count >= sizeof(digits)) return false;

    int written;
    if (!strchr(digits, '.') && !strchr(digits, 'e') &&
        !strchr(digits, 'E')) {
        written = snprintf(buf, buf_sz, "%s.0f", digits);
    } else {
        written = snprintf(buf, buf_sz, "%sf", digits);
    }
    if (written < 0 || (size_t)written >= buf_sz) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

bool format_float(float f, char* buf, size_t buf_sz) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return format_float_bits_hlsl(bits, buf, buf_sz);
}

bool format_immediate_hlsl_masked(const DXBCOperand* op, bool isInt,
                                  bool isUint, int write_mask, char* buf,
                                  size_t buf_sz) {
    if (!op || !buf || buf_sz == 0) return false;
    buf[0] = '\0';
    if (op->imm_value_count == 1) {
        int written = 0;
        if (isInt || isUint) {
            if (isUint) {
                written = snprintf(buf, buf_sz, "%uu", op->imm_values[0]);
            } else {
                written = snprintf(buf, buf_sz, "%d", (int)op->imm_values[0]);
            }
        } else {
            return format_float_bits_hlsl(op->imm_values[0], buf, buf_sz);
        }
        if (written < 0 || (size_t)written >= buf_sz) {
            buf[0] = '\0';
            return false;
        }
        return true;
    }

    int comps[4];
    int comp_count = 0;
    if (write_mask == 0) {
        comps[0] = 0; comps[1] = 1; comps[2] = 2; comps[3] = 3;
        comp_count = 4;
    } else {
        if (write_mask & 16) comps[comp_count++] = 0;
        if (write_mask & 32) comps[comp_count++] = 1;
        if (write_mask & 64) comps[comp_count++] = 2;
        if (write_mask & 128) comps[comp_count++] = 3;
    }
    if (comp_count == 0) return false;

    if (comp_count == 1) {
        int idx = comps[0];
        int written = 0;
        if (isInt || isUint) {
            if (isUint) {
                written = snprintf(buf, buf_sz, "%uu", op->imm_values[idx]);
            } else {
                written = snprintf(buf, buf_sz, "%d", (int)op->imm_values[idx]);
            }
        } else {
            return format_float_bits_hlsl(op->imm_values[idx], buf, buf_sz);
        }
        if (written < 0 || (size_t)written >= buf_sz) {
            buf[0] = '\0';
            return false;
        }
        return true;
    } else {
        char type_prefix[16];
        int type_length;
        if (isInt || isUint) {
            type_length = snprintf(type_prefix, sizeof(type_prefix), "%s%d",
                                   isUint ? "uint" : "int", comp_count);
        } else {
            type_length = snprintf(type_prefix, sizeof(type_prefix), "float%d",
                                   comp_count);
        }
        if (type_length < 0 || (size_t)type_length >= sizeof(type_prefix))
            return false;

        char parts[4][64];
        for (int i = 0; i < comp_count; i++) {
            int idx = comps[i];
            int part_length = 0;
            if (isInt || isUint) {
                if (isUint) {
                    part_length = snprintf(parts[i], sizeof(parts[i]), "%uu",
                                           op->imm_values[idx]);
                } else {
                    part_length = snprintf(parts[i], sizeof(parts[i]), "%d",
                                           (int)op->imm_values[idx]);
                }
            } else {
                if (!format_float_bits_hlsl(op->imm_values[idx], parts[i],
                                            sizeof(parts[i]))) {
                    return false;
                }
                continue;
            }
            if (part_length < 0 ||
                (size_t)part_length >= sizeof(parts[i])) return false;
        }

        int written;
        if (comp_count == 2) {
            written = snprintf(buf, buf_sz, "%s(%s, %s)", type_prefix,
                               parts[0], parts[1]);
        } else if (comp_count == 3) {
            written = snprintf(buf, buf_sz, "%s(%s, %s, %s)", type_prefix,
                               parts[0], parts[1], parts[2]);
        } else {
            written = snprintf(buf, buf_sz, "%s(%s, %s, %s, %s)",
                               type_prefix, parts[0], parts[1], parts[2],
                               parts[3]);
        }
        if (written < 0 || (size_t)written >= buf_sz) {
            buf[0] = '\0';
            return false;
        }
        return true;
    }
}

bool format_immediate_hlsl(const DXBCOperand* op, bool isInt, bool isUint,
                           char* buf, size_t buf_sz) {
    return format_immediate_hlsl_masked(op, isInt, isUint, 0, buf, buf_sz);
}

static bool copy_swizzle_text(char *buf, size_t buf_sz, const char *text) {
    if (!buf || buf_sz == 0 || !text) return false;
    size_t length = strlen(text);
    if (length >= buf_sz) {
        buf[0] = '\0';
        return false;
    }
    memcpy(buf, text, length + 1u);
    return true;
}

bool format_swizzle_hlsl(const DXBCOperand* op, int write_mask,
                         bool preserve_vector, char* buf, size_t buf_sz) {
    if (!op || !buf || buf_sz == 0) return false;
    buf[0] = '\0';
    if (op->type == OPERAND_TYPE_IMMEDIATE32) return true;
    
    if (op->swizzle_mode == 0) {
        int mask_val = (write_mask != 0) ? write_mask : op->destination_mask;
        char mask_str[8] = ".";
        int idx = 1;
        if (mask_val & 16) mask_str[idx++] = 'x';
        if (mask_val & 32) mask_str[idx++] = 'y';
        if (mask_val & 64) mask_str[idx++] = 'z';
        if (mask_val & 128) mask_str[idx++] = 'w';
        mask_str[idx] = '\0';
        
        if (strcmp(mask_str, ".") != 0 && strcmp(mask_str, ".xyzw") != 0) {
            return copy_swizzle_text(buf, buf_sz, mask_str);
        }
    } else if (op->swizzle_mode == 1) {
        /* preserve_vector controls replicate collapsing; it must not widen
         * an explicitly requested operand width.  In particular, dp3 with a
         * replicate source consumes three lanes, not four. */
        if (write_mask == 0) write_mask = 240;
        char swiz_str[8] = "";
        int idx = 0;
        if (write_mask & 16)  swiz_str[idx++] = "xyzw"[op->swizzle[0]];
        if (write_mask & 32)  swiz_str[idx++] = "xyzw"[op->swizzle[1]];
        if (write_mask & 64)  swiz_str[idx++] = "xyzw"[op->swizzle[2]];
        if (write_mask & 128) swiz_str[idx++] = "xyzw"[op->swizzle[3]];
        swiz_str[idx] = '\0';
        
        if (idx > 1) {
            bool all_same = true;
            char first = swiz_str[0];
            for (int i = 1; i < idx; i++) {
                if (swiz_str[i] != first) {
                    all_same = false;
                    break;
                }
            }
            if (all_same && !preserve_vector) {
                swiz_str[0] = first;
                swiz_str[1] = '\0';
                idx = 1;
            }
        }
        
        if (idx > 0) {
            bool is_identity = false;
            if (idx == 4 && strcmp(swiz_str, "xyzw") == 0) is_identity = true;
            if (!is_identity) {
                char result[8] = ".";
                memcpy(result + 1u, swiz_str, (size_t)idx + 1u);
                return copy_swizzle_text(buf, buf_sz, result);
            }
        }
    } else if (op->swizzle_mode == 2) {
        uint8_t sel = op->swizzle[0];
        int wm = (write_mask != 0) ? write_mask : 240;
        if (preserve_vector) {
            int comp_count = get_mask_component_count(wm);
            if (comp_count > 1) {
                char swiz_str[8] = "";
                for (int i = 0; i < comp_count && i < 4; i++) {
                    swiz_str[i] = "xyzw"[sel];
                }
                swiz_str[comp_count] = '\0';
                char result[8] = ".";
                memcpy(result + 1u, swiz_str, (size_t)comp_count + 1u);
                return copy_swizzle_text(buf, buf_sz, result);
            }
        }
        char result[3] = {'.', "xyzw"[sel], '\0'};
        return copy_swizzle_text(buf, buf_sz, result);
    }
    return true;
}

const char* get_packoffset_suffix(uint32_t byte_offset) {
    uint32_t rem = byte_offset % 16;
    if (rem == 4) return ".y";
    if (rem == 8) return ".z";
    if (rem == 12) return ".w";
    return "";
}

bool format_cb_swizzle(const DXBCOperand* op, uint32_t dim,
                       uint32_t byte_offset, int write_mask,
                       bool preserve_vector, char* buf, size_t buf_sz) {
    if (!op || !buf || buf_sz == 0) return false;
    buf[0] = '\0';
    if (op->type == OPERAND_TYPE_IMMEDIATE32) return true;
    
    if (dim <= 1) {
        return true; // Scalars have no swizzle in HLSL
    }
    
    uint32_t comp_offset = (byte_offset % 16) / 4;
    
    if (op->swizzle_mode == 0) {
        char mask_str[8] = ".";
        int idx = 1;
        if (op->destination_mask & 16) {
            int rel = 0 - (int)comp_offset;
            if (rel >= 0 && rel < (int)dim) mask_str[idx++] = "xyzw"[rel];
        }
        if (op->destination_mask & 32) {
            int rel = 1 - (int)comp_offset;
            if (rel >= 0 && rel < (int)dim) mask_str[idx++] = "xyzw"[rel];
        }
        if (op->destination_mask & 64) {
            int rel = 2 - (int)comp_offset;
            if (rel >= 0 && rel < (int)dim) mask_str[idx++] = "xyzw"[rel];
        }
        if (op->destination_mask & 128) {
            int rel = 3 - (int)comp_offset;
            if (rel >= 0 && rel < (int)dim) mask_str[idx++] = "xyzw"[rel];
        }
        mask_str[idx] = '\0';
        if (strcmp(mask_str, ".") != 0) {
            return copy_swizzle_text(buf, buf_sz, mask_str);
        }
    } else if (op->swizzle_mode == 1) {
        if (write_mask == 0) write_mask = 240;
        char swiz_str[8] = "";
        int idx = 0;
        if (write_mask & 16) {
            int rel = (int)op->swizzle[0] - (int)comp_offset;
            if (rel >= 0 && rel < (int)dim) swiz_str[idx++] = "xyzw"[rel];
        }
        if (write_mask & 32) {
            int rel = (int)op->swizzle[1] - (int)comp_offset;
            if (rel >= 0 && rel < (int)dim) swiz_str[idx++] = "xyzw"[rel];
        }
        if (write_mask & 64) {
            int rel = (int)op->swizzle[2] - (int)comp_offset;
            if (rel >= 0 && rel < (int)dim) swiz_str[idx++] = "xyzw"[rel];
        }
        if (write_mask & 128) {
            int rel = (int)op->swizzle[3] - (int)comp_offset;
            if (rel >= 0 && rel < (int)dim) swiz_str[idx++] = "xyzw"[rel];
        }
        swiz_str[idx] = '\0';
        
        if (idx > 1) {
            bool all_same = true;
            char first = swiz_str[0];
            for (int i = 1; i < idx; i++) {
                if (swiz_str[i] != first) {
                    all_same = false;
                    break;
                }
            }
            if (all_same && !preserve_vector) {
                swiz_str[0] = first;
                swiz_str[1] = '\0';
                idx = 1;
            }
        }
        
        if (idx > 0) {
            bool is_default = false;
            if (dim == 4 && idx == 4 && strcmp(swiz_str, "xyzw") == 0) is_default = true;
            if (dim == 3 && idx == 3 && strcmp(swiz_str, "xyz") == 0) is_default = true;
            if (dim == 2 && idx == 2 && strcmp(swiz_str, "xy") == 0) is_default = true;
            if (!is_default) {
                char result[8] = ".";
                memcpy(result + 1u, swiz_str, (size_t)idx + 1u);
                return copy_swizzle_text(buf, buf_sz, result);
            }
        }
    } else if (op->swizzle_mode == 2) {
        int chan = op->swizzle[0];
        int rel = chan - (int)comp_offset;
        if (rel >= 0 && rel < (int)dim) {
            int wm = (write_mask != 0) ? write_mask : 240;
            if (preserve_vector) {
                int comp_count = get_mask_component_count(wm);
                if (comp_count > 1) {
                    char swiz_str[8] = "";
                    for (int i = 0; i < comp_count && i < 4; i++) {
                        swiz_str[i] = "xyzw"[rel];
                    }
                    swiz_str[comp_count] = '\0';
                    char result[8] = ".";
                    memcpy(result + 1u, swiz_str,
                           (size_t)comp_count + 1u);
                    return copy_swizzle_text(buf, buf_sz, result);
                }
            }
            char result[3] = {'.', "xyzw"[rel], '\0'};
            return copy_swizzle_text(buf, buf_sz, result);
        }
    }
    return true;
}

static void ResolveScrambledOperand(HLSLEmitterContext* ctx, const DXBCOperand* original_op, int dst_reg, bool dst_scrambled, int* inout_write_mask, bool* inout_preserve_vector, DXBCOperand* out_op) {
    int write_mask = *inout_write_mask;
    bool preserve_vector = *inout_preserve_vector;
    DXBCOperand local_op = *original_op;

    if (!hlsl_readability_transforms_enabled(ctx)) {
        *inout_write_mask = write_mask;
        *inout_preserve_vector = preserve_vector;
        *out_op = local_op;
        return;
    }

    int src_reg = local_op.type == OPERAND_TYPE_TEMP
                      ? local_op.register_index
                      : -1;
    bool src_scrambled =
        src_reg >= 0 && src_reg < ctx->temp_state_count &&
        hlsl_register_is_scrambled(ctx, ctx->current_instruction_index,
                                   src_reg);
    
    if (dst_scrambled || src_scrambled) {
        preserve_vector = false;
    }

    if (ctx->is_formatting_dest) {
        if (dst_scrambled) {
            const int* P_dst = *hlsl_register_permutation_at_const(
                ctx, ctx->current_instruction_index + 1, dst_reg);
            int logical_write_mask = 0;
            for (int c = 0; c < 4; c++) {
                if (write_mask & (16 << c)) {
                    logical_write_mask |= (16 << P_dst[c]);
                }
            }
            write_mask = logical_write_mask;
            
            int local_write_mask = 0;
            int original_mask = local_op.destination_mask == 0 ? 240 : local_op.destination_mask;
            for (int c = 0; c < 4; c++) {
                if (original_mask & (16 << c)) {
                    local_write_mask |= (16 << P_dst[c]);
                }
            }
            local_op.destination_mask = (uint8_t)local_write_mask;
        }
    } else {
        if (dst_scrambled) {
            const int* P_dst = *hlsl_register_permutation_at_const(
                ctx, ctx->current_instruction_index + 1, dst_reg);
            int logical_write_mask = 0;
            for (int c = 0; c < 4; c++) {
                if (write_mask & (16 << c)) {
                    logical_write_mask |= (16 << P_dst[c]);
                }
            }
            write_mask = logical_write_mask;
        }

        bool is_swizzlable = (local_op.type == OPERAND_TYPE_TEMP ||
                              local_op.type == OPERAND_TYPE_INDEXABLE_TEMP ||
                              local_op.type == OPERAND_TYPE_CONSTANT_BUFFER ||
                              local_op.type == OPERAND_TYPE_INPUT ||
                              local_op.type == OPERAND_TYPE_OUTPUT);
        if (is_swizzlable && (dst_scrambled || src_scrambled)) {
            int identity_P[4] = {0, 1, 2, 3};
            const int* P_dst =
                dst_scrambled
                    ? *hlsl_register_permutation_at_const(
                          ctx, ctx->current_instruction_index + 1, dst_reg)
                    : identity_P;
            const int* P_src =
                src_scrambled
                    ? *hlsl_register_permutation_at_const(
                          ctx, ctx->current_instruction_index, src_reg)
                    : identity_P;
            
            if (local_op.swizzle_mode == 0) {
                local_op.swizzle[0] = 0;
                local_op.swizzle[1] = 1;
                local_op.swizzle[2] = 2;
                local_op.swizzle[3] = 3;
                local_op.swizzle_mode = 1;
            }
            
            if (local_op.swizzle_mode == 1 || local_op.swizzle_mode == 2) {
                int new_swizzle[4];
                bool is_repl = (local_op.swizzle_mode == 2) || 
                               (local_op.swizzle[0] == local_op.swizzle[1] &&
                                local_op.swizzle[1] == local_op.swizzle[2] &&
                                local_op.swizzle[2] == local_op.swizzle[3]);
                if (is_repl) {
                    int default_val = P_src[local_op.swizzle[0]];
                    for (int c = 0; c < 4; c++) {
                        new_swizzle[c] = default_val;
                    }
                } else {
                    for (int c = 0; c < 4; c++) {
                        new_swizzle[c] = P_src[c];
                    }
                }

                for (int p = 0; p < 4; p++) {
                    int phys_comp = (local_op.swizzle_mode == 1) ? local_op.swizzle[p] : local_op.swizzle[0];
                    new_swizzle[P_dst[p]] = P_src[phys_comp];
                }
                for (int c = 0; c < 4; c++) {
                    local_op.swizzle[c] = (uint8_t)new_swizzle[c];
                }
                local_op.swizzle_mode = 1;
            }
        }
    }

    *inout_write_mask = write_mask;
    *inout_preserve_vector = preserve_vector;
    *out_op = local_op;
}

static bool format_relative_operand_hlsl_sb(HLSLEmitterContext *ctx,
                                            const DXBCOperand *operand,
                                            StringBuilder *output) {
    bool previous_is_destination = ctx->is_formatting_dest;
    ctx->is_formatting_dest = false;
    bool success = format_operand_hlsl_sb(ctx, operand, true, false, 0, false,
                                          output);
    ctx->is_formatting_dest = previous_is_destination;
    return success;
}

static bool dynamic_matrix_temp_scalar(const DXBCOperand *operand,
                                       int *register_index,
                                       int *component) {
    if (!operand || operand->type != OPERAND_TYPE_TEMP ||
        operand->register_index_dim != 1 ||
        !operand->index_has_immediate[0] ||
        operand->index_value_exceeds_int[0] ||
        operand->index_representations[0] != 0 ||
        operand->index_values[0] != (uint32_t)operand->register_index ||
        operand->swizzle_mode != 2 || operand->swizzle[0] > 3 ||
        operand->has_abs || operand->has_neg || operand->extended_tokens ||
        operand->extended_token_count != 0 || operand->rel_op0 ||
        operand->rel_op1 || operand->rel_op2) {
        return false;
    }
    if (register_index) *register_index = operand->register_index;
    if (component) *component = operand->swizzle[0];
    return true;
}

static bool dynamic_matrix_writes_temp_lane(
    const USILInstruction *instruction, int register_index, int component) {
    if (!instruction || instruction->operand_count < 1 ||
        !inst_writes_to_dest(instruction)) {
        return false;
    }
    const DXBCOperand *destination = &instruction->operands[0];
    return destination->type == OPERAND_TYPE_TEMP &&
           destination->register_index == register_index &&
           (destination->destination_mask & (16 << component)) != 0;
}

static bool dynamic_matrix_flow_boundary(const USILInstruction *instruction) {
    if (!instruction) return true;
    switch (instruction->opcode) {
        case USIL_OP_IF:
        case USIL_OP_ELSE:
        case USIL_OP_ENDIF:
        case USIL_OP_LOOP:
        case USIL_OP_ENDLOOP:
        case USIL_OP_SWITCH:
        case USIL_OP_CASE:
        case USIL_OP_DEFAULT:
        case USIL_OP_ENDSWITCH:
        case USIL_OP_BREAK:
        case USIL_OP_BREAKC:
        case USIL_OP_CONTINUE:
        case USIL_OP_CONTINUEC:
        case USIL_OP_RET:
        case USIL_OP_DISCARD:
            return true;
        default:
            return false;
    }
}

/* A source matrix array is flattened by D3DCompiler into 16-byte rows.  The
 * array index therefore appears in DXBC as an in-place left shift by
 * log2(matrix_rows), followed by row-relative cbuffer operands.  Recover the
 * pre-shift operand only when the nearest reaching definition is exactly that
 * shift and both it and its source remain in one straight-line region. */
static const DXBCOperand *dynamic_matrix_array_index_source(
    const HLSLEmitterContext *ctx, const DXBCOperand *relative,
    uint32_t matrix_rows) {
    if (!ctx || !ctx->program || ctx->current_instruction_index <= 0 ||
        matrix_rows < 2 || matrix_rows > 4 ||
        (matrix_rows & (matrix_rows - 1u)) != 0u) {
        return NULL;
    }
    int relative_register = -1;
    int relative_component = -1;
    if (!dynamic_matrix_temp_scalar(relative, &relative_register,
                                    &relative_component)) {
        return NULL;
    }
    uint32_t shift_amount = matrix_rows == 4 ? 2u : 1u;
    for (int instruction_index = ctx->current_instruction_index - 1;
         instruction_index >= 0; --instruction_index) {
        const USILInstruction *instruction =
            &ctx->program->instructions[instruction_index];
        if (dynamic_matrix_flow_boundary(instruction)) return NULL;
        if (!dynamic_matrix_writes_temp_lane(
                instruction, relative_register, relative_component)) {
            continue;
        }
        const DXBCOperand *destination = &instruction->operands[0];
        const DXBCOperand *amount = instruction->operand_count == 3
                                        ? &instruction->operands[2]
                                        : NULL;
        if (instruction->opcode != USIL_OP_ISHL ||
            instruction->operand_count != 3 || instruction->saturate ||
            instruction->precise_mask != 0 ||
            destination->register_index_dim != 1 ||
            destination->index_representations[0] != 0 ||
            !destination->index_has_immediate[0] ||
            destination->index_value_exceeds_int[0] ||
            destination->index_values[0] != (uint32_t)relative_register ||
            destination->swizzle_mode != 0 ||
            destination->destination_mask !=
                (uint8_t)(16 << relative_component) ||
            destination->has_abs || destination->has_neg ||
            destination->extended_tokens ||
            destination->extended_token_count != 0 || destination->rel_op0 ||
            destination->rel_op1 || destination->rel_op2 || !amount ||
            amount->type != OPERAND_TYPE_IMMEDIATE32 ||
            amount->imm_value_count != 1 ||
            amount->immediate_word_count != 1 ||
            amount->imm_values[0] != shift_amount ||
            amount->immediate_words[0] != shift_amount ||
            amount->register_index_dim != 0 || amount->has_abs ||
            amount->has_neg || amount->extended_tokens ||
            amount->extended_token_count != 0 || amount->rel_op0 ||
            amount->rel_op1 || amount->rel_op2) {
            return NULL;
        }

        const DXBCOperand *source = &instruction->operands[1];
        int source_register = -1;
        int source_component = -1;
        if (dynamic_matrix_temp_scalar(source, &source_register,
                                       &source_component)) {
            if (source_register == relative_register &&
                source_component == relative_component) {
                return NULL;
            }
            for (int later = instruction_index + 1;
                 later < ctx->current_instruction_index; ++later) {
                const USILInstruction *candidate =
                    &ctx->program->instructions[later];
                if (dynamic_matrix_flow_boundary(candidate) ||
                    dynamic_matrix_writes_temp_lane(
                        candidate, source_register, source_component)) {
                    return NULL;
                }
            }
        }
        return source;
    }
    return NULL;
}

static bool format_serialized_row_struct_row(
    HLSLEmitterContext *ctx, const HLSLCBufferLayout *layout,
    const char *relative, int row_offset, StringBuilder *output) {
    if (!ctx || !layout || !layout->row_struct_parameters ||
        !layout->row_struct_parameter || !output ||
        (layout->row_struct_scale_instruction < 0 && !relative) ||
        row_offset < 0 ||
        row_offset >= layout->row_struct_stride) {
        return hlsl_builder_failed(ctx, output);
    }
    const SerializedStructParam *parameter =
        layout->row_struct_parameter;
    const uint32_t row_byte_offset = (uint32_t)row_offset * 16U;
    const SerializedVariable *match = NULL;
    DecodedVariableLayout match_layout;
    memset(&match_layout, 0, sizeof(match_layout));
    for (int member_index = 0; member_index < parameter->member_count;
         ++member_index) {
        const SerializedVariable *member = &parameter->members[member_index];
        DecodedVariableLayout decoded;
        if (!member->name ||
            !parameter_layout_decode(layout->row_struct_parameters, member,
                                     &decoded)) {
            return hlsl_builder_failed(ctx, output);
        }
        const uint32_t byte_size = parameter_layout_byte_size(&decoded);
        if (byte_size == 0U || decoded.byte_offset > UINT32_MAX - byte_size) {
            return hlsl_builder_failed(ctx, output);
        }
        if (row_byte_offset < decoded.byte_offset ||
            row_byte_offset >= decoded.byte_offset + byte_size) {
            continue;
        }
        if (match) return hlsl_builder_failed(ctx, output);
        match = member;
        match_layout = decoded;
    }
    if (!match || row_byte_offset < match_layout.byte_offset ||
        ((row_byte_offset - match_layout.byte_offset) & 15U) != 0U) {
        return hlsl_builder_failed(ctx, output);
    }
    const uint32_t member_row =
        (row_byte_offset - match_layout.byte_offset) / 16U;
    if (match_layout.is_matrix) {
        if (member_row >= match_layout.rows) {
            return hlsl_builder_failed(ctx, output);
        }
        /* Serialized matrix callbacks require the source-default
         * column-major class.  transpose(...)[row] selects the same physical
         * row proven by stripped DXBC without changing that reflection
         * class. */
        if (layout->row_struct_scale_instruction >= 0) {
            return hlsl_builder_format_checked(
                ctx, output, "transpose(%s[dxbc_row_index_i%d].%s)[%u]",
                parameter->name, layout->row_struct_scale_instruction,
                match->name, member_row);
        }
        return hlsl_builder_format_checked(
            ctx, output, "transpose(%s[%s].%s)[%u]", parameter->name,
            relative, match->name, member_row);
    }
    if (member_row != 0U) return hlsl_builder_failed(ctx, output);
    if (layout->row_struct_scale_instruction < 0) {
        return hlsl_builder_format_checked(
            ctx, output, "%s[%s].%s", parameter->name, relative,
            match->name);
    }
    return hlsl_builder_format_checked(
        ctx, output, "%s[dxbc_row_index_i%d].%s", parameter->name,
        layout->row_struct_scale_instruction, match->name);
}

bool format_operand_hlsl_sb(HLSLEmitterContext* ctx, const DXBCOperand* op,
                            bool isInt, bool isUint, int write_mask,
                            bool preserve_vector, StringBuilder *output) {
    if (!ctx || !ctx->sb || !op || !output) {
        return hlsl_builder_failed(ctx, output);
    }
    sb_clear(output);
    const SerializedProgramParameters* params = ctx->params;

    // Check scrambling properties of current instruction
    int dst_reg = -1;
    const USILInstruction* inst = &ctx->program->instructions[ctx->current_instruction_index];
    if (inst->operand_count >= 1 && inst->operands[0].type == OPERAND_TYPE_TEMP) {
        dst_reg = inst->operands[0].register_index;
    }
    
    bool is_comp_op = (is_componentwise_op(inst->opcode) || inst->opcode == USIL_OP_MOV);
    bool dst_scrambled =
        hlsl_readability_transforms_enabled(ctx) &&
        dst_reg >= 0 && dst_reg < ctx->temp_state_count &&
        hlsl_register_is_scrambled(ctx, ctx->current_instruction_index + 1,
                                   dst_reg) &&
        is_comp_op;

    DXBCOperand local_op;
    ResolveScrambledOperand(ctx, op, dst_reg, dst_scrambled, &write_mask, &preserve_vector, &local_op);
    op = &local_op;

    bool is_declared_int = false;
    bool is_declared_bool = false;
    if (op->type == OPERAND_TYPE_NULL) {
        return hlsl_builder_copy_checked(ctx, output, "0");
    }

    // Swizzle Decomposition check for temp operands
    if (op->type == OPERAND_TYPE_TEMP) {
        int regIdx = op->register_index;
        int wm = write_mask;
        if (wm == 0) {
            if (is_replicate_swizzle(op)) {
                wm = 16 << op->swizzle[0];
            } else {
                wm = (op->swizzle_mode == 0) ? op->destination_mask : 240;
            }
        }
        
        bool neededSpecialResolution = false;
        const char* resolved_names[4] = {NULL, NULL, NULL, NULL};
        char generated_names[4][64];
        int resolvedCount = 0;
        
        for (int i = 0; i < 4; i++) {
            if (wm & (16 << i)) {
                int comp = i;
                if (op->swizzle_mode == 1) {
                    comp = op->swizzle[i];
                } else if (op->swizzle_mode == 2) {
                    comp = op->swizzle[0];
                }
                
                if (ctx->has_ftoi_temp[regIdx][comp]) {
                    resolved_names[resolvedCount++] = ctx->ftoi_temps[regIdx][comp];
                    neededSpecialResolution = true;
                } else if (ctx->has_int_temp[regIdx][comp]) {
                    resolved_names[resolvedCount++] = ctx->int_temps[regIdx][comp];
                    neededSpecialResolution = true;
                } else {
                    int gen = hlsl_instruction_generation(
                        ctx, ctx->is_formatting_dest,
                        ctx->current_instruction_index, regIdx);
                    if (hlsl_register_max_generation(ctx, regIdx) > 0) {
                        hlsl_format_checked(ctx, generated_names[resolvedCount],
                                            sizeof(generated_names[resolvedCount]),
                                            "r%d_%d.%c", regIdx, gen,
                                            "xyzw"[comp]);
                    } else {
                        hlsl_format_checked(ctx, generated_names[resolvedCount],
                                            sizeof(generated_names[resolvedCount]),
                                            "r%d.%c", regIdx, "xyzw"[comp]);
                    }
                    resolved_names[resolvedCount] = generated_names[resolvedCount];
                    resolvedCount++;
                }
            }
        }
        
        if (neededSpecialResolution) {
            int lanes_count = resolvedCount;
            if (lanes_count < 1 || lanes_count > 4) {
                return hlsl_builder_failed(ctx, output);
            }

            StringBuilder expression;
            sb_init(&expression);
            if (op->has_abs && op->has_neg) {
                sb_append(&expression, "-abs(");
            } else if (op->has_abs) {
                sb_append(&expression, "abs(");
            } else if (op->has_neg) {
                sb_append_char(&expression, '-');
            }

            bool should_cast = isInt || isUint;
            if (should_cast) {
                sb_append(&expression, isUint ? "asuint(" : "asint(");
            }
            if (lanes_count == 1) {
                sb_append(&expression, resolved_names[0]);
            } else {
                const char* hlsl_type;
                if (isInt || isUint) {
                    hlsl_type = isUint
                                    ? (lanes_count == 2
                                           ? "uint2"
                                           : (lanes_count == 3 ? "uint3"
                                                               : "uint4"))
                                    : (lanes_count == 2
                                           ? "int2"
                                           : (lanes_count == 3 ? "int3"
                                                               : "int4"));
                } else {
                    hlsl_type = lanes_count == 2
                                    ? "float2"
                                    : (lanes_count == 3 ? "float3" : "float4");
                }
                sb_append(&expression, hlsl_type);
                sb_append_char(&expression, '(');
                for (int k = 0; k < lanes_count; k++) {
                    if (k > 0) sb_append(&expression, ", ");
                    sb_append(&expression, resolved_names[k]);
                }
                sb_append_char(&expression, ')');
            }
            if (should_cast) sb_append_char(&expression, ')');
            if (op->has_abs) sb_append_char(&expression, ')');

            bool success = sb_ok(&expression) &&
                           hlsl_builder_copy_checked(
                               ctx, output, hlsl_builder_text(&expression));
            if (!success) hlsl_builder_failed(ctx, output);
            sb_free(&expression);
            return success;
        }
    }

    StringBuilder reg;
    StringBuilder idx;
    sb_init(&reg);
    sb_init(&idx);
    bool formatting_ok = true;
    char swiz[16] = "";
    
    if (op->type == OPERAND_TYPE_IMMEDIATE32) {
        if (op->imm_value_count > 1 && dst_scrambled) {
            DXBCOperand temp_op = *op;
            const int* P_dst = *hlsl_register_permutation_at_const(
                ctx, ctx->current_instruction_index + 1, dst_reg);
            for (int i = 0; i < 4; i++) {
                temp_op.imm_values[i] = 0;
            }
            for (int c = 0; c < 4; c++) {
                if (c < op->imm_value_count) {
                    temp_op.imm_values[P_dst[c]] = op->imm_values[c];
                }
            }
            temp_op.imm_value_count = 4;
            char immediate[256];
            if (!format_immediate_hlsl_masked(&temp_op, isInt, isUint,
                                              write_mask, immediate,
                                              sizeof(immediate)) ||
                !hlsl_builder_copy_checked(ctx, output, immediate))
                hlsl_builder_failed(ctx, output);
        } else {
            char immediate[256];
            if (!format_immediate_hlsl_masked(op, isInt, isUint, write_mask,
                                              immediate,
                                              sizeof(immediate)) ||
                !hlsl_builder_copy_checked(ctx, output, immediate))
                hlsl_builder_failed(ctx, output);
        }
        sb_free(&idx);
        sb_free(&reg);
        return sb_ok(output);
    }
    
    switch (op->type) {
        case OPERAND_TYPE_TEMP: {
            int reg_idx = op->register_index;
            int gen = hlsl_instruction_generation(
                ctx, ctx->is_formatting_dest, ctx->current_instruction_index,
                reg_idx);
            if (hlsl_register_max_generation(ctx, reg_idx) > 0) {
                hlsl_builder_format_checked(ctx, &reg, "r%d_%d", reg_idx,
                                            gen);
            } else {
                hlsl_builder_format_checked(ctx, &reg, "r%d", reg_idx);
            }
            break;
        }
        case OPERAND_TYPE_INPUT:
            if (ctx->is_geometry && op->register_index_dim == 2 &&
                op->index_has_immediate[0] &&
                op->index_has_immediate[1]) {
                hlsl_builder_format_checked(
                    ctx, &reg, "input[%u].v%u",
                    (unsigned)op->index_values[0],
                    (unsigned)op->index_values[1]);
            } else {
                hlsl_builder_format_checked(ctx, &reg, "v%d",
                                            op->register_index);
            }
            break;
        case OPERAND_TYPE_OUTPUT:
            hlsl_builder_format_checked(ctx, &reg, "o%d", op->register_index);
            break;
        case OPERAND_TYPE_OUTPUT_DEPTH:
            hlsl_builder_copy_checked(ctx, &reg, "oDepth");
            break;
        case OPERAND_TYPE_CONSTANT_BUFFER:
            hlsl_builder_format_checked(ctx, &reg, "cb%d",
                                        op->register_index);
            break;
        case OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER:
            /* dcl_immediateConstantBuffer declares one table. Operand indices
             * select entries in that table; they are not register spaces. */
            hlsl_builder_copy_checked(ctx, &reg, "unk0_arr");
            break;
        case OPERAND_TYPE_SAMPLER: {
            const char* name = (op->register_index >= 0 &&
                                op->register_index < HLSL_SM5_SAMPLER_REGISTER_COUNT &&
                                ctx->sampler_names[op->register_index] &&
                                ctx->sampler_names[op->register_index][0])
                ? ctx->sampler_names[op->register_index]
                : NULL;
            bool uses_regular = false;
            bool uses_comparison = false;
            get_sampler_usage(ctx->program, op->register_index, &uses_regular,
                              &uses_comparison);
            bool comparison_operand =
                ctx->current_instruction_index >= 0 &&
                ctx->current_instruction_index < ctx->program->instruction_count &&
                (ctx->program->instructions[ctx->current_instruction_index].opcode ==
                     USIL_OP_SAMPLE_C ||
                 ctx->program->instructions[ctx->current_instruction_index].opcode ==
                     USIL_OP_SAMPLE_C_LZ);
            if (name && comparison_operand && uses_regular && uses_comparison) {
              hlsl_builder_format_checked(ctx, &reg, "%s_cmp", name);
            } else if (name) {
              hlsl_builder_copy_checked(ctx, &reg, name);
            } else {
              hlsl_builder_format_checked(ctx, &reg, "s%d",
                                          op->register_index);
            }
            break;
        }
        case OPERAND_TYPE_RESOURCE: {
            SerializedResourceType bind_type;
            const char *name = NULL;
            if (!srv_kind_for_program_register(
                    ctx->program, op->register_index, &bind_type) ||
                !resolve_srv_name_ctx(ctx, op->register_index, bind_type,
                                      &name)) {
                hlsl_builder_failed(ctx, output);
                sb_free(&idx);
                sb_free(&reg);
                return false;
            }
            if (name) hlsl_builder_copy_checked(ctx, &reg, name);
            else hlsl_builder_format_checked(ctx, &reg, "t%d",
                                             op->register_index);
            break;
        }
        case OPERAND_TYPE_INDEXABLE_TEMP:
            hlsl_builder_format_checked(ctx, &reg, "x%d",
                                        op->register_index);
            break;
        case OPERAND_TYPE_UAV: {
            const char* name = resolve_uav_name(params, op->register_index);
            if (name) hlsl_builder_copy_checked(ctx, &reg, name);
            else hlsl_builder_format_checked(ctx, &reg, "u%d",
                                             op->register_index);
            break;
        }
        case OPERAND_TYPE_INPUT_GS_INSTANCE_ID:
            /* USIL models architectural registers as raw 32-bit lanes.  Keep
             * the system-value parameter in that representation; integer
             * opcodes' normal asuint/asint wrapper then recovers its value. */
            hlsl_builder_copy_checked(ctx, &reg,
                                      "asfloat(dxbc_instance_id)");
            break;
        default:
            /* Architectural/system operands require explicit semantics.  A
             * synthetic uninitialized variable made unsupported bytecode look
             * compilable while changing its meaning.  Validation should stop
             * these earlier; keep formatting fail-closed as defense in depth. */
            hlsl_builder_failed(ctx, output);
            sb_free(&idx);
            sb_free(&reg);
            return false;
    }

    if (!sb_ok(&reg)) {
        hlsl_builder_failed(ctx, output);
        sb_free(&idx);
        sb_free(&reg);
        return false;
    }
    
    bool swizzle_formatted = false;
    
    if (op->type == OPERAND_TYPE_INPUT || op->type == OPERAND_TYPE_OUTPUT) {
        sb_clear(&idx);
    } else if (op->register_index_dim == 1) {
        if (op->rel_op0) {
            StringBuilder relative;
            sb_init(&relative);
            bool prev_is_dest = ctx->is_formatting_dest;
            ctx->is_formatting_dest = false;
            bool relative_ok = format_operand_hlsl_sb(
                ctx, op->rel_op0, true, false, 0, false, &relative);
            formatting_ok = formatting_ok && relative_ok;
            ctx->is_formatting_dest = prev_is_dest;
            if (relative_ok) {
                hlsl_builder_format_checked(
                    ctx, &idx, "[asint(%s) + %d]",
                    hlsl_builder_text(&relative), op->rel_offset1);
            }
            sb_free(&relative);
        } else if (op->type == OPERAND_TYPE_INDEXABLE_TEMP) {
            hlsl_builder_format_checked(ctx, &idx, "[%d]", op->rel_offset1);
        }
    } else if (op->register_index_dim == 2) {
        if (op->type == OPERAND_TYPE_CONSTANT_BUFFER) {
            int var_offset = 0;
            const char* var_name = NULL;
            // Check if this CB is a Unity builtin
            const char* builtin_cb = resolve_builtin_cb_name_for_reg(params, op->register_index);

            // Extract the first accessed component for sub-register resolution
            // This tells the resolver which exact byte within the register is accessed
            int component_hint = -1; // -1 = unknown/register-level
            if (op->swizzle_mode == 1) {
                // Standard swizzle: first component
                component_hint = op->swizzle[0];
            } else if (op->swizzle_mode == 2) {
                // Scalar: the single component
                component_hint = op->swizzle[0];
            } else if (op->swizzle_mode == 0) {
                // Write mask: first set bit
                if (op->destination_mask & 16) component_hint = 0;
                else if (op->destination_mask & 32) component_hint = 1;
                else if (op->destination_mask & 64) component_hint = 2;
                else if (op->destination_mask & 128) component_hint = 3;
            }

            if (op->rel_op1) {
                // Dynamic indexing - can't use component_hint for array resolution
                var_name = resolve_cb_variable_ctx(ctx, op->register_index, op->rel_offset0, -1, &var_offset);
                /* A relative DXBC index addresses flat 16-byte rows.  A
                 * matrix-array element spans multiple rows, so spelling the
                 * relative value as the HLSL matrix index changes both its
                 * scale and any constant row bias.  Route it through the
                 * row-exact helper, whose cases are built from the serialized
                 * layout.  Vector arrays remain a direct one-row mapping. */
                DecodedVariableLayout dynamic_layout;
                memset(&dynamic_layout, 0, sizeof(dynamic_layout));
                const DXBCOperand *matrix_index_source = NULL;
                if (var_name) {
                    if (!resolve_variable_layout_ctx(
                            ctx, var_name, &dynamic_layout)) {
                        var_name = NULL;
                    } else if (dynamic_layout.is_matrix) {
                        matrix_index_source =
                            dynamic_layout.array_size > 0
                                ? dynamic_matrix_array_index_source(
                                      ctx, op->rel_op1,
                                      dynamic_layout.rows)
                                : NULL;
                        if (!matrix_index_source) var_name = NULL;
                    }
                }
                StringBuilder relative;
                sb_init(&relative);
                bool relative_ok = format_relative_operand_hlsl_sb(
                    ctx, op->rel_op1, &relative);
                formatting_ok = formatting_ok && relative_ok;
                const HLSLCBufferLayout *cbuffer_layout =
                    get_cbuffer_emission_layout(ctx, op->register_index);
                if (cbuffer_layout && cbuffer_layout->row_struct_storage) {
                    if (relative_ok) {
                        if (cbuffer_layout->row_struct_parameter) {
                            formatting_ok = formatting_ok &&
                                format_serialized_row_struct_row(
                                    ctx, cbuffer_layout,
                                    hlsl_builder_text(&relative),
                                    op->rel_offset0, &reg);
                        } else {
                            if (cbuffer_layout->row_struct_scale_instruction <
                                0) {
                                formatting_ok = false;
                            }
                            hlsl_builder_format_checked(
                                ctx, &reg,
                                "cb%d_rows[dxbc_row_index_i%d].row%d",
                                op->register_index,
                                cbuffer_layout->row_struct_scale_instruction,
                                op->rel_offset0);
                        }
                    }
                    sb_clear(&idx);
                    var_name = NULL;
                } else if (var_name && matrix_index_source) {
                    StringBuilder matrix_index;
                    sb_init(&matrix_index);
                    bool matrix_index_ok = format_relative_operand_hlsl_sb(
                        ctx, matrix_index_source, &matrix_index);
                    formatting_ok = formatting_ok && matrix_index_ok;
                    uint32_t first_row = dynamic_layout.byte_offset / 16u;
                    uint32_t fixed_row = (uint32_t)op->rel_offset0;
                    uint32_t relative_row =
                        fixed_row >= first_row ? fixed_row - first_row
                                               : UINT32_MAX;
                    uint32_t matrix_base =
                        relative_row == UINT32_MAX
                            ? UINT32_MAX
                            : relative_row / dynamic_layout.rows;
                    uint32_t matrix_row =
                        relative_row == UINT32_MAX
                            ? UINT32_MAX
                            : relative_row % dynamic_layout.rows;
                    if (!matrix_index_ok ||
                        matrix_base >= dynamic_layout.array_size ||
                        matrix_row >= dynamic_layout.rows) {
                        formatting_ok = false;
                        hlsl_builder_failed(ctx, &reg);
                    } else if (resolve_variable_is_row_major(ctx, var_name)) {
                        if (matrix_base == 0) {
                            hlsl_builder_format_checked(
                                ctx, &reg, "%s[asint(%s)][%u]", var_name,
                                hlsl_builder_text(&matrix_index), matrix_row);
                        } else {
                            hlsl_builder_format_checked(
                                ctx, &reg, "%s[asint(%s) + %u][%u]",
                                var_name, hlsl_builder_text(&matrix_index),
                                matrix_base, matrix_row);
                        }
                    } else if (matrix_base == 0) {
                        hlsl_builder_format_checked(
                            ctx, &reg, "transpose(%s[asint(%s)])[%u]",
                            var_name, hlsl_builder_text(&matrix_index),
                            matrix_row);
                    } else {
                        hlsl_builder_format_checked(
                            ctx, &reg,
                            "transpose(%s[asint(%s) + %u])[%u]", var_name,
                            hlsl_builder_text(&matrix_index), matrix_base,
                            matrix_row);
                    }
                    sb_clear(&idx);
                    sb_free(&matrix_index);
                } else if (var_name) {
                    hlsl_builder_copy_checked(ctx, &reg, var_name);
                    if (relative_ok)
                        hlsl_builder_format_checked(
                            ctx, &idx, "[asint(%s)]",
                            hlsl_builder_text(&relative));
                } else if (builtin_cb &&
                           ctx->omit_unity_builtin_declarations) {
                    if (relative_ok)
                        hlsl_builder_format_checked(
                            ctx, &reg, "get_cb%d(asint(%s) + %d)",
                            op->register_index,
                            hlsl_builder_text(&relative), op->rel_offset0);
                    sb_clear(&idx);
                } else {
                    if (relative_ok)
                        hlsl_builder_format_checked(
                            ctx, &reg, "get_cb%d(asint(%s) + %d)",
                            op->register_index,
                            hlsl_builder_text(&relative), op->rel_offset0);
                    sb_clear(&idx);
                }
                sb_free(&relative);
            } else {
                var_name = resolve_cb_variable_ctx(ctx, op->register_index, op->rel_offset0, component_hint, &var_offset);
                if (var_name) {
                    int row_idx = var_offset / 16;
                    DecodedVariableLayout layout;
                    bool has_layout = resolve_variable_layout_ctx(ctx, var_name,
                                                                  &layout);
                    if (has_layout &&
                        !operand_fits_packed_variable(op, write_mask, &layout)) {
                        hlsl_builder_format_checked(
                            ctx, &reg, "get_cb%d(%d)", op->register_index,
                            op->rel_offset0);
                        sb_clear(&idx);
                        var_name = NULL;
                    }
                    if (var_name) {
                    hlsl_builder_copy_checked(ctx, &reg, var_name);
                    if (has_layout && (layout.is_matrix || layout.array_size)) {
                        if (layout.is_matrix) {
                            if (layout.rows == 0) {
                                hlsl_builder_failed(ctx, output);
                                sb_free(&idx);
                                sb_free(&reg);
                                return false;
                            }
                            int matrix_idx = layout.array_size
                                ? row_idx / (int)layout.rows : 0;
                            int matrix_row = layout.array_size
                                ? row_idx % (int)layout.rows : row_idx;
                            if (resolve_variable_is_row_major(ctx, var_name)) {
                                if (layout.array_size) {
                                    hlsl_builder_format_checked(
                                        ctx, &idx, "[%d][%d]", matrix_idx,
                                        matrix_row);
                                } else {
                                    hlsl_builder_format_checked(
                                        ctx, &idx, "[%d]", matrix_row);
                                }
                            } else {
                                StringBuilder transpose;
                                sb_init(&transpose);
                                if (layout.array_size) {
                                    hlsl_builder_format_checked(
                                        ctx, &transpose, "transpose(%s[%d])",
                                        hlsl_builder_text(&reg), matrix_idx);
                                } else {
                                    hlsl_builder_format_checked(
                                        ctx, &transpose, "transpose(%s)",
                                        hlsl_builder_text(&reg));
                                }
                                hlsl_builder_copy_checked(
                                    ctx, &reg, hlsl_builder_text(&transpose));
                                sb_free(&transpose);
                                hlsl_builder_format_checked(ctx, &idx, "[%d]",
                                                            matrix_row);
                            }
                        } else {
                            hlsl_builder_format_checked(ctx, &idx, "[%d]",
                                                        row_idx);
                        }
                    } else {
                        sb_clear(&idx);
                    }
                    }
                } else if (builtin_cb &&
                           ctx->omit_unity_builtin_declarations) {
                    // Unity builtin CB but no variable resolved - this shouldn't normally happen
                    // because resolve_cb_variable checks builtins first. 
                    // Fallback: emit as get_cb helper access
                    hlsl_builder_format_checked(
                        ctx, &reg, "get_cb%d(%d)", op->register_index,
                        op->rel_offset0);
                    sb_clear(&idx);
                } else {
                    // No variable resolved (e.g., sub-register packed common_params)
                    // Use get_cb helper which handles compositing
                    hlsl_builder_format_checked(
                        ctx, &reg, "get_cb%d(%d)", op->register_index,
                        op->rel_offset0);
                    sb_clear(&idx);
                }
            }
            
            uint32_t dim = 4;
            uint32_t byte_offset = 0;
            bool found_info = false;
            if (var_name) {
                int variable_type = resolve_variable_type(ctx, var_name);
                if (variable_type == 1) {
                    is_declared_int = true;
                } else if (variable_type == 2) {
                    is_declared_bool = true;
                }
                DecodedVariableLayout variable_layout;
                found_info = resolve_variable_layout_ctx(
                    ctx, var_name, &variable_layout);
                if (found_info) {
                    dim = variable_layout.columns;
                    byte_offset = variable_layout.byte_offset;
                }
            }
            if (found_info) {
                if (!format_cb_swizzle(op, dim, byte_offset, write_mask,
                                       preserve_vector, swiz,
                                       sizeof(swiz))) {
                    hlsl_builder_failed(ctx, output);
                    sb_free(&idx);
                    sb_free(&reg);
                    return false;
                }
                swizzle_formatted = true;
            }

        } else {
            if (op->rel_op1) {
                StringBuilder relative;
                sb_init(&relative);
                if (format_relative_operand_hlsl_sb(ctx, op->rel_op1,
                                                    &relative)) {
                    hlsl_builder_format_checked(
                        ctx, &idx, "[asint(%s) + %d]",
                        hlsl_builder_text(&relative), op->rel_offset0);
                } else {
                    formatting_ok = false;
                }
                sb_free(&relative);
            } else {
                hlsl_builder_format_checked(ctx, &idx, "[%d]",
                                            op->rel_offset0);
            }
        }
    }
    
    if (!swizzle_formatted) {
        if (op->type == OPERAND_TYPE_RESOURCE || op->type == OPERAND_TYPE_SAMPLER || op->type == OPERAND_TYPE_UAV) {
            swiz[0] = '\0';
        } else {
            if (!format_swizzle_hlsl(op, write_mask, preserve_vector, swiz,
                                     sizeof(swiz))) {
                hlsl_builder_failed(ctx, output);
                sb_free(&idx);
                sb_free(&reg);
                return false;
            }
        }
    }

    StringBuilder raw_expression;
    StringBuilder cast_expression;
    sb_init(&raw_expression);
    sb_init(&cast_expression);
    hlsl_builder_format_checked(
        ctx, &raw_expression, "%s%s%s", hlsl_builder_text(&reg),
        hlsl_builder_text(&idx), swiz);
    bool success = formatting_ok && sb_ok(&reg) && sb_ok(&idx) &&
                   sb_ok(&raw_expression) &&
                   wrap_operand_cast(
                       ctx, &cast_expression,
                       hlsl_builder_text(&raw_expression), op, isInt, isUint,
                       is_declared_int, is_declared_bool) &&
                   wrap_operand_modifiers(
                       ctx, output, hlsl_builder_text(&cast_expression),
                       op->has_abs, op->has_neg);
    if (!success) hlsl_builder_failed(ctx, output);
    sb_free(&cast_expression);
    sb_free(&raw_expression);
    sb_free(&idx);
    sb_free(&reg);
    return success;
}

static bool operand_fits_packed_variable(const DXBCOperand *operand,
                                         int write_mask,
                                         const DecodedVariableLayout *layout) {
    if (layout->is_matrix || layout->array_size) return true;
    int first_component = (int)((layout->byte_offset % 16) / 4);
    int last_component = first_component + (int)layout->columns;
    int active_mask = write_mask ? write_mask : 0xF0;
    for (int lane = 0; lane < 4; lane++) {
        if ((active_mask & (16 << lane)) == 0) continue;
        int source_component = lane;
        if (operand->swizzle_mode == 1) {
            source_component = operand->swizzle[lane];
        } else if (operand->swizzle_mode == 2) {
            source_component = operand->swizzle[0];
        }
        if (source_component < first_component ||
            source_component >= last_component) {
            return false;
        }
    }
    return true;
}

void format_operand_hlsl(HLSLEmitterContext* ctx, const DXBCOperand* op,
                         bool isInt, bool isUint, int write_mask,
                         bool preserve_vector, char* buf, size_t buf_sz) {
    StringBuilder expression;
    sb_init(&expression);
    if (!format_operand_hlsl_sb(ctx, op, isInt, isUint, write_mask,
                                preserve_vector, &expression) ||
        !hlsl_copy_checked(ctx, buf, buf_sz,
                           hlsl_builder_text(&expression))) {
        if (buf && buf_sz > 0) buf[0] = '\0';
    }
    sb_free(&expression);
}

bool format_dest_operand_hlsl_sb(HLSLEmitterContext* ctx,
                                 const DXBCOperand* op, bool isInt,
                                 bool isUint, int write_mask,
                                 bool preserve_vector,
                                 StringBuilder *output) {
    if (!ctx) return hlsl_builder_failed(ctx, output);
    bool previous_is_destination = ctx->is_formatting_dest;
    ctx->is_formatting_dest = true;
    bool success = format_operand_hlsl_sb(
        ctx, op, isInt, isUint, write_mask, preserve_vector, output);
    ctx->is_formatting_dest = previous_is_destination;
    return success;
}

void format_dest_operand_hlsl(HLSLEmitterContext* ctx, const DXBCOperand* op, bool isInt, bool isUint, int write_mask, bool preserve_vector, char* buf, size_t buf_sz) {
    StringBuilder expression;
    sb_init(&expression);
    if (!format_dest_operand_hlsl_sb(ctx, op, isInt, isUint, write_mask,
                                     preserve_vector, &expression) ||
        !hlsl_copy_checked(ctx, buf, buf_sz,
                           hlsl_builder_text(&expression))) {
        if (buf && buf_sz > 0) buf[0] = '\0';
    }
    sb_free(&expression);
}
