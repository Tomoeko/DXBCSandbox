// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

const char* get_type_str(uint32_t comp_type, int components) {
    if (comp_type == 1) { // uint
        if (components == 1) return "uint";
        if (components == 2) return "uint2";
        if (components == 3) return "uint3";
        return "uint4";
    } else if (comp_type == 2) { // int
        if (components == 1) return "int";
        if (components == 2) return "int2";
        if (components == 3) return "int3";
        return "int4";
    } else { // float
        if (components == 1) return "float";
        if (components == 2) return "float2";
        if (components == 3) return "float3";
        return "float4";
    }
}

int get_mask_component_count(int mask) {
    if (mask == 0) return 4;
    int count = 0;
    if (mask & 16) count++;
    if (mask & 32) count++;
    if (mask & 64) count++;
    if (mask & 128) count++;
    return count;
}

bool is_mask_non_contiguous(int mask) {
    int m = mask >> 4;
    return (m == 5 || m == 9 || m == 10 || m == 11 || m == 13);
}

bool has_non_contiguous_swizzle(const DXBCOperand* op, int write_mask) {
    int active_count = 0;
    if (write_mask & 16)  active_count++;
    if (write_mask & 32)  active_count++;
    if (write_mask & 64)  active_count++;
    if (write_mask & 128) active_count++;
    if (active_count <= 1) return false;

    if (op->swizzle_mode == 0) return false;
    if (op->swizzle_mode == 2) return true;
    
    if (op->swizzle_mode == 1) {
        int active[4];
        int ac = 0;
        if (write_mask & 16)  active[ac++] = 0;
        if (write_mask & 32)  active[ac++] = 1;
        if (write_mask & 64)  active[ac++] = 2;
        if (write_mask & 128) active[ac++] = 3;
        
        for (int i = 1; i < ac; i++) {
            if (op->swizzle[active[i]] != op->swizzle[active[0]] + i) {
                return true;
            }
        }
    }
    return false;
}

bool is_signed_int_op(USILOpcode opcode) {
    return opcode == USIL_OP_IADD || opcode == USIL_OP_IMUL ||
           opcode == USIL_OP_IMAD || opcode == USIL_OP_IMAX ||
           opcode == USIL_OP_IMIN || opcode == USIL_OP_INEG ||
           opcode == USIL_OP_IGE || opcode == USIL_OP_ILT ||
           opcode == USIL_OP_IEQ || opcode == USIL_OP_INE;
}

uint32_t hlsl_output_register_component_type(const USILProgram* program,
                                             uint32_t register_index) {
    if (!program || program->output_count < 0 ||
        (program->output_count > 0 && !program->outputs)) return 0;
    uint32_t result = 0;
    for (int index = 0; index < program->output_count; ++index) {
        const DXBCSignatureElement* element = &program->outputs[index];
        if (element->register_id != register_index) continue;
        if (element->component_type < 1 || element->component_type > 3 ||
            (result != 0 && result != element->component_type)) {
            return 0;
        }
        result = element->component_type;
    }
    return result;
}

bool is_unsigned_int_op(USILOpcode opcode) {
    return opcode == USIL_OP_UMAX || opcode == USIL_OP_UMIN ||
           opcode == USIL_OP_UDIV || opcode == USIL_OP_UGE ||
           opcode == USIL_OP_ULT;
}

const char* get_cast_type_str(const char* base_type, int comps) {
    if (strcmp(base_type, "int") == 0) {
        if (comps == 1) return "int";
        if (comps == 2) return "int2";
        if (comps == 3) return "int3";
        return "int4";
    } else if (strcmp(base_type, "uint") == 0) {
        if (comps == 1) return "uint";
        if (comps == 2) return "uint2";
        if (comps == 3) return "uint3";
        return "uint4";
    } else { // float
        if (comps == 1) return "float";
        if (comps == 2) return "float2";
        if (comps == 3) return "float3";
        return "float4";
    }
}

bool is_replicate_swizzle(const DXBCOperand* op) {
    if (op->swizzle_mode == 2) return true;
    if (op->swizzle_mode == 1) {
        return (op->swizzle[0] == op->swizzle[1] && 
                op->swizzle[1] == op->swizzle[2] && 
                op->swizzle[2] == op->swizzle[3]);
    }
    return false;
}

bool is_operand_scalar(const DXBCOperand* op, const SerializedProgramParameters* params) {
    if (op->type == OPERAND_TYPE_IMMEDIATE32) {
        return op->imm_value_count == 1;
    }
    
    if (is_replicate_swizzle(op)) {
        return true;
    }
    
    // Check if it's a constant buffer variable
    if (op->type == OPERAND_TYPE_CONSTANT_BUFFER && op->register_index_dim == 2) {
        int var_offset = 0;
        const char* var_name = resolve_cb_variable(params, op->register_index, op->rel_offset0, -1, &var_offset);
        if (var_name) {
            uint32_t dim = 4;
            resolve_variable_info(params, var_name, &dim, NULL, NULL);
            if (dim == 1) {
                return true;
            }
        }
    }
    
    return false;
}

int get_operand_priority(const DXBCOperand* op) {
    switch (op->type) {
        case OPERAND_TYPE_TEMP: return 0;
        case OPERAND_TYPE_INPUT: return 1;
        case OPERAND_TYPE_CONSTANT_BUFFER: return 2;
        case OPERAND_TYPE_IMMEDIATE32: return 3;
        default: return 4;
    }
}

bool is_non_canonical_add(const DXBCOperand* op0, const DXBCOperand* op1) {
    int p0 = get_operand_priority(op0);
    int p1 = get_operand_priority(op1);
    if (p0 > p1) return true;
    if (p0 < p1) return false;
    
    if (op0->register_index > op1->register_index) return true;
    if (op0->register_index < op1->register_index) return false;
    
    int comp0 = (op0->swizzle_mode == 2) ? op0->swizzle[0] : (op0->swizzle_mode == 1 ? op0->swizzle[0] : 0);
    int comp1 = (op1->swizzle_mode == 2) ? op1->swizzle[0] : (op1->swizzle_mode == 1 ? op1->swizzle[0] : 0);
    if (comp0 > comp1) return true;
    if (comp0 < comp1) return false;
    
    // Sort negated operands first in HLSL (so compiler swaps them to last in DXBC)
    if (!op0->has_neg && op1->has_neg) return true;
    
    return false;
}

bool is_register_program_output(HLSLEmitterContext* ctx, int reg_idx) {
    if (!ctx->program) return false;
    for (int i = 0; i < ctx->program->output_count; i++) {
        if (ctx->program->outputs[i].register_id == (uint32_t)reg_idx) {
            return true;
        }
    }
    return false;
}

bool is_scalar_integer_op(USILOpcode op) {
    return (op == USIL_OP_AND || op == USIL_OP_OR || op == USIL_OP_XOR || op == USIL_OP_NOT ||
            op == USIL_OP_ISHL || op == USIL_OP_ISHR || op == USIL_OP_USHR ||
            op == USIL_OP_UBFE ||
            op == USIL_OP_IADD || op == USIL_OP_IMUL || op == USIL_OP_IMAD ||
            op == USIL_OP_INEG ||
            op == USIL_OP_IMIN || op == USIL_OP_IMAX || op == USIL_OP_UMIN || op == USIL_OP_UMAX);
}

bool is_componentwise_op(USILOpcode op) {
  return (op == USIL_OP_ADD || op == USIL_OP_SUB || op == USIL_OP_MUL ||
          op == USIL_OP_DIV || op == USIL_OP_IADD || op == USIL_OP_IMUL ||
          op == USIL_OP_IMAD ||
          op == USIL_OP_IMAX || op == USIL_OP_IMIN || op == USIL_OP_UMAX ||
          op == USIL_OP_UMIN || op == USIL_OP_UDIV || op == USIL_OP_INEG ||
          op == USIL_OP_NOT || op == USIL_OP_AND || op == USIL_OP_OR ||
          op == USIL_OP_XOR || op == USIL_OP_ISHL || op == USIL_OP_ISHR ||
          op == USIL_OP_USHR || op == USIL_OP_UBFE ||
          op == USIL_OP_MIN || op == USIL_OP_MAX ||
          op == USIL_OP_EQ || op == USIL_OP_GE || op == USIL_OP_LT ||
          op == USIL_OP_NE || op == USIL_OP_MAD || op == USIL_OP_MOVC ||
          op == USIL_OP_ILT || op == USIL_OP_IGE || op == USIL_OP_IEQ ||
          op == USIL_OP_INE || op == USIL_OP_ULT || op == USIL_OP_UGE ||
          op == USIL_OP_FTOI || op == USIL_OP_FTOU || op == USIL_OP_ITOF ||
          op == USIL_OP_UTOF);
}

bool is_cross_product_pattern(const USILProgram *program, int idx,
                              int *out_op1_reg, int *out_op2_reg) {
  (void)out_op1_reg;
  (void)out_op2_reg;
  if (idx + 1 >= program->instruction_count)
    return false;
  const USILInstruction *inst1 = &program->instructions[idx];
  const USILInstruction *inst2 = &program->instructions[idx + 1];

  if (inst1->opcode != USIL_OP_MUL || inst2->opcode != USIL_OP_MAD)
    return false;
  if (inst1->operand_count < 3 || inst2->operand_count < 4)
    return false;

  const DXBCOperand *dest1 = &inst1->operands[0];
  const DXBCOperand *src1_0 = &inst1->operands[1];
  const DXBCOperand *src1_1 = &inst1->operands[2];

  const DXBCOperand *dest2 = &inst2->operands[0];
  const DXBCOperand *src2_0 = &inst2->operands[1];
  const DXBCOperand *src2_1 = &inst2->operands[2];
  const DXBCOperand *src2_2 = &inst2->operands[3];

  if (dest1->type != OPERAND_TYPE_TEMP)
    return false;
  if (dest2->type != OPERAND_TYPE_TEMP)
    return false;

  if (src2_2->type != OPERAND_TYPE_TEMP ||
      src2_2->register_index != dest1->register_index || !src2_2->has_neg)
    return false;

  if (src1_0->type != src2_0->type ||
      src1_0->register_index != src2_0->register_index)
    return false;
  if (src1_1->type != src2_1->type ||
      src1_1->register_index != src2_1->register_index)
    return false;

  if (src1_0->swizzle_mode != 1 || src1_0->swizzle[0] != 2 ||
      src1_0->swizzle[1] != 0 || src1_0->swizzle[2] != 1)
    return false;
  if (src1_1->swizzle_mode != 1 || src1_1->swizzle[0] != 0 ||
      src1_1->swizzle[1] != 1 || src1_1->swizzle[2] != 2)
    return false;
  if (src2_0->swizzle_mode != 1 || src2_0->swizzle[0] != 1 ||
      src2_0->swizzle[1] != 2 || src2_0->swizzle[2] != 0)
    return false;
  if (src2_1->swizzle_mode != 1 || src2_1->swizzle[0] != 1 ||
      src2_1->swizzle[1] != 2 || src2_1->swizzle[2] != 0)
    return false;

  return true;
}

bool inst_writes_to_dest(const USILInstruction* inst) {
  USILOpcode op = inst->opcode;
  if (op == USIL_OP_IF || op == USIL_OP_ELSE || op == USIL_OP_ENDIF ||
      op == USIL_OP_LOOP || op == USIL_OP_ENDLOOP ||
      op == USIL_OP_SWITCH || op == USIL_OP_CASE || op == USIL_OP_DEFAULT || op == USIL_OP_ENDSWITCH ||
      op == USIL_OP_BREAK || op == USIL_OP_BREAKC || op == USIL_OP_CONTINUE || op == USIL_OP_CONTINUEC ||
      op == USIL_OP_RET || op == USIL_OP_DISCARD ||
      op == USIL_OP_GEOMETRY_APPEND ||
      op == USIL_OP_GEOMETRY_RESTART_STRIP) {
    return false;
  }
  return (inst->operand_count >= 1 && inst->operands[0].type != OPERAND_TYPE_NULL);
}

static HLSLBackingStorage temp_lane_backing_storage(
    const HLSLEmitterContext* ctx, int reg, int component) {
    if (!ctx || reg < 0 || reg >= ctx->temp_state_count || component < 0 ||
        component >= 4)
        return HLSL_BACKING_STORAGE_INVALID;
    if (ctx->has_int_temp[reg][component])
        return HLSL_BACKING_STORAGE_SINT;
    if (ctx->has_ftoi_temp[reg][component] ||
        ctx->has_deferred_float[reg][component])
        return HLSL_BACKING_STORAGE_FLOAT;
    if (ctx->has_write_redirect[reg][component]) {
        uint8_t storage = ctx->write_redirect_storage[reg][component];
        if (storage > HLSL_BACKING_STORAGE_UINT)
            return HLSL_BACKING_STORAGE_INVALID;
        return (HLSLBackingStorage)storage;
    }
    return ctx->use_uint_temps ? HLSL_BACKING_STORAGE_UINT
                               : HLSL_BACKING_STORAGE_FLOAT;
}

HLSLBackingStorage hlsl_operand_backing_storage(
    const HLSLEmitterContext* ctx, const DXBCOperand* operand,
    int write_mask, bool destination) {
    if (!ctx || !operand) return HLSL_BACKING_STORAGE_INVALID;
    if (operand->type == OPERAND_TYPE_INDEXABLE_TEMP) {
        return ctx->use_uint_temps ? HLSL_BACKING_STORAGE_UINT
                                   : HLSL_BACKING_STORAGE_FLOAT;
    }
    if (operand->type != OPERAND_TYPE_TEMP)
        return HLSL_BACKING_STORAGE_FLOAT;
    if (operand->register_index < 0 ||
        operand->register_index >= ctx->temp_state_count)
        return HLSL_BACKING_STORAGE_INVALID;

    int active_mask = write_mask;
    if (active_mask == 0) {
        if (!destination && is_replicate_swizzle(operand))
            active_mask = 16 << operand->swizzle[0];
        else
            active_mask = operand->swizzle_mode == 0
                              ? operand->destination_mask
                              : 0xf0;
    }

    HLSLBackingStorage combined = HLSL_BACKING_STORAGE_INVALID;
    for (int lane = 0; lane < 4; ++lane) {
        if ((active_mask & (16 << lane)) == 0) continue;
        int component = lane;
        if (!destination) {
            if (operand->swizzle_mode == 1)
                component = operand->swizzle[lane];
            else if (operand->swizzle_mode == 2)
                component = operand->swizzle[0];
        }
        HLSLBackingStorage lane_storage = temp_lane_backing_storage(
            ctx, operand->register_index, component);
        if (lane_storage == HLSL_BACKING_STORAGE_INVALID)
            return lane_storage;
        if (combined == HLSL_BACKING_STORAGE_INVALID)
            combined = lane_storage;
        else if (combined != lane_storage)
            return HLSL_BACKING_STORAGE_MIXED;
    }
    return combined;
}

static void format_native_operand_common(
    HLSLEmitterContext* ctx, const DXBCOperand* operand, int write_mask,
    bool preserve_vector, bool destination, char* buffer,
    size_t buffer_size) {
    HLSLBackingStorage storage = hlsl_operand_backing_storage(
        ctx, operand, write_mask, destination);
    if (storage == HLSL_BACKING_STORAGE_INVALID ||
        storage == HLSL_BACKING_STORAGE_MIXED) {
        if (ctx && ctx->sb) ctx->sb->failed = true;
        if (buffer && buffer_size > 0) buffer[0] = '\0';
        return;
    }
    bool as_uint = storage == HLSL_BACKING_STORAGE_UINT;
    if (destination)
        format_dest_operand_hlsl(ctx, operand, false, as_uint, write_mask,
                                 preserve_vector, buffer, buffer_size);
    else
        format_operand_hlsl(ctx, operand, false, as_uint, write_mask,
                            preserve_vector, buffer, buffer_size);
}

static bool format_native_operand_common_sb(
    HLSLEmitterContext* ctx, const DXBCOperand* operand, int write_mask,
    bool preserve_vector, bool destination, StringBuilder* output) {
    HLSLBackingStorage storage = hlsl_operand_backing_storage(
        ctx, operand, write_mask, destination);
    if (storage == HLSL_BACKING_STORAGE_INVALID ||
        storage == HLSL_BACKING_STORAGE_MIXED) {
        if (ctx && ctx->sb) ctx->sb->failed = true;
        if (output) {
            sb_clear(output);
            output->failed = true;
        }
        return false;
    }
    bool as_uint = storage == HLSL_BACKING_STORAGE_UINT;
    if (destination)
        return format_dest_operand_hlsl_sb(
            ctx, operand, false, as_uint, write_mask, preserve_vector, output);
    return format_operand_hlsl_sb(ctx, operand, false, as_uint, write_mask,
                                  preserve_vector, output);
}

void format_native_operand_hlsl(HLSLEmitterContext* ctx,
                                const DXBCOperand* operand, int write_mask,
                                bool preserve_vector, char* buffer,
                                size_t buffer_size) {
    format_native_operand_common(ctx, operand, write_mask, preserve_vector,
                                 false, buffer, buffer_size);
}

void format_native_dest_operand_hlsl(HLSLEmitterContext* ctx,
                                     const DXBCOperand* operand,
                                     int write_mask, bool preserve_vector,
                                     char* buffer, size_t buffer_size) {
    format_native_operand_common(ctx, operand, write_mask, preserve_vector,
                                 true, buffer, buffer_size);
}

bool format_native_operand_hlsl_sb(HLSLEmitterContext* ctx,
                                   const DXBCOperand* operand,
                                   int write_mask, bool preserve_vector,
                                   StringBuilder* output) {
    return format_native_operand_common_sb(ctx, operand, write_mask,
                                           preserve_vector, false, output);
}

bool format_native_dest_operand_hlsl_sb(HLSLEmitterContext* ctx,
                                        const DXBCOperand* operand,
                                        int write_mask,
                                        bool preserve_vector,
                                        StringBuilder* output) {
    return format_native_operand_common_sb(ctx, operand, write_mask,
                                           preserve_vector, true, output);
}

HLSLExpressionKind hlsl_instruction_expression_kind(
    const HLSLEmitterContext* ctx, const USILInstruction* instruction) {
    if (instruction && is_scalar_integer_op(instruction->opcode)) {
        return ctx && ctx->use_uint_temps
                   ? HLSL_EXPRESSION_NATIVE_INTEGER
                   : HLSL_EXPRESSION_INTEGER_BITS_AS_FLOAT;
    }
    return HLSL_EXPRESSION_FLOAT_VALUE;
}

static bool unwrap_complete_asfloat_call(char* expression) {
    static const char prefix[] = "asfloat(";
    if (!expression || strncmp(expression, prefix, sizeof(prefix) - 1) != 0)
        return false;
    size_t length = strlen(expression);
    if (length <= sizeof(prefix) || expression[length - 1] != ')')
        return false;

    int depth = 1;
    for (size_t offset = sizeof(prefix) - 1; offset < length; ++offset) {
        char value = expression[offset];
        if (value == '(') {
            ++depth;
        } else if (value == ')') {
            --depth;
            if (depth == 0 && offset != length - 1) return false;
            if (depth < 0) return false;
        }
    }
    if (depth != 0) return false;

    size_t inner_length = length - (sizeof(prefix) - 1) - 1;
    memmove(expression, expression + sizeof(prefix) - 1, inner_length);
    expression[inner_length] = '\0';
    return true;
}

bool hlsl_retarget_expression_for_storage(
    HLSLEmitterContext* ctx, HLSLExpressionKind expression_kind,
    HLSLBackingStorage destination_storage, char* expression) {
    if (destination_storage != HLSL_BACKING_STORAGE_SINT &&
        destination_storage != HLSL_BACKING_STORAGE_UINT)
        return true;
    if (expression_kind == HLSL_EXPRESSION_NATIVE_INTEGER) return true;
    if (expression_kind == HLSL_EXPRESSION_INTEGER_BITS_AS_FLOAT &&
        unwrap_complete_asfloat_call(expression))
        return true;
    if (ctx && ctx->sb) ctx->sb->failed = true;
    if (expression) expression[0] = '\0';
    return false;
}

bool hlsl_format_checked(HLSLEmitterContext* ctx, char* buf, size_t buf_sz,
                         const char* format, ...) {
    if (!buf || buf_sz == 0 || !format) {
        if (ctx && ctx->sb) ctx->sb->failed = true;
        return false;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(buf, buf_sz, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= buf_sz) {
        buf[0] = '\0';
        if (ctx && ctx->sb) ctx->sb->failed = true;
        return false;
    }
    return true;
}

bool hlsl_copy_checked(HLSLEmitterContext* ctx, char* buf, size_t buf_sz,
                       const char* value) {
    return hlsl_format_checked(ctx, buf, buf_sz, "%s", value ? value : "");
}

void get_signature_swizzle(const DXBCSignatureElement* el, char* out_swizzle, size_t max_sz) {
    if (el->mask == 0 || el->mask == 15) {
        if (max_sz > 0) {
            out_swizzle[0] = '\0';
        }
        return;
    }
    int idx = 0;
    if (idx < (int)max_sz - 1) out_swizzle[idx++] = '.';
    if ((el->mask & 1) && idx < (int)max_sz - 1) out_swizzle[idx++] = 'x';
    if ((el->mask & 2) && idx < (int)max_sz - 1) out_swizzle[idx++] = 'y';
    if ((el->mask & 4) && idx < (int)max_sz - 1) out_swizzle[idx++] = 'z';
    if ((el->mask & 8) && idx < (int)max_sz - 1) out_swizzle[idx++] = 'w';
    out_swizzle[idx] = '\0';
}

uint8_t hlsl_output_written_mask(const USILProgram* program,
                                 const DXBCSignatureElement* element) {
    if (!element) return 0u;
    /* OSGN/OSG1 ReadWriteMask has output-specific polarity: set bits are
     * lanes the executable never writes.  Emitting those lanes as initialized
     * HLSL fields fabricates dcl_output/write instructions.  It is authority
     * only when the signature chunks were actually parsed; hand-constructed
     * USIL programs and legacy callers have no such guarantee. */
    if (!program || !program->has_parsed_signature_authority)
        return (uint8_t)(element->mask & 0x0fu);
    return (uint8_t)(element->mask & (uint8_t)~element->rw_mask & 0x0fu);
}

const DXBCSignatureElement* find_matching_output_signature(const USILProgram* program, uint32_t register_id, int mask) {
    if (mask == 0) mask = 15;
    else mask = (mask >> 4) & 15;
    const DXBCSignatureElement* best_match = NULL;
    int best_overlap = 0;

    for (int i = 0; i < program->output_count; i++) {
        const DXBCSignatureElement* el = &program->outputs[i];
        if (el->register_id == register_id) {
            int overlap = hlsl_output_written_mask(program, el) & mask;
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best_match = el;
            }
        }
    }

    if (!best_match) {
        for (int i = 0; i < program->output_count; i++) {
            const DXBCSignatureElement* el = &program->outputs[i];
            if (el->register_id == register_id &&
                hlsl_output_written_mask(program, el) != 0u) {
                return el;
            }
        }
    }

    return best_match;
}

