// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include "translation/hlsl_patch_constants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK %s:%d: %s\n",__FILE__,__LINE__,#c); return false; } } while (0)
#define OP(o,n) ((uint32_t)(o) | (uint32_t)(n) << 24)
static void u32(uint8_t *p,uint32_t v) { for(unsigned b=0;b<4;++b)p[b]=(uint8_t)(v>>(8*b)); }
static size_t signature(uint8_t *p,unsigned role,bool hull,unsigned packed_width) {
    const unsigned count=role==2?(packed_width?5:7):1;
    const char *names[]={"SV_Position","SV_TessFactor","SV_InsideTessFactor","TEXCOORD"};
    size_t offsets[4],size=8+24*count;
    for(unsigned n=0;n<4;++n){offsets[n]=size;size+=strlen(names[n])+1;}
    memcpy(p,role==2?"PCSG":role?"OSGN":"ISGN",4);u32(p+4,(uint32_t)size);p+=8;
    u32(p,count);u32(p+4,8);
    for(unsigned n=0;n<count;++n){
        unsigned name=role==2?(n<3?1:n==3?2:3):0;
        uint8_t *e=p+8+24*n;
        u32(e,(uint32_t)offsets[name]);u32(e+4,role==2?(n<3?n:n==3?0:n-4):0);
        u32(e+8,role==2?(n<3?13:n==3?14:0):1);u32(e+12,3);u32(e+16,role==2?(packed_width&&n==4?0:n):0);
        const unsigned mask=role==2&&packed_width&&n==4?((packed_width&3u)==2?6:14):role==2?1:15;
        const unsigned rw=role==2?(hull?(mask^15u):n==4?(packed_width?(packed_width>3?mask:2):1):0):role?0:15;
        u32(e+20,mask|(rw<<8));
    }
    for(unsigned n=0;n<4;++n)memcpy(p+offsets[n],names[n],strlen(names[n])+1);
    return size+8;
}
/* Independent authored token grammar exercises scalar PCSG arrays and actual
 * FORK/JOIN ownership, with no captured binary or source matcher. */
static const uint32_t hull_words[]={
    OP(113,1),OP(147,1)|(3u<<11),OP(148,1)|(3u<<11),OP(149,1)|(2u<<11),OP(150,1)|(1u<<11),OP(151,1)|(3u<<11),
    OP(152,2),0x42000000,OP(106,1)|(1u<<11),
    OP(115,1),OP(153,2),3,OP(95,2),0x00017000,
    OP(103,4),0x00102012,0,17,OP(103,4),0x00102012,1,18,OP(103,4),0x00102012,2,19,
    OP(104,2),1,OP(91,4),0x00102012,0,3,
    OP(54,4),0x00100012,0,0x0001700a,OP(54,6),0x00902012,0x0010000a,0,0x00004001,0x40000000,OP(62,1),
    OP(115,1),OP(153,2),3,OP(95,2),0x00017000,OP(95,4),0x00219012,3,0,
    OP(101,3),0x00102012,4,OP(101,3),0x00102012,5,OP(101,3),0x00102012,6,
    OP(104,2),1,OP(91,4),0x00102012,4,3,
    OP(54,4),0x00100012,0,0x0001700a,
    OP(0,10),0x00100022,0,0x00004001,0x40000000,0x80a1900a,0x00000081,0x0010000a,0,0,
    OP(54,7),0x00d02012,4,0x0010000a,0,0x0010001a,0,OP(62,1),
    OP(116,1),OP(95,3),0x0011b012,4,OP(95,3),0x0011b012,5,OP(95,3),0x0011b012,6,
    OP(103,4),0x00102012,3,20,OP(104,2),1,
    OP(0,7),0x00100012,0,0x0011b00a,4,0x0011b00a,5,
    OP(0,7),0x00100012,0,0x0010000a,0,0x0011b00a,6,
    OP(51,7),0x00102012,3,0x0010000a,0,0x00004001,0x42000000,OP(62,1)
};
static const uint32_t domain_words[]={
    OP(147,1)|(3u<<11),OP(149,1)|(2u<<11),OP(106,1)|(1u<<11),
    OP(95,2),0x0001c072,OP(95,4),0x002190f2,3,0,OP(95,3),0x0011b012,4,
    OP(103,4),0x001020f2,0,1,OP(104,2),2,
    OP(56,7),0x001000f2,0,0x0001c556,0x00219e46,1,0,
    OP(50,9),0x001000f2,0,0x00219e46,0,0,0x0001c006,0x00100e46,0,
    OP(50,9),0x001000f2,0,0x00219e46,2,0,0x0001caa6,0x00100e46,0,
    OP(54,5),0x00100012,1,0x0011b00a,4,
    OP(54,8),0x001000e2,1,0x00004002,0,0,0,0,
    OP(0,7),0x001020f2,0,0x00100e46,0,0x00100e46,1,OP(62,1)
};
typedef struct {DXBCDocument document;DXBCContainer semantic;DXBCStageContract contract;USILProgram program;} Fixture;
static bool init_words(Fixture *f,bool hull,const uint32_t *words,size_t count,unsigned packed_width) {
    memset(f,0,sizeof(*f));dxbc_document_init(&f->document);dxbc_stage_contract_init(&f->contract);
    uint8_t bytes[2048]={0};memcpy(bytes,"DXBC",4);u32(bytes+20,1);u32(bytes+28,4);size_t off=48;
    for(unsigned role=0;role<3;++role){u32(bytes+32+4*role,(uint32_t)off);off+=signature(bytes+off,role,hull,packed_width);off=(off+3)&~(size_t)3;}
    u32(bytes+44,(uint32_t)off);memcpy(bytes+off,"SHEX",4);u32(bytes+off+4,(uint32_t)(count*4+8));
    u32(bytes+off+8,hull?0x00030050:0x00040050);u32(bytes+off+12,(uint32_t)(count+2));
    for (size_t n = 0; n < count; ++n) {
        u32(bytes + off + 16 + 4 * n, words[n]);
    }
    off += 16 + 4 * count;
    u32(bytes + 24, (uint32_t)off);
    CHECK(dxbc_compute_hash(bytes,off,bytes+4));DXBCDocumentDiagnostic dd;DXBCStageContractDiagnostic sd;
    CHECK(dxbc_document_parse(&f->document,bytes,off,&dd));CHECK(dxbc_document_decode_semantic(&f->document,&f->semantic));
    CHECK(dxbc_stage_contract_decode(&f->document,&f->semantic,&f->contract,&sd));
    CHECK(usil_translate_with_stage_contract(&f->program,&f->semantic,&f->contract));return true;
}
static bool init(Fixture *f,bool hull) {
    return init_words(f,hull,hull?hull_words:domain_words,hull?sizeof(hull_words)/4:sizeof(domain_words)/4,0);
}
static bool packed_init(Fixture *f,bool hull,unsigned width) {
    CHECK(width==2||width==3);
    if(!hull){
        uint32_t words[sizeof(domain_words)/4];memcpy(words,domain_words,sizeof(words));
        for(size_t n=0;n+1<sizeof(words)/4;++n){
            if(words[n]==0x0011b012){words[n]=0x0011b022;words[n+1]=0;}
            if(words[n]==0x0011b00a){words[n]=0x0011b01a;words[n+1]=0;}
        }
        return init_words(f,false,words,sizeof(words)/4,width);
    }
    uint32_t words[256];size_t count=0;
    /* Reuse only the independently authored factor token grammar above. The
     * new FORKs each copy an actual constant CP index into one custom lane. */
    const size_t outer_end=43;
    memcpy(words,hull_words,outer_end*4);count=outer_end;
    CHECK(words[count-1]==OP(62,1));
    for(unsigned component=0;component<width;++component){
        const uint32_t lane=1u<<(component+1);
        const uint32_t phase[]={OP(115,1),OP(95,4),0x00219012,3,0,OP(101,3),0x00102002|(lane<<4),0,
            OP(54,6),0x00102002|(lane<<4),0,0x0021900a,component,0,OP(62,1)};
        memcpy(words+count,phase,sizeof(phase));count+=sizeof(phase)/4;
    }
    const uint32_t join[]={OP(116,1),OP(95,3),0x0011b002|((width==2?6u:14u)<<4),0,
        OP(103,4),0x00102012,3,20,OP(104,2),1,
        OP(0,7),0x00100012,0,0x0011b01a,0,0x0011b02a,0};
    memcpy(words+count,join,sizeof(join));count+=sizeof(join)/4;
    if(width==3){const uint32_t add[]={OP(0,7),0x00100012,0,0x0010000a,0,0x0011b03a,0};memcpy(words+count,add,sizeof(add));count+=sizeof(add)/4;}
    const uint32_t end[]={OP(51,7),0x00102012,3,0x0010000a,0,0x00004001,0x42000000,OP(62,1)};
    memcpy(words+count,end,sizeof(end));count+=sizeof(end)/4;
    return init_words(f,true,words,count,width);
}
/* Independent ADD grammar varies decoded static-index/read order. No selected
 * target or authored source is consulted by this fixture. */
static bool packed_add_init(Fixture *f, unsigned width, bool reverse) {
    uint32_t words[256]; size_t count = 43;
    memcpy(words, hull_words, count * sizeof(*words));
    for (unsigned component = 0; component < width; ++component) {
        const uint32_t lane = 1u << (component + 1);
        const uint32_t phase[] = {
            OP(115,1), OP(95,4), 0x00219012, 3, 0,
            OP(101,3), 0x00102002 | (lane << 4), 0, OP(104,2), 1,
            OP(0,9), 0x00100012, 0, 0x0021900a, reverse ? 2u : 0u, 0,
                0x0021900a, reverse ? 0u : 2u, 0,
            OP(56,7), 0x00102002 | (lane << 4), 0, 0x0010000a, 0, 0x00004001, 0x3f000000,
            OP(62,1)
        };
        memcpy(words + count, phase, sizeof(phase)); count += sizeof(phase) / 4;
    }
    const uint32_t join[] = {
        OP(116,1), OP(95,3), 0x0011b002 | ((width == 2 ? 6u : 14u) << 4), 0,
        OP(103,4), 0x00102012, 3, 20, OP(104,2), 1,
        OP(0,7), 0x00100012, 0, reverse ? 0x0011b02a : 0x0011b01a, 0,
            reverse ? 0x0011b01a : 0x0011b02a, 0,
        OP(51,7), 0x00102012, 3, 0x0010000a, 0, 0x00004001, 0x42000000, OP(62,1)
    };
    memcpy(words + count, join, sizeof(join)); count += sizeof(join) / 4;
    return init_words(f, true, words, count, width);
}
static void dispose(Fixture *f){usil_free(&f->program);dxbc_free(&f->semantic);dxbc_stage_contract_free(&f->contract);dxbc_document_free(&f->document);}
static bool emit(USILProgram *p,bool accepted,const char *macro) {
    StringBuilder s;sb_init(&s);HLSLEmitOptions options=HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult q;HLSLExpressionSourceMap map;HLSLEmitDiagnostic diagnostic;
    options.source_quality=&q;options.expression_source_map=&map;
    if(macro){options.reserved_preprocessor_identifiers=&macro;options.reserved_preprocessor_identifier_count=1;}
    bool result=hlsl_emit_with_options_diagnostic(p,&s,NULL,NULL,NULL,&options,&diagnostic);
    CHECK(result==accepted);
    if(accepted){CHECK(q.classification==HLSL_SOURCE_QUALITY_CLEAN);CHECK(!q.counts.unknown_provenance&&!q.counts.incomplete_units);
        CHECK(hlsl_expression_source_map_matches(&map,p,s.buf));CHECK(strstr(s.buf,"patchValues2[3] : TEXCOORD0"));
        if(p->program_type==DXBC_PROGRAM_TYPE_HULL){CHECK(strstr(s.buf,"min("));CHECK(strstr(s.buf,"factors.patchValues2[0]"));}
        else {CHECK(strstr(s.buf,"float4(dxbc_value_i3, dxbc_value_i4)"));CHECK(!strstr(s.buf,"dxbc_value_i4.x"));}
    }else CHECK(q.classification!=HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&s);return true;
}
static bool positive(void){Fixture f;CHECK(init(&f,true));CHECK(emit(&f.program,true,NULL));dispose(&f);CHECK(init(&f,false));CHECK(emit(&f.program,true,NULL));dispose(&f);return true;}
static bool ownership_negatives(void){
    for(unsigned scenario=0;scenario<16;++scenario){Fixture f;CHECK(init(&f,true));USILProgram *p=&f.program;
        switch(scenario){
        case 0:p->tessellation.phases[2].kind=DXBC_HULL_PHASE_FORK;break; /* A patch read in FORK is not a JOIN dependency. */
        case 1:p->tessellation.phases[1].kind=DXBC_HULL_PHASE_JOIN;break; /* No cross-JOIN reads and no vector JOIN instances. */
        case 2:p->tessellation.phases[1].instance_count=4;break;
        case 3:p->tessellation.phases[1].first_source_instruction_index++;break;
        case 4:p->instructions[7].operands[1].register_index=3;p->instructions[7].operands[1].index_values[0]=3;break; /* Reads current JOIN output. */
        case 5:p->instructions[4].precise_mask=2;break;
        case 6:p->instructions[9].saturate=true;break;
        case 7:p->signature_declarations[6].array_element_count=4;CHECK(!usil_signature_authority_is_valid(p));break;
        case 8:p->signature_declarations[6].operand_type=OPERAND_TYPE_INPUT;CHECK(!usil_signature_authority_is_valid(p));break;
        case 9:p->signature_declarations[6].source_instruction_index=1;CHECK(!usil_signature_authority_is_valid(p));break;
        case 10:p->patch_constants[4].mask=3;p->patch_constants[4].rw_mask=12;break;
        case 11:p->patch_constants[5].semantic_index=0;break;
        case 12:p->index_ranges[1].register_count=4;break;
        case 13:p->instructions[3].operands[1].type=OPERAND_TYPE_JOIN_INSTANCE_ID;break;
        case 14:p->instructions[7].operands[1].swizzle[0]=1;break;
        case 15:p->instructions[4].operands[2].extended_tokens[0]^=4;break;
        }
        CHECK(emit(p,false,NULL));dispose(&f);
    }return true;
}
static bool composition_negatives(void){
    for(unsigned scenario=0;scenario<8;++scenario){Fixture f;CHECK(init(&f,false));USILProgram *p=&f.program;
        switch(scenario){
        case 0:p->instructions[5].operands[2].register_index=2;p->instructions[5].operands[2].index_values[0]=2;break; /* Entire undefined vector. */
        case 1:p->instructions[4].operands[0].destination_mask=0x60;break; /* Undefined W lane. */
        case 2:p->instructions[3].precise_mask=1;break;
        case 3:p->instructions[4].saturate=true;break;
        case 4:p->instructions[4].opcode=USIL_OP_IF;break;
        case 5:p->instructions[3].operands[1].has_neg=true;break; /* Modifier flag without owned extension. */
        case 6:p->instructions[5].operands[2].index_representations[0]=2;p->instructions[5].operands[2].index_has_immediate[0]=false;break;
        case 7:p->instructions[4].operands[1].type=OPERAND_TYPE_CONSTANT_BUFFER;break;
        }
        CHECK(emit(p,false,NULL));dispose(&f);
    }return true;
}
static bool aliases(void){Fixture f;CHECK(init(&f,true));CHECK(emit(&f.program,false,"TEXCOORD0"));CHECK(emit(&f.program,false,"min"));dispose(&f);CHECK(init(&f,false));CHECK(emit(&f.program,false,"TEXCOORD0"));dispose(&f);return true;}
static bool scan_bounds(void) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        Fixture f; CHECK(init(&f, scenario != 1));
        if (scenario < 2) {
            USILSignatureDeclaration *large = calloc(1025, sizeof(*large)); CHECK(large);
            memcpy(large, f.program.signature_declarations,
                   (size_t)f.program.signature_declaration_count * sizeof(*large));
            free(f.program.signature_declarations); f.program.signature_declarations = large;
            f.program.signature_declaration_alloc = f.program.signature_declaration_count = 1025;
        } else {
            USILIndexRange *large = calloc(33, sizeof(*large)); CHECK(large);
            memcpy(large, f.program.index_ranges, (size_t)f.program.index_range_count * sizeof(*large));
            free(f.program.index_ranges); f.program.index_ranges = large;
            f.program.index_range_alloc = f.program.index_range_count = 33;
        }
        CHECK(emit(&f.program, false, NULL)); dispose(&f);
    }
    return true;
}
static bool packed_emit(USILProgram *program,bool accepted,unsigned width){
    StringBuilder source;sb_init(&source);HLSLEmitOptions options=HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;HLSLExpressionSourceMap map;HLSLEmitDiagnostic diagnostic;
    options.source_quality=&quality;options.expression_source_map=&map;
    CHECK(hlsl_emit_with_options_diagnostic(program,&source,NULL,NULL,NULL,&options,&diagnostic)==accepted);
    if(accepted){
        CHECK(quality.classification==HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(!quality.counts.unknown_provenance&&!quality.counts.incomplete_units&&!quality.counts.residual_total);
        CHECK(hlsl_expression_source_map_matches(&map,program,source.buf));
        CHECK(strstr(source.buf,width==2?"float2 patchValues2 : TEXCOORD0":"float3 patchValues2 : TEXCOORD0"));
        if(program->program_type==DXBC_PROGRAM_TYPE_HULL){
            CHECK(strstr(source.buf,"patch[0].clipPosition.x"));CHECK(strstr(source.buf,"patch[1].clipPosition.x"));
            CHECK(!strstr(source.buf,"patch[factorIndex].clipPosition"));
            CHECK(strstr(source.buf,"factors.patchValues2.x ="));CHECK(strstr(source.buf,"factors.patchValues2.y ="));
            if(width==3)CHECK(strstr(source.buf,"patch[2].clipPosition.x"));
        }else CHECK(strstr(source.buf,"factors.patchValues2.x"));
    }else CHECK(quality.classification!=HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);return true;
}
static bool packed_positive(void){
    for(unsigned width=2;width<=3;++width)for(unsigned stage=0;stage<2;++stage){
        Fixture f;CHECK(packed_init(&f,stage==0,width));HLSLPatchLayout layout;
        CHECK(hlsl_patch_layout(&f.program,&layout));CHECK(layout.row_count==4&&layout.signature_count==5);
        CHECK(layout.lane_field[0][0]==0&&layout.lane_field[0][1]==2&&layout.lane_component[0][1]==0);
        CHECK(packed_emit(&f.program,true,width));
        /* PCSG record reordering does not alter field or producer ownership. */
        DXBCSignatureElement swap=f.program.patch_constants[0];f.program.patch_constants[0]=f.program.patch_constants[4];f.program.patch_constants[4]=swap;
        CHECK(packed_emit(&f.program,true,width));dispose(&f);
    }return true;
}
static bool packed_vector_domain(void){
    uint32_t words[sizeof(domain_words)/4];memcpy(words,domain_words,sizeof(words));
    for(size_t n=0;n+1<sizeof(words)/4;++n){
        if(words[n]==0x0011b012){words[n]=0x0011b0e2;words[n+1]=0;}
        if(words[n]==0x0011b00a){words[n]=0x0011bf96;words[n+1]=0;CHECK(n>=3);words[n-2]=0x00100072;}
        if(words[n]==0x001000e2)words[n]=0x00100082;
    }
    Fixture f;CHECK(init_words(&f,false,words,sizeof(words)/4,7));
    StringBuilder source;sb_init(&source);HLSLEmitOptions options=HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;HLSLExpressionSourceMap map;HLSLEmitDiagnostic diagnostic;
    options.source_quality=&quality;options.expression_source_map=&map;
    CHECK(hlsl_emit_with_options_diagnostic(&f.program,&source,NULL,NULL,NULL,&options,&diagnostic));
    CHECK(quality.classification==HLSL_SOURCE_QUALITY_CLEAN&&!quality.counts.residual_total);
    CHECK(hlsl_expression_source_map_matches(&map,&f.program,source.buf));
    CHECK(strstr(source.buf,"const float3 dxbc_value_i3 = (factors.patchValues2)"));
    CHECK(strstr(source.buf,"float4(dxbc_value_i3, dxbc_value_i4)"));
    CHECK(!strstr(source.buf,"patchValues2.x")&&!strstr(source.buf,"dxbc_value_i3.x"));
    sb_free(&source);dispose(&f);return true;
}
static bool packed_layout_negatives(void){
    Fixture f;CHECK(packed_init(&f,true,3));HLSLPatchLayout layout;
    DXBCSignatureElement original=f.program.patch_constants[4];
    strcpy(f.program.patch_constants[4].semantic_name,"sv_tessfactor");f.program.patch_constants[4].semantic_name_length=13;
    CHECK(!hlsl_patch_layout(&f.program,&layout));f.program.patch_constants[4]=original;
    DXBCSignatureElement *larger=calloc(6,sizeof(*larger));CHECK(larger);memcpy(larger,f.program.patch_constants,5*sizeof(*larger));
    larger[5]=original;larger[5].register_id=4;larger[5].semantic_index=1;
    free(f.program.patch_constants);f.program.patch_constants=larger;f.program.patch_constant_count=f.program.patch_constant_alloc=6;
    CHECK(!hlsl_patch_layout(&f.program,&layout)); /* Vector arrays/mixed same-semantic shapes remain unsupported. */
    dispose(&f);return true;
}
static bool packed_negatives(void){
    for(unsigned scenario=0;scenario<14;++scenario){Fixture f;CHECK(packed_init(&f,true,3));USILProgram *p=&f.program;
        switch(scenario){
        case 0:p->patch_constants[4].mask=3;p->patch_constants[4].rw_mask=12;break; /* Factor/custom overlap. */
        case 1:p->patch_constants[4].mask=10;p->patch_constants[4].rw_mask=5;break; /* Noncontiguous field/holes. */
        case 2:p->patch_constants[4].min_precision=1;break;
        case 3:p->patch_constants[4].rw_mask=0;break;
        case 4:p->patch_constants[4].component_type=1;break;
        case 5:p->signature_declarations[7].mask=2;break; /* Duplicate component producer. */
        case 6:p->tessellation.phases[3].kind=DXBC_HULL_PHASE_JOIN;break; /* Cross-JOIN/later lane owner. */
        case 7:p->signature_declarations[p->signature_declaration_count-2].mask=6;break; /* JOIN reads undeclaredW. */
        case 8:p->instructions[3].operands[1].index_values[0]=3;p->instructions[3].operands[1].register_index=3;break; /* Actual CP extent. */
        case 9:p->instructions[3].operands[1].swizzle[0]=1;break; /* UndeclaredCPcomponent. */
        case 10:p->signature_declarations[5].source_instruction_index=p->tessellation.phases[2].first_source_instruction_index;break;
        case 11:p->instructions[p->instruction_count-2].saturate=true;break;
        case 12:p->instructions[p->instruction_count-2].precise_mask=1;break;
        case 13:memmove(p->signature_declarations+9,p->signature_declarations+10,
            (size_t)(p->signature_declaration_count-10)*sizeof(*p->signature_declarations));
            --p->signature_declaration_count;CHECK(usil_signature_authority_is_valid(p));break; /* Missing complete component owner. */
        }
        if(!packed_emit(p,false,3)){fprintf(stderr,"Packed HULL negative %u\n",scenario);dispose(&f);return false;}dispose(&f);
    }
    for(unsigned scenario=0;scenario<4;++scenario){Fixture f;CHECK(packed_init(&f,false,2));USILProgram *p=&f.program;
        switch(scenario){
        case 0:p->instructions[3].operands[1].swizzle[0]=3;break; /* Read unowned padding laneW. */
        case 1:p->signature_declarations[2].mask=1;break; /* Logical field read lacks physicalY declaration. */
        case 2:p->patch_constants[4].rw_mask=8;break;
        case 3:p->instructions[4].precise_mask=2;break;
        }
        if(!packed_emit(p,false,2)){fprintf(stderr,"Packed DOMAIN negative %u\n",scenario);dispose(&f);return false;}dispose(&f);
    }return true;
}
static bool ordered_add_source(void) {
    for (unsigned width = 2; width <= 3; ++width) {
        for (unsigned reverse = 0; reverse < 2; ++reverse) {
            Fixture f; CHECK(packed_add_init(&f, width, reverse != 0));
            StringBuilder source; sb_init(&source);
            HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
            HLSLSourceQualityResult quality; HLSLExpressionSourceMap map;
            options.source_quality = &quality; options.expression_source_map = &map;
            CHECK(hlsl_emit_with_options(&f.program, &source, NULL, NULL, NULL, &options));
            CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
            CHECK(hlsl_expression_source_map_matches(&map, &f.program, source.buf));
            /* The MAD source span belongs to the original ADD, and keeps the
             * decoded first/second CP read rather than swapping the AST. */
            const HLSLExpressionOrigin *origin = &map.origins[3];
            CHECK(origin->kind == HLSL_EXPRESSION_ORIGIN_EXPRESSION);
            CHECK(origin->source_end - origin->source_begin < 256);
            char expression[256] = {0};
            memcpy(expression, source.buf + origin->source_begin, origin->source_end - origin->source_begin);
            const char *first = strstr(expression, reverse ? "patch[2].clipPosition.x" : "patch[0].clipPosition.x");
            const char *second = strstr(expression, reverse ? "patch[0].clipPosition.x" : "patch[2].clipPosition.x");
            CHECK(strstr(expression, "mad(") && first && second && first < second);
            const int join = f.program.tessellation.phases[width + 1].first_instruction_index;
            origin = &map.origins[join]; CHECK(origin->source_end - origin->source_begin < sizeof(expression));
            memset(expression, 0, sizeof(expression));
            memcpy(expression, source.buf + origin->source_begin, origin->source_end - origin->source_begin);
            first = strstr(expression, reverse ? "factors.patchValues2.y" : "factors.patchValues2.x");
            second = strstr(expression, reverse ? "factors.patchValues2.x" : "factors.patchValues2.y");
            CHECK(strstr(expression, "mad(") && first && second && first < second);
            sb_free(&source); dispose(&f);
        }
    }
    /* Other admitted operand forms keep the original ADD spelling. They do
     * not inherit the selected-compiler scope simply from stage/field names. */
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        Fixture f; CHECK(packed_add_init(&f, 2, false));
        DXBCOperand *right = &f.program.instructions[3].operands[2];
        if (scenario == 0) { right->register_index = 0; right->index_values[0] = 0; }
        if (scenario == 1) *right = f.program.instructions[4].operands[2]; /* Immediate. */
        if (scenario == 2) {
            right->has_abs = true; right->extended_token_count = 1;
            right->extended_tokens = malloc(sizeof(*right->extended_tokens)); CHECK(right->extended_tokens);
            right->extended_tokens[0] = 0x81; /* Actual modifier ownership, no ordered ADD permission. */
        }
        StringBuilder source; sb_init(&source); HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
        HLSLExpressionSourceMap map; options.expression_source_map = &map;
        CHECK(hlsl_emit_with_options(&f.program, &source, NULL, NULL, NULL, &options));
        const HLSLExpressionOrigin *origin = &map.origins[3];
        CHECK(origin->source_end - origin->source_begin < 256);
        char expression[256] = {0};
        memcpy(expression, source.buf + origin->source_begin, origin->source_end - origin->source_begin);
        CHECK(!strstr(expression, "mad(") && strstr(expression, " + "));
        sb_free(&source); dispose(&f);
    }
    /* Invalid precision/effect/read grammar fails before any new spelling. */
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        Fixture f; CHECK(packed_add_init(&f, 2, false));
        USILInstruction *add = &f.program.instructions[3];
        switch (scenario) {
        case 0: add->precise_mask = 1; break;
        case 1: add->saturate = true; break;
        case 2: add->operands[2].min_precision = 1; break;
        case 3: add->operands[2].type = OPERAND_TYPE_TEMP; break;
        case 4: add->operands[2].swizzle[0] = 1; break;
        case 5: f.program.signature_declarations[5].array_element_count = 2; break;
        case 6: f.program.tessellation.phases[1].kind = DXBC_HULL_PHASE_JOIN; break;
        }
        CHECK(packed_emit(&f.program, false, 2)); dispose(&f);
    }
    return true;
}
int main(void){if(!positive()||!ownership_negatives()||!composition_negatives()||!aliases()||!scan_bounds()||!packed_positive()||!packed_vector_domain()||!packed_layout_negatives()||!packed_negatives()||!ordered_add_source())return 1;puts("Patch phase units passed");return 0;}
