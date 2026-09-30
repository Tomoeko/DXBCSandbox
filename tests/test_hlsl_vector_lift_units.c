// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter.h"
#include "translation/hlsl_source_quality.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)

static DXBCOperand operand(DXBCOperandType type, int index, uint8_t lanes) {
    DXBCOperand value = {0};
    value.type = type;
    value.register_index = index;
    value.register_index_dim = 1;
    value.index_has_immediate[0] = true;
    value.index_values[0] = (uint32_t)index;
    value.destination_mask = lanes << 4;
    value.swizzle_mode = 1;
    for (uint8_t lane = 0; lane < 4; ++lane)
        value.swizzle[lane] = lane;
    return value;
}

static USILProgram program(USILInstruction *instructions, int count,
                           DXBCSignatureElement *input, DXBCSignatureElement *output) {
    USILProgram value = {0};
    memcpy(value.shader_type_model, "ps_5_0", sizeof("ps_5_0"));
    value.instructions = instructions;
    value.instruction_count = value.instruction_alloc = count;
    value.temp_count = 2;
    value.inputs = input;
    value.input_count = value.input_alloc = 1;
    value.outputs = output;
    value.output_count = value.output_alloc = 1;
    value.has_stage_contract = true;
    value.program_type = DXBC_PROGRAM_TYPE_PIXEL;
    value.shader_model_major = 5;
    return value;
}

static bool check_widths(void) {
    for (uint8_t lanes = 1; lanes <= 15; ++lanes) {
        USILInstruction instructions[4] = {0};
        instructions[0].opcode = USIL_OP_MUL;
        instructions[0].operand_count = 3;
        instructions[0].operands[0] = operand(OPERAND_TYPE_TEMP, 0, lanes);
        instructions[0].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
        instructions[0].operands[2] = operand(OPERAND_TYPE_INPUT, 0, 0);
        instructions[1].opcode = USIL_OP_ADD;
        instructions[1].operand_count = 3;
        instructions[1].operands[0] = operand(OPERAND_TYPE_TEMP, 1, lanes);
        instructions[1].operands[1] = operand(OPERAND_TYPE_TEMP, 0, 0);
        instructions[1].operands[2] = operand(OPERAND_TYPE_TEMP, 0, 0);
        instructions[2].opcode = USIL_OP_MOV;
        instructions[2].operand_count = 2;
        instructions[2].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, lanes);
        instructions[2].operands[1] = operand(OPERAND_TYPE_TEMP, 1, 0);
        instructions[3].opcode = USIL_OP_RET;
        DXBCSignatureElement input = {
            .semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15};
        DXBCSignatureElement output = {
            .semantic_name = "SV_Target", .component_type = 3, .system_value = 64,
            .mask = lanes};
        USILProgram value = program(instructions, 4, &input, &output);
        HLSLExpressionSourceMap map;
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        options.expression_source_map = &map;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
        int width = 0;
        for (int lane = 0; lane < 4; ++lane)
            width += (lanes >> lane) & 1;
        const char *declaration = width == 1 ? "const float dxbc_value_i0" :
                                  width == 2 ? "const float2 dxbc_value_i0" :
                                  width == 3 ? "const float3 dxbc_value_i0" :
                                               "const float4 dxbc_value_i0";
        CHECK(strstr(source.buf, declaration));
        CHECK(!strstr(source.buf, "float4 r") && !strstr(source.buf, "r0."));
        CHECK(map.origins[0].destination_lanes == lanes);
        CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
        map.origins[0].destination_lanes ^= 1;
        CHECK(!hlsl_expression_source_map_matches(&map, &value, source.buf));
        sb_free(&source);
    }
    return true;
}

static bool check_partial_lifetime_and_rejections(void) {
    USILInstruction instructions[4] = {0};
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].operands[0] = operand(OPERAND_TYPE_TEMP, 0, 6);
    instructions[0].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[1] = instructions[0];
    instructions[1].operands[0] = operand(OPERAND_TYPE_TEMP, 0, 1);
    instructions[2] = instructions[0];
    instructions[2].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 3);
    instructions[2].operands[1] = operand(OPERAND_TYPE_TEMP, 0, 0);
    instructions[2].operands[1].swizzle[0] = 2;
    instructions[2].operands[1].swizzle[1] = 1;
    instructions[3].opcode = USIL_OP_RET;
    DXBCSignatureElement input = {
        .semantic_name = "TEXCOORD", .component_type = 3, .mask = 15, .rw_mask = 15};
    DXBCSignatureElement output = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 3};
    USILProgram value = program(instructions, 4, &input, &output);
    HLSLExpressionSourceMap map;
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    CHECK(map.origins[1].kind == HLSL_EXPRESSION_ORIGIN_DEAD);
    CHECK(strstr(source.buf, ").yx"));
    CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
    sb_free(&source);
    /* Lanes from distinct partial writes cannot impersonate one logical value. */
    instructions[2].operands[1].swizzle[0] = 0;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    CHECK(!map.complete && map.count == 0);
    sb_free(&source);
    /* Nor can a never-defined lane, precision control, or output hole pass. */
    instructions[2].operands[1].swizzle[0] = 3;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    sb_free(&source);
    instructions[2].operands[1].swizzle[0] = 2;
    instructions[0].precise_mask = 1;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    sb_free(&source);
    instructions[0].precise_mask = 0;
    output.mask = 7;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    sb_free(&source);
    return true;
}

static bool check_material_authority(void) {
    USILInstruction instructions[2] = {0};
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
    instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 0, 0);
    instructions[0].operands[1].register_index_dim = 2;
    instructions[0].operands[1].index_has_immediate[1] = true;
    instructions[1].opcode = USIL_OP_RET;
    DXBCSignatureElement output = {
        .semantic_name = "SV_Target", .component_type = 3, .system_value = 64, .mask = 15};
    USILProgram value = program(instructions, 2, NULL, &output);
    value.input_count = value.input_alloc = 0;
    USILConstantBuffer decoded_buffer = {.reg_idx = 0, .size = 1};
    value.cbuffers = &decoded_buffer;
    value.cbuffer_count = value.cbuffer_alloc = 1;
    SerializedVariable color = {.name = "_Color", .layout = {0, 0, 0, 4, 0, 0}};
    SerializedConstantBuffer buffer = {.name = "$Globals", .variables = &color, .var_count = 1};
    SerializedResourceParam binding = {.name = "$Globals",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER, .bind_index = 0};
    SerializedProgramParameters metadata = {.constant_buffers = &buffer, .cb_count = 1,
                                            .resources = &binding, .res_count = 1};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, &metadata, NULL, NULL, &options));
    CHECK(strstr(source.buf, "return (_Color)"));
    CHECK(!strstr(source.buf, "o0") && !strstr(source.buf, "get_cb"));
    CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
    sb_free(&source);
    for (int mutation = 0; mutation < 5; ++mutation) {
        if (mutation == 0) color.layout[2] = 1; /* Integer bits cannot become float values. */
        if (mutation == 1) color.layout[3] = 2; /* Raw row crossing a field's end. */
        if (mutation == 2) color.layout[1] = 1; /* Array contract is not yet admitted. */
        if (mutation == 3) decoded_buffer.dynamic_indexed = true;
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&value, &source, mutation == 4 ? NULL : &metadata,
                                       NULL, NULL, &options));
        CHECK(!map.complete && map.count == 0);
        sb_free(&source);
        color.layout[1] = color.layout[2] = 0;
        color.layout[3] = 4;
        decoded_buffer.dynamic_indexed = false;
    }
    return true;
}

static bool check_vector_operations(void) {
    const USILOpcode opcodes[] = {USIL_OP_MAD, USIL_OP_DIV, USIL_OP_MIN, USIL_OP_MAX};
    const uint8_t masks[] = {1, 3, 7, 15};
    for (size_t operation = 0; operation < sizeof(opcodes) / sizeof(opcodes[0]); ++operation) {
        for (size_t width = 0; width < sizeof(masks) / sizeof(masks[0]); ++width) {
            USILInstruction instructions[2] = {0};
            instructions[0].opcode = opcodes[operation];
            instructions[0].operand_count = opcodes[operation] == USIL_OP_MAD ? 4 : 3;
            instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, masks[width]);
            for (int source = 1; source < instructions[0].operand_count; ++source)
                instructions[0].operands[source] = operand(OPERAND_TYPE_INPUT, 0, 0);
            instructions[1].opcode = USIL_OP_RET;
            DXBCSignatureElement input = {.semantic_name = "TEXCOORD", .component_type = 3,
                                          .mask = 15, .rw_mask = 15};
            DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                           .system_value = 64, .mask = masks[width]};
            USILProgram value = program(instructions, 2, &input, &output);
            HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
            HLSLExpressionSourceMap map;
            options.expression_source_map = &map;
            StringBuilder source;
            sb_init(&source);
            CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
            CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
            CHECK(strstr(source.buf, operation == 0 ? " * " : operation == 1 ? " / " :
                                      operation == 2 ? "min(" : "max("));
            CHECK(operation != 0 || strstr(source.buf, " + "));
            sb_free(&source);
            /* Precision and saturation remain outside this candidate domain. */
            for (int mutation = 0; mutation < 3; ++mutation) {
                instructions[0].precise_mask = mutation == 0;
                instructions[0].saturate = mutation == 1;
                instructions[0].operands[1].min_precision = mutation == 2;
                sb_init(&source);
                CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
                CHECK(!map.complete && !map.count);
                sb_free(&source);
            }
            instructions[0].precise_mask = 0;
            instructions[0].saturate = false;
            instructions[0].operands[1].min_precision = 0;
            instructions[0].operands[1].has_abs = true;
            instructions[0].operands[1].has_neg = true;
            sb_init(&source);
            CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
            CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
            CHECK(strstr(source.buf, "abs("));
            CHECK(strstr(source.buf, "-"));
            sb_free(&source);
        }
    }
    return true;
}

static bool check_material_reflection_layout(void) {
    USILInstruction instructions[2] = {0};
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
    instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 0, 0);
    instructions[0].operands[1].register_index_dim = 2;
    instructions[0].operands[1].index_has_immediate[1] = true;
    instructions[0].operands[1].index_values[1] = 2;
    instructions[0].operands[1].rel_offset0 = 2;
    instructions[1].opcode = USIL_OP_RET;
    DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                   .system_value = 64, .mask = 15};
    USILProgram value = program(instructions, 2, NULL, &output);
    value.input_count = value.input_alloc = 0;
    USILConstantBuffer decoded = {.reg_idx = 0, .size = 3};
    value.cbuffers = &decoded;
    value.cbuffer_count = value.cbuffer_alloc = 1;
    SerializedVariable fields[] = {
        {.name = "_Color", .layout = {32, 0, 0, 4, 0, 0}},
        {.name = "_Scale", .layout = {48, 0, 0, 1, 0, 0}},
    };
    SerializedConstantBuffer buffer = {.name = "$Globals", .size = 64,
        .role = SERIALIZED_CBUFFER_NAMED, .variables = fields, .var_count = 2};
    SerializedResourceParam binding = {.name = "$Globals", .bind_index = 0,
                                       .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER};
    SerializedProgramParameters metadata = {.constant_buffers = &buffer, .cb_count = 1,
                                            .resources = &binding, .res_count = 1};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, &metadata, NULL, NULL, &options));
    CHECK(strstr(source.buf, "float4 _Color : register(c2);"));
    CHECK(strstr(source.buf, "float _Scale : register(c3);"));
    CHECK(!strstr(source.buf, "cb0_") && !strstr(source.buf, "get_cb"));
    sb_free(&source);
    /* Packing, unknown trailing fields and missing reflection size cannot
     * authorize the compact shell. Their existing layout remains available. */
    for (int mutation = 0; mutation < 3; ++mutation) {
        fields[1].layout[0] = mutation == 0 ? 52 : 48;
        buffer.var_count = mutation == 1 ? 1 : 2;
        buffer.size = mutation == 2 ? 0 : 64;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&value, &source, &metadata, NULL, NULL, &options));
        CHECK(!strstr(source.buf, "register(c"));
        sb_free(&source);
    }
    fields[1].layout[0] = 48;
    buffer.var_count = 2;
    buffer.size = 64;
    options = (HLSLEmitOptions)HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, &metadata, NULL, NULL, &options));
    CHECK(strstr(source.buf, "cb0_0") && !strstr(source.buf, "register(c"));
    sb_free(&source);
    return true;
}

static bool check_float_intrinsics(void) {
    const USILOpcode operations[] = {USIL_OP_DP2, USIL_OP_DP3, USIL_OP_DP4,
        USIL_OP_RCP, USIL_OP_RSQ, USIL_OP_SQRT, USIL_OP_LOG, USIL_OP_EXP,
        USIL_OP_FRC, USIL_OP_ROUND_NE, USIL_OP_ROUND_NI, USIL_OP_ROUND_PI,
        USIL_OP_ROUND_Z};
    for (size_t operation = 0; operation < sizeof(operations) / sizeof(operations[0]); ++operation) {
        const bool dot = operation < 3;
        for (unsigned width = 1; width <= 4; ++width) {
            USILInstruction instructions[3] = {0};
            /* A dot consumes source xyz independently of its destination mask.
             * Put the source in one compact SSA value to test that demand. */
            const uint8_t input_lanes = dot ? (uint8_t)((1u << (operation + 2)) - 1u)
                                            : (uint8_t)((1u << width) - 1u);
            instructions[0].opcode = USIL_OP_MOV;
            instructions[0].operand_count = 2;
            instructions[0].operands[0] = operand(OPERAND_TYPE_TEMP, 0, input_lanes);
            instructions[0].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
            instructions[1].opcode = operations[operation];
            instructions[1].operand_count = dot ? 3 : 2;
            instructions[1].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0,
                                                  (uint8_t)((1u << width) - 1u));
            instructions[1].operands[1] = operand(OPERAND_TYPE_TEMP, 0, 0);
            if (dot) instructions[1].operands[2] = operand(OPERAND_TYPE_INPUT, 0, 0);
            instructions[2].opcode = USIL_OP_RET;
            DXBCSignatureElement input = {.semantic_name = "TEXCOORD", .component_type = 3,
                                           .mask = 15, .rw_mask = 15};
            DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                .system_value = 64, .mask = (uint8_t)((1u << width) - 1u)};
            USILProgram value = program(instructions, 3, &input, &output);
            HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
            HLSLExpressionSourceMap map;
            options.expression_source_map = &map;
            StringBuilder source;
            sb_init(&source);
            CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
            CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
            CHECK(!strstr(source.buf, "float4 r0"));
            if (dot) CHECK(strstr(source.buf, "dot("));
            sb_free(&source);
            if (dot && operation > 0) {
                /* One demanded lane from a different/undefined definition
                 * cannot inherit the other lanes' producer. */
                instructions[0].operands[0].destination_mask &= ~(1 << (operation + 5));
                sb_init(&source);
                CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
                CHECK(!map.complete);
                sb_free(&source);
            }
        }
    }
    return true;
}

static bool check_packed_material_selection(void) {
    USILInstruction instructions[2] = {0};
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 6);
    instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 0, 0);
    instructions[0].operands[1].register_index_dim = 2;
    instructions[0].operands[1].index_has_immediate[1] = true;
    instructions[1].opcode = USIL_OP_RET;
    DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                   .system_value = 64, .mask = 6};
    USILProgram value = program(instructions, 2, NULL, &output);
    value.input_count = value.input_alloc = 0;
    USILConstantBuffer decoded = {.reg_idx = 0, .size = 1};
    value.cbuffers = &decoded;
    value.cbuffer_count = value.cbuffer_alloc = 1;
    SerializedVariable fields[] = {
        {.name = "unusedScalar", .layout = {0, 0, 0, 1, 0, 0}},
        {.name = "materialPair", .layout = {4, 0, 0, 2, 0, 0}},
    };
    SerializedConstantBuffer buffer = {.name = "$Globals", .size = 16,
                                       .variables = fields, .var_count = 2};
    SerializedResourceParam binding = {.name = "$Globals", .bind_index = 0,
                                       .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER};
    SerializedProgramParameters metadata = {.constant_buffers = &buffer, .cb_count = 1,
                                            .resources = &binding, .res_count = 1};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, &metadata, NULL, NULL, &options));
    CHECK(strstr(source.buf, "= (materialPair);") && !strstr(source.buf, "get_cb"));
    CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
    sb_free(&source);
    /* Consuming the unused scalar instead would cross two metadata fields. */
    instructions[0].operands[1].swizzle[1] = 0;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&value, &source, &metadata, NULL, NULL, &options));
    CHECK(!map.complete);
    sb_free(&source);
    return true;
}

static bool check_scalar_broadcast_and_projection(void) {
    USILInstruction instructions[5] = {0};
    instructions[0].opcode = USIL_OP_DP3;
    instructions[0].operand_count = 3;
    instructions[0].operands[0] = operand(OPERAND_TYPE_TEMP, 0, 7);
    instructions[0].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[0].operands[2] = operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[1].opcode = USIL_OP_SQRT;
    instructions[1].operand_count = 2;
    instructions[1].operands[0] = operand(OPERAND_TYPE_TEMP, 1, 7);
    instructions[1].operands[1] = operand(OPERAND_TYPE_TEMP, 0, 0);
    instructions[2].opcode = USIL_OP_ADD;
    instructions[2].operand_count = 3;
    instructions[2].operands[0] = operand(OPERAND_TYPE_TEMP, 0, 7);
    instructions[2].operands[1] = operand(OPERAND_TYPE_TEMP, 1, 0);
    instructions[2].operands[2] = operand(OPERAND_TYPE_TEMP, 1, 0);
    instructions[3].opcode = USIL_OP_MUL;
    instructions[3].operand_count = 3;
    instructions[3].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
    instructions[3].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[3].operands[2] = operand(OPERAND_TYPE_TEMP, 0, 0);
    for (int component = 0; component < 4; ++component)
        instructions[3].operands[2].swizzle[component] = 1;
    instructions[4].opcode = USIL_OP_RET;
    DXBCSignatureElement input = {.semantic_name = "TEXCOORD", .component_type = 3,
                                   .mask = 15, .rw_mask = 15};
    DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                   .system_value = 64, .mask = 15};
    USILProgram value = program(instructions, 5, &input, &output);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    options.expression_source_map = &map;
    options.source_quality = &quality;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "const float dxbc_value_i1 = sqrt(dot("));
    CHECK(!strstr(source.buf, "float3(sqrt(") && !strstr(source.buf, ".yyyy"));
    CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);

    /* A true vector keeps its width; selecting one repeated weight is scalar.
     * Reverse component transport remains visible in the quality result. */
    instructions[0].opcode = USIL_OP_ADD;
    instructions[1].opcode = USIL_OP_MOV;
    instructions[2].operands[1] = operand(OPERAND_TYPE_TEMP, 0, 0);
    instructions[2].operands[2] = operand(OPERAND_TYPE_TEMP, 0, 0);
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, ").y") && !strstr(source.buf, ".yyyy"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    instructions[3].operands[0].destination_mask = 0x30;
    output.mask = 3;
    instructions[3].operands[2].swizzle[0] = 2;
    instructions[3].operands[2].swizzle[1] = 1;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, ").zy"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_MIXED);
    sb_free(&source);
    return true;
}

static bool check_modifier_extension_authority(void) {
    USILInstruction instructions[2] = {0};
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
    instructions[0].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[0].operands[1].has_abs = instructions[0].operands[1].has_neg = true;
    uint32_t extensions[2] = {UINT32_C(0xc1), UINT32_C(0xc1)};
    instructions[0].operands[1].extended_tokens = extensions;
    instructions[0].operands[1].extended_token_count = 1;
    instructions[1].opcode = USIL_OP_RET;
    DXBCSignatureElement input = {.semantic_name = "TEXCOORD", .component_type = 3,
                                   .mask = 15, .rw_mask = 15};
    DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                   .system_value = 64, .mask = 15};
    USILProgram value = program(instructions, 2, &input, &output);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    options.expression_source_map = &map;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
    CHECK(strstr(source.buf, "abs("));
    sb_free(&source);
    for (int mutation = 0; mutation < 4; ++mutation) {
        extensions[0] = mutation == 0 ? UINT32_C(0x41) : UINT32_C(0xc1);
        instructions[0].operands[1].extended_token_count = mutation == 1 ? 2 : 1;
        instructions[0].operands[1].extended_tokens = mutation == 2 ? NULL : extensions;
        instructions[0].operands[1].min_precision = mutation == 3 ? 1 : 0;
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
        CHECK(!map.complete);
        sb_free(&source);
    }
    return true;
}

static bool check_derivative_sites(void) {
    const USILOpcode operations[] = {USIL_OP_DERIV_RTX, USIL_OP_DERIV_RTY,
        USIL_OP_DERIV_RTX_COARSE, USIL_OP_DERIV_RTY_COARSE,
        USIL_OP_DERIV_RTX_FINE, USIL_OP_DERIV_RTY_FINE};
    for (size_t operation = 0; operation < sizeof(operations) / sizeof(operations[0]); ++operation) {
        USILInstruction instructions[3] = {0};
        instructions[0].opcode = operations[operation];
        instructions[0].operand_count = 2;
        instructions[0].operands[0] = operand(OPERAND_TYPE_TEMP, 0, 15);
        instructions[0].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
        instructions[1].opcode = USIL_OP_MOV;
        instructions[1].operand_count = 2;
        instructions[1].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
        instructions[1].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
        instructions[2].opcode = USIL_OP_RET;
        DXBCSignatureElement input = {.semantic_name = "TEXCOORD", .component_type = 3,
                                       .mask = 15, .rw_mask = 15};
        DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                       .system_value = 64, .mask = 15};
        USILProgram value = program(instructions, 3, &input, &output);
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLExpressionSourceMap map;
        options.expression_source_map = &map;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
        const char *effect = strstr(source.buf, "const float4 dxbc_value_i0 = ");
        const char *returned = strstr(source.buf, "return ");
        CHECK(effect && returned && effect < returned);
        CHECK(map.origins[0].kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION);
        CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
        map.origins[0].kind = HLSL_EXPRESSION_ORIGIN_DEAD;
        map.origins[0].source_begin = map.origins[0].source_end = 0;
        CHECK(!hlsl_expression_source_map_matches(&map, &value, source.buf));
        sb_free(&source);
        for (int mutation = 0; mutation < 4; ++mutation) {
            value.program_type = mutation == 0 ? DXBC_PROGRAM_TYPE_VERTEX : DXBC_PROGRAM_TYPE_PIXEL;
            instructions[0].precise_mask = mutation == 1;
            instructions[0].saturate = mutation == 2;
            instructions[0].operands[1].min_precision = mutation == 3;
            sb_init(&source);
            CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
            CHECK(!map.complete);
            sb_free(&source);
        }
    }
    return true;
}

static bool check_material_component_purpose(void) {
    USILInstruction instructions[2] = {0};
    instructions[0].opcode = USIL_OP_MOV;
    instructions[0].operand_count = 2;
    instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 7);
    instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 0, 0);
    instructions[0].operands[1].register_index_dim = 2;
    instructions[0].operands[1].index_has_immediate[1] = true;
    instructions[1].opcode = USIL_OP_RET;
    DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                   .system_value = 64, .mask = 7};
    USILProgram value = program(instructions, 2, NULL, &output);
    value.input_count = value.input_alloc = 0;
    USILConstantBuffer decoded = {.reg_idx = 0, .size = 1};
    value.cbuffers = &decoded;
    value.cbuffer_count = value.cbuffer_alloc = 1;
    SerializedVariable color = {.name = "materialColor", .layout = {0, 0, 0, 4, 0, 0}};
    SerializedConstantBuffer buffer = {.name = "$Globals", .size = 16,
                                       .variables = &color, .var_count = 1};
    SerializedResourceParam binding = {.name = "$Globals", .bind_index = 0,
                                       .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER};
    SerializedProgramParameters metadata = {.constant_buffers = &buffer, .cb_count = 1,
                                            .resources = &binding, .res_count = 1};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
    options.source_quality = &quality;
    options.expression_source_map = &map;
    for (int selection = 0; selection < 3; ++selection) {
        for (int component = 0; component < 3; ++component)
            instructions[0].operands[1].swizzle[component] =
                selection == 0 ? (uint8_t)component : selection == 1 ? 1 : (uint8_t)(2 - component);
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&value, &source, &metadata, NULL, NULL, &options));
        CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
        CHECK(strstr(source.buf, selection == 0 ? "materialColor.xyz" :
                                  selection == 1 ? "materialColor.y" : "materialColor.zyx"));
        CHECK(!strstr(source.buf, ".yyy"));
        CHECK(quality.classification == (selection == 2 ? HLSL_SOURCE_QUALITY_LOW_LEVEL
                                                       : HLSL_SOURCE_QUALITY_CLEAN));
        CHECK(quality.counts.residual_total == (selection == 2 ? 1u : 0u));
        sb_free(&source);
    }
    return true;
}

static bool check_replicated_dot_arity(void) {
    for (int width = 2; width <= 4; ++width) {
        USILInstruction instructions[2] = {0};
        instructions[0].opcode = width == 2 ? USIL_OP_DP2 : width == 3 ? USIL_OP_DP3 : USIL_OP_DP4;
        instructions[0].operand_count = 3;
        instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
        instructions[0].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
        instructions[0].operands[2] = operand(OPERAND_TYPE_INPUT, 0, 0);
        for (int component = 0; component < 4; ++component) {
            instructions[0].operands[1].swizzle[component] = 0;
            instructions[0].operands[2].swizzle[component] = 1;
        }
        instructions[1].opcode = USIL_OP_RET;
        DXBCSignatureElement input = {.semantic_name = "TEXCOORD", .component_type = 3,
                                     .mask = 3, .rw_mask = 3};
        DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                      .system_value = 64, .mask = 15};
        USILProgram value = program(instructions, 2, &input, &output);
        HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLExpressionSourceMap map;
        HLSLSourceQualityResult quality;
        options.expression_source_map = &map;
        options.source_quality = &quality;
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
        const char *cast = width == 2 ? "((float2)(" : width == 3 ? "((float3)(" : "((float4)(";
        const char *first = strstr(source.buf, cast);
        CHECK(first && strstr(first + strlen(cast), cast));
        CHECK(strstr(source.buf, "dot(") && !strstr(source.buf, ".xxx"));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(hlsl_expression_source_map_matches(&map, &value, source.buf));
        sb_free(&source);

        /* Replication does not grant authority to an absent signature lane. */
        input.mask = input.rw_mask = 1;
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&value, &source, NULL, NULL, NULL, &options));
        CHECK(!map.complete);
        sb_free(&source);
    }
    return true;
}

int main(void) {
    return check_widths() && check_partial_lifetime_and_rejections() && check_material_authority() &&
           check_vector_operations() && check_float_intrinsics() && check_material_reflection_layout() &&
           check_packed_material_selection() && check_scalar_broadcast_and_projection() &&
           check_modifier_extension_authority() && check_derivative_sites() &&
           check_material_component_purpose() && check_replicated_dot_arity()
               ? 0 : 1;
}
