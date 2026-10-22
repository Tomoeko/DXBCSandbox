// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "common/sha256.h"
#include "test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "Failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);                \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    const char *path;
    const char *digest;
    bool literal_first;
    unsigned model;
    USILOpcode comparison_opcode;
} DiscardFixtureCase;

static bool check_variant_emission(USILProgram *program);

static bool plan_empty(const HLSLSampledDiscardPlan *plan) {
    return !plan->present && plan->comparison_opcode == USIL_OP_NOP && !plan->color_register &&
           !plan->predicate_register && !plan->color_component && !plan->predicate_component &&
           !plan->literal_first && !plan->discard_on_zero && !plan->threshold_bits;
}

static void set_temp_register(DXBCOperand *operand, int index) {
    operand->register_index = index;
    operand->rel_offset1 = index;
    operand->index_values[0] = (uint32_t)index;
}

static void set_scalar_component(DXBCOperand *operand, unsigned component) {
    operand->raw_token = UINT32_C(0x0010000a) | (uint32_t)component << 4u;
    for (unsigned lane = 0; lane < 4; ++lane) {
        operand->swizzle[lane] = (uint8_t)component;
    }
}

static bool check_analysis(USILProgram *program) {
    HLSLEmitterContext *ctx = calloc(1, sizeof(*ctx));
    bool passed = false;
    REQUIRE(ctx);
    ctx->program = program;
    ctx->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
    REQUIRE(build_control_flow_graph(ctx) && compute_dominance(&ctx->cfg) &&
            build_hlsl_ssa_graph(ctx) && build_component_provenance(ctx) &&
            build_hlsl_use_def_graph(ctx));
    REQUIRE(hlsl_sampled_discard_preflight(ctx));
    ctx->cfg.blocks[1].may_exit = false;
    REQUIRE(!hlsl_sampled_discard_preflight(ctx));
    ctx->cfg.blocks[1].may_exit = true;
    ctx->cfg.idom[2] = 0;
    REQUIRE(!hlsl_sampled_discard_preflight(ctx));
    ctx->cfg.idom[2] = 1;
    ctx->ssa.block_phis[2].phi_count = 1;
    REQUIRE(!hlsl_sampled_discard_preflight(ctx));
    ctx->ssa.block_phis[2].phi_count = 0;
    ctx->use_def.definition_use_counts[0] += 1;
    REQUIRE(!hlsl_sampled_discard_preflight(ctx));
    ctx->use_def.definition_use_counts[0] -= 1;
    ctx->compiler_model.replacement_count = 1;
    REQUIRE(!hlsl_sampled_discard_preflight(ctx));
    ctx->compiler_model.replacement_count = 0;
    REQUIRE(hlsl_sampled_discard_preflight(ctx));
    passed = true;

cleanup:
    if (ctx) {
        if (ctx->ssa.block_phis && ctx->cfg.block_count == 4) {
            ctx->ssa.block_phis[2].phi_count = 0;
        }
        free_hlsl_use_def_graph(ctx);
        free_component_provenance(ctx);
        free_hlsl_ssa_graph(ctx);
        free_control_flow_graph(ctx);
        free(ctx);
    }
    return passed;
}

static bool check_variants(USILProgram *program, bool literal_first) {
    USILInstruction original[5];
    memcpy(original, program->instructions, sizeof(original));
    bool passed = false;
    const unsigned color_operand = literal_first ? 2 : 1;
    for (int color_register = 0; color_register < 2; ++color_register) {
        for (unsigned color_component = 0; color_component < 4; ++color_component) {
            for (unsigned predicate_component = 0; predicate_component < 4; ++predicate_component) {
                for (unsigned zero = 0; zero < 2; ++zero) {
                    memcpy(program->instructions, original, sizeof(original));
                    USILInstruction *instructions = program->instructions;
                    const int predicate_register = 1 - color_register;
                    set_temp_register(&instructions[0].operands[0], color_register);
                    set_temp_register(&instructions[1].operands[0], predicate_register);
                    set_temp_register(&instructions[1].operands[color_operand], color_register);
                    set_temp_register(&instructions[2].operands[0], predicate_register);
                    set_temp_register(&instructions[3].operands[1], color_register);
                    set_scalar_component(&instructions[1].operands[color_operand], color_component);
                    set_scalar_component(&instructions[2].operands[0], predicate_component);
                    const uint32_t mask = UINT32_C(1) << predicate_component;
                    instructions[1].operands[0].raw_token = UINT32_C(0x00100002) | mask << 4u;
                    instructions[1].operands[0].destination_mask = (uint8_t)(mask << 4u);
                    instructions[2].condition_test =
                        zero ? DXBC_INSTRUCTION_TEST_ZERO : DXBC_INSTRUCTION_TEST_NONZERO;
                    HLSLSampledDiscardPlan plan;
                    REQUIRE(hlsl_sampled_discard_plan_prepare(program, &plan));
                    REQUIRE(plan.present && plan.comparison_opcode == original[1].opcode &&
                            plan.color_register == color_register &&
                            plan.predicate_register == predicate_register &&
                            plan.color_component == color_component &&
                            plan.predicate_component == predicate_component &&
                            plan.literal_first == literal_first &&
                            plan.discard_on_zero == (zero != 0) &&
                            plan.threshold_bits == UINT32_C(0x3ec00000));
                    REQUIRE(check_analysis(program) && check_variant_emission(program));
                }
            }
        }
    }
    memcpy(program->instructions, original, sizeof(original));
    static const uint32_t bits[] = {0,
        UINT32_C(0x80000000),
        UINT32_C(0x00000001),
        UINT32_C(0x007fffff),
        UINT32_C(0x00800000),
        UINT32_C(0x3f800000),
        UINT32_C(0xbf800000),
        UINT32_C(0x7f7fffff),
        UINT32_C(0x7f800000),
        UINT32_C(0xff800000),
        UINT32_C(0x7fc00001),
        UINT32_C(0xffc12345)};
    DXBCOperand *literal = &program->instructions[1].operands[literal_first ? 1 : 2];
    for (size_t index = 0; index < sizeof(bits) / sizeof(*bits); ++index) {
        literal->immediate_words[0] = bits[index];
        for (unsigned lane = 0; lane < 4; ++lane) {
            literal->imm_values[lane] = bits[index];
        }
        HLSLSampledDiscardPlan plan;
        REQUIRE(hlsl_sampled_discard_plan_prepare(program, &plan) &&
                plan.threshold_bits == bits[index]);
        REQUIRE(check_analysis(program) && check_variant_emission(program));
    }
    passed = true;

cleanup:
    memcpy(program->instructions, original, sizeof(original));
    return passed;
}

static bool check_boundaries(USILProgram *program, bool literal_first) {
    USILInstruction original[5];
    memcpy(original, program->instructions, sizeof(original));
    bool passed = false;
    for (unsigned mutation = 0; mutation < 15; ++mutation) {
        memcpy(program->instructions, original, sizeof(original));
        USILInstruction *instructions = program->instructions;
        DXBCOperand *literal = &instructions[1].operands[literal_first ? 1 : 2];
        switch (mutation) {
            case 0:
                instructions[1].opcode = USIL_OP_EQ;
                break;
            case 1:
                instructions[1].precise_mask = 1;
                break;
            case 2:
                instructions[0].saturate = true;
                break;
            case 3:
                instructions[2].condition_test = 0;
                break;
            case 4:
                instructions[2].condition_test = 3;
                break;
            case 5:
                instructions[2].opcode = USIL_OP_RET;
                break;
            case 6:
                set_temp_register(&instructions[1].operands[0], 0);
                break;
            case 7:
                set_temp_register(&instructions[2].operands[0], 0);
                break;
            case 8:
                set_scalar_component(&instructions[2].operands[0], 1);
                break;
            case 9:
                instructions[3].operands[0].destination_mask = 0x70;
                break;
            case 10:
                instructions[1].source_instruction_index += 1;
                break;
            case 11:
                literal->immediate_word_count = 4;
                break;
            case 12:
                literal->imm_values[1] ^= 1;
                break;
            case 13:
                instructions[3].operands[1].has_neg = true;
                break;
            case 14:
                instructions[4].opcode = USIL_OP_NOP;
                break;
        }
        HLSLSampledDiscardPlan plan = {.present = true, .threshold_bits = UINT32_MAX};
        REQUIRE(!hlsl_sampled_discard_plan_prepare(program, &plan) && plan_empty(&plan));
    }
    passed = true;

cleanup:
    memcpy(program->instructions, original, sizeof(original));
    return passed;
}

static bool check_boolean_snapshot(void) {
    ASTExpr *value = ast_create_literal_bool(true);
    ASTExpr *root = ast_create_logical_not(value);
    ASTExpr *copy = NULL;
    size_t nodes = 0;
    bool passed = false;
    if (!root) {
        ast_free_expr(value);
        return false;
    }
    copy = hlsl_owned_expression_copy(root, &nodes);
    REQUIRE(copy && nodes == 2 && copy != root && copy->u.unary.sub != value &&
            hlsl_matrix_uses_trees_equal(root, copy));
    value->u.literal.val[0] = 0;
    REQUIRE(!hlsl_matrix_uses_trees_equal(root, copy));
    value->u.literal.val[0] = 1;
    REQUIRE(hlsl_matrix_uses_trees_equal(root, copy));
    passed = true;

cleanup:
    ast_free_expr(copy);
    ast_free_expr(root);
    return passed;
}

typedef struct {
    USILProgram *program;
    SerializedResourceParam *texture;
    HLSLEmitOptions *options;
    HLSLEmitNames *names;
    StringBuilder *source;
    HLSLExpressionSourceMap *map;
    unsigned observations;
    unsigned mutate_at;
    unsigned mutation;
    unsigned statements;
    bool mutated;
} DiscardObserver;

static bool observe_discard(void *context, const HLSLSourceQualityObservation *observation) {
    DiscardObserver *state = context;
    ++state->observations;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_STATEMENT) {
        ++state->statements;
    }
    if (state->observations != state->mutate_at) {
        return true;
    }
    state->mutated = true;
    switch (state->mutation) {
        case 0:
            return false;
        case 1:
            state->program->instructions[1].opcode = USIL_OP_EQ;
            break;
        case 2:
            state->program->instructions[2].condition_test ^= 3;
            break;
        case 3:
            state->texture->bind_index = 1;
            break;
        case 4:
            state->map->count = 0;
            state->map->complete = true;
            break;
        case 5:
            state->options->source_quality_entry_point_index = 1;
            break;
        case 6:
            state->names->entry_point = "changed";
            break;
        case 7:
            state->source->buf[0] = '!';
            break;
        default:
            return false;
    }
    return true;
}

static bool emit_discard(USILProgram *program,
    SerializedResourceParam *texture,
    StringBuilder *source,
    HLSLExpressionSourceMap *map,
    HLSLSourceQualityResult *quality,
    DiscardObserver *observer,
    const char *reserved) {
    SerializedProgramParameters parameters = {.res_count = 1, .resources = texture};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    HLSLEmitNames names = {.entry_point = "frag"};
    HLSLEmitDiagnostic diagnostic;
    options.expression_source_map = map;
    options.source_quality = quality;
    options.reserved_preprocessor_identifiers = reserved ? &reserved : NULL;
    options.reserved_preprocessor_identifier_count = reserved ? 1 : 0;
    if (observer) {
        observer->program = program;
        observer->texture = texture;
        observer->source = source;
        observer->map = map;
        observer->options = &options;
        observer->names = &names;
        options.source_quality_observer = observe_discard;
        options.source_quality_observer_context = observer;
    }
    return hlsl_emit_with_options_diagnostic(
        program, source, &parameters, NULL, &names, &options, &diagnostic);
}

/* Decoded mutations test the lift's contract; only the separate native probe
 * can establish complete-container compiler equality. */
static bool check_variant_emission(USILProgram *program) {
    SerializedResourceParam texture = {.name = "_CoverageTexture",
        .bind_type = SERIALIZED_RESOURCE_TEXTURE,
        .bind_index = 0,
        .sampler_index = 0,
        .dimension = 2,
        .extra = {0, 0}};
    StringBuilder source;
    sb_init(&source);
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    const bool emitted = emit_discard(program, &texture, &source, &map, &quality, NULL, NULL);
    const bool passed = emitted && quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                        !quality.reasons && map.complete && map.count == 5 &&
                        hlsl_expression_source_map_matches(&map, program, source.buf);
    sb_free(&source);
    return passed;
}

static bool check_emission(USILProgram *program) {
    SerializedResourceParam texture = {.name = "_CoverageTexture",
        .bind_type = SERIALIZED_RESOURCE_TEXTURE,
        .bind_index = 0,
        .sampler_index = 0,
        .dimension = 2,
        .extra = {0, 0}};
    StringBuilder baseline;
    sb_init(&baseline);
    StringBuilder changed;
    sb_init(&changed);
    const char *prefix = "// caller prefix\n";
    HLSLExpressionSourceMap map;
    HLSLSourceQualityResult quality;
    DiscardObserver observer = {0};
    const USILInstruction comparison = program->instructions[1];
    const USILInstruction discard = program->instructions[2];
    bool passed = false;
    sb_append(&baseline, prefix);
    REQUIRE(emit_discard(program, &texture, &baseline, &map, &quality, &observer, NULL));
    REQUIRE(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.reasons &&
            quality.counts.ast_statements == 3 && observer.statements == 3 && map.complete &&
            map.count == 5 && hlsl_expression_source_map_matches(&map, program, baseline.buf));
    REQUIRE(strstr(baseline.buf, "const float4 sampledValue = _CoverageTexture.Sample(") &&
            strstr(baseline.buf, "const bool discardFragment = ") &&
            strstr(baseline.buf, "if (discardFragment)\n") && strstr(baseline.buf, "discard;\n") &&
            strstr(baseline.buf, "return sampledValue;"));
    REQUIRE(
        map.origins[2].kind == HLSL_EXPRESSION_ORIGIN_EFFECT && !map.origins[2].destination_lanes);
    map.origins[2].kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
    REQUIRE(!hlsl_expression_source_map_matches(&map, program, baseline.buf));
    map.origins[2].kind = HLSL_EXPRESSION_ORIGIN_EFFECT;
    map.origins[1].kind = HLSL_EXPRESSION_ORIGIN_EFFECT;
    REQUIRE(!hlsl_expression_source_map_matches(&map, program, baseline.buf));

    for (unsigned outputs = 0; outputs < 4; ++outputs) {
        sb_append(&changed, prefix);
        REQUIRE(emit_discard(program,
            &texture,
            &changed,
            outputs & 1 ? &map : NULL,
            outputs & 2 ? &quality : NULL,
            NULL,
            NULL));
        REQUIRE(changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len));
        sb_free(&changed);
        sb_init(&changed);
    }
    program->instructions[2].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
    REQUIRE(emit_discard(program, &texture, &changed, &map, &quality, NULL, NULL));
    REQUIRE(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
            strstr(changed.buf, "if (!(discardFragment))\n") &&
            hlsl_expression_source_map_matches(&map, program, changed.buf));
    program->instructions[2] = discard;
    sb_free(&changed);
    sb_init(&changed);

    static const char *const reserved[] = {"sampledValue", "discardFragment"};
    for (unsigned name = 0; name < 2; ++name) {
        sb_append(&changed, prefix);
        REQUIRE(!emit_discard(program, &texture, &changed, &map, &quality, NULL, reserved[name]));
        REQUIRE(changed.failed && !map.complete && !map.count && !strcmp(changed.buf, prefix));
        sb_free(&changed);
        sb_init(&changed);
    }
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        for (unsigned event = 1; event <= observer.observations; ++event) {
            DiscardObserver drift = {.mutation = mutation, .mutate_at = event};
            sb_append(&changed, prefix);
            const bool emitted =
                emit_discard(program, &texture, &changed, &map, &quality, &drift, NULL);
            program->instructions[1] = comparison;
            program->instructions[2] = discard;
            texture.bind_index = 0;
            if (emitted || !drift.mutated || !changed.failed || map.complete || map.count ||
                quality.classification != HLSL_SOURCE_QUALITY_FAILED ||
                strcmp(changed.buf, prefix)) {
                fprintf(stderr,
                    "Discard callback mutation=%u event=%u emitted=%d mutated=%d failed=%d map=%d/%zu quality=%d prefix=%d\n",
                    mutation,
                    event,
                    emitted,
                    drift.mutated,
                    changed.failed,
                    map.complete,
                    map.count,
                    quality.classification,
                    strcmp(changed.buf, prefix));
            }
            REQUIRE(!emitted && drift.mutated && changed.failed && !map.complete && !map.count &&
                    quality.classification == HLSL_SOURCE_QUALITY_FAILED &&
                    !strcmp(changed.buf, prefix));
            sb_free(&changed);
            sb_init(&changed);
        }
    }
    sb_append(&changed, prefix);
    REQUIRE(emit_discard(program, &texture, &changed, &map, &quality, NULL, NULL));
    REQUIRE(changed.len == baseline.len && !memcmp(changed.buf, baseline.buf, baseline.len));
    passed = true;

cleanup:
    program->instructions[1] = comparison;
    program->instructions[2] = discard;
    sb_free(&changed);
    sb_free(&baseline);
    return passed;
}

static bool check_fixture(const DiscardFixtureCase *fixture) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    DXBCDocument document;
    dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract;
    dxbc_stage_contract_init(&contract);
    USILProgram program = {0};
    bool passed = false;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    char hex[COMMON_SHA256_HEX_SIZE];
    HLSLSampledDiscardPlan plan;
    HLSLSampledFragmentPlan existing;
    REQUIRE(test_fixture_decode_base64(fixture->path, &bytes, &size));
    common_sha256(bytes, size, digest);
    common_sha256_digest_to_hex(digest, hex);
    REQUIRE(strcmp(hex, fixture->digest) == 0);
    REQUIRE(dxbc_document_parse(&document, bytes, size, NULL) &&
            dxbc_document_decode_semantic(&document, &semantic) &&
            dxbc_stage_contract_decode(&document, &semantic, &contract, NULL) &&
            usil_translate_with_stage_contract(&program, &semantic, &contract));
    REQUIRE(program.program_type == DXBC_PROGRAM_TYPE_PIXEL && program.instruction_count == 5 &&
            program.shader_model_major == fixture->model);
    REQUIRE(hlsl_sampled_discard_plan_prepare(&program, &plan) && plan.present &&
            plan.comparison_opcode == fixture->comparison_opcode && plan.color_register == 0 &&
            plan.predicate_register == 1 && plan.color_component == 3 &&
            plan.predicate_component == 0 && plan.literal_first == fixture->literal_first &&
            !plan.discard_on_zero && plan.threshold_bits == UINT32_C(0x3ec00000));
    REQUIRE(hlsl_sampled_fragment_plan_prepare(&program, &existing) && existing.present &&
            existing.discard && !existing.nan_guard);
    REQUIRE(check_variants(&program, fixture->literal_first) &&
            check_boundaries(&program, fixture->literal_first) && check_emission(&program));
    plan = (HLSLSampledDiscardPlan){.present = true, .threshold_bits = UINT32_MAX};
    REQUIRE(!hlsl_sampled_discard_plan_prepare(NULL, &plan) && plan_empty(&plan));
    REQUIRE(!hlsl_sampled_discard_plan_prepare(&program, NULL));
    passed = true;

cleanup:
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&semantic);
    dxbc_document_free(&document);
    mem_free(bytes, size);
    return passed;
}

int main(void) {
    if (!check_boolean_snapshot()) {
        return 1;
    }
    static const DiscardFixtureCase fixtures[] = {
        {DXBC_SAMPLED_DISCARD_FIXTURE_ROOT "/below_sm4.dxbc.b64",
            "fc17a830da91a82a00da2b2321e68f4e6ffc29d9e8cacc5295d7745f4d7fc277",
            false,
            4,
            USIL_OP_LT},
        {DXBC_SAMPLED_DISCARD_FIXTURE_ROOT "/above_sm4.dxbc.b64",
            "c8735a3055755febc54663f936d36edace69241e605dbfb0a2743d6adb204e78",
            true,
            4,
            USIL_OP_LT},
        {DXBC_SAMPLED_DISCARD_FIXTURE_ROOT "/below_sm5.dxbc.b64",
            "5b49a8fabb0930153dc1d0ee954e2236bcc845a0ffef5d4028b2fecf22f9f866",
            false,
            5,
            USIL_OP_LT},
        {DXBC_SAMPLED_DISCARD_FIXTURE_ROOT "/above_sm5.dxbc.b64",
            "5eebc871ffec60fdc6226122b6ea37171682a3669a1c2fd056aa1173251fb95f",
            true,
            5,
            USIL_OP_LT},
        {DXBC_SAMPLED_DISCARD_FIXTURE_ROOT "/negated_below_sm4.dxbc.b64",
            "d09afb9ed5826e3d0bf9563f68829d21b2471da00a1a85a56af52a8f369dbfec",
            false,
            4,
            USIL_OP_GE},
        {DXBC_SAMPLED_DISCARD_FIXTURE_ROOT "/negated_above_sm4.dxbc.b64",
            "845263803fd9b2c422d384bf1b9d8c33a29d7b99ce12f4c9a11893a29bcf88d7",
            true,
            4,
            USIL_OP_GE},
        {DXBC_SAMPLED_DISCARD_FIXTURE_ROOT "/negated_below_sm5.dxbc.b64",
            "72b927e5cff5c4e5b83b3b2d1b88e421cb92d5859f0c743ca6b2be4271aece82",
            false,
            5,
            USIL_OP_GE},
        {DXBC_SAMPLED_DISCARD_FIXTURE_ROOT "/negated_above_sm5.dxbc.b64",
            "d205172f5066e5414a061c3fa2a335bc10ab36203745225529ecf51e327960e4",
            true,
            5,
            USIL_OP_GE}};
    for (size_t index = 0; index < sizeof(fixtures) / sizeof(*fixtures); ++index) {
        if (!check_fixture(&fixtures[index])) {
            return 1;
        }
    }
    return 0;
}
