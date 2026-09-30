// SPDX-License-Identifier: GPL-3.0-only
#include "translation/hlsl_global_declarations.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_source_quality.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                   \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    SerializedPass pass;
    int platform;
    SerializedSubProgram sub[3];
    SerializedSubProgramIdentity identities[3];
    PlayerSubProgramMetadata player[2];
    SerializedProgramParameters parameters[2];
    SerializedConstantBuffer buffers[2];
    SerializedResourceParam bindings[2];
    SerializedVariable variables[2][2];
    char *keyword;
    HLSLGlobalDeclarationWitness witnesses[2];
    USILProgram program;
    USILInstruction instructions[2];
    USILConstantBuffer cbuffer;
    DXBCSignatureElement output;
} Fixture;

typedef struct {
    size_t witness_records;
    HLSLSourceQualityFacts copied;
} Ledger;

static bool observe_witness(void *context, const HLSLSourceQualityObservation *observation) {
    Ledger *ledger = context;
    if (observation->facts.declaration_witness_record) {
        ++ledger->witness_records;
        ledger->copied = observation->facts;
    }
    return true;
}

static DXBCOperand operand(DXBCOperandType type, uint32_t row, uint8_t lanes) {
    DXBCOperand value = {0};
    value.type = type;
    value.register_index = 0;
    value.register_index_dim = 1;
    value.index_has_immediate[0] = true;
    value.index_values[0] = 0;
    value.destination_mask = lanes << 4;
    value.swizzle_mode = 1;
    for (unsigned lane = 0; lane < 4; ++lane)
        value.swizzle[lane] = (uint8_t)lane;
    if (type == OPERAND_TYPE_CONSTANT_BUFFER) {
        value.register_index_dim = 2;
        value.index_has_immediate[1] = true;
        value.index_values[1] = row;
        value.rel_offset0 = (int)row;
    }
    return value;
}

static void fixture_init(Fixture *f) {
    memset(f, 0, sizeof(*f));
    f->platform = 4;
    f->keyword = "SCALED";
    f->pass.has_serialized_platforms = true;
    f->pass.platform_count = 1;
    f->pass.platforms = &f->platform;
    f->pass.program_mask = 3;
    f->pass.subprogram_count[1] = 2;
    f->pass.subprograms[1] = f->sub;
    f->pass.subprogram_identities[1] = f->identities;
    for (unsigned i = 0; i < 2; ++i) {
        f->sub[i].program_type = 18;
        f->sub[i].shader_requirements = 4;
        f->identities[i].hardware_tier_group = 3;
        f->identities[i].inner_subprogram_index = (int)i;
        f->player[i].program_type = 18;
        f->player[i].has_player_blob_header = true;
        f->parameters[i].cb_count = 1;
        f->parameters[i].constant_buffers = &f->buffers[i];
        f->parameters[i].res_count = 1;
        f->parameters[i].resources = &f->bindings[i];
        f->buffers[i].name = "$Globals";
        f->buffers[i].size = 64;
        f->buffers[i].var_count = (int)i + 1;
        f->buffers[i].variables = f->variables[i];
        f->bindings[i].name = "$Globals";
        f->bindings[i].bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER;
        f->variables[i][0] = (SerializedVariable){"_Color", {32, 0, 0, 4, 0, 0}};
        f->variables[i][1] = (SerializedVariable){"_Scale", {48, 0, 0, 1, 0, 0}};
        f->witnesses[i] = (HLSLGlobalDeclarationWitness){(int)i, &f->player[i], &f->parameters[i]};
    }
    f->sub[1].local_keyword_count = 1;
    f->sub[1].local_keywords = &f->keyword;
    f->player[1].local_keyword_count = 1;
    f->player[1].local_keywords = &f->keyword;
    f->instructions[0].opcode = USIL_OP_MOV;
    f->instructions[0].operand_count = 2;
    f->instructions[0].source_instruction_index = 17;
    f->instructions[0].operands[0] = operand(OPERAND_TYPE_OUTPUT, 0, 15);
    f->instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 2, 0);
    f->instructions[1].opcode = USIL_OP_RET;
    f->instructions[1].source_instruction_index = 18;
    f->cbuffer = (USILConstantBuffer){.reg_idx = 0, .size = 3};
    f->output = (DXBCSignatureElement){.semantic_name = "SV_Target",
                                       .component_type = 3,
                                       .system_value = 64,
                                       .mask = 15,
                                       .rw_mask = 15};
    f->program = (USILProgram){.instructions = f->instructions,
                               .instruction_count = 2,
                               .instruction_alloc = 2,
                               .outputs = &f->output,
                               .output_count = 1,
                               .output_alloc = 1,
                               .cbuffers = &f->cbuffer,
                               .cbuffer_count = 1,
                               .cbuffer_alloc = 1,
                               .has_stage_contract = true,
                               .program_type = DXBC_PROGRAM_TYPE_PIXEL,
                               .shader_model_major = 5};
    memcpy(f->program.shader_type_model, "ps_5_0", sizeof("ps_5_0"));
}

static bool check_owned_union_and_target_authority(void) {
    Fixture f;
    fixture_init(&f);
    HLSLGlobalDeclarationUnion *u = NULL;
    HLSLGlobalDeclarationDiagnostic diagnostic;
    CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, f.witnesses, 2, &u, &diagnostic) ==
          HLSL_GLOBAL_DECLARATIONS_OK);
    CHECK(u && hlsl_global_declarations_shell_size(u) == 64 &&
          hlsl_global_declarations_current_variant(u) == 0);
    size_t count;
    const HLSLGlobalDeclarationField *fields = hlsl_global_declarations_fields(u, &count);
    CHECK(count == 2 && fields[0].current_authority && !fields[1].current_authority);
    CHECK(fields[0].witness_count == 2 && fields[0].witness_subprogram_indices[0] == 0 &&
          fields[0].witness_subprogram_indices[1] == 1);
    CHECK(fields[1].witness_count == 1 && fields[1].witness_subprogram_indices[0] == 1);
    CHECK(fields[1].name != f.variables[1][1].name && strcmp(fields[1].name, "_Scale") == 0 &&
          fields[1].layout.byte_offset == 48);
    CHECK(f.buffers[0].var_count == 1 && f.pass.common_parameters[1].cb_count == 0);
    CHECK(hlsl_global_declarations_validate_target(u, &f.program, &f.parameters[0], NULL) ==
          HLSL_GLOBAL_DECLARATIONS_OK);
    f.variables[1][1].name = "_MutatedSibling";
    CHECK(strcmp(fields[1].name, "_Scale") == 0);
    f.variables[0][0].layout[0] = 16;
    CHECK(hlsl_global_declarations_validate_target(u, &f.program, &f.parameters[0], NULL) ==
          HLSL_GLOBAL_DECLARATIONS_FIELD_CONFLICT);
    f.variables[0][0].layout[0] = 32;
    f.cbuffer.size = 4;
    f.instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 3, 0);
    CHECK(hlsl_global_declarations_validate_target(u, &f.program, &f.parameters[0], NULL) ==
          HLSL_GLOBAL_DECLARATIONS_CURRENT_READ_UNAUTHORIZED);
    f.instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 0, 0);
    CHECK(hlsl_global_declarations_validate_target(u, &f.program, &f.parameters[0], NULL) ==
          HLSL_GLOBAL_DECLARATIONS_CURRENT_READ_UNAUTHORIZED);
    f.instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 2, 0);
    f.instructions[0].operands[1].index_has_immediate[1] = false;
    DXBCOperand relative = operand(OPERAND_TYPE_TEMP, 0, 0);
    f.instructions[0].operands[1].rel_op1 = &relative;
    CHECK(hlsl_global_declarations_validate_target(u, &f.program, &f.parameters[0], NULL) ==
          HLSL_GLOBAL_DECLARATIONS_CURRENT_READ_UNAUTHORIZED);
    f.program.program_type = DXBC_PROGRAM_TYPE_VERTEX;
    CHECK(hlsl_global_declarations_validate_target(u, &f.program, &f.parameters[0], NULL) ==
          HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT);
    hlsl_global_declarations_free(u);
    return true;
}

static bool check_conflicting_witnesses(void) {
    for (unsigned mutation = 0; mutation < 21; ++mutation) {
        Fixture f;
        fixture_init(&f);
        HLSLGlobalDeclarationStatus expected = HLSL_GLOBAL_DECLARATIONS_FIELD_CONFLICT;
        size_t witness_count = 2;
        switch (mutation) {
        case 0:
            f.variables[1][0].layout[0] = 16;
            break; // Same name, different offset.
        case 1:
            f.variables[1][0].layout[2] = 1;
            break; // Same name, different scalar type.
        case 2:
            f.variables[1][0].name = "_Alias";
            break; // Different names, overlapping bytes.
        case 3:
            f.variables[1][0].layout[3] = 3;
            break; // Same name, different width.
        case 4:
            f.buffers[1].size = 80;
            expected = HLSL_GLOBAL_DECLARATIONS_SHELL_CONFLICT;
            break;
        case 5:
            f.identities[1].hardware_tier_group = 2;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 6:
            f.sub[1].shader_requirements |= UINT64_C(1) << 40;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 7:
            f.sub[1].has_hardware_tier = true;
            f.sub[1].hardware_tier = 2;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 8:
            f.player[1].local_keyword_count = 0;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 9:
            f.witnesses[1].subprogram_index = 0;
            expected = HLSL_GLOBAL_DECLARATIONS_INVALID;
            break;
        case 10:
            witness_count = 1;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 11:
            f.player[1].has_player_blob_header = false;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 12:
            f.bindings[1].bind_index = 1;
            expected = HLSL_GLOBAL_DECLARATIONS_SHELL_CONFLICT;
            break;
        case 13:
            f.variables[1][1].name = "not a field";
            expected = HLSL_GLOBAL_DECLARATIONS_INVALID;
            break;
        case 14:
            f.sub[1].program_type = 17;
            f.player[1].program_type = 17;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 15:
            f.buffers[1].has_is_partial = true;
            f.buffers[1].is_partial = true;
            expected = HLSL_GLOBAL_DECLARATIONS_SHELL_CONFLICT;
            break;
        case 16:
            f.variables[1][1].layout[1] = 2;
            break; // Arrays cannot close this natural scalar/vector shell.
        case 17:
            f.variables[1][1].layout[0] = 52;
            break; // Non-row-aligned field.
        case 18:
            f.sub[1].global_keyword_count = 1;
            f.sub[1].global_keywords = &f.keyword;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 19:
            f.witnesses[1].player = NULL;
            expected = HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT;
            break;
        case 20:
            f.buffers[1].var_count = 4097;
            expected = HLSL_GLOBAL_DECLARATIONS_INVALID;
            break;
        }
        HLSLGlobalDeclarationUnion *u = (void *)1;
        HLSLGlobalDeclarationDiagnostic diagnostic;
        CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, f.witnesses, witness_count, &u,
                                             &diagnostic) == expected);
        CHECK(!u && diagnostic.status == expected && diagnostic.subprogram_index == 0);
        if (mutation != 9 && mutation != 10)
            CHECK(diagnostic.conflicting_subprogram_index == 1);
    }
    Fixture f;
    fixture_init(&f);
    f.bindings[0].name = "OtherBuffer";
    HLSLGlobalDeclarationUnion *u = NULL;
    CHECK(hlsl_global_declarations_scope_status(&f.parameters[0], NULL) ==
          HLSL_GLOBAL_DECLARATIONS_NOT_APPLICABLE);
    CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, f.witnesses, 2, &u, NULL) ==
              HLSL_GLOBAL_DECLARATIONS_NOT_APPLICABLE &&
          !u);
    fixture_init(&f);
    HLSLGlobalDeclarationWitness reverse[] = {f.witnesses[1], f.witnesses[0]};
    CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, reverse, 2, &u, NULL) ==
          HLSL_GLOBAL_DECLARATIONS_OK);
    size_t count;
    const HLSLGlobalDeclarationField *fields = hlsl_global_declarations_fields(u, &count);
    CHECK(count == 2 && fields[0].witness_count == 2 &&
          fields[0].witness_subprogram_indices[0] == 0 &&
          fields[0].witness_subprogram_indices[1] == 1);
    hlsl_global_declarations_free(u);
    u = NULL;
    // Missing selected witness and conflicting original common metadata reject.
    CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, &f.witnesses[1], 1, &u, NULL) ==
              HLSL_GLOBAL_DECLARATIONS_INVALID &&
          !u);
    SerializedVariable conflict = {"_Color", {16, 0, 0, 4, 0, 0}};
    SerializedConstantBuffer common = {
        .name = "$Globals", .size = 64, .var_count = 1, .variables = &conflict};
    f.pass.common_parameters[1].cb_count = 1;
    f.pass.common_parameters[1].constant_buffers = &common;
    CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, f.witnesses, 2, &u, NULL) ==
              HLSL_GLOBAL_DECLARATIONS_FIELD_CONFLICT &&
          !u);
    return true;
}

static bool check_family_filter(void) {
    Fixture f;
    fixture_init(&f);
    f.pass.subprogram_count[1] = 3;
    f.sub[2] = f.sub[0];
    f.identities[2] = f.identities[0];
    f.identities[2].inner_subprogram_index = 2;
    f.identities[2].hardware_tier_group = 2;
    CHECK(hlsl_global_declarations_same_family(&f.pass, 1, 0, 0));
    CHECK(hlsl_global_declarations_same_family(&f.pass, 1, 0, 1));
    CHECK(!hlsl_global_declarations_same_family(&f.pass, 1, 0, 2));
    HLSLGlobalDeclarationUnion *u = NULL;
    CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, f.witnesses, 2, &u, NULL) ==
          HLSL_GLOBAL_DECLARATIONS_OK);
    hlsl_global_declarations_free(u);
    u = NULL;

    /* Family selection retains every keyword sibling, while excluding
     * unrelated tiers, requirements and program types before loading blobs. */
    f.identities[2].hardware_tier_group = 3;
    f.sub[2].shader_requirements |= UINT64_C(1) << 40;
    CHECK(!hlsl_global_declarations_same_family(&f.pass, 1, 0, 2));
    f.sub[2] = f.sub[0];
    f.sub[2].program_type = 17;
    CHECK(!hlsl_global_declarations_same_family(&f.pass, 1, 0, 2));
    f.sub[2] = f.sub[0];
    f.sub[2].has_hardware_tier = true;
    f.sub[2].hardware_tier = 2;
    CHECK(!hlsl_global_declarations_same_family(&f.pass, 1, 0, 2));
    f.sub[2] = f.sub[0];
    CHECK(hlsl_global_declarations_same_family(&f.pass, 1, 0, 2));
    CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, f.witnesses, 2, &u, NULL) ==
              HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT &&
          !u);
    CHECK(!hlsl_global_declarations_same_family(NULL, 1, 0, 0));
    CHECK(!hlsl_global_declarations_same_family(&f.pass, 5, 0, 0));
    CHECK(!hlsl_global_declarations_same_family(&f.pass, 1, 0, 3));
    f.pass.has_serialized_platforms = false;
    CHECK(!hlsl_global_declarations_same_family(&f.pass, 1, 0, 1));
    return true;
}

static bool check_emission_and_witness_quality(void) {
    Fixture f;
    fixture_init(&f);
    HLSLGlobalDeclarationUnion *u = NULL;
    CHECK(hlsl_global_declarations_build(&f.pass, 1, 0, f.witnesses, 2, &u, NULL) ==
          HLSL_GLOBAL_DECLARATIONS_OK);
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLSourceQualityResult quality;
    options.source_quality = &quality;
    options.global_declarations = u;
    Ledger ledger = {0};
    options.source_quality_observer = observe_witness;
    options.source_quality_observer_context = &ledger;
    StringBuilder source;
    sb_init(&source);
    HLSLEmitDiagnostic diagnostic;
    CHECK(hlsl_emit_with_options_diagnostic(&f.program, &source, &f.parameters[0], NULL, NULL,
                                            &options, &diagnostic));
    CHECK(strstr(source.buf, "float4 _Color : register(c2)") &&
          strstr(source.buf, "float _Scale : register(c3)"));
    CHECK(strstr(source.buf, "return (_Color);") && !strstr(source.buf, "cb0_0") &&
          !strstr(source.buf, "get_cb0"));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
          quality.counts.sibling_declarations == 1 &&
          quality.counts.sibling_declaration_witnesses == 1 && !quality.counts.residual_total &&
          !quality.counts.unknown_provenance);
    CHECK(ledger.witness_records == 1 && ledger.copied.declaration_variant_index == 0 &&
          ledger.copied.declaration_field_index == 1 &&
          ledger.copied.declaration_witness_subprogram_index == 1 &&
          ledger.copied.declaration_witness_count == 1);
    sb_free(&source);
    // Imported declarations never grant executable read or runtime metadata authority.
    f.cbuffer.size = 4;
    f.instructions[0].operands[1] = operand(OPERAND_TYPE_CONSTANT_BUFFER, 3, 0);
    sb_init(&source);
    CHECK(!hlsl_emit_with_options_diagnostic(&f.program, &source, &f.parameters[0], NULL, NULL,
                                             &options, &diagnostic));
    CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    options.mode = HLSL_EMIT_MODE_RECOMPILE;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options_diagnostic(&f.program, &source, &f.parameters[0], NULL, NULL,
                                             &options, &diagnostic));
    CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT);
    sb_free(&source);
    hlsl_global_declarations_free(u);
    CHECK(ledger.copied.declaration_witness_subprogram_index == 1);
    return true;
}

int main(void) {
    if (!check_owned_union_and_target_authority() || !check_conflicting_witnesses() ||
        !check_family_filter() || !check_emission_and_witness_quality())
        return 1;
    puts("Strict declaration-only global sibling union passed");
    return 0;
}
