// SPDX-License-Identifier: GPL-3.0-only
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_source_quality.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK failed %s:%d: %s\n",__FILE__,__LINE__,#c); return false; } } while (0)

typedef struct {
    USILProgram program;
    USILInstruction instructions[2];
    DXBCSignatureElement input, output;
    USILTexture textures[2];
    USILSampler samplers[2];
    SerializedResourceParam bindings[2];
    SerializedProgramParameters parameters;
    HLSLEmitOptions options;
    HLSLSourceQualityResult quality;
    HLSLExpressionSourceMap map;
    HLSLSourceQualityFacts copied_resources[4];
    size_t resource_events;
    bool reject_resource_event;
} Fixture;

static bool observation(void *context, const HLSLSourceQualityObservation *value) {
    Fixture *f=context;
    if (value->facts.resource_declaration_kind!=HLSL_SOURCE_RESOURCE_NONE) {
        if (f->resource_events>=4) return false;
        f->copied_resources[f->resource_events++]=value->facts;
        if (f->reject_resource_event) return false;
    }
    return true;
}

static DXBCOperand operand(DXBCOperandType type, int reg, uint8_t lanes) {
    DXBCOperand result={0};
    result.type=type; result.register_index=reg; result.register_index_dim=1;
    result.index_has_immediate[0]=true; result.index_values[0]=(uint32_t)reg;
    result.destination_mask=lanes<<4; result.swizzle_mode=1;
    for (unsigned component=0;component<4;++component) result.swizzle[component]=(uint8_t)component;
    return result;
}

static void fixture_init(Fixture *f) {
    memset(f,0,sizeof(*f));
    f->instructions[0].opcode=USIL_OP_SAMPLE; f->instructions[0].operand_count=4;
    f->instructions[0].operands[0]=operand(OPERAND_TYPE_OUTPUT,0,15);
    f->instructions[0].operands[1]=operand(OPERAND_TYPE_INPUT,0,0);
    f->instructions[0].operands[2]=operand(OPERAND_TYPE_RESOURCE,0,0);
    f->instructions[0].operands[3]=operand(OPERAND_TYPE_SAMPLER,0,0);
    f->instructions[0].source_instruction_index=5;
    f->instructions[1].opcode=USIL_OP_RET; f->instructions[1].source_instruction_index=6;
    f->input=(DXBCSignatureElement){.semantic_name="TEXCOORD",.component_type=3,.mask=3,.rw_mask=3};
    f->output=(DXBCSignatureElement){.semantic_name="SV_Target",.component_type=3,.mask=15,.rw_mask=15,.system_value=64};
    f->textures[0]=(USILTexture){.reg_idx=0,.dimension="2d",.return_types={5,5,5,5}};
    f->samplers[0]=(USILSampler){.reg_idx=0};
    f->program=(USILProgram){.instructions=f->instructions,.instruction_count=2,.instruction_alloc=2,
        .inputs=&f->input,.input_count=1,.input_alloc=1,.outputs=&f->output,.output_count=1,.output_alloc=1,
        .textures=f->textures,.texture_count=1,.texture_alloc=2,.samplers=f->samplers,.sampler_count=1,.sampler_alloc=2,
        .has_stage_contract=true,.program_type=DXBC_PROGRAM_TYPE_PIXEL,.shader_model_major=5};
    memcpy(f->program.shader_type_model,"ps_5_0",sizeof("ps_5_0"));
    f->bindings[0]=(SerializedResourceParam){.name="materialTexture",.bind_type=SERIALIZED_RESOURCE_TEXTURE,.bind_index=0,.sampler_index=0};
    f->parameters=(SerializedProgramParameters){.resources=f->bindings,.res_count=1};
    f->options=(HLSLEmitOptions)HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    f->options.source_quality=&f->quality; f->options.expression_source_map=&f->map;
    f->options.source_quality_observer=observation; f->options.source_quality_observer_context=f;
}

static bool emit(Fixture *f, StringBuilder *source) {
    sb_init(source); f->resource_events=0;
    return hlsl_emit_with_options(&f->program,source,&f->parameters,NULL,NULL,&f->options);
}

static bool check_actual_resource_inventory(void) {
    Fixture f; fixture_init(&f);
    StringBuilder source;
    CHECK(emit(&f,&source));
    CHECK(f.quality.classification==HLSL_SOURCE_QUALITY_CLEAN && !f.quality.reasons &&
          !f.quality.counts.incomplete_units && !f.quality.counts.unknown_provenance &&
          !f.quality.counts.residual_total && f.quality.counts.resource_declarations==2);
    CHECK(f.resource_events==2 && f.copied_resources[0].resource_declaration_kind==HLSL_SOURCE_RESOURCE_TEXTURE &&
          f.copied_resources[1].resource_declaration_kind==HLSL_SOURCE_RESOURCE_SAMPLER &&
          f.copied_resources[0].resource_binding_register==0 && f.copied_resources[1].resource_binding_register==0);
    CHECK(strstr(source.buf,"float4 main(float2 texcoord0 : TEXCOORD0)") &&
          strstr(source.buf,"return materialTexture.Sample((samplermaterialTexture), (texcoord0))"));
    CHECK(hlsl_expression_source_map_matches(&f.map,&f.program,source.buf));
    sb_free(&source);
    // The option itself creates no dependency when no declaration is omitted.
    f.options.omit_unity_builtin_declarations=true;
    CHECK(emit(&f,&source));
    CHECK(f.quality.classification==HLSL_SOURCE_QUALITY_CLEAN && f.resource_events==2);
    sb_free(&source);
    f.bindings[0].name="unity_Lightmap";
    CHECK(emit(&f,&source));
    CHECK(f.quality.classification!=HLSL_SOURCE_QUALITY_CLEAN && f.quality.counts.incomplete_units==1 &&
          f.resource_events==0 && !strstr(source.buf,"Texture2D<float4> unity_Lightmap"));
    sb_free(&source);
    fixture_init(&f); f.reject_resource_event=true;
    CHECK(!emit(&f,&source));
    CHECK(f.quality.classification==HLSL_SOURCE_QUALITY_FAILED && !f.map.complete);
    sb_free(&source);
    return true;
}

static bool check_interface_namespace(void) {
    Fixture f; fixture_init(&f);
    StringBuilder source;
    f.bindings[0].name="texcoord0";
    CHECK(emit(&f,&source));
    CHECK(strstr(source.buf,"float2 texcoord0_1 : TEXCOORD0") &&
          strstr(source.buf,"texcoord0.Sample((samplertexcoord0), (texcoord0_1))"));
    CHECK(f.quality.classification==HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    memcpy(f.input.semantic_name,"CUSTOM",sizeof("CUSTOM")); f.bindings[0].name="attribute0";
    CHECK(emit(&f,&source));
    CHECK(strstr(source.buf,"float2 attribute0_1 : CUSTOM"));
    sb_free(&source);
    const char *reserved[]={"texcoord0"};
    memcpy(f.input.semantic_name,"TEXCOORD",sizeof("TEXCOORD")); f.bindings[0].name="texcoord0_1";
    f.options.reserved_preprocessor_identifiers=reserved;
    f.options.reserved_preprocessor_identifier_count=1;
    CHECK(emit(&f,&source));
    CHECK(strstr(source.buf,"float2 texcoord0_2 : TEXCOORD0"));
    sb_free(&source);
    return true;
}

static bool check_uninspected_declarations_stay_incomplete(void) {
    for (unsigned mutation=0;mutation<3;++mutation) {
        Fixture f; fixture_init(&f);
        StringBuilder source;
        if (mutation==0) {
            f.samplers[1]=(USILSampler){.reg_idx=1}; f.program.sampler_count=2;
        } else {
            f.textures[1]=(USILTexture){.reg_idx=1,.dimension="raw"};
            if (mutation==2) { memcpy(f.textures[1].dimension,"structured",sizeof("structured")); f.textures[1].stride=16; }
            f.program.texture_count=2;
            f.bindings[1]=(SerializedResourceParam){.name="unusedBuffer",.bind_type=SERIALIZED_RESOURCE_BUFFER,.bind_index=1};
            f.parameters.res_count=2;
        }
        CHECK(emit(&f,&source));
        CHECK(f.quality.classification!=HLSL_SOURCE_QUALITY_CLEAN && f.quality.counts.incomplete_units==1);
        CHECK(f.quality.counts.resource_declarations==3);
        if (mutation) CHECK(f.quality.counts.raw_buffer_reconstruction==1);
        sb_free(&source);
    }
    return true;
}

int main(void) {
    if (!check_actual_resource_inventory() || !check_interface_namespace() ||
        !check_uninspected_declarations_stay_incomplete()) return 1;
    puts("Owned resource declaration quality and natural interface coverage passed");
    return 0;
}
