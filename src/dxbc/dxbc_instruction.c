// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_parser_internal.h"
#include "dxbc/dxbc_decoder.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void format_instruction_text(char* output, size_t output_size,
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
        if (output_size >= 4u) {
            memcpy(output + output_size - 4u, "...", 4u);
        } else {
            output[output_size - 1u] = '\0';
        }
    }
}

static void append_instruction_text(char* output, size_t output_size,
                                    const char* suffix) {
    if (!output || output_size == 0U || !suffix) return;
    size_t output_length = 0U;
    while (output_length < output_size && output[output_length] != '\0') {
        ++output_length;
    }
    size_t suffix_length = strlen(suffix);
    if (output_length == output_size) return;
    size_t available = output_size - output_length - 1u;
    if (suffix_length > available) {
        if (available > 0u) memcpy(output + output_length, suffix, available);
        output[output_size - 1u] = '\0';
        if (output_size >= 4u) {
            memcpy(output + output_size - 4u, "...", 4u);
        }
        return;
    }
    memcpy(output + output_length, suffix, suffix_length + 1U);
}

static const char* get_ret_type_str(uint32_t t) {
    switch (t) {
        case 1: return "unorm";
        case 2: return "snorm";
        case 3: return "int";
        case 4: return "uint";
        case 5: return "float";
        case 6: return "mixed";
        case 7: return "double";
        case 8: return "continued";
        case 9: return "unused";
        default: return "unknown";
    }
}

static bool decode_resource_return_types(uint32_t token,
                                         uint8_t return_types[4]) {
    if (!return_types || (token & 0xffff0000u) != 0) {
        return false;
    }
    for (unsigned int component = 0; component < 4; ++component) {
        uint32_t type = (token >> (component * 4u)) & 0xfu;
        if (type == 0 || type > 9) return false;
        return_types[component] = (uint8_t)type;
    }
    return true;
}

static void format_resource_return_types_diagnostic(
    const uint8_t return_types[4], char* formatted, size_t formatted_size) {
    if (!return_types || !formatted || formatted_size == 0u) return;
    (void)format_instruction_text(
        formatted, formatted_size, "%s,%s,%s,%s",
        get_ret_type_str(return_types[0]), get_ret_type_str(return_types[1]),
        get_ret_type_str(return_types[2]), get_ret_type_str(return_types[3]));
}

static bool instruction_read_u32(ByteStream* stream, size_t instruction_end,
                                 uint32_t* value) {
    return stream && stream->position <= instruction_end &&
           instruction_end <= stream->size &&
           instruction_end - stream->position >= sizeof(uint32_t) &&
           stream_read_uint32(stream, value);
}

static void clear_instruction_operands(DXBCInstruction* instruction) {
    if (!instruction) return;
    for (int operand = 0; operand < instruction->operand_count; ++operand) {
        free_operand(&instruction->operands[operand]);
    }
    instruction->operand_count = 0;
}

static bool reject_instruction(DXBCInstruction* instruction) {
    if (instruction && getenv("DXBC_DEBUG_STAGE")) {
        fprintf(stderr,
                "[dxbc] reject instruction offset=%u opcode=%u name=%s "
                "token=0x%08x operands=%d\n",
                instruction->file_offset, instruction->opcode,
                instruction->opcode_str, instruction->token,
                instruction->operand_count);
    }
    clear_instruction_operands(instruction);
    return false;
}

static bool append_required_operand(ByteStream* stream, size_t instruction_end,
                                    DXBCOperandParseContext context,
                                    uint32_t opcode,
                                    DXBCInstruction* instruction) {
    if (!instruction || instruction->operand_count >= DXBC_MAX_OPERANDS)
        return false;
    DXBCOperand* operand = parse_operand_recursive(
        stream, instruction_end, context, opcode, NULL);
    if (!operand) return false;
    instruction->operands[instruction->operand_count++] = *operand;
    mem_free(operand, sizeof(DXBCOperand));
    return true;
}

static bool reserve_instructions(DXBCContainer* container, int required) {
    if (!container || required < 0 || container->instruction_count < 0 ||
        container->instruction_alloc < container->instruction_count ||
        ((container->instruction_alloc == 0) !=
         (container->instructions == NULL))) {
        return false;
    }
    if (required <= container->instruction_alloc) return true;

    int new_allocation =
        container->instruction_alloc == 0 ? 128 : container->instruction_alloc;
    while (new_allocation < required) {
        if (new_allocation > INT_MAX / 2) {
            new_allocation = required;
            break;
        }
        new_allocation *= 2;
    }
    if (dxbc_size_multiply_overflows((size_t)container->instruction_alloc,
                                     sizeof(*container->instructions)) ||
        dxbc_size_multiply_overflows((size_t)new_allocation,
                                     sizeof(*container->instructions))) {
        return false;
    }
    const size_t old_size = (size_t)container->instruction_alloc *
                            sizeof(*container->instructions);
    const size_t new_size =
        (size_t)new_allocation * sizeof(*container->instructions);
    void* replacement =
        mem_realloc(container->instructions, old_size, new_size);
    if (!replacement) return false;
    container->instructions = (DXBCInstruction*)replacement;
    container->instruction_alloc = new_allocation;
    return true;
}

static bool allocate_immediate_constants(DXBCContainer* container,
                                         int dword_count) {
    if (!container || dword_count <= 0 || container->icb_value_count != 0 ||
        container->icb_value_alloc != 0 || container->icb_values ||
        dxbc_size_multiply_overflows((size_t)dword_count,
                                     sizeof(*container->icb_values))) {
        return false;
    }
    const size_t allocation_size =
        (size_t)dword_count * sizeof(*container->icb_values);
    container->icb_values = (uint32_t*)mem_alloc(allocation_size);
    if (!container->icb_values) return false;
    container->icb_value_alloc = dword_count;
    return true;
}

static bool declaration_binding_register(const DXBCInstruction *instruction,
                                         DXBCOperandType expected_type,
                                         int *out_register) {
    if (!instruction || instruction->operand_count != 1 || !out_register)
        return false;
    const DXBCOperand *operand = &instruction->operands[0];
    if (operand->type != expected_type || operand->register_index_dim != 1 ||
        operand->register_index < 0 ||
        operand->rel_op0 || !operand->index_has_immediate[0] ||
        operand->index_value_exceeds_int[0] ||
        operand->index_values[0] != (uint32_t)operand->register_index) {
        return false;
    }
    *out_register = operand->register_index;
    return true;
}

bool parse_shader_logic(ByteStream* stream, size_t next_pos, DXBCContainer* container) {
    if (!stream || !container || next_pos > stream->size) return false;
    uint32_t version_token;
    if (!instruction_read_u32(stream, next_pos, &version_token)) return false;
    uint32_t len;
    if (!instruction_read_u32(stream, next_pos, &len) || len < 2) return false;
    
    uint32_t type = version_token >> 16;
    uint8_t major = (uint8_t)((version_token >> 4) & 0xF);
    uint8_t minor = (uint8_t)(version_token & 0xF);

    /* Keep the semantic parser's accepted version domain identical to the
     * lossless stage contract.  Truncating the 16-bit program type to eight
     * bits previously let malformed tokens alias a supported stage and the
     * fallback "unk_*" string was still considered a successful parse. */
    if ((version_token & UINT32_C(0x0000ff00)) != 0u || type > 5u ||
        (major != 4u && major != 5u) ||
        (major == 4u && minor > 1u) ||
        (major == 5u && minor != 0u) ||
        ((type == 3u || type == 4u) && major != 5u)) {
        return false;
    }
    
    const char* shader_type = "unk";
    switch (type) {
        case 0: shader_type = "ps"; break;
        case 1: shader_type = "vs"; break;
        case 2: shader_type = "gs"; break;
        case 3: shader_type = "hs"; break;
        case 4: shader_type = "ds"; break;
        case 5: shader_type = "cs"; break;
    }
    format_instruction_text(container->shader_type_model,
                            sizeof(container->shader_type_model),
                            "%s_%u_%u", shader_type, (unsigned int)major,
                            (unsigned int)minor);
    if (stream->position > next_pos) return false;
    size_t instruction_bytes = (size_t)(len - 2) * 4;
    /* The program-length field covers the complete SHDR/SHEX payload.  A
     * shorter declaration would otherwise let malformed trailing bytes evade
     * instruction validation. */
    if (instruction_bytes != next_pos - stream->position) return false;
    size_t end_pos = stream->position + instruction_bytes;
    int instruction_id = 0;
    int indexable_temp_counter = 0;
    (void)indexable_temp_counter;
    
    while (stream->position < end_pos) {
        size_t start_pos = stream->position;
        bool skip_asm_formatting = false;
        uint32_t token;
        if (!instruction_read_u32(stream, end_pos, &token)) return false;
        
        uint32_t opcode = token & 0x7FF;
        /* SM4/5 encodes instruction length in bits 30:24.  Bit 31 is the
         * extended-opcode flag, so masking only five bits silently
         * desynchronizes on legal instructions longer than 31 DWORDs (most
         * notably large immediate constant buffers). */
        uint32_t inst_len = dxbc_tokenized_instruction_length(token);
        
        if (opcode == 53u) {
            if (!instruction_read_u32(stream, end_pos, &inst_len)) return false;
        } else if (inst_len == 0u) {
            return false;
        }
        if (inst_len == 0 || inst_len > (end_pos - start_pos) / 4) {
            return false;
        }
        size_t next_inst_pos = start_pos + (inst_len * 4);
        if (next_inst_pos < stream->position) return false;
        
        if (container->instruction_count == INT_MAX ||
            !reserve_instructions(container,
                                  container->instruction_count + 1)) {
            LOG_ERROR("Failed to allocate DXBC instructions array");
            return false;
        }
        
        int current_instruction_index = container->instruction_count;
        DXBCInstruction* inst =
            &container->instructions[current_instruction_index];
        memset(inst, 0, sizeof(DXBCInstruction));
        inst->token = token;
        inst->file_offset = (uint32_t)start_pos;
        inst->byte_length = (uint32_t)(next_inst_pos - start_pos);
        inst->opcode = opcode;
        
        const char* name = dxbc_opcode_name(opcode);
        if (!dxbc_opcode_is_known(opcode)) {
            (void)format_instruction_text(inst->opcode_str,
                                          sizeof(inst->opcode_str),
                                          "op_%u", opcode);
        } else {
            int j = 0;
            while (name[j] && (size_t)j + 1U < sizeof(inst->opcode_str)) {
                char c = name[j];
                if (c >= 'A' && c <= 'Z') c = c + 32;
                inst->opcode_str[j] = c;
                j++;
            }
            inst->opcode_str[j] = '\0';
        }
        
        inst->is_decl = dxbc_opcode_is_declaration(opcode);
        if (!inst->is_decl) {
            inst->precise_mask = (uint8_t)((token >> 19u) & 0x0fu);
        }

        /* CUSTOMDATA is a family, not an unconditional immediate-constant
         * buffer declaration.  The semantic projection currently models only
         * D3D10_SB_CUSTOMDATA_DCL_IMMEDIATE_CONSTANT_BUFFER (class 3).
         * Comments, debug info, opaque data, shader messages, and future
         * classes remain losslessly available through DXBCDocument but must
         * not be reinterpreted as float4 constants here. */
        if (opcode == 53 && ((token >> 11) & 0x001fffffu) != 3u) {
            return reject_instruction(inst);
        }

        if (opcode == 61) { /* RESINFO */
            inst->resource_info_return_type = (uint8_t)((token >> 11) & 0x3u);
            if (inst->resource_info_return_type == 3)
                return reject_instruction(inst);
        } else if (opcode == 111) { /* SAMPLEINFO */
            inst->sample_info_return_type =
                (uint8_t)((token >> 11u) & 0x3u);
            if (inst->sample_info_return_type > 1u)
                return reject_instruction(inst);
        }
        
        if (!inst->is_decl && (token & 0x2000) != 0) {
            inst->saturate = true;
        }
        
        const char *conditional_name = NULL;
        if (opcode == 31) conditional_name = "if";
        else if (opcode == 3) conditional_name = "breakc";
        else if (opcode == 8) conditional_name = "continuec";
        else if (opcode == 13) conditional_name = "discard";
        if (conditional_name) {
            inst->condition_test = (token & 0x40000u)
                                       ? DXBC_INSTRUCTION_TEST_NONZERO
                                       : DXBC_INSTRUCTION_TEST_ZERO;
            format_instruction_text(
                inst->opcode_str, sizeof(inst->opcode_str), "%s_%s",
                conditional_name,
                inst->condition_test == DXBC_INSTRUCTION_TEST_NONZERO
                    ? "nz" : "z");
        }
        
        int texOffsetU = 0, texOffsetV = 0, texOffsetW = 0;
        bool hasTexOffset = false;

        if ((token & 0x80000000) != 0) {
            bool has_more_extensions = true;
            while (has_more_extensions) {
                uint32_t ext;
                if (!instruction_read_u32(stream, next_inst_pos, &ext))
                    return reject_instruction(inst);
                uint32_t data = ext & 0x7FFFFFFF;
                uint32_t extType = data & 0x3F;

                if (extType == 1) {
                    if (inst->has_texel_offset) return reject_instruction(inst);
                    texOffsetU = (int)((data >> 9) & 0xF);
                    texOffsetV = (int)((data >> 13) & 0xF);
                    texOffsetW = (int)((data >> 17) & 0xF);
                    if (texOffsetU >= 8) texOffsetU -= 16;
                    if (texOffsetV >= 8) texOffsetV -= 16;
                    if (texOffsetW >= 8) texOffsetW -= 16;
                    inst->has_texel_offset = true;
                    inst->texel_offsets[0] = (int8_t)texOffsetU;
                    inst->texel_offsets[1] = (int8_t)texOffsetV;
                    inst->texel_offsets[2] = (int8_t)texOffsetW;
                    hasTexOffset = true;
                } else if (extType == 2) {
                    if (inst->has_resource_dimension)
                        return reject_instruction(inst);
                    uint32_t dim = (data >> 6) & 0x1F;
                    if (dim < 1 || dim > 12) return reject_instruction(inst);
                    inst->has_resource_dimension = true;
                    inst->resource_dimension = dim;
                    inst->structured_stride = (int)((data >> 11) & 0xfffu);
                } else if (extType == 3) {
                    if (inst->has_resource_return_types)
                        return reject_instruction(inst);
                    const uint32_t packed_types = data >> 6;
                    if (!decode_resource_return_types(
                            packed_types, inst->resource_return_types)) {
                        return reject_instruction(inst);
                    }
                    inst->has_resource_return_types = true;
                } else {
                    /* The semantic projection cannot faithfully represent an
                     * unknown opcode extension. DXBCDocument still preserves
                     * its raw bytes for diagnostics and future decoders. */
                    return reject_instruction(inst);
                }

                has_more_extensions = (ext & 0x80000000u) != 0;
            }
        }
        
        bool processed = false;
        
        if (opcode == 53) { // DCL_IMMEDIATECONSTANTBUFFER
            size_t data_bytes = next_inst_pos - stream->position;
            if (data_bytes == 0 || data_bytes % (4 * sizeof(uint32_t)) != 0)
                return reject_instruction(inst);
            const size_t dword_count_size = data_bytes / sizeof(uint32_t);
            if (dword_count_size > INT_MAX)
                return reject_instruction(inst);
            int dword_count = (int)dword_count_size;
            int vector_count = dword_count / 4;
            if (!allocate_immediate_constants(container, dword_count) ||
                vector_count > INT_MAX - container->instruction_count ||
                !reserve_instructions(
                    container, container->instruction_count + vector_count)) {
                return reject_instruction(inst);
            }
            inst = &container->instructions[current_instruction_index];
            
            strncpy(inst->opcode_str, "dcl_immediateConstantBuffer", sizeof(inst->opcode_str) - 1);
            inst->opcode_str[sizeof(inst->opcode_str) - 1] = '\0';
            inst->is_decl = true;
            
            container->icb_value_count = 0;
            for (int v = 0; v < vector_count; v++) {
                uint32_t raw_vals[4];
                for (int c = 0; c < 4; c++) {
                    if (!instruction_read_u32(stream, next_inst_pos,
                                              &raw_vals[c]))
                        return reject_instruction(inst);
                    container->icb_values[container->icb_value_count++] =
                        raw_vals[c];
                }

                char v_strs[4][64];
                for (int c = 0; c < 4; c++) {
                    dxbc_format_immediate_diagnostic(
                        raw_vals[c], 53u, false, v_strs[c],
                        sizeof(v_strs[c]));
                }
                
                if (v == 0) {
                    format_instruction_text(
                        inst->formatted_asm, sizeof(inst->formatted_asm),
                        "      dcl_immediateConstantBuffer { { %s, %s, "
                        "%s, %s},",
                        v_strs[0], v_strs[1], v_strs[2], v_strs[3]);
                } else {
                    container->instruction_count++;
                    DXBCInstruction* v_inst = &container->instructions[container->instruction_count];
                    memset(v_inst, 0, sizeof(DXBCInstruction));
                    v_inst->token = token;
                    v_inst->opcode = opcode;
                    v_inst->is_decl = true;
                    
                    format_instruction_text(
                        v_inst->formatted_asm,
                        sizeof(v_inst->formatted_asm),
                        v == vector_count - 1
                            ? "                              { %s, %s, "
                              "%s, %s} }"
                            : "                              { %s, %s, "
                              "%s, %s},",
                        v_strs[0], v_strs[1], v_strs[2], v_strs[3]);
                }
            }
            /* A large immediate-constant buffer can grow and relocate the
             * instruction array while emitting its continuation rows. */
            inst = &container->instructions[current_instruction_index];
            skip_asm_formatting = true;
            processed = true;
        } else if (opcode == 89) { // DCL_CONSTANTBUFFER
            if ((token & 0x80fff000u) != 0u)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_CONSTANT_BUFFER_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            uint32_t indexing = (token >> 11) & 0x1;
            if (indexing == 1) {
                append_instruction_text(inst->operands[0].text,
                                        sizeof(inst->operands[0].text),
                                        ", dynamicIndexed");
            } else {
                append_instruction_text(inst->operands[0].text,
                                        sizeof(inst->operands[0].text),
                                        ", immediateIndexed");
            }
            processed = true;
        } else if (opcode == 90) { // DCL_SAMPLER
            if ((token & 0x80ff8000u) != 0u)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            uint32_t mode = (token >> 11) & 0xF;
            if (mode > 2u) return reject_instruction(inst);
            if (mode == 1) {
                append_instruction_text(inst->operands[0].text,
                                        sizeof(inst->operands[0].text),
                                        ", mode_comparison");
            } else if (mode == 2) {
                append_instruction_text(inst->operands[0].text,
                                        sizeof(inst->operands[0].text),
                                        ", mode_mono");
            } else {
                append_instruction_text(inst->operands[0].text,
                                        sizeof(inst->operands[0].text),
                                        ", mode_default");
            }
            processed = true;
        } else if (opcode == 88) { // DCL_RESOURCE
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            int reg_idx = -1;
            if (!declaration_binding_register(
                    inst, OPERAND_TYPE_RESOURCE, &reg_idx))
                return reject_instruction(inst);

            uint32_t ret_type_token;
            if (!instruction_read_u32(stream, next_inst_pos, &ret_type_token))
                return reject_instruction(inst);
            {
                uint32_t dim = (token >> 11) & 0x1F;
                const uint32_t sample_count = (token >> 16) & 0x7fu;
                if (dim < 1 || dim > 10 ||
                    ((dim == 4 || dim == 9)
                         ? ((token & 0x00800000u) != 0)
                         : ((token & 0x00ff0000u) != 0))) {
                    return reject_instruction(inst);
                }
                const char* dim_str = dxbc_resource_dim_name(dim);
                
                char op_dim_str[128];
                if (dim == 4 || dim == 9) {
                    snprintf(op_dim_str, sizeof(op_dim_str), "%s(%u)", dim_str, sample_count);
                } else {
                    strcpy(op_dim_str, dim_str);
                }
                format_instruction_text(inst->opcode_str,
                                        sizeof(inst->opcode_str),
                                        "dcl_resource_%s", op_dim_str);

                char r_types[64];
                uint8_t return_types[4];
                if (!decode_resource_return_types(ret_type_token,
                                                  return_types)) {
                    return reject_instruction(inst);
                }
                format_resource_return_types_diagnostic(
                    return_types, r_types, sizeof(r_types));
                
                char operand_text[sizeof(inst->operands[0].text)];
                memcpy(operand_text, inst->operands[0].text,
                       sizeof(operand_text));
                format_instruction_text(inst->operands[0].text,
                                        sizeof(inst->operands[0].text),
                                        "(%s) %s", r_types, operand_text);
                
                DXBCResourceDecl* res =
                    dxbc_get_or_add_resource_declaration(container, reg_idx,
                                                         false);
                if (!res) return reject_instruction(inst);
                strcpy(res->dim_name, dim_str);
                strcpy(res->ret_types, r_types);
                res->dimension = dim;
                memcpy(res->return_types, return_types,
                       sizeof(res->return_types));
                res->sample_count =
                    (dim == 4 || dim == 9) ? sample_count : 0;
                res->is_structured = false;
                res->declared = true;
            }
            processed = true;
        } else if (opcode == 156) { // DCL_UAV_TYPED
            if ((token & 0x00fc0000u) != 0)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            int reg_idx = -1;
            if (!declaration_binding_register(inst, OPERAND_TYPE_UAV,
                                              &reg_idx))
                return reject_instruction(inst);
            
            uint32_t ret_type_token;
            if (!instruction_read_u32(stream, next_inst_pos, &ret_type_token))
                return reject_instruction(inst);
            {
                uint32_t dim = (token >> 11) & 0x1F;
                if (dim != 1 && dim != 2 && dim != 3 && dim != 5 &&
                    dim != 7 && dim != 8) return reject_instruction(inst);
                const char* dim_str = dxbc_resource_dim_name(dim);
                
                format_instruction_text(inst->opcode_str,
                                        sizeof(inst->opcode_str),
                                        "dcl_uav_typed_%s", dim_str);

                char r_types[64];
                uint8_t return_types[4];
                if (!decode_resource_return_types(ret_type_token,
                                                  return_types)) {
                    return reject_instruction(inst);
                }
                format_resource_return_types_diagnostic(
                    return_types, r_types, sizeof(r_types));
                
                char operand_text[sizeof(inst->operands[0].text)];
                memcpy(operand_text, inst->operands[0].text,
                       sizeof(operand_text));
                format_instruction_text(inst->operands[0].text,
                                        sizeof(inst->operands[0].text),
                                        "(%s) %s", r_types, operand_text);
                
                DXBCResourceDecl* res =
                    dxbc_get_or_add_resource_declaration(container, reg_idx,
                                                         true);
                if (!res) return reject_instruction(inst);
                strcpy(res->dim_name, dim_str);
                strcpy(res->ret_types, r_types);
                res->dimension = dim;
                memcpy(res->return_types, return_types,
                       sizeof(res->return_types));
                res->is_structured = false;
                res->globally_coherent = (token & 0x00010000u) != 0;
                res->rasterizer_ordered = (token & 0x00020000u) != 0;
                res->declared = true;
            }
            processed = true;
        } else if (opcode == 157) { // DCL_UAV_RAW
            if ((token & 0x00fcf800u) != 0)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            int reg_idx = -1;
            if (!declaration_binding_register(inst, OPERAND_TYPE_UAV,
                                              &reg_idx))
                return reject_instruction(inst);
            DXBCResourceDecl* res =
                dxbc_get_or_add_resource_declaration(container, reg_idx, true);
            if (!res) return reject_instruction(inst);
            strcpy(res->dim_name, "raw_buffer");
            res->dimension = 11;
            res->globally_coherent = (token & 0x00010000u) != 0;
            res->rasterizer_ordered = (token & 0x00020000u) != 0;
            res->declared = true;
            processed = true;
        } else if (opcode == 161) { // DCL_RESOURCE_RAW
            if ((token & 0x00fff800u) != 0)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            int reg_idx = -1;
            if (!declaration_binding_register(
                    inst, OPERAND_TYPE_RESOURCE, &reg_idx))
                return reject_instruction(inst);
            DXBCResourceDecl* res =
                dxbc_get_or_add_resource_declaration(container, reg_idx,
                                                     false);
            if (!res) return reject_instruction(inst);
            strcpy(res->dim_name, "raw_buffer");
            res->dimension = 11;
            res->declared = true;
            processed = true;
        } else if (opcode == 162) { // DCL_RESOURCE_STRUCTURED
            if ((token & 0x00fff800u) != 0)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            int reg_idx = -1;
            if (!declaration_binding_register(
                    inst, OPERAND_TYPE_RESOURCE, &reg_idx))
                return reject_instruction(inst);
            uint32_t stride_val = 0;
            if (!instruction_read_u32(stream, next_inst_pos, &stride_val))
                return reject_instruction(inst);
            if (stride_val == 0 || (stride_val & 3u) != 0 ||
                stride_val > INT_MAX) return reject_instruction(inst);
            inst->structured_stride = stride_val;
            snprintf(inst->operands[1].text, sizeof(inst->operands[1].text), "%d", inst->structured_stride);
            inst->operand_count = 2;
            
            DXBCResourceDecl* res =
                dxbc_get_or_add_resource_declaration(container, reg_idx,
                                                     false);
            if (!res) return reject_instruction(inst);
            strcpy(res->dim_name, "structured_buffer");
            res->dimension = 12;
            res->stride = inst->structured_stride;
            res->is_structured = true;
            res->declared = true;
            processed = true;
        } else if (opcode == 158) { // DCL_UAV_STRUCTURED
            if ((token & 0x007cf800u) != 0)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            int reg_idx = -1;
            if (!declaration_binding_register(inst, OPERAND_TYPE_UAV,
                                              &reg_idx))
                return reject_instruction(inst);
            uint32_t stride_val = 0;
            if (!instruction_read_u32(stream, next_inst_pos, &stride_val))
                return reject_instruction(inst);
            if (stride_val == 0 || (stride_val & 3u) != 0 ||
                stride_val > INT_MAX) return reject_instruction(inst);
            inst->structured_stride = stride_val;
            snprintf(inst->operands[1].text, sizeof(inst->operands[1].text), "%d", inst->structured_stride);
            inst->operand_count = 2;
            
            DXBCResourceDecl* res =
                dxbc_get_or_add_resource_declaration(container, reg_idx, true);
            if (!res) return reject_instruction(inst);
            strcpy(res->dim_name, "structured_buffer");
            res->dimension = 12;
            res->stride = inst->structured_stride;
            res->is_structured = true;
            res->globally_coherent = (token & 0x00010000u) != 0;
            res->rasterizer_ordered = (token & 0x00020000u) != 0;
            res->has_order_preserving_counter =
                (token & 0x00800000u) != 0;
            res->declared = true;
            processed = true;
        } else if (opcode == 91) { // DCL_INDEX_RANGE
            /* DCL_INDEX_RANGE is one tokenized register operand followed by
             * a scalar register count.  The count is not a second operand
             * token.  Preserve it as an exact immediate declaration payload
             * so hull phase/index-range authority survives semantic decode. */
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst)) {
                return reject_instruction(inst);
            }
            uint32_t register_count = 0;
            if (!instruction_read_u32(stream, next_inst_pos,
                                      &register_count) ||
                register_count == 0 || register_count > INT_MAX) {
                return reject_instruction(inst);
            }
            DXBCOperand *count = &inst->operands[1];
            count->type = OPERAND_TYPE_IMMEDIATE32;
            count->imm_value_count = 1;
            count->imm_values[0] = register_count;
            count->immediate_word_count = 1;
            count->immediate_words[0] = register_count;
            format_instruction_text(count->text, sizeof(count->text), "%u",
                                    register_count);
            inst->operand_count = 2;
            processed = true;
        } else if ((opcode >= 113 && opcode <= 116) ||
                   (opcode >= 147 && opcode <= 151)) {
            /* Hull declaration/phase tokens and the scalar tessellator
             * declarations encode their complete payload in the opcode
             * token.  Treating the control bits as an operand token rejects
             * valid hs_5_0 programs (and, worse, loses the phase boundary).
             * DXBCStageContract validates the stage, ordering and value
             * domains directly from the lossless token stream. */
            if (stream->position != next_inst_pos)
                return reject_instruction(inst);
            processed = true;
        } else if ((opcode >= 152 && opcode <= 154) || opcode == 206) {
            /* The hull maximum tessellation factor, hull phase instance
             * counts, and geometry-shader instance count are one scalar
             * DWORD, not a tokenized operand. Preserve the exact word in the
             * semantic declaration for diagnostics; the raw stage contract
             * remains the authority consumed by USIL. */
            uint32_t scalar = 0;
            if (!instruction_read_u32(stream, next_inst_pos,
                                      &scalar) ||
                ((opcode == 153 || opcode == 154) && scalar == 0) ||
                (opcode == 206 && (scalar == 0 || scalar > 32))) {
                return reject_instruction(inst);
            }
            format_instruction_text(inst->operands[0].text,
                                    sizeof(inst->operands[0].text),
                                    opcode == 152 ? "0x%08X" : "%u",
                                    scalar);
            inst->operands[0].type = OPERAND_TYPE_IMMEDIATE32;
            inst->operands[0].imm_value_count = 1;
            inst->operands[0].imm_values[0] = scalar;
            inst->operands[0].immediate_word_count = 1;
            inst->operands[0].immediate_words[0] = scalar;
            inst->operand_count = 1;
            processed = true;
        } else if (opcode == 104) { // DCL_TEMPS
            uint32_t num_temps;
            if (!instruction_read_u32(stream, next_inst_pos, &num_temps) ||
                num_temps > INT_MAX)
                return reject_instruction(inst);
            snprintf(inst->operands[0].text, sizeof(inst->operands[0].text), "%u", num_temps);
            inst->operands[0].register_index = (int)num_temps;
            inst->operand_count = 1;
            processed = true;
        } else if (opcode == 105) { // DCL_INDEXABLETEMP
            strcpy(inst->opcode_str, "dcl_indexabletemp");
            uint32_t reg_idx, reg_count, comp_count;
            if (!instruction_read_u32(stream, next_inst_pos, &reg_idx) ||
                !instruction_read_u32(stream, next_inst_pos, &reg_count) ||
                !instruction_read_u32(stream, next_inst_pos, &comp_count) ||
                reg_idx > INT_MAX || reg_count > INT_MAX ||
                comp_count > INT_MAX)
                return reject_instruction(inst);
            snprintf(inst->operands[0].text, sizeof(inst->operands[0].text), "x%u[%u]", reg_idx, reg_count);
            inst->operands[0].type = OPERAND_TYPE_INDEXABLE_TEMP;
            inst->operands[0].register_index = (int)reg_idx;
            inst->operands[0].rel_offset1 = (int)reg_count;
            snprintf(inst->operands[1].text, sizeof(inst->operands[1].text), "%u", comp_count);
            inst->operands[1].register_index = (int)comp_count;
            inst->operand_count = 2;
            processed = true;
        } else if (opcode == 93) { // DCL_INPUTPRIMITIVE
            strcpy(inst->opcode_str, "dcl_inputprimitive");
            uint32_t prim = (token >> 11) & 0x3F;
            const char* prim_str = "";
            char patch_buf[32];
            bool has_primitive_diagnostic = true;
            switch (prim) {
                case 1: prim_str = "point"; break;
                case 2: prim_str = "line"; break;
                case 3: prim_str = "triangle"; break;
                case 6: prim_str = "line_adj"; break;
                case 7: prim_str = "triangle_adj"; break;
                default:
                    if (prim >= 8 && prim <= 39) {
                        snprintf(patch_buf, sizeof(patch_buf), "patch%u", prim - 7);
                        prim_str = patch_buf;
                    } else {
                        has_primitive_diagnostic = false;
                    }
                    break;
            }
            if (has_primitive_diagnostic) {
                strcpy(inst->operands[0].text, prim_str);
                inst->operand_count = 1;
            }
            processed = true;
        } else if (opcode == 92) { // DCL_OUTPUTTOPOLOGY
            strcpy(inst->opcode_str, "dcl_outputtopology");
            uint32_t topo = (token >> 11) & 0x3F;
            const char* topo_str = "";
            bool has_topology_diagnostic = true;
            switch (topo) {
                case 1: topo_str = "pointlist"; break;
                case 2: topo_str = "linelist"; break;
                case 3: topo_str = "linestrip"; break;
                case 4: topo_str = "trianglelist"; break;
                case 5: topo_str = "trianglestrip"; break;
                case 10: topo_str = "linelist_adj"; break;
                case 11: topo_str = "linestrip_adj"; break;
                case 12: topo_str = "trianglelist_adj"; break;
                case 13: topo_str = "trianglestrip_adj"; break;
                default: has_topology_diagnostic = false; break;
            }
            if (has_topology_diagnostic) {
                strcpy(inst->operands[0].text, topo_str);
                inst->operand_count = 1;
            }
            processed = true;
        } else if (opcode == 94) { // DCL_MAXOUT
            strcpy(inst->opcode_str, "dcl_maxout");
            uint32_t max_out;
            if (!instruction_read_u32(stream, next_inst_pos, &max_out))
                return reject_instruction(inst);
            snprintf(inst->operands[0].text, sizeof(inst->operands[0].text), "%u", max_out);
            inst->operand_count = 1;
            processed = true;
        } else if (opcode == 106) { // DCL_GLOBALFLAGS
            strcpy(inst->opcode_str, "dcl_globalFlags");
            uint32_t flags = (token >> 11) & 0x1FFF;
            /* D3D11's complete global-flag domain is bits 0..8. Unknown
             * control bits must not survive as apparently benign metadata. */
            if ((flags & ~UINT32_C(0x1ff)) != 0)
                return reject_instruction(inst);
            char flags_str[256] = "";
            if (flags & 1) strcat(flags_str, "refactoringAllowed ");
            if (flags & 2) strcat(flags_str, "doublePrecisionFloatOps ");
            if (flags & 4) strcat(flags_str, "forceEarlyDepthStencil ");
            if (flags & 8) strcat(flags_str, "enableRawAndStructuredBuffers ");
            if (flags & 16) strcat(flags_str, "skipOptimization ");
            if (flags & 32) strcat(flags_str, "enableMinimumPrecision ");
            if (flags & 64) strcat(flags_str, "enableDoubleExtensions ");
            if (flags & 128) strcat(flags_str, "enableShaderExtensions ");
            if (flags & 256) strcat(flags_str, "allResourcesBound ");
            
            size_t flags_length = strlen(flags_str);
            if (flags_length > 0u) {
                flags_str[flags_length - 1u] = '\0';
            }
            
            format_instruction_text(inst->operands[0].text,
                                    sizeof(inst->operands[0].text), "%s",
                                    flags_str);
            inst->operand_count = flags != 0u ? 1 : 0;
            processed = true;
        } else if (opcode == 95 || opcode == 101) {
            if ((token & UINT32_C(0x80fff800)) != 0u ||
                !append_required_operand(
                    stream, next_inst_pos,
                    DXBC_OPERAND_CONTEXT_DECLARATION, opcode, inst)) {
                return reject_instruction(inst);
            }
            processed = true;
        } else if (opcode == 96 || opcode == 97 || opcode == 99 ||
                   opcode == 100 || opcode == 102 || opcode == 103) {
            // SGV / SIV declarations
            /* The SDK prose labels PS_SGV control bits ignored, but FXC emits
             * CONSTANT (1) there for integer SGVs. Preserve the canonical
             * token projection instead of discarding observed authority. */
            const bool has_interpolation = opcode == 99u || opcode == 100u;
            if ((token & (has_interpolation ? UINT32_C(0x80ff8000)
                                             : UINT32_C(0x80fff800))) != 0u)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            
            uint32_t sv_id;
            if (!instruction_read_u32(stream, next_inst_pos, &sv_id) ||
                (sv_id & UINT32_C(0xffff0000)) != 0u || sv_id == 0u ||
                sv_id > 25u)
                return reject_instruction(inst);
            inst->has_declaration_system_value = true;
            inst->declaration_system_value = sv_id;
            const char* sv_name = dxbc_sys_value_name(sv_id);
            snprintf(inst->operands[1].text, sizeof(inst->operands[1].text), "%s", sv_name);
            inst->operand_count = 2;
            
            if (has_interpolation) {
                uint32_t interp = (token >> 11) & 0x1F;
                if (interp > 7u) return reject_instruction(inst);
                inst->has_declaration_interpolation = true;
                inst->declaration_interpolation = (uint8_t)interp;
                const char* interp_str = "";
                switch (interp) {
                    case 1: interp_str = "constant "; break;
                    case 2: interp_str = "linear "; break;
                    case 3: interp_str = "linear centroid "; break;
                    case 4: interp_str = "linear noperspective "; break;
                    case 5: interp_str = "linear noperspective centroid "; break;
                    case 6: interp_str = "linear sample "; break;
                    case 7: interp_str = "linear noperspective sample "; break;
                }
                if (interp >= 1u && interp <= 7u) {
                    char operand_text[sizeof(inst->operands[0].text)];
                    memcpy(operand_text, inst->operands[0].text,
                           sizeof(operand_text));
                    format_instruction_text(inst->operands[0].text,
                                            sizeof(inst->operands[0].text),
                                            "%s%s", interp_str,
                                            operand_text);
                }
            }
            processed = true;
        } else if (opcode == 98) { // DCL_INPUT_PS
            if ((token & UINT32_C(0x80ff8000)) != 0u)
                return reject_instruction(inst);
            if (!append_required_operand(stream, next_inst_pos,
                                         DXBC_OPERAND_CONTEXT_DECLARATION,
                                         opcode, inst))
                return reject_instruction(inst);
            
            uint32_t interp = (token >> 11) & 0x1F;
            if (interp > 7u) return reject_instruction(inst);
            inst->has_declaration_interpolation = true;
            inst->declaration_interpolation = (uint8_t)interp;
            const char* interp_str = "";
            switch (interp) {
                case 1: interp_str = "constant "; break;
                case 2: interp_str = "linear "; break;
                case 3: interp_str = "linear centroid "; break;
                case 4: interp_str = "linear noperspective "; break;
                case 5: interp_str = "linear noperspective centroid "; break;
                case 6: interp_str = "linear sample "; break;
                case 7: interp_str = "linear noperspective sample "; break;
            }
            if (interp >= 1u && interp <= 7u) {
                char operand_text[sizeof(inst->operands[0].text)];
                memcpy(operand_text, inst->operands[0].text,
                       sizeof(operand_text));
                format_instruction_text(inst->operands[0].text,
                                        sizeof(inst->operands[0].text),
                                        "%s%s", interp_str, operand_text);
            }
            processed = true;
        }
        
        if (!processed) {
            inst->operand_count = 0;
            while (stream->position < next_inst_pos) {
                if (!append_required_operand(
                        stream, next_inst_pos,
                        inst->is_decl ? DXBC_OPERAND_CONTEXT_DECLARATION
                                      : DXBC_OPERAND_CONTEXT_EXECUTABLE,
                        opcode, inst)) {
                    return reject_instruction(inst);
                }
            }
        }

        if (stream->position != next_inst_pos)
            return reject_instruction(inst);
        
        // SM5.0 instruction overrides
        if (major >= 5u) {
            int res_op_idx = -1;
            int uav_op_idx = -1;
            for (int k = 0; k < inst->operand_count; k++) {
                if (inst->operands[k].type == OPERAND_TYPE_RESOURCE) {
                    res_op_idx = k;
                } else if (inst->operands[k].type == OPERAND_TYPE_UAV) {
                    uav_op_idx = k;
                }
            }
            
            if (res_op_idx != -1) {
                int reg_idx = inst->operands[res_op_idx].register_index;
                const DXBCResourceDecl* res =
                    dxbc_find_resource_declaration(container, reg_idx, false);
                if (res && res->declared) {
                    if ((opcode >= 69 && opcode <= 74) || opcode == 109 ||
                        opcode == 45 || opcode == 46 || opcode == 61) {
                        
                        char base_opcode[sizeof(inst->opcode_str)];
                        memcpy(base_opcode, inst->opcode_str,
                               sizeof(base_opcode));
                        format_instruction_text(
                            inst->opcode_str, sizeof(inst->opcode_str),
                            "%s_indexable(%s)(%s)", base_opcode,
                            res->dim_name, res->ret_types);
                    } else if (opcode == 167) {
                        if (res->is_structured) {
                            format_instruction_text(
                                inst->opcode_str,
                                sizeof(inst->opcode_str),
                                "ld_structured_indexable(structured_buffer, "
                                "stride=%u)(mixed,mixed,mixed,mixed)",
                                res->stride);
                        }
                    }
                }
            } else if (uav_op_idx != -1) {
                int reg_idx = inst->operands[uav_op_idx].register_index;
                const DXBCResourceDecl* uav =
                    dxbc_find_resource_declaration(container, reg_idx, true);
                if (uav && uav->declared) {
                    if (opcode == 168) {
                        if (uav->is_structured) {
                            format_instruction_text(
                                inst->opcode_str,
                                sizeof(inst->opcode_str),
                                "store_structured_indexable(structured_buffer, "
                                "stride=%u)",
                                uav->stride);
                        }
                    } else if (opcode == 167) {
                        if (uav->is_structured) {
                            format_instruction_text(
                                inst->opcode_str,
                                sizeof(inst->opcode_str),
                                "ld_structured_indexable(structured_buffer, "
                                "stride=%u)(mixed,mixed,mixed,mixed)",
                                uav->stride);
                        }
                    }
                }
            }
        }
        
        if (!skip_asm_formatting) {
            char full_opcode[256] = {0};
            format_instruction_text(full_opcode, sizeof(full_opcode), "%s",
                                    inst->opcode_str);
            if (inst->saturate)
                append_instruction_text(full_opcode, sizeof(full_opcode),
                                        "_sat");
            if (hasTexOffset) {
                char suffix[64];
                format_instruction_text(suffix, sizeof(suffix),
                                        "_aoffimmi(%d,%d,%d)", texOffsetU,
                                        texOffsetV, texOffsetW);
                append_instruction_text(full_opcode, sizeof(full_opcode),
                                        suffix);
            }
            uint32_t ret_modifier = (inst->token >> 11) & 0x3;
            if (opcode == 61) {
                if (ret_modifier == 1) {
                    append_instruction_text(full_opcode,
                                            sizeof(full_opcode),
                                            "_rcpfloat");
                } else if (ret_modifier == 2) {
                    append_instruction_text(full_opcode,
                                            sizeof(full_opcode), "_uint");
                }
            } else if (opcode == 111 &&
                       inst->sample_info_return_type == 1u) {
                append_instruction_text(full_opcode, sizeof(full_opcode),
                                        "_uint");
            }
            
            // Build formatted assembly string
            char asm_buf[512] = "";
            if (inst->is_decl) {
                format_instruction_text(asm_buf, sizeof(asm_buf), "      ");
            } else {
                format_instruction_text(asm_buf, sizeof(asm_buf), "%4d: ",
                                        instruction_id);
                instruction_id++;
            }
            append_instruction_text(asm_buf, sizeof(asm_buf), full_opcode);
            
            for (int k = 0; k < inst->operand_count; k++) {
                append_instruction_text(asm_buf, sizeof(asm_buf),
                                        k == 0 ? " " : ", ");
                append_instruction_text(asm_buf, sizeof(asm_buf),
                                        inst->operands[k].text);
            }
            
            format_instruction_text(inst->formatted_asm,
                                    sizeof(inst->formatted_asm), "%s",
                                    asm_buf);
        }
        
        stream->position = next_inst_pos;
        container->instruction_count++;
    }
    if (stream->position != end_pos) return false;
    container->program_type = (DXBCProgramType)type;
    container->major_version = major;
    container->minor_version = minor;
    container->has_executable_program = true;
    return true;
}
