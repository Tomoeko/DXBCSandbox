// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_emitted_matrix_uses_internal.h"
#include "translation/shaderlab_source_quality_internal.h"
#include "translation/hlsl_emitter_internal.h"
#include "common/sha256.h"

#include <stdlib.h>
#include <string.h>

enum { MATRIX_ENTRY_LIMIT = 32, MATRIX_TREE_LIMIT = 512, MATRIX_TREE_DEPTH = 64,
       MATRIX_INPUT_BYTE_LIMIT = 4 * 1024 * 1024 };

/* Mechanical bounded tree copy shared by matrix and stage observations.
 * It copies syntax and owned origins, never recovers or invents provenance. */
static ASTExpr *copy_tree(const ASTExpr *source, unsigned depth, size_t *nodes) {
    if (!source || depth > MATRIX_TREE_DEPTH || ++*nodes > MATRIX_TREE_LIMIT) return NULL;
    ASTExpr *copy = NULL;
    switch (source->kind) {
    case AST_EXPR_VAR:
        copy = ast_create_var(source->u.var.ssa_var, source->u.var.register_index, source->u.var.operand_type, source->u.var.name);
        break;
    case AST_EXPR_UNARY: {
        ASTExpr *child = copy_tree(source->u.unary.sub, depth + 1, nodes);
        if (child) copy = ast_create_unary(source->u.unary.op, child);
        if (!copy) ast_free_expr(child);
        break;
    }
    case AST_EXPR_BINARY:
    case AST_EXPR_COMPARISON: {
        ASTExpr *left = copy_tree(source->u.binary.left, depth + 1, nodes);
        ASTExpr *right = left ? copy_tree(source->u.binary.right, depth + 1, nodes) : NULL;
        if (left && right) copy = source->kind == AST_EXPR_BINARY
            ? ast_create_binary(source->u.binary.op, left, right)
            : ast_create_comparison(source->u.binary.op, left, right);
        if (!copy) { ast_free_expr(left); ast_free_expr(right); }
        break;
    }
    case AST_EXPR_CAST:
    case AST_EXPR_BITCAST: {
        const ASTExpr *original = source->kind == AST_EXPR_CAST ? source->u.cast.sub : source->u.bitcast.sub;
        ASTExpr *child = copy_tree(original, depth + 1, nodes);
        if (child) copy = source->kind == AST_EXPR_CAST
            ? ast_create_cast(source->u.cast.type_name, child)
            : ast_create_bitcast(source->u.bitcast.scalar_type, child);
        if (!copy) ast_free_expr(child);
        break;
    }
    case AST_EXPR_TERNARY: {
        ASTExpr *condition = copy_tree(source->u.ternary.cond, depth + 1, nodes);
        ASTExpr *yes = condition ? copy_tree(source->u.ternary.true_expr, depth + 1, nodes) : NULL;
        ASTExpr *no = yes ? copy_tree(source->u.ternary.false_expr, depth + 1, nodes) : NULL;
        if (condition && yes && no) copy = ast_create_ternary(condition, yes, no);
        if (!copy) { ast_free_expr(condition); ast_free_expr(yes); ast_free_expr(no); }
        break;
    }
    case AST_EXPR_LITERAL:
        copy = ast_create_literal_bits(source->u.literal.val, source->u.literal.components,
                                       source->u.literal.scalar_type);
        break;
    case AST_EXPR_EMITTER_OPERAND:
        copy = ast_create_emitter_operand_with_provenance(source->u.emitter_operand,
                                                         &source->operand_provenance);
        break;
    case AST_EXPR_SWIZZLE: {
        ASTExpr *child = copy_tree(source->u.swizzle.sub, depth + 1, nodes);
        if (child) copy = ast_create_swizzle(child, source->u.swizzle.swizzle,
                                            source->u.swizzle.swizzle_count);
        if (!copy) ast_free_expr(child);
        break;
    }
    case AST_EXPR_CALL: {
        if (source->u.call.arg_count < 1 || source->u.call.arg_count > 4 || !source->u.call.args) break;
        ASTExpr *arguments[4] = {0};
        int count = 0;
        for (; count < source->u.call.arg_count; ++count) {
            arguments[count] = copy_tree(source->u.call.args[count], depth + 1, nodes);
            if (!arguments[count]) break;
        }
        if (count == source->u.call.arg_count)
            copy = ast_create_call(source->u.call.name, arguments, count);
        if (!copy) for (int index = 0; index < count; ++index) ast_free_expr(arguments[index]);
        break;
    }
    default:
        break;
    }
    if (copy) copy->logical_origin = source->logical_origin;
    return copy;
}

ASTExpr *hlsl_owned_expression_copy(const ASTExpr *source, size_t *node_count) {
    if (!node_count) return NULL;
    *node_count = 0;
    return copy_tree(source, 0, node_count);
}

static void hash_number(CommonSha256Context *hash, uint64_t number) {
    uint8_t bytes[8];
    for (unsigned index = 0; index < 8; ++index) bytes[index] = (uint8_t)(number >> (index * 8));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

static bool tree_digest_node(CommonSha256Context *hash, const ASTExpr *tree,
                             unsigned depth, size_t *nodes) {
    if (!tree || depth > MATRIX_TREE_DEPTH || ++*nodes > MATRIX_TREE_LIMIT) return false;
    hash_number(hash, tree->kind);
    const ASTLogicalValueOrigin *logical = &tree->logical_origin;
    hash_number(hash, logical->complete); hash_number(hash, logical->scalar_type);
    hash_number(hash, logical->components); hash_number(hash, logical->logical_value_id);
    hash_number(hash, (uint64_t)logical->instruction_index);
    hash_number(hash, logical->source_instruction_index); hash_number(hash, logical->destination_lanes);
    hash_number(hash, logical->semantic_projection); hash_number(hash, logical->program_bitcast);
    if (tree->kind == AST_EXPR_EMITTER_OPERAND) {
        const ASTOperandProvenance *origin = &tree->operand_provenance;
        hash_number(hash, origin->complete); hash_number(hash, origin->value_role);
        hash_number(hash, origin->logical_value_id); hash_number(hash, origin->natural_components);
        hash_number(hash, origin->result_components); hash_number(hash, origin->selection_role);
        for (unsigned lane = 0; lane < 4; ++lane) hash_number(hash, origin->selected_components[lane]);
        hash_number(hash, origin->bitcast_role); hash_number(hash, origin->raw_buffer_reconstruction);
        hash_number(hash, origin->synthetic_interface); hash_number(hash, (uint64_t)origin->instruction_index);
        hash_number(hash, origin->source_instruction_index); hash_number(hash, (uint64_t)origin->operand_index);
        hash_number(hash, origin->destination_lanes);
        if (!tree->u.emitter_operand) return false;
        size_t size = strlen(tree->u.emitter_operand);
        hash_number(hash, size); common_sha256_update(hash, tree->u.emitter_operand, size);
        return true;
    }
    if (tree->kind == AST_EXPR_LITERAL) {
        hash_number(hash, tree->u.literal.components); hash_number(hash, tree->u.literal.scalar_type);
        for (unsigned lane = 0; lane < 4; ++lane) hash_number(hash, tree->u.literal.val[lane]);
        return true;
    }
    if (tree->kind == AST_EXPR_SWIZZLE) {
        hash_number(hash, tree->u.swizzle.swizzle_count);
        for (int lane = 0; lane < tree->u.swizzle.swizzle_count; ++lane)
            hash_number(hash, (uint64_t)tree->u.swizzle.swizzle[lane]);
        return tree_digest_node(hash, tree->u.swizzle.sub, depth + 1, nodes);
    }
    if (tree->kind == AST_EXPR_CALL && tree->u.call.name && tree->u.call.args &&
        tree->u.call.arg_count > 0 && tree->u.call.arg_count <= 4) {
        size_t size = strlen(tree->u.call.name);
        hash_number(hash, size); common_sha256_update(hash, tree->u.call.name, size);
        hash_number(hash, tree->u.call.arg_count);
        for (int argument = 0; argument < tree->u.call.arg_count; ++argument)
            if (!tree_digest_node(hash, tree->u.call.args[argument], depth + 1, nodes)) return false;
        return true;
    }
    return false;
}

static bool tree_digest(const ASTExpr *tree, uint8_t digest[32]) {
    CommonSha256Context hash; common_sha256_init(&hash);
    size_t nodes = 0;
    if (!tree_digest_node(&hash, tree, 0, &nodes)) return false;
    common_sha256_final(&hash, digest);
    return true;
}

static bool logical_origins_equal(const ASTLogicalValueOrigin *a, const ASTLogicalValueOrigin *b) {
    return a->complete == b->complete && a->scalar_type == b->scalar_type &&
        a->components == b->components && a->logical_value_id == b->logical_value_id &&
        a->instruction_index == b->instruction_index &&
        a->source_instruction_index == b->source_instruction_index &&
        a->destination_lanes == b->destination_lanes &&
        a->semantic_projection == b->semantic_projection && a->program_bitcast == b->program_bitcast;
}

static bool operand_origins_equal(const ASTOperandProvenance *a, const ASTOperandProvenance *b) {
    if (a->complete != b->complete || a->value_role != b->value_role ||
        a->logical_value_id != b->logical_value_id || a->natural_components != b->natural_components ||
        a->result_components != b->result_components || a->selection_role != b->selection_role ||
        a->bitcast_role != b->bitcast_role || a->raw_buffer_reconstruction != b->raw_buffer_reconstruction ||
        a->synthetic_interface != b->synthetic_interface || a->instruction_index != b->instruction_index ||
        a->source_instruction_index != b->source_instruction_index || a->operand_index != b->operand_index ||
        a->destination_lanes != b->destination_lanes) return false;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (a->selected_components[lane] != b->selected_components[lane]) return false;
    return true;
}

static bool trees_equal(const ASTExpr *a, const ASTExpr *b, unsigned depth, size_t *nodes) {
    if (!a || !b || depth > MATRIX_TREE_DEPTH || ++*nodes > MATRIX_TREE_LIMIT ||
        a->kind != b->kind || !logical_origins_equal(&a->logical_origin, &b->logical_origin)) return false;
    if (a->kind == AST_EXPR_VAR)
        return a->u.var.ssa_var == b->u.var.ssa_var && a->u.var.register_index == b->u.var.register_index &&
            a->u.var.operand_type == b->u.var.operand_type && a->u.var.name && b->u.var.name && !strcmp(a->u.var.name, b->u.var.name);
    if (a->kind == AST_EXPR_UNARY)
        return a->u.unary.op == b->u.unary.op && trees_equal(a->u.unary.sub, b->u.unary.sub, depth + 1, nodes);
    if (a->kind == AST_EXPR_BINARY || a->kind == AST_EXPR_COMPARISON)
        return a->u.binary.op == b->u.binary.op &&
            trees_equal(a->u.binary.left, b->u.binary.left, depth + 1, nodes) &&
            trees_equal(a->u.binary.right, b->u.binary.right, depth + 1, nodes);
    if (a->kind == AST_EXPR_CAST)
        return a->u.cast.type_name && b->u.cast.type_name && !strcmp(a->u.cast.type_name, b->u.cast.type_name) &&
            trees_equal(a->u.cast.sub, b->u.cast.sub, depth + 1, nodes);
    if (a->kind == AST_EXPR_BITCAST)
        return a->u.bitcast.scalar_type == b->u.bitcast.scalar_type &&
            trees_equal(a->u.bitcast.sub, b->u.bitcast.sub, depth + 1, nodes);
    if (a->kind == AST_EXPR_TERNARY)
        return trees_equal(a->u.ternary.cond, b->u.ternary.cond, depth + 1, nodes) &&
            trees_equal(a->u.ternary.true_expr, b->u.ternary.true_expr, depth + 1, nodes) &&
            trees_equal(a->u.ternary.false_expr, b->u.ternary.false_expr, depth + 1, nodes);
    if (a->kind == AST_EXPR_EMITTER_OPERAND)
        return a->u.emitter_operand && b->u.emitter_operand &&
            !strcmp(a->u.emitter_operand, b->u.emitter_operand) &&
            operand_origins_equal(&a->operand_provenance, &b->operand_provenance);
    if (a->kind == AST_EXPR_LITERAL) {
        if (a->u.literal.components != b->u.literal.components ||
            a->u.literal.scalar_type != b->u.literal.scalar_type) return false;
        for (unsigned lane = 0; lane < 4; ++lane)
            if (a->u.literal.val[lane] != b->u.literal.val[lane]) return false;
        return true;
    }
    if (a->kind == AST_EXPR_SWIZZLE) {
        if (a->u.swizzle.swizzle_count < 1 || a->u.swizzle.swizzle_count > 4 ||
            a->u.swizzle.swizzle_count != b->u.swizzle.swizzle_count) return false;
        for (int lane = 0; lane < a->u.swizzle.swizzle_count; ++lane)
            if (a->u.swizzle.swizzle[lane] != b->u.swizzle.swizzle[lane]) return false;
        return trees_equal(a->u.swizzle.sub, b->u.swizzle.sub, depth + 1, nodes);
    }
    if (a->kind != AST_EXPR_CALL || !a->u.call.name || !b->u.call.name ||
        !a->u.call.args || !b->u.call.args || a->u.call.arg_count < 1 || a->u.call.arg_count > 4 ||
        a->u.call.arg_count != b->u.call.arg_count || strcmp(a->u.call.name, b->u.call.name)) return false;
    for (int argument = 0; argument < a->u.call.arg_count; ++argument)
        if (!trees_equal(a->u.call.args[argument], b->u.call.args[argument], depth + 1, nodes)) return false;
    return true;
}

bool hlsl_matrix_uses_trees_equal(const ASTExpr *left, const ASTExpr *right) {
    size_t nodes = 0;
    return trees_equal(left, right, 0, &nodes);
}

/* Field lookup is bounded independently, even though capture accepts only the
 * mechanically copied, already validated matrix planner AST forms. */
static bool tree_has_field_bounded(const ASTExpr *tree, const HLSLCurrentMatrixField *field,
    unsigned depth, size_t *nodes) {
    if (!tree || !field || depth > MATRIX_TREE_DEPTH || ++*nodes > MATRIX_TREE_LIMIT) return false;
    if (tree->kind == AST_EXPR_EMITTER_OPERAND) {
        const ASTOperandProvenance *origin = &tree->operand_provenance;
        return tree->u.emitter_operand && origin->complete && origin->value_role == AST_OPERAND_VALUE_LOGICAL &&
            !origin->natural_components && !origin->result_components &&
            origin->logical_value_id == field->logical_aggregate_id &&
            !strcmp(tree->u.emitter_operand, field->field_name);
    }
    if (tree->kind == AST_EXPR_SWIZZLE)
        return tree_has_field_bounded(tree->u.swizzle.sub, field, depth + 1, nodes);
    if (tree->kind == AST_EXPR_CALL && tree->u.call.args && tree->u.call.arg_count > 0 && tree->u.call.arg_count <= 4)
        for (int index = 0; index < tree->u.call.arg_count; ++index)
            if (tree_has_field_bounded(tree->u.call.args[index], field, depth + 1, nodes)) return true;
    return false;
}

static bool tree_has_field(const ASTExpr *tree, const HLSLCurrentMatrixField *field) {
    size_t nodes = 0;
    return tree_has_field_bounded(tree, field, 0, &nodes);
}

static void entry_dispose(HLSLMatrixUseCapture *entry) {
    hlsl_stage_coverage_dispose(&entry->coverage);
    for (size_t index = 0; index < entry->use_count; ++index) ast_free_expr(entry->uses[index].tree);
    free(entry->uses); free(entry->target);
    subprogram_metadata_free_variant(&entry->player); free(entry->player_payload);
    serialized_program_parameters_free(&entry->current);
    serialized_program_parameters_free(&entry->common);
    hlsl_current_matrix_reads_dispose(&entry->reads);
    memset(entry, 0, sizeof(*entry));
}

void shaderlab_emitted_matrix_uses_free(ShaderLabEmittedMatrixUses *owned) {
    if (!owned) return;
    for (size_t index = 0; index < owned->entry_count; ++index) entry_dispose(&owned->entries[index]);
    free(owned->entries);
    shaderlab_source_quality_inventory_dispose(&owned->inventory);
    sb_free(&owned->source);
    free(owned);
}

bool shaderlab_matrix_uses_begin(ShaderLabEmittedMatrixUses *owned,
    const ShaderLabExpressionSourceRecord *record, const USILProgram *program,
    const uint8_t *target, size_t target_size, const uint8_t *payload, size_t payload_size,
    const SerializedProgramParameters *current, const SerializedProgramParameters *common,
    HLSLMatrixUseCapture **capture) {
    if (!owned || owned->sealed || !record || !program || !target || !target_size ||
        !payload || !payload_size || owned->entry_count == MATRIX_ENTRY_LIMIT || !capture ||
        target_size > MATRIX_INPUT_BYTE_LIMIT || payload_size > MATRIX_INPUT_BYTE_LIMIT - target_size ||
        owned->owned_input_bytes > MATRIX_INPUT_BYTE_LIMIT - target_size - payload_size) return false;
    owned->owned_input_bytes += target_size + payload_size;
    HLSLMatrixUseCapture *entry = &owned->entries[owned->entry_count++];
    entry->observation = (ShaderLabEmittedMatrixEntry){
        .subshader_index = record->subshader_index, .pass_index = record->pass_index,
        .stage_index = record->stage_index, .subprogram_index = record->subprogram_index,
        .blob_index = record->blob_index, .hardware_tier_group = record->hardware_tier_group,
        .serialized_state = record->serialized_state, .entry_record_index = owned->entry_count - 1};
    entry->target = malloc(target_size); entry->player_payload = malloc(payload_size);
    if (!entry->target || !entry->player_payload) return false;
    entry->target_size = target_size; memcpy(entry->target, target, target_size);
    entry->player_payload_size = payload_size; memcpy(entry->player_payload, payload, payload_size);
    common_sha256(target, target_size, entry->observation.target_digest);
    ByteStream stream; stream_init(&stream, entry->player_payload, payload_size);
    stream_set_endian(&stream, false);
    if (!subprogram_metadata_parse_variant(&stream, &entry->player) ||
        (current && !serialized_program_parameters_copy(&entry->current, current)) ||
        (common && !serialized_program_parameters_copy(&entry->common, common))) return false;
    HLSLCurrentMatrixStatus status = hlsl_current_matrix_reads_build(program, current, common, &entry->reads);
    if (status != HLSL_CURRENT_MATRIX_OK && status != HLSL_CURRENT_MATRIX_NOT_APPLICABLE) return false;
    entry->coverage.global_node_count = &owned->owned_stage_node_count;
    entry->coverage.global_event_count = &owned->owned_stage_event_count;
    entry->observation.field_count = entry->reads.field_count;
    entry->observation.read_count = entry->reads.read_count;
    *capture = entry;
    return true;
}

bool hlsl_matrix_uses_plan(HLSLEmitterContext *ctx, const HLSLMatrixLiftPlan *plan) {
    if (!ctx->matrix_use_capture) return true;
    HLSLMatrixUseCapture *entry = ctx->matrix_use_capture;
    if (!plan || !plan->expression || entry->finished || entry->use_count >= HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT)
        return false;
    HLSLEmittedMatrixUse *uses = realloc(entry->uses, (entry->use_count + 1) * sizeof(*uses));
    if (!uses) return false;
    entry->uses = uses;
    HLSLEmittedMatrixUse *use = &uses[entry->use_count];
    memset(use, 0, sizeof(*use));
    size_t nodes = 0;
    use->tree = copy_tree(plan->expression, 0, &nodes);
    if (!use->tree) return false;
    use->live_tree = plan->expression; use->owners = plan->instruction_owners;
    use->observation = (ShaderLabEmittedMatrixUse){.final_instruction = plan->end_instruction,
        .result_components = plan->result_components};
    for (int instruction = 0; instruction < ctx->program->instruction_count; ++instruction)
        if (hlsl_instruction_owners_contains(&use->owners, instruction))
            use->observation.instructions[use->observation.instruction_count++] = (uint16_t)instruction;
    if (!tree_digest(use->tree, use->observation.expression_digest)) {
        ast_free_expr(use->tree); memset(use, 0, sizeof(*use)); return false;
    }
    ++entry->use_count;
    return true;
}

bool hlsl_matrix_uses_span(HLSLMatrixUseCapture *entry,
    const ASTExpr *expression, size_t begin, size_t end) {
    if (!entry) return true;
    if (!hlsl_stage_coverage_span(&entry->coverage, expression, begin, end)) return false;
    for (size_t index = 0; index < entry->use_count; ++index) {
        HLSLEmittedMatrixUse *use = &entry->uses[index];
        if (use->live_tree != expression) continue;
        uint8_t digest[32];
        if (use->emitted || begin >= end || !tree_digest(expression, digest) ||
            memcmp(digest, use->observation.expression_digest, 32)) return false;
        use->raw_begin = begin; use->raw_end = end; use->emitted = true;
        use->live_tree = NULL;
    }
    return true;
}

bool shaderlab_matrix_uses_finish(HLSLMatrixUseCapture *entry,
    const StringBuilder *source, const HLSLExpressionSourceMap *map) {
    if (!entry || entry->finished || !source || !sb_ok(source) ||
        !hlsl_stage_coverage_validate(&entry->coverage, source) || !map || !map->complete ||
        map->count > HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT) return false;
    entry->raw_map = *map;
    for (size_t index = 0; index < entry->use_count; ++index) {
        HLSLEmittedMatrixUse *use = &entry->uses[index];
        const int final = use->observation.final_instruction;
        if (!use->emitted || final < 0 || (size_t)final >= map->count ||
            !use->observation.instruction_count || use->raw_end > source->len) return false;
        const HLSLExpressionOrigin *origin = &map->origins[final];
        if (origin->kind != HLSL_EXPRESSION_ORIGIN_EXPRESSION ||
            origin->source_begin != use->raw_begin || origin->source_end != use->raw_end) return false;
        StringBuilder formatted;
        sb_init(&formatted);
        ast_format_expr(use->tree, &formatted);
        bool matches = sb_ok(&formatted) && formatted.len == use->raw_end - use->raw_begin &&
            !memcmp(formatted.buf, source->buf + use->raw_begin, formatted.len);
        sb_free(&formatted);
        if (!matches) return false;
    }
    /* Every retained current matrix read is owned by exactly one emitted plan.
     * A matching aggregate name alone cannot replace instruction ownership. */
    for (size_t index = 0; index < entry->reads.read_count; ++index) {
        const HLSLCurrentMatrixRead *read = &entry->reads.reads[index];
        if (read->field_index >= entry->reads.field_count) return false;
        size_t owners = 0;
        for (size_t use_index = 0; use_index < entry->use_count; ++use_index) {
            HLSLEmittedMatrixUse *use = &entry->uses[use_index];
            if (!hlsl_instruction_owners_contains(&use->owners, read->instruction_index)) continue;
            if (!tree_has_field(use->tree, &entry->reads.fields[read->field_index])) return false;
            ++owners;
            ++use->observation.read_count;
        }
        if (owners != 1) return false;
    }
    for (size_t index = 0; index < entry->use_count; ++index)
        if (!entry->uses[index].observation.read_count) return false;
    entry->observation.use_count = entry->use_count;
    entry->finished = true;
    return true;
}

static bool entry_coordinates_match(const ShaderLabEmittedMatrixEntry *entry,
    const ShaderLabExpressionSourceRecord *record) {
    return entry->subshader_index == record->subshader_index &&
        entry->pass_index == record->pass_index && entry->stage_index == record->stage_index &&
        entry->subprogram_index == record->subprogram_index && entry->blob_index == record->blob_index &&
        entry->hardware_tier_group == record->hardware_tier_group &&
        entry->serialized_state == record->serialized_state &&
        !memcmp(entry->target_digest, record->target_digest, 32);
}

bool shaderlab_matrix_uses_seal(ShaderLabEmittedMatrixUses *owned) {
    if (!owned || owned->sealed || !owned->inventory.complete || !sb_ok(&owned->source) ||
        owned->inventory.entries.count != owned->entry_count) return false;
    for (size_t index = 0; index < owned->entry_count; ++index) {
        HLSLMatrixUseCapture *entry = &owned->entries[index];
        const ShaderLabExpressionSourceRecord *record = &owned->inventory.entries.records[index];
        if (!entry->finished || !entry_coordinates_match(&entry->observation, record) ||
            entry->raw_map.count != record->instructions.count || !record->has_source_quality) return false;
        const ShaderLabSourceSyntaxReceipt *body = NULL;
        for (size_t receipt = 0; receipt < owned->inventory.receipt_count; ++receipt) {
            const ShaderLabSourceSyntaxReceipt *candidate = &owned->inventory.receipts[receipt];
            if (candidate->kind != SHADERLAB_SOURCE_SYNTAX_LINKED_ENTRY ||
                candidate->entry_record_index != index) continue;
            if (body) return false;
            body = candidate;
        }
        if (!body || body->source_begin >= body->source_end || body->source_end > owned->source.len ||
            body->stage_index != entry->observation.stage_index ||
            body->pass_index != entry->observation.pass_index ||
            body->subshader_index != entry->observation.subshader_index) return false;
        entry->observation.source_begin = body->source_begin;
        entry->observation.source_end = body->source_end;
        memcpy(entry->observation.body_digest, body->source_digest, 32);
        entry->observation.base_quality = record->source_quality;
        for (size_t root_index = 0; root_index < entry->coverage.root_count; ++root_index) {
            HLSLStageOwnedRoot *root = &entry->coverage.roots[root_index];
            if (root->instruction < 0 || (size_t)root->instruction >= record->instructions.count) return false;
            const HLSLExpressionOrigin *raw = &entry->raw_map.origins[root->instruction];
            const HLSLExpressionOrigin *whole = &record->instructions.origins[root->instruction];
            if (raw->kind != HLSL_EXPRESSION_ORIGIN_EXPRESSION || raw->source_begin != root->begin ||
                raw->source_end != root->end || whole->kind != HLSL_EXPRESSION_ORIGIN_EXPRESSION ||
                whole->source_begin < body->source_begin || whole->source_end > body->source_end ||
                whole->source_end - whole->source_begin != root->end - root->begin ||
                memcmp(owned->source.buf + whole->source_begin,
                    entry->coverage.source + root->begin, root->end - root->begin)) return false;
            root->whole_begin = whole->source_begin;
            root->whole_end = whole->source_end;
        }
        for (size_t use_index = 0; use_index < entry->use_count; ++use_index) {
            HLSLEmittedMatrixUse *use = &entry->uses[use_index];
            const HLSLExpressionOrigin *origin = &record->instructions.origins[use->observation.final_instruction];
            if (origin->kind != HLSL_EXPRESSION_ORIGIN_EXPRESSION ||
                origin->source_begin < body->source_begin || origin->source_end > body->source_end ||
                origin->source_begin >= origin->source_end) return false;
            StringBuilder formatted;
            sb_init(&formatted);
            ast_format_expr(use->tree, &formatted);
            bool matches = sb_ok(&formatted) && formatted.len == origin->source_end - origin->source_begin &&
                !memcmp(formatted.buf, owned->source.buf + origin->source_begin, formatted.len);
            sb_free(&formatted);
            if (!matches) return false;
            use->observation.entry_index = index;
            use->observation.source_begin = origin->source_begin;
            use->observation.source_end = origin->source_end;
        }
    }
    owned->sealed = true;
    return true;
}

static bool initial_scope(const ShaderLabSourceQualityRequest *request) {
    if (!request || !request->shader || !request->archive) return false;
    const SerializedShader *shader = request->shader;
    if (shader->subshader_count != 1 || !shader->subshaders ||
        shader->subshaders[0].pass_count != 1 || !shader->subshaders[0].passes) return false;
    const SerializedPass *pass = &shader->subshaders[0].passes[0];
    if (!pass->subprogram_count[0] || !pass->subprogram_count[1]) return false;
    for (int stage = 2; stage < 6; ++stage) if (pass->subprogram_count[stage]) return false;
    return true;
}

ShaderLabMatrixUsesStatus shaderlab_emitted_matrix_uses_capture(
    const ShaderLabSourceQualityRequest *request, ShaderLabEmittedMatrixUses **output) {
    if (!output || *output || !request || !request->shader || !request->archive)
        return SHADERLAB_MATRIX_USES_INVALID_ARGUMENT;
    if (!initial_scope(request)) return SHADERLAB_MATRIX_USES_SCOPE_UNAVAILABLE;
    ShaderLabEmittedMatrixUses *owned = calloc(1, sizeof(*owned));
    if (!owned) return SHADERLAB_MATRIX_USES_ALLOCATION_FAILED;
    sb_init(&owned->source);
    owned->entries = calloc(MATRIX_ENTRY_LIMIT, sizeof(*owned->entries));
    if (!owned->entries) {
        shaderlab_emitted_matrix_uses_free(owned);
        return SHADERLAB_MATRIX_USES_ALLOCATION_FAILED;
    }
    ShaderLabSourceQualityStatus status = shaderlab_source_quality_emit_with_matrix_capture(
        request, &owned->source, &owned->inventory, owned, NULL);
    if (status != SHADERLAB_SOURCE_QUALITY_OK) {
        shaderlab_emitted_matrix_uses_free(owned);
        return status == SHADERLAB_SOURCE_QUALITY_SCOPE_UNAVAILABLE ?
            SHADERLAB_MATRIX_USES_SCOPE_UNAVAILABLE : SHADERLAB_MATRIX_USES_EMISSION_FAILED;
    }
    size_t uses = 0;
    for (size_t index = 0; index < owned->entry_count; ++index) uses += owned->entries[index].use_count;
    if (!uses || !shaderlab_matrix_uses_seal(owned)) {
        shaderlab_emitted_matrix_uses_free(owned);
        return uses ? SHADERLAB_MATRIX_USES_CAPTURE_FAILED : SHADERLAB_MATRIX_USES_NOT_APPLICABLE;
    }
    *output = owned;
    return SHADERLAB_MATRIX_USES_OK;
}

static bool fields_equal(const HLSLCurrentMatrixField *a, const HLSLCurrentMatrixField *b) {
    return !strcmp(a->block_name, b->block_name) && !strcmp(a->field_name, b->field_name) &&
        a->binding_register == b->binding_register && a->declared_byte_size == b->declared_byte_size &&
        a->reflected_byte_size == b->reflected_byte_size && a->field_byte_offset == b->field_byte_offset &&
        a->field_byte_size == b->field_byte_size && a->logical_aggregate_id == b->logical_aggregate_id &&
        a->field_authority == b->field_authority && a->binding_authority == b->binding_authority &&
        a->full_shell_authorities == b->full_shell_authorities &&
        a->metadata_buffer_index == b->metadata_buffer_index &&
        a->metadata_field_index == b->metadata_field_index &&
        a->metadata_binding_index == b->metadata_binding_index && a->row_major == b->row_major;
}

static bool reads_equal(const HLSLCurrentMatrixRead *a, const HLSLCurrentMatrixRead *b) {
    return a->field_index == b->field_index && a->instruction_index == b->instruction_index &&
        a->operand_index == b->operand_index && a->source_instruction_index == b->source_instruction_index &&
        a->destination_lanes == b->destination_lanes && a->logical_lane == b->logical_lane &&
        a->physical_lane == b->physical_lane && a->physical_row == b->physical_row &&
        a->byte_offset == b->byte_offset && a->field_relative_byte_offset == b->field_relative_byte_offset;
}

bool hlsl_matrix_uses_field_matches(const HLSLMatrixUseCapture *entry, size_t index,
    const HLSLCurrentMatrixField *field) {
    return entry && field && index < entry->reads.field_count &&
        fields_equal(&entry->reads.fields[index], field);
}

bool hlsl_matrix_uses_read_matches(const HLSLMatrixUseCapture *entry, size_t index,
    const HLSLCurrentMatrixRead *read) {
    return entry && read && index < entry->reads.read_count &&
        reads_equal(&entry->reads.reads[index], read);
}

static bool observations_equal(const ShaderLabEmittedMatrixUses *a, const ShaderLabEmittedMatrixUses *b) {
    if (!a->sealed || !b->sealed || a->source.len != b->source.len || a->entry_count != b->entry_count ||
        a->owned_input_bytes != b->owned_input_bytes || a->owned_stage_node_count != b->owned_stage_node_count ||
        a->owned_stage_event_count != b->owned_stage_event_count ||
        memcmp(a->source.buf, b->source.buf, a->source.len)) return false;
    for (size_t index = 0; index < a->entry_count; ++index) {
        const HLSLMatrixUseCapture *left = &a->entries[index], *right = &b->entries[index];
        if (!left->finished || !entry_coordinates_match(&left->observation, &b->inventory.entries.records[index]) ||
            left->observation.source_begin != right->observation.source_begin ||
            left->observation.source_end != right->observation.source_end ||
            memcmp(left->observation.body_digest, right->observation.body_digest, 32) ||
            left->target_size != right->target_size || left->player_payload_size != right->player_payload_size ||
            memcmp(left->target, right->target, left->target_size) ||
            memcmp(left->player_payload, right->player_payload, left->player_payload_size) ||
            !subprogram_metadata_variant_equal(&left->player, &right->player) ||
            !serialized_program_parameters_equal(&left->current, &right->current) ||
            !serialized_program_parameters_equal(&left->common, &right->common) ||
            left->reads.field_count != right->reads.field_count || left->reads.read_count != right->reads.read_count ||
            left->observation.field_count != right->observation.field_count ||
            left->observation.read_count != right->observation.read_count ||
            left->observation.use_count != right->observation.use_count ||
            !hlsl_stage_coverage_equal(&left->coverage, &right->coverage) ||
            left->use_count != right->use_count || left->raw_map.count != right->raw_map.count ||
            !left->raw_map.complete || !right->raw_map.complete) return false;
        for (size_t instruction = 0; instruction < left->raw_map.count; ++instruction) {
            const HLSLExpressionOrigin *x = &left->raw_map.origins[instruction];
            const HLSLExpressionOrigin *y = &right->raw_map.origins[instruction];
            if (x->kind != y->kind || x->instruction_index != y->instruction_index ||
                x->source_instruction_index != y->source_instruction_index ||
                x->destination_lanes != y->destination_lanes ||
                x->source_begin != y->source_begin || x->source_end != y->source_end ||
                x->definition_begin != y->definition_begin || x->definition_end != y->definition_end) return false;
        }
        for (size_t field = 0; field < left->reads.field_count; ++field)
            if (!fields_equal(&left->reads.fields[field], &right->reads.fields[field])) return false;
        for (size_t read = 0; read < left->reads.read_count; ++read)
            if (!reads_equal(&left->reads.reads[read], &right->reads.reads[read])) return false;
        for (size_t use_index = 0; use_index < left->use_count; ++use_index) {
            const HLSLEmittedMatrixUse *x = &left->uses[use_index], *y = &right->uses[use_index];
            uint8_t digest[32];
            if (!x->emitted || x->live_tree || !hlsl_matrix_uses_trees_equal(x->tree, y->tree) ||
                !tree_digest(x->tree, digest) ||
                memcmp(digest, x->observation.expression_digest, 32) ||
                memcmp(digest, y->observation.expression_digest, 32) ||
                x->raw_begin != y->raw_begin || x->raw_end != y->raw_end ||
                x->observation.entry_index != y->observation.entry_index ||
                x->observation.source_begin != y->observation.source_begin ||
                x->observation.source_end != y->observation.source_end ||
                x->observation.final_instruction != y->observation.final_instruction ||
                x->observation.result_components != y->observation.result_components ||
                x->observation.read_count != y->observation.read_count ||
                x->observation.instruction_count != y->observation.instruction_count ||
                memcmp(x->observation.instructions, y->observation.instructions,
                    x->observation.instruction_count * sizeof(x->observation.instructions[0]))) return false;
            for (int instruction = 0; instruction < HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT; ++instruction)
                if (hlsl_instruction_owners_contains(&x->owners, instruction) !=
                    hlsl_instruction_owners_contains(&y->owners, instruction)) return false;
        }
    }
    return true;
}

bool shaderlab_emitted_matrix_uses_replay(
    const ShaderLabSourceQualityRequest *request, const ShaderLabEmittedMatrixUses *owned) {
    if (!owned || !owned->sealed) return false;
    ShaderLabSourceQualityResult base;
    if (shaderlab_source_quality_inventory_analyze(request, &owned->source, &owned->inventory, &base, NULL) !=
        SHADERLAB_SOURCE_QUALITY_OK) return false;
    ShaderLabEmittedMatrixUses *current = NULL;
    if (shaderlab_emitted_matrix_uses_capture(request, &current) != SHADERLAB_MATRIX_USES_OK) return false;
    bool equal = observations_equal(owned, current);
    shaderlab_emitted_matrix_uses_free(current);
    return equal;
}

bool shaderlab_emitted_matrix_uses_describe(
    const ShaderLabEmittedMatrixUses *owned, ShaderLabEmittedMatrixSummary *summary) {
    if (!owned || !owned->sealed || !summary) return false;
    ShaderLabEmittedMatrixSummary result = {.source_size = owned->source.len,
        .entry_count = owned->entry_count, .base_quality = owned->inventory.quality};
    memcpy(result.source_digest, owned->inventory.source_digest, 32);
    memcpy(result.modeled_input_digest, owned->inventory.modeled_input_digest, 32);
    for (size_t index = 0; index < owned->entry_count; ++index) {
        result.use_count += owned->entries[index].use_count;
        result.field_count += owned->entries[index].reads.field_count;
        result.read_count += owned->entries[index].reads.read_count;
    }
    *summary = result;
    return true;
}

bool shaderlab_emitted_matrix_uses_entry(
    const ShaderLabEmittedMatrixUses *owned, size_t index, ShaderLabEmittedMatrixEntry *entry) {
    if (!owned || !owned->sealed || !entry || index >= owned->entry_count) return false;
    *entry = owned->entries[index].observation;
    entry->base_quality = owned->inventory.entries.records[index].source_quality;
    return true;
}

bool shaderlab_emitted_matrix_uses_use(const ShaderLabEmittedMatrixUses *owned, size_t entry,
    size_t index, ShaderLabEmittedMatrixUse *use) {
    if (!owned || !owned->sealed || !use || entry >= owned->entry_count ||
        index >= owned->entries[entry].use_count) return false;
    *use = owned->entries[entry].uses[index].observation;
    return true;
}

bool shaderlab_emitted_matrix_uses_field(const ShaderLabEmittedMatrixUses *owned, size_t entry,
    size_t index, HLSLCurrentMatrixField *field) {
    if (!owned || !owned->sealed || !field || entry >= owned->entry_count ||
        index >= owned->entries[entry].reads.field_count) return false;
    *field = owned->entries[entry].reads.fields[index];
    return true;
}

bool shaderlab_emitted_matrix_uses_read(const ShaderLabEmittedMatrixUses *owned, size_t entry,
    size_t index, HLSLCurrentMatrixRead *read) {
    if (!owned || !owned->sealed || !read || entry >= owned->entry_count ||
        index >= owned->entries[entry].reads.read_count) return false;
    *read = owned->entries[entry].reads.reads[index];
    return true;
}

bool shaderlab_emitted_matrix_uses_source(const ShaderLabEmittedMatrixUses *owned,
    const char **source, size_t *size) {
    if (!owned || !owned->sealed || !source || !size) return false;
    *source = owned->source.buf;
    *size = owned->source.len;
    return true;
}
