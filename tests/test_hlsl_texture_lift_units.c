// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter.h"
#include "translation/hlsl_source_quality.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return 1; } } while (0)

static DXBCOperand operand(DXBCOperandType type, int reg, uint8_t lanes) {
    DXBCOperand value = {0};
    value.type = type;
    value.register_index = reg;
    value.register_index_dim = 1;
    value.index_has_immediate[0] = true;
    value.index_values[0] = (uint32_t)reg;
    value.destination_mask = lanes << 4;
    value.swizzle_mode = 1;
    for (int component = 0; component < 4; ++component)
        value.swizzle[component] = (uint8_t)component;
    return value;
}

static int check_sample_argument_order(const USILProgram *base,
                                      const SerializedProgramParameters *parameters,
                                      HLSLEmitOptions *options,
                                      HLSLSourceQualityResult *quality,
                                      HLSLExpressionSourceMap *map) {
    USILInstruction instructions[4] = {0};
    instructions[0].opcode = USIL_OP_ADD;
    instructions[0].operand_count = 3;
    instructions[0].operands[0] = operand(OPERAND_TYPE_TEMP, 0, 1);
    instructions[0].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[0].operands[1].swizzle_mode = 2;
    instructions[0].operands[1].swizzle[0] = 2;
    instructions[0].operands[2].type = OPERAND_TYPE_IMMEDIATE32;
    instructions[0].operands[2].imm_value_count = 1;
    instructions[0].operands[2].imm_values[0] = UINT32_C(0x3ec00000);
    instructions[1].opcode = USIL_OP_MUL;
    instructions[1].operand_count = 3;
    instructions[1].operands[0] = operand(OPERAND_TYPE_TEMP, 1, 3);
    instructions[1].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
    instructions[1].operands[2].type = OPERAND_TYPE_IMMEDIATE32;
    instructions[1].operands[2].imm_value_count = 1;
    instructions[1].operands[2].imm_values[0] = UINT32_C(0x3fa00000);
    instructions[2].opcode = USIL_OP_SAMPLE_L;
    instructions[2].operand_count = 5;
    instructions[2].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
    instructions[2].operands[1] = operand(OPERAND_TYPE_TEMP, 1, 0);
    instructions[2].operands[2] = operand(OPERAND_TYPE_RESOURCE, 0, 0);
    instructions[2].operands[3] = operand(OPERAND_TYPE_SAMPLER, 0, 0);
    instructions[2].operands[4] = operand(OPERAND_TYPE_TEMP, 0, 0);
    instructions[2].operands[4].swizzle_mode = 2;
    instructions[3].opcode = USIL_OP_RET;
    for (unsigned index = 0; index < 4; ++index)
        instructions[index].source_instruction_index = index + 9;
    USILProgram program = *base;
    program.instructions = instructions;
    for (unsigned ordered = 0; ordered < 2; ++ordered) {
        StringBuilder source;
        sb_init(&source);
        CHECK(hlsl_emit_with_options(&program, &source, parameters, NULL, NULL, options));
        CHECK(quality->classification == HLSL_SOURCE_QUALITY_CLEAN &&
              !quality->counts.register_storage && !quality->counts.lane_transport);
        CHECK(hlsl_expression_source_map_matches(map, &program, source.buf));
        const char *scalar = strstr(source.buf, "const float dxbc_value_i");
        const char *coordinates = strstr(source.buf, "const float2 dxbc_value_i");
        const char *sample = strstr(source.buf, ".SampleLevel(");
        if (!ordered) CHECK(scalar && coordinates && sample && scalar < coordinates && coordinates < sample);
        else CHECK(!scalar && !coordinates && sample);
        sb_free(&source);
        USILInstruction first = instructions[0];
        instructions[0] = instructions[1];
        instructions[1] = first;
    }
    return 0;
}

int main(void) {
    USILInstruction instructions[4] = {0};
    for (int index = 0; index < 2; ++index) {
        instructions[index].opcode = USIL_OP_SAMPLE;
        instructions[index].operand_count = 4;
        instructions[index].operands[0] = operand(OPERAND_TYPE_TEMP, index, 15);
        instructions[index].operands[1] = operand(OPERAND_TYPE_INPUT, 0, 0);
        instructions[index].operands[2] = operand(OPERAND_TYPE_RESOURCE, 0, 0);
        instructions[index].operands[3] = operand(OPERAND_TYPE_SAMPLER, 0, 0);
        instructions[index].source_instruction_index = (uint32_t)(index + 5);
    }
    instructions[1].operands[1].swizzle[0] = 2;
    instructions[1].operands[1].swizzle[1] = 3;
    instructions[2].opcode = USIL_OP_MUL;
    instructions[2].operand_count = 3;
    instructions[2].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
    instructions[2].operands[1] = operand(OPERAND_TYPE_TEMP, 0, 0);
    instructions[2].operands[2] = operand(OPERAND_TYPE_TEMP, 1, 0);
    instructions[3].opcode = USIL_OP_RET;
    DXBCSignatureElement input = {.semantic_name = "TEXCOORD", .component_type = 3,
                                    .mask = 15, .rw_mask = 15};
    DXBCSignatureElement output = {.semantic_name = "SV_Target", .component_type = 3,
                                    .mask = 15, .system_value = 64};
    USILTexture texture = {.reg_idx = 0, .dimension = "2d", .return_types = {5, 5, 5, 5}};
    USILSampler sampler = {.reg_idx = 0};
    USILProgram program = {.instructions = instructions, .instruction_count = 4,
        .instruction_alloc = 4, .temp_count = 2, .inputs = &input, .input_count = 1,
        .input_alloc = 1, .outputs = &output, .output_count = 1, .output_alloc = 1,
        .textures = &texture, .texture_count = 1, .texture_alloc = 1,
        .samplers = &sampler, .sampler_count = 1, .sampler_alloc = 1,
        .has_stage_contract = true, .program_type = DXBC_PROGRAM_TYPE_PIXEL,
        .shader_model_major = 5};
    memcpy(program.shader_type_model, "ps_5_0", sizeof("ps_5_0"));
    SerializedResourceParam binding = {.name = "materialTexture", .bind_index = 0,
        .bind_type = SERIALIZED_RESOURCE_TEXTURE, .sampler_index = 0};
    SerializedProgramParameters parameters = {.resources = &binding, .res_count = 1};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    options.expression_source_map = &map;
    options.source_quality = &quality;
    StringBuilder source;
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, &parameters, NULL, NULL, &options));
    CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
    const char *first = strstr(source.buf, "const float4 dxbc_value_i0 = materialTexture.Sample(");
    const char *second = strstr(source.buf, "const float4 dxbc_value_i1 = materialTexture.Sample(");
    CHECK(first && second && first < second);
    CHECK(strstr(source.buf, "samplermaterialTexture") && strstr(source.buf, "texcoord0.zw"));
    CHECK(!strstr(source.buf, "float4 r0") && !strstr(source.buf, "v0") &&
          !strstr(source.buf, "o0 ="));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    HLSLExpressionOrigin saved_origin = map.origins[0];
    map.origins[0].kind = HLSL_EXPRESSION_ORIGIN_DEAD;
    map.origins[0].source_begin = map.origins[0].source_end = 0;
    CHECK(!hlsl_expression_source_map_matches(&map, &program, source.buf));
    map.origins[0] = saved_origin;
    CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
    sb_free(&source);
    const USILInstruction saved_sample = instructions[0];
    const USILTexture saved_texture = texture;
    for (int mutation = 0; mutation < 11; ++mutation) {
        if (mutation == 0) texture.return_types[0] = 4;
        if (mutation == 1) sampler.mode = 1;
        if (mutation == 2) instructions[0].has_texel_offset = true;
        if (mutation == 3) instructions[0].operands[2].swizzle[0] = 1;
        if (mutation == 4) instructions[0].operands[0].destination_mask = 0x70;
        if (mutation == 5) binding.bind_type = SERIALIZED_RESOURCE_BUFFER;
        if (mutation == 6) program.program_type = DXBC_PROGRAM_TYPE_VERTEX;
        if (mutation == 7) instructions[0].operands[1].type = OPERAND_TYPE_TEMP;
        if (mutation == 8) instructions[0].precise_mask = 1;
        if (mutation == 9) strcpy(texture.dimension, "3d");
        sb_init(&source);
        CHECK(!hlsl_emit_with_options(&program, &source, mutation == 10 ? NULL : &parameters,
                                       NULL, NULL, &options));
        CHECK(!map.complete && map.count == 0);
        sb_free(&source);
        instructions[0] = saved_sample;
        texture = saved_texture;
        sampler.mode = 0;
        binding.bind_type = SERIALIZED_RESOURCE_TEXTURE;
        program.program_type = DXBC_PROGRAM_TYPE_PIXEL;
    }
    const USILOpcode modes[] = {USIL_OP_SAMPLE_L, USIL_OP_SAMPLE_B, USIL_OP_SAMPLE_D};
    const char *methods[] = {".SampleLevel(", ".SampleBias(", ".SampleGrad("};
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (unsigned stage = 0; stage < 2; ++stage) {
            program.program_type = stage ? DXBC_PROGRAM_TYPE_VERTEX : DXBC_PROGRAM_TYPE_PIXEL;
            strcpy(program.shader_type_model, stage ? "vs_5_0" : "ps_5_0");
            strcpy(output.semantic_name, stage ? "SV_POSITION" : "SV_Target");
            output.system_value = stage ? 1u : 64u;
            for (int index = 0; index < 2; ++index) {
                instructions[index].opcode = modes[mode];
                instructions[index].operand_count = mode == 2 ? 6 : 5;
                instructions[index].operands[4] = operand(OPERAND_TYPE_INPUT, 0, 0);
                instructions[index].operands[5] = operand(OPERAND_TYPE_INPUT, 0, 0);
                for (int lane = 0; lane < 4; ++lane) {
                    instructions[index].operands[4].swizzle[lane] =
                        mode == 2 ? (uint8_t)lane : 2u;
                    instructions[index].operands[5].swizzle[lane] =
                        (uint8_t)(2 + (lane % 2));
                }
            }
            sb_init(&source);
            if (stage && mode == 1) {
                CHECK(!hlsl_emit_with_options(&program, &source, &parameters, NULL, NULL, &options));
                CHECK(!map.complete);
                sb_free(&source);
                continue;
            }
            CHECK(hlsl_emit_with_options(&program, &source, &parameters, NULL, NULL, &options));
            CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
            const char *sample = strstr(source.buf, methods[mode]);
            CHECK(sample && strstr(sample + 1, methods[mode]));
            CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
            CHECK(strstr(source.buf, mode == 2 ? "(texcoord0.xy), (texcoord0.zw)" : "texcoord0.z)"));
            map.origins[0].kind = HLSL_EXPRESSION_ORIGIN_DEAD;
            map.origins[0].source_begin = map.origins[0].source_end = 0;
            CHECK(!hlsl_expression_source_map_matches(&map, &program, source.buf));
            sb_free(&source);
            for (int mutation = 0; mutation < 4; ++mutation) {
                const USILInstruction saved = instructions[0];
                if (mutation == 0) instructions[0].has_texel_offset = true;
                if (mutation == 1) instructions[0].precise_mask = 1;
                if (mutation == 2) instructions[0].operands[4].type = OPERAND_TYPE_OUTPUT;
                if (mutation == 3) instructions[0].operand_count = 4;
                sb_init(&source);
                CHECK(!hlsl_emit_with_options(&program, &source, &parameters, NULL, NULL, &options));
                CHECK(!map.complete);
                sb_free(&source);
                instructions[0] = saved;
            }
        }
    }
    program.program_type = DXBC_PROGRAM_TYPE_PIXEL;
    strcpy(program.shader_type_model, "ps_5_0");
    strcpy(output.semantic_name, "SV_Target");
    output.system_value = 64u;
    CHECK(check_sample_argument_order(&program, &parameters, &options, &quality, &map) == 0);
    for (int index = 0; index < 2; ++index) {
        instructions[index].opcode = USIL_OP_SAMPLE;
        instructions[index].operand_count = 4;
    }
    binding.name = "texcoord0";
    sb_init(&source);
    CHECK(hlsl_emit_with_options(&program, &source, &parameters, NULL, NULL, &options));
    CHECK(strstr(source.buf, "float4 texcoord0_1 : TEXCOORD0"));
    CHECK(strstr(source.buf, "texcoord0.Sample("));
    CHECK(hlsl_expression_source_map_matches(&map, &program, source.buf));
    sb_free(&source);
    binding.name = "dxbc_value_i0";
    sb_init(&source);
    CHECK(!hlsl_emit_with_options(&program, &source, &parameters, NULL, NULL, &options));
    CHECK(!map.complete);
    sb_free(&source);
    return 0;
}
