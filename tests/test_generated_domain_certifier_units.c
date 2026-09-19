#include "compiler/unity_generated_domain_certifier.h"

#include "common/common.h"
#include "common/sha256.h"
#include "dxbc/dxbc_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static void write_u32_le(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static uint32_t instruction_token(uint32_t opcode, uint32_t length) {
    return opcode | (length << 24U);
}

static size_t build_vertex_container(
    uint8_t* bytes, size_t capacity, uint32_t opcode) {
    const size_t total_size = 56U;
    if (!bytes || capacity < total_size) return 0U;
    memset(bytes, 0, total_size);
    memcpy(bytes, "DXBC", 4U);
    write_u32_le(bytes + 20U, 1U);
    write_u32_le(bytes + 24U, (uint32_t)total_size);
    write_u32_le(bytes + 28U, 1U);
    write_u32_le(bytes + 32U, 36U);
    memcpy(bytes + 36U, "SHDR", 4U);
    write_u32_le(bytes + 40U, 12U);
    write_u32_le(bytes + 44U, UINT32_C(0x00010040));
    write_u32_le(bytes + 48U, 3U);
    write_u32_le(bytes + 52U, instruction_token(opcode, 1U));
    uint8_t hash[16];
    if (!dxbc_compute_hash(bytes, total_size, hash)) return 0U;
    memcpy(bytes + 4U, hash, sizeof(hash));
    return total_size;
}

typedef struct {
    SerializedShader shader;
    SerializedPass pass;
    ShaderLabVariantPlan plan;
    SnippetCompileContract contract;
    char* names[2];
    uint8_t flags[2];
    int platforms[1];
    SerializedSubProgram subprograms[4];
    SerializedSubProgramIdentity identities[4];
    ShaderLabVariantState ordered[4];
    ShaderLabVariantState generated[4];
    size_t aliases[4];
    ShaderLabPlannedVariant variants[4];
    ShaderLabVariantAxis axes[2];
    uint16_t axis_a[1];
    uint16_t axis_l[1];
    uint16_t state_a[1];
    uint16_t state_l[1];
    uint16_t state_al[2];
    int identity_a[1];
    int identity_l[1];
    int identity_al_global[1];
    int identity_al_local[1];
    int identity_builtin_both[2];
} DomainFixture;

static int initialize_domain_fixture(DomainFixture* fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->names[0] = (char*)"A";
    fixture->names[1] = (char*)"L";
    fixture->flags[0] = 0U;
    fixture->flags[1] = 1U;
    fixture->shader.name = "Unit/Domain";
    fixture->shader.keyword_names.count = 2;
    fixture->shader.keyword_names.keywords = fixture->names;
    fixture->shader.keyword_flags = fixture->flags;

    fixture->platforms[0] = 4;
    fixture->pass.has_serialized_platforms = true;
    fixture->pass.platform_count = 1;
    fixture->pass.platforms = fixture->platforms;
    fixture->pass.program_mask = UINT32_C(2);
    fixture->pass.subprogram_count[0] = 4;
    fixture->pass.subprograms[0] = fixture->subprograms;
    fixture->pass.subprogram_identities[0] = fixture->identities;
    for (int i = 0; i < 4; ++i) {
        fixture->subprograms[i].program_type = 15;
        fixture->identities[i].hardware_tier_group = 3;
        fixture->identities[i].inner_subprogram_index = i;
    }

    fixture->state_a[0] = 0U;
    fixture->state_l[0] = 1U;
    fixture->state_al[0] = 0U;
    fixture->state_al[1] = 1U;
    fixture->ordered[0] = (ShaderLabVariantState){NULL, 0U};
    fixture->ordered[1] =
        (ShaderLabVariantState){fixture->state_a, 1U};
    fixture->ordered[2] =
        (ShaderLabVariantState){fixture->state_l, 1U};
    fixture->ordered[3] =
        (ShaderLabVariantState){fixture->state_al, 2U};
    memcpy(fixture->generated, fixture->ordered,
           sizeof(fixture->generated));
    for (size_t i = 0; i < 4U; ++i) {
        fixture->aliases[i] = i;
        fixture->variants[i].subprogram_index = (int)i;
        fixture->variants[i].hardware_tier_group = 3;
        fixture->variants[i].keyword_count = fixture->ordered[i].keyword_count;
        fixture->variants[i].keyword_indices =
            (uint16_t*)fixture->ordered[i].keyword_indices;
    }
    fixture->identity_a[0] = 0;
    fixture->identity_l[0] = 1;
    fixture->identity_al_global[0] = 0;
    fixture->identity_al_local[0] = 1;
    fixture->identities[1].global_keyword_index_count = 1;
    fixture->identities[1].global_keyword_indices = fixture->identity_a;
    fixture->identities[2].local_keyword_index_count = 1;
    fixture->identities[2].local_keyword_indices = fixture->identity_l;
    fixture->identities[3].global_keyword_index_count = 1;
    fixture->identities[3].global_keyword_indices =
        fixture->identity_al_global;
    fixture->identities[3].local_keyword_index_count = 1;
    fixture->identities[3].local_keyword_indices = fixture->identity_al_local;

    fixture->axis_a[0] = 0U;
    fixture->axis_l[0] = 1U;
    fixture->axes[0] = (ShaderLabVariantAxis){
        .has_default = true,
        .is_local = false,
        .keyword_count = 1U,
        .keyword_indices = fixture->axis_a,
    };
    fixture->axes[1] = (ShaderLabVariantAxis){
        .has_default = true,
        .is_local = true,
        .keyword_count = 1U,
        .keyword_indices = fixture->axis_l,
    };
    fixture->plan.shader = &fixture->shader;
    fixture->plan.pass = &fixture->pass;
    ShaderLabPassStageVariantPlan* stage = &fixture->plan.stages[0];
    stage->active = true;
    stage->variant_count = 4U;
    stage->variants = fixture->variants;
    stage->state_count = 4U;
    stage->ordered_states = fixture->ordered;
    stage->generated_state_count = 4U;
    stage->generated_states = fixture->generated;
    stage->generated_aliases = fixture->aliases;
    stage->axis_count = 2U;
    stage->axes = fixture->axes;

    unity_compiler_snippet_contract_init(&fixture->contract);
    fixture->contract.program_types_mask = UINT32_C(1);
    static const char* const global[] = {"__ A"};
    static const char* const local[] = {"__ L"};
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture->contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        global, 1));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture->contract, 0, UNITY_KEYWORD_VARIANTS_USER_LOCAL,
        local, 1));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture->contract, 0, UNITY_KEYWORD_VARIANTS_BUILTIN, NULL, 0));
    return 0;
}

static int test_contract_attestation(void) {
    DomainFixture fixture;
    CHECK(initialize_domain_fixture(&fixture) == 0);
    UnityGeneratedDomainReport report;
    unity_generated_domain_report_init(&report);
    CHECK(unity_generated_domain_attest_contract(
              &fixture.shader, &fixture.pass, &fixture.plan,
              &fixture.contract, &report) == UNITY_GENERATED_DOMAIN_OK);
    CHECK(report.active_stage_count == 1U);
    CHECK(report.attested_stage_count == 1U);
    CHECK(report.generated_state_count == 4U);
    CHECK(report.planned_compile_count == 4U);
    CHECK(report.glsl_status ==
          UNITY_GENERATED_GLSL_UNAVAILABLE_NO_PRECISION_AUTHORITY);

    ShaderLabPassStageVariantPlan* stage = &fixture.plan.stages[0];
    stage->generated_domain_is_symbolic_boolean = true;
    stage->generated_states = NULL;
    stage->generated_aliases = NULL;
    CHECK(unity_generated_domain_attest_contract(
              &fixture.shader, &fixture.pass, &fixture.plan,
              &fixture.contract, &report) == UNITY_GENERATED_DOMAIN_OK);
    CHECK(report.generated_state_count == 4U);
    CHECK(report.planned_compile_count == 4U);
    stage->generated_domain_is_symbolic_boolean = false;
    stage->generated_states = fixture.generated;
    stage->generated_aliases = fixture.aliases;

    static const char* const malformed[] = {"A __"};
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture.contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        malformed, 1));
    CHECK(unity_generated_domain_attest_contract(
              &fixture.shader, &fixture.pass, &fixture.plan,
              &fixture.contract, &report) ==
          UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MALFORMED);
    CHECK(report.keyword_families[0][0].expected_valid);
    CHECK(!report.keyword_families[0][0].observed_valid);

    static const char* const duplicate[] = {"__ A", "__ A"};
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture.contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        duplicate, 2));
    CHECK(unity_generated_domain_attest_contract(
              &fixture.shader, &fixture.pass, &fixture.plan,
              &fixture.contract, &report) ==
          UNITY_GENERATED_DOMAIN_CONTRACT_ROW_DUPLICATE);
    CHECK(report.keyword_families[0][0].expected_valid);
    CHECK(report.keyword_families[0][0].observed_valid);
    CHECK(memcmp(report.keyword_families[0][0].expected_digest,
                 report.keyword_families[0][0].observed_digest, 32) != 0);

    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture.contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        NULL, 0));
    CHECK(unity_generated_domain_attest_contract(
              &fixture.shader, &fixture.pass, &fixture.plan,
              &fixture.contract, &report) ==
          UNITY_GENERATED_DOMAIN_CONTRACT_ROW_MISSING);

    static const char* const wrong_scope[] = {"__ L"};
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture.contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        wrong_scope, 1));
    CHECK(unity_generated_domain_attest_contract(
              &fixture.shader, &fixture.pass, &fixture.plan,
              &fixture.contract, &report) ==
          UNITY_GENERATED_DOMAIN_KEYWORD_SCOPE_MISMATCH);

    CHECK(strcmp(unity_generated_domain_status_name(
                     UNITY_GENERATED_DOMAIN_COMPILER_DIAGNOSTIC),
                 "compiler-diagnostic") == 0);
    CHECK(strcmp(unity_generated_glsl_status_name(report.glsl_status),
                 "unavailable-no-gl-source-or-precision-authority") == 0);
    unity_generated_domain_report_free(&report);
    unity_compiler_snippet_contract_free(&fixture.contract);
    return 0;
}

static int test_distinct_cross_stage_raw_aliases_build_emit_and_attest(void) {
    SerializedShader shader;
    SerializedPass pass;
    SerializedSubProgram vertex_subprograms[2];
    SerializedSubProgram fragment_subprograms[2];
    SerializedSubProgramIdentity vertex_identities[2];
    SerializedSubProgramIdentity fragment_identities[2];
    memset(&shader, 0, sizeof(shader));
    memset(&pass, 0, sizeof(pass));
    memset(vertex_subprograms, 0, sizeof(vertex_subprograms));
    memset(fragment_subprograms, 0, sizeof(fragment_subprograms));
    memset(vertex_identities, 0, sizeof(vertex_identities));
    memset(fragment_identities, 0, sizeof(fragment_identities));

    char* names[] = {(char*)"STAGE_SHARED", (char*)"STAGE_SHARED"};
    uint8_t flags[] = {0U, 0U};
    uint16_t pass_mask[] = {0U, 1U};
    int platforms[] = {4};
    int vertex_enabled[] = {0};
    int fragment_enabled[] = {1};
    shader.name = "Unit/CrossStageRawAlias";
    shader.keyword_names.count = 2;
    shader.keyword_names.keywords = names;
    shader.keyword_flags = flags;
    pass.has_serialized_platforms = true;
    pass.platform_count = 1;
    pass.platforms = platforms;
    pass.program_mask = UINT32_C(6);
    pass.serialized_keyword_state_mask = pass_mask;
    pass.serialized_keyword_state_mask_count = 2;
    pass.subprogram_count[0] = 2;
    pass.subprogram_count[1] = 2;
    pass.subprograms[0] = vertex_subprograms;
    pass.subprograms[1] = fragment_subprograms;
    pass.subprogram_identities[0] = vertex_identities;
    pass.subprogram_identities[1] = fragment_identities;
    for (size_t variant = 0U; variant < 2U; ++variant) {
        vertex_subprograms[variant].program_type = 15;
        fragment_subprograms[variant].program_type = 15;
        vertex_identities[variant].hardware_tier_group = 3;
        fragment_identities[variant].hardware_tier_group = 3;
    }
    vertex_identities[1].local_keyword_indices = vertex_enabled;
    vertex_identities[1].local_keyword_index_count = 1;
    fragment_identities[1].local_keyword_indices = fragment_enabled;
    fragment_identities[1].local_keyword_index_count = 1;

    ShaderLabVariantPlan plan;
    ShaderLabVariantPlanDiagnostic diagnostic;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(
              &shader, &pass, &plan, &diagnostic) ==
          SHADERLAB_VARIANT_PLAN_OK);
    CHECK(plan.stages[0].axes[0].keyword_indices[0] == 0U);
    CHECK(plan.stages[1].axes[0].keyword_indices[0] == 1U);

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    CHECK(strcmp(pragmas.buf,
                 "#pragma multi_compile __ STAGE_SHARED\n") == 0);
    sb_free(&pragmas);

    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    contract.program_types_mask = UINT32_C(3);
    static const char* const shared[] = {"__ STAGE_SHARED"};
    for (int program = 0; program < 2; ++program) {
        CHECK(unity_compiler_snippet_contract_set_variant_combinations(
            &contract, program, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
            shared, 1));
        CHECK(unity_compiler_snippet_contract_set_variant_combinations(
            &contract, program, UNITY_KEYWORD_VARIANTS_USER_LOCAL,
            NULL, 0));
        CHECK(unity_compiler_snippet_contract_set_variant_combinations(
            &contract, program, UNITY_KEYWORD_VARIANTS_BUILTIN,
            NULL, 0));
    }

    UnityGeneratedDomainReport report;
    unity_generated_domain_report_init(&report);
    CHECK(unity_generated_domain_attest_contract(
              &shader, &pass, &plan, &contract, &report) ==
          UNITY_GENERATED_DOMAIN_OK);
    CHECK(report.active_stage_count == 2U);
    CHECK(report.attested_stage_count == 2U);
    CHECK(report.generated_state_count == 4U);
    CHECK(report.planned_compile_count == 4U);
    unity_generated_domain_report_free(&report);
    unity_compiler_snippet_contract_free(&contract);
    shaderlab_variant_plan_free(&plan);
    return 0;
}

static int test_user_axis_option_order_is_attested(void) {
    SerializedShader shader;
    SerializedPass pass;
    SerializedSubProgram subprograms[3];
    SerializedSubProgramIdentity identities[3];
    memset(&shader, 0, sizeof(shader));
    memset(&pass, 0, sizeof(pass));
    memset(subprograms, 0, sizeof(subprograms));
    memset(identities, 0, sizeof(identities));

    char* names[] = {(char*)"OPTION_A", (char*)"OPTION_B"};
    uint8_t flags[] = {0U, 0U};
    uint16_t pass_mask[] = {0U, 1U};
    int platforms[] = {4};
    int enabled_a[] = {0};
    int enabled_b[] = {1};
    shader.name = "Unit/UserOptionOrder";
    shader.keyword_names.count = 2;
    shader.keyword_names.keywords = names;
    shader.keyword_flags = flags;
    pass.has_serialized_platforms = true;
    pass.platform_count = 1;
    pass.platforms = platforms;
    pass.program_mask = UINT32_C(2);
    pass.serialized_keyword_state_mask = pass_mask;
    pass.serialized_keyword_state_mask_count = 2;
    pass.subprogram_count[0] = 3;
    pass.subprograms[0] = subprograms;
    pass.subprogram_identities[0] = identities;
    for (size_t variant = 0U; variant < 3U; ++variant) {
        subprograms[variant].program_type = 15;
        identities[variant].hardware_tier_group = 3;
    }
    identities[1].local_keyword_indices = enabled_a;
    identities[1].local_keyword_index_count = 1;
    identities[2].local_keyword_indices = enabled_b;
    identities[2].local_keyword_index_count = 1;

    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(&shader, &pass, &plan, NULL) ==
          SHADERLAB_VARIANT_PLAN_OK);
    CHECK(plan.stages[0].axis_count == 1U);
    CHECK(plan.stages[0].axes[0].keyword_count == 2U);

    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    contract.program_types_mask = UINT32_C(1);
    static const char* const ordered[] = {"__ OPTION_A OPTION_B"};
    static const char* const reversed[] = {"__ OPTION_B OPTION_A"};
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL, ordered, 1));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 0, UNITY_KEYWORD_VARIANTS_USER_LOCAL, NULL, 0));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 0, UNITY_KEYWORD_VARIANTS_BUILTIN, NULL, 0));

    UnityGeneratedDomainReport report;
    unity_generated_domain_report_init(&report);
    CHECK(unity_generated_domain_attest_contract(
              &shader, &pass, &plan, &contract, &report) ==
          UNITY_GENERATED_DOMAIN_OK);
    uint8_t expected_family[32];
    memcpy(expected_family, report.keyword_families[0][0].expected_digest, 32);
    for (size_t family = 0; family < 3; ++family) {
        CHECK(report.keyword_families[0][family].expected_valid);
        CHECK(report.keyword_families[0][family].observed_valid);
        CHECK(memcmp(report.keyword_families[0][family].expected_digest,
                     report.keyword_families[0][family].observed_digest, 32) == 0);
    }
    CHECK(!report.keyword_families[1][0].expected_valid);
    CHECK(!report.keyword_families[1][0].observed_valid);
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL, reversed, 1));
    CHECK(unity_generated_domain_attest_contract(
              &shader, &pass, &plan, &contract, &report) ==
          UNITY_GENERATED_DOMAIN_CONTRACT_ORDER_MISMATCH);
    CHECK(report.keyword_families[0][0].expected_valid);
    CHECK(report.keyword_families[0][0].observed_valid);
    CHECK(memcmp(expected_family, report.keyword_families[0][0].expected_digest, 32) == 0);
    CHECK(memcmp(expected_family, report.keyword_families[0][0].observed_digest, 32) != 0);
    CHECK(!report.keyword_families[0][1].observed_valid);

    unity_generated_domain_report_free(&report);
    unity_compiler_snippet_contract_free(&contract);
    shaderlab_variant_plan_free(&plan);
    return 0;
}

static int test_builtin_cross_stage_raw_aliases_use_stage_authority(void) {
    SerializedShader shader;
    SerializedPass pass;
    SerializedSubProgram subprograms[2][4];
    SerializedSubProgramIdentity identities[2][4];
    memset(&shader, 0, sizeof(shader));
    memset(&pass, 0, sizeof(pass));
    memset(subprograms, 0, sizeof(subprograms));
    memset(identities, 0, sizeof(identities));

    char* names[] = {
        (char*)"SHADOWS_SPLIT_SPHERES",
        (char*)"SHADOWS_SINGLE_CASCADE",
        (char*)"SHADOWS_SPLIT_SPHERES",
        (char*)"SHADOWS_SINGLE_CASCADE",
    };
    uint8_t flags[] = {0U, 0U, 0U, 0U};
    uint16_t pass_mask[] = {0U, 1U, 2U, 3U};
    int platforms[] = {4};
    int vertex_split[] = {0};
    int vertex_single[] = {1};
    int vertex_both[] = {0, 1};
    int fragment_split[] = {2};
    int fragment_single[] = {3};
    int fragment_both[] = {2, 3};
    int* const states[2][4] = {
        {NULL, vertex_split, vertex_single, vertex_both},
        {NULL, fragment_split, fragment_single, fragment_both},
    };
    const int state_counts[] = {0, 1, 1, 2};

    shader.name = "Unit/BuiltinCrossStageRawAlias";
    shader.keyword_names.count = 4;
    shader.keyword_names.keywords = names;
    shader.keyword_flags = flags;
    pass.has_serialized_platforms = true;
    pass.platform_count = 1;
    pass.platforms = platforms;
    pass.program_mask = UINT32_C(6);
    pass.serialized_keyword_state_mask = pass_mask;
    pass.serialized_keyword_state_mask_count = 4;
    for (size_t stage = 0U; stage < 2U; ++stage) {
        pass.subprogram_count[stage] = 4;
        pass.subprograms[stage] = subprograms[stage];
        pass.subprogram_identities[stage] = identities[stage];
        for (size_t variant = 0U; variant < 4U; ++variant) {
            subprograms[stage][variant].program_type = 15;
            identities[stage][variant].hardware_tier_group = 3;
            identities[stage][variant].local_keyword_indices =
                states[stage][variant];
            identities[stage][variant].local_keyword_index_count =
                state_counts[variant];
        }
    }

    ShaderLabVariantPlan plan;
    shaderlab_variant_plan_init(&plan);
    CHECK(shaderlab_variant_plan_build(&shader, &pass, &plan, NULL) ==
          SHADERLAB_VARIANT_PLAN_OK);
    CHECK(plan.has_builtin);
    CHECK(plan.builtin_family ==
          SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR);
    for (size_t state = 1U;
         state < plan.stages[1].generated_state_count; ++state) {
        for (size_t keyword = 0U;
             keyword < plan.stages[1].generated_states[state].keyword_count;
             ++keyword) {
            CHECK(plan.stages[1].generated_states[state]
                      .keyword_indices[keyword] >= 2U);
        }
    }

    StringBuilder pragmas;
    sb_init(&pragmas);
    CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
    CHECK(strcmp(pragmas.buf,
                 "#pragma multi_compile_shadowcollector\n") == 0);
    sb_free(&pragmas);

    SnippetCompileContract contract;
    unity_compiler_snippet_contract_init(&contract);
    contract.program_types_mask = UINT32_C(3);
    static const char* const builtin[] = {
        "",
        "SHADOWS_SPLIT_SPHERES",
        "SHADOWS_SINGLE_CASCADE",
        "SHADOWS_SINGLE_CASCADE SHADOWS_SPLIT_SPHERES",
    };
    for (int program = 0; program < 2; ++program) {
        CHECK(unity_compiler_snippet_contract_set_variant_combinations(
            &contract, program, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
            NULL, 0));
        CHECK(unity_compiler_snippet_contract_set_variant_combinations(
            &contract, program, UNITY_KEYWORD_VARIANTS_USER_LOCAL,
            NULL, 0));
        CHECK(unity_compiler_snippet_contract_set_variant_combinations(
            &contract, program, UNITY_KEYWORD_VARIANTS_BUILTIN,
            builtin, 4));
    }

    UnityGeneratedDomainReport report;
    unity_generated_domain_report_init(&report);
    CHECK(unity_generated_domain_attest_contract(
              &shader, &pass, &plan, &contract, &report) ==
          UNITY_GENERATED_DOMAIN_OK);
    CHECK(report.active_stage_count == 2U);
    CHECK(report.attested_stage_count == 2U);

    unity_generated_domain_report_free(&report);
    unity_compiler_snippet_contract_free(&contract);
    shaderlab_variant_plan_free(&plan);
    return 0;
}

static int test_builtin_empty_default_contract_row(void) {
    DomainFixture fixture;
    CHECK(initialize_domain_fixture(&fixture) == 0);
    fixture.names[0] = (char*)"SHADOWS_SPLIT_SPHERES";
    fixture.names[1] = (char*)"SHADOWS_SINGLE_CASCADE";
    fixture.flags[1] = 0U;
    fixture.identities[2].local_keyword_index_count = 0;
    fixture.identities[2].local_keyword_indices = NULL;
    fixture.identities[2].global_keyword_index_count = 1;
    fixture.identities[2].global_keyword_indices = fixture.identity_l;
    fixture.identities[3].local_keyword_index_count = 0;
    fixture.identities[3].local_keyword_indices = NULL;
    fixture.identities[3].global_keyword_index_count = 2;
    fixture.identity_builtin_both[0] = 0;
    fixture.identity_builtin_both[1] = 1;
    fixture.identities[3].global_keyword_indices =
        fixture.identity_builtin_both;

    fixture.plan.has_builtin = true;
    fixture.plan.builtin_family =
        SHADERLAB_BUILTIN_VARIANT_SHADOWCOLLECTOR;
    fixture.plan.stages[0].axis_count = 0U;
    fixture.plan.stages[0].axes = NULL;
    unity_compiler_snippet_contract_free(&fixture.contract);
    unity_compiler_snippet_contract_init(&fixture.contract);
    fixture.contract.program_types_mask = UINT32_C(1);
    static const char* const builtin[] = {
        "",
        "SHADOWS_SPLIT_SPHERES",
        "SHADOWS_SINGLE_CASCADE",
        /* Token order inside one simultaneous combination is not semantic. */
        "SHADOWS_SINGLE_CASCADE SHADOWS_SPLIT_SPHERES",
    };
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture.contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL,
        NULL, 0));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture.contract, 0, UNITY_KEYWORD_VARIANTS_USER_LOCAL,
        NULL, 0));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &fixture.contract, 0, UNITY_KEYWORD_VARIANTS_BUILTIN,
        builtin, 4));

    UnityGeneratedDomainReport report;
    unity_generated_domain_report_init(&report);
    CHECK(unity_generated_domain_attest_contract(
              &fixture.shader, &fixture.pass, &fixture.plan,
              &fixture.contract, &report) == UNITY_GENERATED_DOMAIN_OK);
    CHECK(report.generated_state_count == 4U);
    CHECK(report.planned_compile_count == 4U);
    unity_generated_domain_report_free(&report);
    unity_compiler_snippet_contract_free(&fixture.contract);
    return 0;
}

typedef struct {
    const uint8_t* bytes;
    size_t size;
    size_t calls;
    bool add_diagnostic;
    int32_t diagnostic_type;
    bool add_unexpected_reflection;
    bool cache_only_miss;
    bool include_authority_unavailable;
    bool transport_failure;
    bool omit_identity;
} FakeCompiler;

static char* duplicate_string(const char* value) {
    const size_t size = strlen(value) + 1U;
    char* copy = (char*)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

static bool set_fake_diagnostic(
    UnityCompilerResponseStatus* status, int32_t type, int32_t code,
    int32_t line, const char* record, const char* file,
    const char* message) {
    if (!status || !record || !file || !message) return false;
    status->diagnostics = (UnityCompilerDiagnostic*)calloc(
        1U, sizeof(*status->diagnostics));
    if (!status->diagnostics) return false;
    status->diagnostic_count = 1U;
    UnityCompilerDiagnostic* diagnostic = &status->diagnostics[0];
    diagnostic->fields[0] = type;
    diagnostic->fields[1] = code;
    diagnostic->fields[2] = line;
    diagnostic->record = duplicate_string(record);
    diagnostic->file = duplicate_string(file);
    diagnostic->message = duplicate_string(message);
    return diagnostic->record && diagnostic->file && diagnostic->message;
}

static bool set_fake_reflection(
    UnityCompilerBinaryResponse* response, const char* record) {
    response->reflection_records =
        (UnityCompilerReflectionRecord*)calloc(
            1U, sizeof(*response->reflection_records));
    if (!response->reflection_records) return false;
    if (!unity_compiler_reflection_record_parse(
            record, response->reflection_records)) {
        free(response->reflection_records);
        response->reflection_records = NULL;
        return false;
    }
    response->reflection_record_count = 1U;
    return true;
}

/* Distinct synthetic request identities test transfer only. Canonical request
 * serialization and toolchain authority are exercised by compiler-client tests. */
static void set_fake_request_identity(UnityCompilerBinaryResponse *response, size_t call) {
    response->has_request_identity = true;
    memset(response->request_digest, (int)call, sizeof(response->request_digest));
    memset(response->controls_digest, 0x55, sizeof(response->controls_digest));
}

static bool fake_compile(
    void* context, const UnityCompilerSnippetCompileRequest* request,
    UnityCompilerBinaryResponse* response) {
    FakeCompiler* compiler = (FakeCompiler*)context;
    if (!compiler || !request || !response || request->platform != 4 ||
        request->shader_type != 0 || request->user_keyword_count != 0) {
        return false;
    }
    const char* expected_tier = compiler->calls == 0U
        ? "UNITY_HARDWARE_TIER1"
        : (compiler->calls == 1U ? "UNITY_HARDWARE_TIER2"
                                 : "UNITY_HARDWARE_TIER3");
    bool found_tier = false;
    for (int i = 0; i < request->variant_keyword_count; ++i) {
        if (strcmp(request->variant_keywords[i], expected_tier) == 0) {
            found_tier = true;
        }
    }
    if (!found_tier) return false;
    ++compiler->calls;
    if (!compiler->omit_identity) set_fake_request_identity(response, compiler->calls);
    if (compiler->transport_failure) return false;
    if (compiler->cache_only_miss || compiler->include_authority_unavailable) {
        response->status.availability = compiler->cache_only_miss
            ? UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS
            : UNITY_COMPILER_RESPONSE_INCLUDE_AUTHORITY_UNAVAILABLE;
        return true;
    }
    response->status.compiler_success = true;
    if (compiler->add_diagnostic) {
        if (!set_fake_diagnostic(
                &response->status, compiler->diagnostic_type, 8, 9,
                "err: 7 8 9", "unit.shader", "unit diagnostic"))
            return false;
    }
    if (compiler->add_unexpected_reflection &&
        !set_fake_reflection(response, "input: 0 0")) {
        return false;
    }
    response->data = (uint8_t*)malloc(compiler->size);
    if (!response->data) return false;
    memcpy(response->data, compiler->bytes, compiler->size);
    response->size = compiler->size;
    return true;
}

typedef struct {
    const uint8_t* bytes;
    size_t size;
    size_t calls;
    bool mismatch_original_message;
    bool mismatch_original_reflection;
} DiagnosticParityCompiler;

static bool diagnostic_parity_compile(
    void* context, const UnityCompilerSnippetCompileRequest* request,
    UnityCompilerBinaryResponse* response) {
    DiagnosticParityCompiler* compiler =
        (DiagnosticParityCompiler*)context;
    if (!compiler || !request || !response || request->platform != 4 ||
        request->shader_type != 0 || !request->snippet_source) {
        return false;
    }
    const bool original = strstr(request->snippet_source, "original") != NULL;
    if (original != ((compiler->calls & 1U) != 0U)) return false;
    const size_t tier = compiler->calls / 2U;
    const char* expected_tier = tier == 0U
        ? "UNITY_HARDWARE_TIER1"
        : (tier == 1U ? "UNITY_HARDWARE_TIER2"
                      : "UNITY_HARDWARE_TIER3");
    bool found_tier = false;
    for (int i = 0; i < request->variant_keyword_count; ++i) {
        if (strcmp(request->variant_keywords[i], expected_tier) == 0)
            found_tier = true;
    }
    if (!found_tier || tier >= 3U) return false;
    ++compiler->calls;
    set_fake_request_identity(response, compiler->calls);
    response->status.compiler_success = true;
    if (!set_fake_diagnostic(
            &response->status, 1, 4, original ? 39 : 132,
            original ? "err: 1 4 39" : "err: 1 4 132",
            original ? "original.cginc" : "generated.shader",
            original && compiler->mismatch_original_message
                ? "different warning"
                : "Output value 'vert' is not completely initialized")) {
        return false;
    }
    if (original && compiler->mismatch_original_reflection &&
        !set_fake_reflection(response, "stats: 1 2 3 4")) {
        return false;
    }
    response->data = (uint8_t*)malloc(compiler->size);
    if (!response->data) return false;
    memcpy(response->data, compiler->bytes, compiler->size);
    response->size = compiler->size;
    return true;
}

static size_t build_player_blob(
    uint8_t* output, size_t capacity, const uint8_t* dxbc, size_t dxbc_size) {
    if (!output || !dxbc || dxbc_size > UINT32_MAX ||
        dxbc_size > capacity || capacity - dxbc_size < 40U) {
        return 0U;
    }
    size_t at = 0U;
#define APPEND_U32(value)                                                      \
    do {                                                                       \
        write_u32_le(output + at, (uint32_t)(value));                          \
        at += 4U;                                                              \
    } while (0)
    APPEND_U32(UNITY_2021_3_PLAYER_BLOB_VERSION);
    APPEND_U32(15U);
    APPEND_U32(0U);
    APPEND_U32(0U);
    APPEND_U32(0U);
    APPEND_U32(0U);
    APPEND_U32(0U);
    APPEND_U32(dxbc_size);
    memcpy(output + at, dxbc, dxbc_size);
    at += dxbc_size;
    while ((at & 3U) != 0U) output[at++] = 0U;
    APPEND_U32(0U);
    APPEND_U32(0U);
#undef APPEND_U32
    return at;
}

static int test_full_tiered_certification(void) {
    uint8_t reference_dxbc[64];
    uint8_t mismatch_dxbc[64];
    const size_t dxbc_size = build_vertex_container(
        reference_dxbc, sizeof(reference_dxbc), 62U);
    CHECK(dxbc_size != 0U);
    CHECK(build_vertex_container(
              mismatch_dxbc, sizeof(mismatch_dxbc), 58U) == dxbc_size);
    uint8_t player_blob[128];
    const size_t player_size = build_player_blob(
        player_blob, sizeof(player_blob), reference_dxbc, dxbc_size);
    CHECK(player_size != 0U && player_size <= INT32_MAX);

    SerializedShader shader;
    SerializedPass pass;
    ShaderLabVariantPlan plan;
    memset(&shader, 0, sizeof(shader));
    memset(&pass, 0, sizeof(pass));
    memset(&plan, 0, sizeof(plan));
    shader.name = "Unit/Tiered";
    int platforms[] = {4};
    pass.has_serialized_platforms = true;
    pass.platform_count = 1;
    pass.platforms = platforms;
    pass.program_mask = UINT32_C(2);
    pass.name = "UnitPass";
    SerializedSubProgram subprograms[3];
    SerializedSubProgramIdentity identities[3];
    memset(subprograms, 0, sizeof(subprograms));
    memset(identities, 0, sizeof(identities));
    for (int tier = 0; tier < 3; ++tier) {
        subprograms[tier].blob_index = 0;
        subprograms[tier].program_type = 15;
        identities[tier].hardware_tier_group = tier;
        identities[tier].inner_subprogram_index = 0;
    }
    pass.subprogram_count[0] = 3;
    pass.subprograms[0] = subprograms;
    pass.subprogram_identities[0] = identities;

    ShaderLabVariantState empty = {NULL, 0U};
    size_t alias = 0U;
    ShaderLabPlannedVariant variants[3];
    memset(variants, 0, sizeof(variants));
    for (int tier = 0; tier < 3; ++tier) {
        variants[tier].subprogram_index = tier;
        variants[tier].hardware_tier_group = tier;
    }
    plan.shader = &shader;
    plan.pass = &pass;
    plan.uses_specific_hardware_tiers = true;
    ShaderLabPassStageVariantPlan* stage = &plan.stages[0];
    stage->active = true;
    stage->uses_specific_hardware_tiers = true;
    stage->variant_count = 3U;
    stage->variants = variants;
    stage->state_count = 1U;
    stage->ordered_states = &empty;
    stage->generated_state_count = 1U;
    stage->generated_states = &empty;
    stage->generated_aliases = &alias;

    PreprocessedSnippet snippet;
    memset(&snippet, 0, sizeof(snippet));
    snippet.source = (char*)"void vert() {}";
    snippet.has_contract = true;
    unity_compiler_snippet_contract_init(&snippet.contract);
    snippet.contract.program_types_mask = UINT32_C(1);
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &snippet.contract, 0, UNITY_KEYWORD_VARIANTS_USER_GLOBAL, NULL, 0));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &snippet.contract, 0, UNITY_KEYWORD_VARIANTS_USER_LOCAL, NULL, 0));
    CHECK(unity_compiler_snippet_contract_set_variant_combinations(
        &snippet.contract, 0, UNITY_KEYWORD_VARIANTS_BUILTIN, NULL, 0));

    BlobEntry entry = {.offset = 0, .length = (int32_t)player_size, .segment = 0};
    uint8_t* segments[] = {player_blob};
    int segment_lengths[] = {(int)player_size};
    ShaderBlobArchive archive = {
        .entries = &entry,
        .entry_count = 1,
        .segments = segments,
        .segment_lengths = segment_lengths,
        .segment_count = 1,
    };
    UnityCompileProfile profile;
    unity_compile_profile_init(&profile);
    profile.build_platform = 19U;
    profile.valid_apis = 295472U;
    memcpy(profile.provenance, "unit-test", sizeof("unit-test"));

    FakeCompiler compiler = {
        .bytes = reference_dxbc,
        .size = dxbc_size,
    };
    UnityGeneratedDomainCertificationInput input = {
        .shader = &shader,
        .pass = &pass,
        .plan = &plan,
        .generated_snippet = &snippet,
        .d3d11_archive = &archive,
        .compile_profile = &profile,
        .source_directory = "Assets",
        .source_basename = "unit.shader",
        .pass_name = "UnitPass",
        .compile_callback = fake_compile,
        .compile_context = &compiler,
    };
    UnityGeneratedDomainReport report;
    unity_generated_domain_report_init(&report);
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_OK);
    CHECK(compiler.calls == 3U);
    CHECK(report.planned_compile_count == 3U);
    CHECK(report.compile_attempt_count == 3U);
    CHECK(report.clean_compile_count == 3U);
    CHECK(report.matched_dxbc_count == 3U);
    CHECK(report.runtime_binding_attested_compile_count == 3U);
    CHECK(report.runtime_binding_compatible_compile_count == 0U);

    CHECK(report.compiler_response_count == 0U);
    input.retain_compile_provenance = true;
    compiler.calls = 0;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) == UNITY_GENERATED_DOMAIN_OK);
    CHECK(report.compiler_response_count == 3U);
    uint8_t target_digest[COMMON_SHA256_DIGEST_SIZE];
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(reference_dxbc, dxbc_size, target_digest);
    common_sha256(snippet.source, strlen(snippet.source), source_digest);
    for (size_t i = 0; i < report.compiler_response_count; ++i) {
        const UnityGeneratedCompileProvenance *provenance = &report.compiler_responses[i].provenance;
        CHECK(provenance->recorded && provenance->response_received);
        CHECK(provenance->has_request_identity && provenance->has_output_digest);
        CHECK(provenance->request_digest[0] == i + 1U);
        CHECK(provenance->controls_digest[0] == 0x55);
        CHECK(memcmp(provenance->source_digest, source_digest, sizeof(source_digest)) == 0);
        CHECK(memcmp(provenance->target_digest, target_digest, sizeof(target_digest)) == 0);
        CHECK(memcmp(provenance->output_digest, target_digest, sizeof(target_digest)) == 0);
        CHECK(!report.compiler_responses[i].original_provenance.recorded);
        CHECK(report.compiler_responses[i].reflection_certificate_present);
        const UnityReflectionCertificateReport *bindings =
            &report.compiler_responses[i].reflection_certificate;
        CHECK(bindings->expected_bindings_digest_valid && bindings->observed_bindings_digest_valid);
        CHECK(memcmp(bindings->expected_bindings_digest,
                     bindings->observed_bindings_digest, COMMON_SHA256_DIGEST_SIZE) == 0);

    }
    compiler = (FakeCompiler){.bytes = reference_dxbc, .size = dxbc_size,
                              .transport_failure = true};
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_COMPILER_TRANSPORT_FAILED);
    CHECK(report.compiler_response_count == 1U);
    CHECK(report.compiler_responses[0].provenance.recorded);
    CHECK(!report.compiler_responses[0].provenance.response_received);
    CHECK(!report.compiler_responses[0].reflection_certificate_present);
    CHECK(report.compiler_responses[0].provenance.has_request_identity);
    CHECK(!report.compiler_responses[0].provenance.has_output_digest);
    compiler = (FakeCompiler){.bytes = reference_dxbc, .size = 1U};
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_COMPILED_DXBC_INVALID);
    CHECK(report.compiler_response_count == 1U);
    CHECK(report.compiler_responses[0].provenance.response_received);
    CHECK(report.compiler_responses[0].provenance.has_request_identity);
    CHECK(!report.compiler_responses[0].provenance.has_output_digest);
    compiler = (FakeCompiler){.bytes = reference_dxbc, .size = dxbc_size, .omit_identity = true};
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) == UNITY_GENERATED_DOMAIN_OK);
    CHECK(report.compiler_response_count == 3U);
    CHECK(!report.compiler_responses[0].provenance.has_request_identity);
    CHECK(report.compiler_responses[0].provenance.request_digest[0] == 0);

    compiler = (FakeCompiler){
        .bytes = reference_dxbc,
        .size = dxbc_size,
        .add_unexpected_reflection = true,
    };
    input.compile_context = &compiler;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_REFLECTION_MISMATCH);
    CHECK(compiler.calls == 1U);
    CHECK(report.runtime_binding_attested_compile_count == 0U);
    CHECK(report.runtime_binding_compatible_compile_count == 0U);
    CHECK(report.diagnostic.reflection_certificate.status ==
          UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);

    compiler = (FakeCompiler){
        .bytes = reference_dxbc,
        .size = dxbc_size,
        .cache_only_miss = true,
    };
    input.compile_context = &compiler;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_COMPILER_CACHE_ONLY_MISS);
    CHECK(compiler.calls == 1U);
    CHECK(report.compile_attempt_count == 1U);
    CHECK(report.clean_compile_count == 0U);
    CHECK(report.diagnostic.compiler_response.availability ==
          UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS);
    CHECK(strcmp(unity_generated_domain_status_name(report.status),
                 "compiler-cache-only-miss") == 0);
    CHECK(report.compiler_response_count == 1U);
    CHECK(report.compiler_responses[0].provenance.recorded);
    CHECK(report.compiler_responses[0].provenance.response_received);
    CHECK(report.compiler_responses[0].provenance.has_request_identity);
    CHECK(!report.compiler_responses[0].provenance.has_output_digest);

    compiler = (FakeCompiler){.include_authority_unavailable = true, .omit_identity = true};
    input.compile_context = &compiler;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_INCLUDE_AUTHORITY_UNAVAILABLE);
    CHECK(compiler.calls == 1U && report.clean_compile_count == 0U &&
          report.matched_dxbc_count == 0U && report.compiler_response_count == 1U);
    CHECK(report.diagnostic.compiler_response.availability ==
          UNITY_COMPILER_RESPONSE_INCLUDE_AUTHORITY_UNAVAILABLE);
    CHECK(strcmp(unity_generated_domain_status_name(report.status),
                 "include-authority-unavailable") == 0);
    CHECK(!report.compiler_responses[0].provenance.has_request_identity);

    compiler = (FakeCompiler){
        .bytes = reference_dxbc,
        .size = dxbc_size,
        .add_diagnostic = true,
        .diagnostic_type = 7,
    };
    input.compile_context = &compiler;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_COMPILER_DIAGNOSTIC);
    CHECK(report.compile_attempt_count == 1U);
    CHECK(report.compiler_diagnostic_count == 1U);
    CHECK(report.compiler_response_count == 1U);
    CHECK(report.compiler_responses[0].stage_index == 0);
    CHECK(report.compiler_responses[0].hardware_tier_group == 0);
    CHECK(report.compiler_responses[0].generated_state_index == 0U);
    CHECK(report.compiler_responses[0].aliased_state_index == 0U);
    CHECK(report.compiler_responses[0].subprogram_index == 0);
    CHECK(report.compiler_responses[0].response.diagnostic_count == 1U);
    CHECK(report.compiler_responses[0].diagnostic_parity_status ==
          UNITY_GENERATED_DIAGNOSTIC_PARITY_SOURCE_UNAVAILABLE);
    CHECK(!report.compiler_responses[0].original_response_present);
    CHECK(report.diagnostic_attestation_compile_count == 0U);
    CHECK(report.diagnostic.compiler_response.diagnostic_count == 1U);
    CHECK(strcmp(report.diagnostic.compiler_response.diagnostics[0].message,
                 "unit diagnostic") == 0);

    /* Source-backed parity admits the warning only after compiling both
     * snippets with identical controls and proving both complete containers
     * equal the reference. Source line/file/record differences are retained
     * but are deliberately outside the normalized multiset. */
    PreprocessedSnippet original_snippet;
    memset(&original_snippet, 0, sizeof(original_snippet));
    original_snippet.source = (char*)"void original_vert() {}";
    original_snippet.has_contract = true;
    unity_compiler_snippet_contract_init(&original_snippet.contract);
    CHECK(unity_compiler_snippet_contract_copy(
        &original_snippet.contract, &snippet.contract));
    DiagnosticParityCompiler parity_compiler = {
        .bytes = reference_dxbc,
        .size = dxbc_size,
    };
    input.compile_callback = diagnostic_parity_compile;
    input.compile_context = &parity_compiler;
    input.original_snippet = &original_snippet;
    input.original_source_directory = "Original";
    input.original_source_basename = "original.shader";
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_OK);
    CHECK(parity_compiler.calls == 6U);
    CHECK(report.compile_attempt_count == 3U);
    CHECK(report.clean_compile_count == 0U);
    CHECK(report.matched_dxbc_count == 3U);
    CHECK(report.diagnostic_attestation_compile_count == 3U);
    CHECK(report.diagnostic_attested_compile_count == 3U);
    CHECK(report.diagnostic_attested_actionable_count == 3U);
    CHECK(report.compiler_response_count == 3U);
    for (size_t response_index = 0U;
         response_index < report.compiler_response_count;
         ++response_index) {
        const UnityGeneratedDomainCompilerResponseRecord* record =
            &report.compiler_responses[response_index];
        CHECK(record->diagnostic_parity_status ==
              UNITY_GENERATED_DIAGNOSTIC_PARITY_ATTESTED);
        CHECK(record->original_response_present);
        CHECK(record->response.diagnostics[0].fields[2] == 132);
        CHECK(record->original_response.diagnostics[0].fields[2] == 39);
        CHECK(record->original_dxbc_compare.status == DXBC_COMPARE_EQUAL);
        CHECK(record->provenance.recorded && record->original_provenance.recorded);
        CHECK(record->original_provenance.response_received);
        CHECK(record->original_provenance.has_request_identity);
        CHECK(record->original_provenance.has_output_digest);
        CHECK(memcmp(record->provenance.source_digest, record->original_provenance.source_digest,
                     COMMON_SHA256_DIGEST_SIZE) != 0);
        CHECK(memcmp(record->provenance.request_digest, record->original_provenance.request_digest,
                     COMMON_SHA256_DIGEST_SIZE) != 0);
        CHECK(memcmp(record->provenance.target_digest, record->original_provenance.output_digest,
                     COMMON_SHA256_DIGEST_SIZE) == 0);
    }

    parity_compiler = (DiagnosticParityCompiler){
        .bytes = reference_dxbc,
        .size = dxbc_size,
        .mismatch_original_message = true,
    };
    input.compile_context = &parity_compiler;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_DIAGNOSTIC_ATTESTATION_FAILED);
    CHECK(parity_compiler.calls == 2U);
    CHECK(report.compiler_response_count == 1U);
    CHECK(report.compiler_responses[0].diagnostic_parity_status ==
          UNITY_GENERATED_DIAGNOSTIC_PARITY_DIAGNOSTICS_MISMATCH);

    parity_compiler = (DiagnosticParityCompiler){
        .bytes = reference_dxbc,
        .size = dxbc_size,
        .mismatch_original_reflection = true,
    };
    input.compile_context = &parity_compiler;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_DIAGNOSTIC_ATTESTATION_FAILED);
    CHECK(parity_compiler.calls == 2U);
    CHECK(report.compiler_response_count == 1U);
    CHECK(report.compiler_responses[0].diagnostic_parity_status ==
          UNITY_GENERATED_DIAGNOSTIC_PARITY_REFLECTION_MISMATCH);

    compiler = (FakeCompiler){
        .bytes = reference_dxbc,
        .size = dxbc_size,
        .add_diagnostic = true,
    };
    input.compile_callback = fake_compile;
    input.compile_context = &compiler;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_OK);
    CHECK(report.compile_attempt_count == 3U);
    CHECK(report.compiler_diagnostic_count == 3U);
    CHECK(report.compiler_response_count == 3U);
    CHECK(report.matched_dxbc_count == 3U);

    compiler = (FakeCompiler){
        .bytes = mismatch_dxbc,
        .size = dxbc_size,
    };
    input.compile_context = &compiler;
    CHECK(unity_generated_domain_certify_d3d11(&input, &report) ==
          UNITY_GENERATED_DOMAIN_DXBC_MISMATCH);
    CHECK(report.diagnostic.dxbc_compare.status ==
          DXBC_COMPARE_INSTRUCTION_OPCODE);
    CHECK(report.compiler_response_count == 1U);
    CHECK(report.compiler_responses[0].provenance.has_output_digest);
    CHECK(memcmp(report.compiler_responses[0].provenance.target_digest,
                 report.compiler_responses[0].provenance.output_digest,
                 COMMON_SHA256_DIGEST_SIZE) != 0);

    unity_generated_domain_report_free(&report);
    unity_compiler_snippet_contract_free(&original_snippet.contract);
    unity_compiler_snippet_contract_free(&snippet.contract);
    return 0;
}

static int test_diagnostic_fingerprints(void) {
    UnityCompilerDiagnostic records[2] = {
        {.fields = {1, 3206, 12}, .message = "conversion"},
        {.fields = {0, 0, 18}, .message = "information"},
    };
    UnityCompilerResponseStatus response = {.diagnostics = records, .diagnostic_count = 2};
    uint8_t baseline[32], changed[32];
    CHECK(unity_generated_domain_diagnostics_fingerprint(&response, baseline));
    UnityCompilerDiagnostic swap = records[0];
    records[0] = records[1];
    records[1] = swap;
    records[0].fields[2]++;
    CHECK(unity_generated_domain_diagnostics_fingerprint(&response, changed));
    CHECK(memcmp(baseline, changed, 32) == 0);
    records[0].message = "different information";
    CHECK(unity_generated_domain_diagnostics_fingerprint(&response, changed));
    CHECK(memcmp(baseline, changed, 32) != 0);
    records[0].message = NULL;
    CHECK(!unity_generated_domain_diagnostics_fingerprint(&response, changed));
    response.diagnostic_count = 0;
    CHECK(unity_generated_domain_diagnostics_fingerprint(&response, changed));
    CHECK(memcmp(baseline, changed, 32) != 0);
    static const uint8_t empty_digest[32] = {0x20, 0x6c, 0x77, 0x1c, 0x36, 0x45, 0x00, 0x76,
                                             0x65, 0x06, 0x41, 0x81, 0x7e, 0x2b, 0xb1, 0xa0,
                                             0x12, 0x2d, 0x14, 0xe7, 0x51, 0x81, 0x48, 0x03,
                                             0x4b, 0x59, 0x4a, 0x0f, 0x01, 0xc3, 0xff, 0x0c};
    CHECK(memcmp(empty_digest, changed, 32) == 0);
    return 0;
}

int main(void) {
    CHECK(test_diagnostic_fingerprints() == 0);
    CHECK(test_contract_attestation() == 0);
    CHECK(test_distinct_cross_stage_raw_aliases_build_emit_and_attest() == 0);
    CHECK(test_user_axis_option_order_is_attested() == 0);
    CHECK(test_builtin_cross_stage_raw_aliases_use_stage_authority() == 0);
    CHECK(test_builtin_empty_default_contract_row() == 0);
    CHECK(test_full_tiered_certification() == 0);
    puts("generated domain certifier unit tests passed");
    return 0;
}
