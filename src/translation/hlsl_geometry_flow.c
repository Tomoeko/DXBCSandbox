// SPDX-License-Identifier: GPL-3.0-only
/* Bounded typed geometry control-flow reconstruction from decoded ownership. */
#include "hlsl_source_identifier.h"
#include "hlsl_geometry_flow.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil_validation.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FLOW_LIMIT = 64 };
typedef struct {
  bool valid;
  int32_t lower, upper;
} SignedRange;
typedef struct {
  int instruction, block, width;
  uint8_t mask;
  ASTScalarType type;
  char name[64];
} Value;
typedef struct {
  Value values[FLOW_LIMIT];
  int *variable_value, *variable_component;
  int loop, end, compare, test, increment, initial, counter_phi;
  int initial_edge, latch_edge, body_block, latch_block;
  int32_t initial_integer;
  SignedRange bound;
  char counter_name[64];
} Plan;

static int variable(const HLSLEmitterContext *ctx, int instruction, int operand,
                    int lane) {
  return ctx->ssa.operand_ssa_vars[((size_t)instruction * DXBC_MAX_OPERANDS +
                                    (unsigned)operand) *
                                       4u +
                                   (unsigned)lane];
}
static int single_lane(uint8_t mask) {
  for (int lane = 0; lane < 4; ++lane)
    if (mask == (1u << lane))
      return lane;
  return -1;
}
static int width(uint8_t mask) {
  int result = 0;
  for (int lane = 0; lane < 4; ++lane)
    result += (mask >> lane) & 1u;
  return result;
}
static int32_t signed_bits(uint32_t bits) {
  int32_t result;
  memcpy(&result, &bits, sizeof(result));
  return result;
}
static bool signed_operation(USILOpcode opcode) {
  return opcode == USIL_OP_IMAX || opcode == USIL_OP_IMIN ||
         opcode == USIL_OP_IADD || opcode == USIL_OP_IGE ||
         opcode == USIL_OP_ILT;
}

static const TempVariable *buffer_field(const HLSLEmitterContext *ctx,
                                        const DXBCOperand *operand, int lane) {
  if (operand->type != OPERAND_TYPE_CONSTANT_BUFFER ||
      !hlsl_lift_operand_is_plain(operand) ||
      operand->register_index_dim != 2 || !operand->index_has_immediate[0] ||
      !operand->index_has_immediate[1] || operand->index_representations[0] ||
      operand->index_representations[1] ||
      operand->index_value_exceeds_int[0] ||
      operand->index_value_exceeds_int[1] || operand->index_values[1] >= 4096u)
    return NULL;
  const uint64_t offset =
      operand->index_values[1] * 16u +
      (unsigned)usil_operand_source_component(operand, lane) * 4u;
  for (int b = 0; b < ctx->cbuffer_layout_count; ++b) {
    const HLSLCBufferLayout *layout = &ctx->cbuffer_layouts[b];
    if (layout->reg != operand->register_index)
      continue;
    if (layout->raw_storage || layout->row_struct_storage ||
        layout->is_unity_builtin || layout->omit_declaration)
      return NULL;
    for (int f = 0; f < layout->variable_count; ++f) {
      const TempVariable *field = &layout->variables[f];
      if (offset < field->byte_offset ||
          offset >= field->byte_offset + field->byte_size)
        continue;
      if ((field->authority != 1 && field->authority != 2) ||
          !hlsl_source_identifier_valid(field->name) || field->is_matrix ||
          field->matrix_array_size || field->rows != 1 || field->dim < 1 ||
          field->dim > 4 || field->byte_size != field->dim * 4u)
        return NULL;
      return field;
    }
  }
  return NULL;
}

static SignedRange source_range(const HLSLEmitterContext *ctx, int instruction,
                                int operand, int lane, unsigned depth);
static SignedRange definition_range(const HLSLEmitterContext *ctx,
                                    int instruction, int lane, unsigned depth) {
  SignedRange unknown = {0};
  if (depth > FLOW_LIMIT || instruction < 0 ||
      instruction >= ctx->program->instruction_count)
    return unknown;
  const USILInstruction *owner = &ctx->program->instructions[instruction];
  if (owner->opcode == USIL_OP_MOV)
    return source_range(ctx, instruction, 1, lane, depth + 1);
  if (owner->opcode != USIL_OP_IMAX && owner->opcode != USIL_OP_IMIN)
    return unknown;
  SignedRange a = source_range(ctx, instruction, 1, lane, depth + 1),
              b = source_range(ctx, instruction, 2, lane, depth + 1);
  if (!a.valid || !b.valid)
    return unknown;
  if (owner->opcode == USIL_OP_IMAX)
    return (SignedRange){true, a.lower > b.lower ? a.lower : b.lower,
                         a.upper > b.upper ? a.upper : b.upper};
  return (SignedRange){true, a.lower < b.lower ? a.lower : b.lower,
                       a.upper < b.upper ? a.upper : b.upper};
}
static SignedRange source_range(const HLSLEmitterContext *ctx, int instruction,
                                int operand, int lane, unsigned depth) {
  SignedRange unknown = {0};
  const DXBCOperand *source =
      &ctx->program->instructions[instruction].operands[operand];
  if (!hlsl_lift_operand_is_plain(source))
    return unknown;
  if (source->type == OPERAND_TYPE_IMMEDIATE32) {
    int component = source->imm_value_count == 1
                        ? 0
                        : usil_operand_source_component(source, lane);
    if (component < 0 || component >= source->imm_value_count)
      return unknown;
    int32_t value = signed_bits(source->imm_values[component]);
    return (SignedRange){true, value, value};
  }
  if (source->type == OPERAND_TYPE_CONSTANT_BUFFER) {
    const TempVariable *field = buffer_field(ctx, source, lane);
    return field && field->type == 1 ? (SignedRange){true, INT32_MIN, INT32_MAX}
                                     : unknown;
  }
  if (source->type != OPERAND_TYPE_TEMP)
    return unknown;
  int ssa = variable(ctx, instruction, operand, lane);
  if (ssa < 0 || ssa >= ctx->ssa.ssa_var_count)
    return unknown;
  return definition_range(ctx, ctx->ssa.ssa_var_defs[ssa],
                          usil_operand_source_component(source, lane),
                          depth + 1);
}

static bool match_loop(HLSLEmitterContext *ctx, Plan *plan, int index,
                       bool prove_bound) {
  const USILProgram *program = ctx->program;
  plan->loop = index;
  plan->end = ctx->cfg.instruction_flow[index].end;
  plan->compare = index + 1;
  plan->test = index + 2;
  plan->increment = plan->end - 1;
  if (plan->end <= plan->test + 1 ||
      plan->end >= program->instruction_count - 1)
    return false;
  const USILInstruction *compare = &program->instructions[plan->compare],
                        *test = &program->instructions[plan->test],
                        *increment = &program->instructions[plan->increment];
  int lane =
      single_lane(usil_operand_destination_lane_mask(&compare->operands[0]));
  int counter_lane =
      single_lane(usil_operand_destination_lane_mask(&increment->operands[0]));
  if (compare->opcode != USIL_OP_IGE || test->opcode != USIL_OP_BREAKC ||
      test->condition_test != DXBC_INSTRUCTION_TEST_NONZERO ||
      increment->opcode != USIL_OP_IADD || lane < 0 || counter_lane < 0 ||
      compare->operands[1].type != OPERAND_TYPE_TEMP ||
      increment->operands[0].type != OPERAND_TYPE_TEMP ||
      increment->operands[1].type != OPERAND_TYPE_TEMP ||
      increment->operands[2].type != OPERAND_TYPE_IMMEDIATE32 ||
      increment->operands[2].imm_value_count != 1 ||
      increment->operands[2].imm_values[0] != 1)
    return false;
  plan->counter_phi = variable(ctx, plan->compare, 1, lane);
  int increment_ssa = variable(ctx, plan->increment, 0, counter_lane);
  int predicate_ssa = variable(ctx, plan->compare, 0, lane);
  if (plan->counter_phi < 0 ||
      variable(ctx, plan->increment, 1, counter_lane) != plan->counter_phi ||
      variable(ctx, plan->test, 0, 0) != predicate_ssa ||
      !hlsl_predicate_control_only(ctx, plan->compare, predicate_ssa))
    return false;
  plan->body_block = ctx->cfg.instruction_block[plan->compare];
  plan->latch_block = ctx->cfg.instruction_block[plan->end];
  int preheader = ctx->cfg.instruction_block[index];
  const HLSLBasicBlock *body = &ctx->cfg.blocks[plan->body_block];
  if (body->predecessor_count != 2)
    return false;
  plan->initial_edge = body->predecessors[0] == preheader ? 0 : 1;
  plan->latch_edge = 1 - plan->initial_edge;
  if (body->predecessors[plan->initial_edge] != preheader ||
      body->predecessors[plan->latch_edge] != plan->latch_block ||
      !hlsl_cfg_must_reach(&ctx->cfg,
                           ctx->cfg.instruction_block[plan->test + 1],
                           plan->latch_block))
    return false;
  const HLSLPhiNode *counter = NULL;
  const HLSLBlockPhis *phis = &ctx->ssa.block_phis[plan->body_block];
  for (int p = 0; p < phis->phi_count; ++p)
    if (phis->phis[p].ssa_var == plan->counter_phi)
      counter = &phis->phis[p];
  if (!counter || counter->component != counter_lane ||
      counter->register_index != increment->operands[0].register_index ||
      counter->incoming_vars[plan->latch_edge] != increment_ssa)
    return false;
  int initial_ssa = counter->incoming_vars[plan->initial_edge];
  if (initial_ssa < 0 || initial_ssa >= ctx->ssa.ssa_var_count)
    return false;
  plan->initial = ctx->ssa.ssa_var_defs[initial_ssa];
  if (plan->initial < 0 || plan->initial >= index ||
      !hlsl_cfg_dominates(&ctx->cfg, ctx->cfg.instruction_block[plan->initial],
                          preheader))
    return false;
  const USILInstruction *initial = &program->instructions[plan->initial];
  if (initial->opcode != USIL_OP_MOV ||
      single_lane(usil_operand_destination_lane_mask(&initial->operands[0])) !=
          counter_lane ||
      initial->operands[1].type != OPERAND_TYPE_IMMEDIATE32 ||
      initial->operands[1].imm_value_count != 1)
    return false;
  plan->initial_integer = signed_bits(initial->operands[1].imm_values[0]);
  if (prove_bound) {
    plan->bound = source_range(ctx, plan->compare, 2, lane, 0);
    if (!plan->bound.valid || plan->initial_integer < 0 ||
        plan->bound.lower < plan->initial_integer ||
        plan->bound.upper >= INT32_MAX)
      return false;
  }
  for (int i = 0; i < program->instruction_count; ++i) {
    if (program->instructions[i].opcode == USIL_OP_BREAKC && i != plan->test)
      return false;
    if (i > index && i < plan->end &&
        (program->instructions[i].opcode == USIL_OP_LOOP ||
         program->instructions[i].opcode == USIL_OP_IF))
      return false;
  }
  snprintf(plan->counter_name, sizeof(plan->counter_name), "index_i%d", index);
  return !prove_bound ||
         hlsl_high_level_name_available(ctx, plan->counter_name);
}

static bool output_initialized_at_appends(const HLSLEmitterContext *ctx) {
  uint8_t in[FLOW_LIMIT][32], out[FLOW_LIMIT][32];
  int blocks = ctx->cfg.block_count;
  if (blocks <= 0 || blocks > FLOW_LIMIT)
    return false;
  memset(in, 15, sizeof(in));
  memset(out, 15, sizeof(out));
  memset(in[0], 0, 32);
  bool changed = true;
  for (int iteration = 0; changed && iteration < blocks * 33; ++iteration) {
    changed = false;
    for (int b = 0; b < blocks; ++b) {
      const HLSLBasicBlock *block = &ctx->cfg.blocks[b];
      uint8_t current[32];
      memset(current, b ? 15 : 0, 32);
      if (b && !block->predecessor_count)
        return false;
      for (int p = 0; p < block->predecessor_count; ++p)
        for (int r = 0; r < 32; ++r)
          current[r] &= out[block->predecessors[p]][r];
      if (memcmp(current, in[b], 32)) {
        memcpy(in[b], current, 32);
        changed = true;
      }
      for (int i = block->first_instruction; i <= block->last_instruction;
           ++i) {
        const USILInstruction *owner = &ctx->program->instructions[i];
        if (owner->operand_count &&
            owner->operands[0].type == OPERAND_TYPE_OUTPUT)
          current[owner->operands[0].register_index] |=
              usil_operand_destination_lane_mask(&owner->operands[0]);
      }
      if (memcmp(current, out[b], 32)) {
        memcpy(out[b], current, 32);
        changed = true;
      }
    }
  }
  if (changed)
    return false;
  for (int b = 0; b < blocks; ++b) {
    uint8_t current[32];
    memcpy(current, in[b], 32);
    const HLSLBasicBlock *block = &ctx->cfg.blocks[b];
    for (int i = block->first_instruction; i <= block->last_instruction; ++i) {
      const USILInstruction *owner = &ctx->program->instructions[i];
      if (owner->operand_count &&
          owner->operands[0].type == OPERAND_TYPE_OUTPUT)
        current[owner->operands[0].register_index] |=
            usil_operand_destination_lane_mask(&owner->operands[0]);
      if (owner->opcode == USIL_OP_GEOMETRY_APPEND)
        for (int f = 0; f < ctx->program->output_count; ++f) {
          const DXBCSignatureElement *field = &ctx->program->outputs[f];
          if ((current[field->register_id] & field->mask) != field->mask)
            return false;
        }
    }
  }
  return true;
}

bool hlsl_geometry_control_flow_admission(const USILProgram *program,
                                          HLSLEmitMode mode) {
  if (!program || mode != HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE ||
      program->program_type != DXBC_PROGRAM_TYPE_GEOMETRY ||
      !program->has_stage_contract ||
      !program->has_parsed_signature_authority ||
      !usil_signature_authority_is_valid(program) ||
      program->instruction_count < 2 ||
      program->instruction_count > FLOW_LIMIT || !program->instructions ||
      program->instruction_alloc < program->instruction_count ||
      !program->geometry.valid ||
      !program->geometry.output_tuple_state_persists ||
      !dxbc_geometry_input_vertex_count(program->geometry.input_primitive) ||
      program->geometry.input_vertex_count !=
          dxbc_geometry_input_vertex_count(program->geometry.input_primitive) ||
      program->geometry.has_instance_count ||
      program->geometry.instance_count != 1 ||
      program->geometry.declared_stream_mask > 1 ||
      program->geometry.referenced_stream_mask != 1 ||
      !program->geometry.max_output_vertex_count ||
      program->geometry.max_output_vertex_count > 1024 ||
      (program->geometry.output_topology != DXBC_OUTPUT_TOPOLOGY_POINT_LIST &&
       program->geometry.output_topology != DXBC_OUTPUT_TOPOLOGY_LINE_STRIP &&
       program->geometry.output_topology !=
           DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP) ||
      program->texture_count || program->sampler_count || program->uav_count ||
      program->icb_value_count || program->indexable_temp_count ||
      program->index_range_count || program->patch_constant_count ||
      program->compute.valid || program->tessellation.valid ||
      !program->input_count || !program->output_count)
    return false;
  for (int list = 0; list < 2; ++list) {
    const DXBCSignatureElement *elements =
        list ? program->outputs : program->inputs;
    int count = list ? program->output_count : program->input_count;
    uint32_t registers = 0;
    for (int f = 0; f < count; ++f) {
      const DXBCSignatureElement *field = &elements[f];
      if (field->register_id >= 32 || field->component_type != 3 ||
          !field->mask || (field->mask & (field->mask + 1u)) ||
          field->min_precision || field->stream_index ||
          (registers & (UINT32_C(1) << field->register_id)))
        return false;
      registers |= UINT32_C(1) << field->register_id;
    }
  }
  int loops = 0;
  for (int i = 0; i < program->instruction_count; ++i)
    if (program->instructions[i].opcode == USIL_OP_LOOP)
      ++loops;
  return loops == 1;
}

static bool build_plan(HLSLEmitterContext *ctx, Plan *plan) {
  const USILProgram *program = ctx->program;
  if (!hlsl_geometry_control_flow_admission(program, ctx->emit_mode))
    return false;
  if (program->instructions[program->instruction_count - 1].opcode !=
          USIL_OP_RET ||
      !ctx->ssa.operand_ssa_vars || !ctx->ssa.ssa_var_defs ||
      ctx->ssa.instruction_count != program->instruction_count ||
      ctx->ssa.ssa_var_count < 0)
    return false;
  /* Shape and operand boundaries precede the induction matcher, which reads
   * the canonical header and latch operands directly. */
  for (int i = 0; i < program->instruction_count; ++i) {
    const USILInstruction *owner = &program->instructions[i];
    if (owner->precise_mask || owner->saturate ||
        !usil_instruction_shape_valid(program, owner) ||
        (i && owner->source_instruction_index <=
                  program->instructions[i - 1].source_instruction_index))
      return false;
    for (int operand = 0; operand < owner->operand_count; ++operand) {
      const DXBCOperand *value = &owner->operands[operand];
      if (!hlsl_lift_operand_is_plain(value) ||
          (value->type == OPERAND_TYPE_OUTPUT &&
           (value->register_index < 0 || value->register_index >= 32)))
        return false;
    }
  }
  plan->loop = -1;
  for (int i = 0; i < program->instruction_count; ++i)
    if (program->instructions[i].opcode == USIL_OP_LOOP) {
      if (plan->loop >= 0 || !match_loop(ctx, plan, i, true))
        return false;
    }
  if (plan->loop < 0)
    return false;
  size_t vars = ctx->ssa.ssa_var_count ? (size_t)ctx->ssa.ssa_var_count : 1;
  plan->variable_value = malloc(vars * sizeof(int));
  plan->variable_component = malloc(vars * sizeof(int));
  if (!plan->variable_value || !plan->variable_component)
    return false;
  for (size_t v = 0; v < vars; ++v)
    plan->variable_value[v] = plan->variable_component[v] = -1;
  size_t effects = 0;
  unsigned appends = 0, static_appends = 0;
  for (int i = 0; i < program->instruction_count; ++i) {
    const USILInstruction *owner = &program->instructions[i];
    if (owner->opcode == USIL_OP_GEOMETRY_APPEND ||
        owner->opcode == USIL_OP_GEOMETRY_RESTART_STRIP) {
      if (!hlsl_high_level_geometry_effect_supported(ctx, i))
        return false;
      ++effects;
      if (owner->opcode == USIL_OP_GEOMETRY_APPEND) {
        if (i > plan->loop && i < plan->end)
          ++appends;
        else
          ++static_appends;
      }
      continue;
    }
    if (owner->opcode == USIL_OP_IF) {
      if (owner->condition_test != DXBC_INSTRUCTION_TEST_NONZERO ||
          owner->operands[0].type != OPERAND_TYPE_TEMP)
        return false;
      continue;
    }
    if (owner->opcode == USIL_OP_RET) {
      if (i != program->instruction_count - 1)
        return false;
      continue;
    }
    if (owner->opcode == USIL_OP_LOOP || owner->opcode == USIL_OP_ENDLOOP ||
        owner->opcode == USIL_OP_BREAKC || owner->opcode == USIL_OP_ELSE ||
        owner->opcode == USIL_OP_ENDIF || owner->opcode == USIL_OP_NOP)
      continue;
    if (owner->opcode != USIL_OP_MOV && owner->opcode != USIL_OP_ADD &&
        owner->opcode != USIL_OP_MUL && owner->opcode != USIL_OP_MAD &&
        owner->opcode != USIL_OP_DIV && owner->opcode != USIL_OP_IMAX &&
        owner->opcode != USIL_OP_IMIN && owner->opcode != USIL_OP_IADD &&
        owner->opcode != USIL_OP_ITOF && owner->opcode != USIL_OP_GE &&
        owner->opcode != USIL_OP_IGE)
      return false;
    if (owner->opcode == USIL_OP_IADD && i != plan->increment)
      return false;
    const DXBCOperand *destination = &owner->operands[0];
    uint8_t mask = usil_operand_destination_lane_mask(destination);
    if (destination->type == OPERAND_TYPE_OUTPUT) {
      const DXBCSignatureElement *field = NULL;
      for (int f = 0; f < program->output_count; ++f)
        if (program->outputs[f].register_id ==
            (uint32_t)destination->register_index)
          field = &program->outputs[f];
      if (!field || field->component_type != 3 || (mask & ~field->mask) ||
          !mask || destination->register_index >= 32)
        return false;
      continue;
    }
    if (destination->type != OPERAND_TYPE_TEMP)
      return false;
    Value *value = &plan->values[i];
    value->instruction = i;
    value->block = ctx->cfg.instruction_block[i];
    value->mask = mask;
    value->width = width(mask);
    value->type = signed_operation(owner->opcode) ? AST_SCALAR_SINT32
                                                  : AST_SCALAR_FLOAT32;
    if (owner->opcode == USIL_OP_GE || owner->opcode == USIL_OP_IGE) {
      if (value->width != 1 ||
          !hlsl_predicate_control_only(ctx, i,
                                  variable(ctx, i, 0, single_lane(mask))))
        return false;
      value->type = AST_SCALAR_BOOL;
    }
    if (owner->opcode == USIL_OP_MOV && i != plan->initial) {
      unsigned facts = 0;
      for (int lane = 0; lane < 4; ++lane)
        if (mask & (1u << lane))
          facts |= get_lane_value_facts(ctx, i + 1, destination->register_index,
                                        lane);
      if (facts == HLSL_VALUE_SINT)
        value->type = AST_SCALAR_SINT32;
      else if (facts != HLSL_VALUE_FLOAT)
        return false;
    }
    if (i == plan->initial || i == plan->increment)
      value->type = AST_SCALAR_SINT32;
    snprintf(value->name, sizeof(value->name), "value_i%d", i);
    if (!hlsl_high_level_name_available(ctx, value->name))
      return false;
    int component = 0;
    for (int lane = 0; lane < 4; ++lane)
      if (mask & (1u << lane)) {
        int ssa = variable(ctx, i, 0, lane);
        if (ssa < 0 || ssa >= ctx->ssa.ssa_var_count)
          return false;
        plan->variable_value[ssa] = i;
        plan->variable_component[ssa] = component++;
      }
  }
  if (!appends || effects != program->geometry.effect_count ||
      (uint64_t)appends *
                  (uint32_t)(plan->bound.upper - plan->initial_integer) +
              static_appends >
          program->geometry.max_output_vertex_count ||
      !output_initialized_at_appends(ctx))
    return false;
  return true;
}

/* The same bounded semantic proof is required both before an entry can
 * promise coverage and after its actual syntax ledger has been emitted. */
bool hlsl_geometry_control_flow_inventory_supported(HLSLEmitterContext *ctx) {
  if (!ctx || !ctx->program || !ctx->high_level_geometry ||
      !ctx->high_level_interface_prepared)
    return false;
  Plan plan = {0};
  const bool valid = build_plan(ctx, &plan);
  free(plan.variable_value);
  free(plan.variable_component);
  return valid;
}

static uint64_t syntax_digest(const char *source, size_t begin, size_t end) {
  uint64_t digest = UINT64_C(14695981039346656037);
  for (size_t index = begin; index < end; ++index) {
    digest ^= (unsigned char)source[index];
    digest *= UINT64_C(1099511628211);
  }
  return digest;
}

/* These receipts cover actual signature-backed field/frame syntax and the
 * actual geometry attribute, parameters, function opening and result local.
 * The first field receipt includes its struct opening; empty input/output
 * structs are excluded by the retained stage/interface authority. */
bool hlsl_geometry_control_flow_record_interface(HLSLEmitterContext *ctx,
    HLSLGeometryFlowInterfaceKind kind, int field_index, size_t begin) {
  if (!ctx || !ctx->source_quality_analysis ||
      !hlsl_geometry_control_flow_admission(ctx->program, ctx->emit_mode)) return true;
  if (!sb_ok(ctx->sb) || begin >= ctx->sb->len) return false;
  HLSLGeometryFlowSourceInventory *inventory = ctx->geometry_flow_source_inventory;
  if (!inventory) {
    inventory = calloc(1, sizeof(*inventory));
    if (!inventory) return false;
    inventory->instruction_count = ctx->program->instruction_count;
    inventory->source_begin = SIZE_MAX;
    inventory->interface_begin = begin;
    ctx->geometry_flow_source_inventory = inventory;
  }
  if (inventory->interface_record_count >= 72 || inventory->source_begin != SIZE_MAX)
    return false;
  HLSLGeometryFlowInterfaceRecord *record =
      &inventory->interface_records[inventory->interface_record_count++];
  record->kind = kind;
  record->field_index = field_index;
  record->source_begin = begin;
  record->source_end = ctx->sb->len;
  record->source_digest = syntax_digest(ctx->sb->buf, begin, ctx->sb->len);
  return true;
}

static bool interface_receipt_matches(const HLSLEmitterContext *ctx,
    const HLSLGeometryFlowSourceInventory *inventory, size_t *index, size_t *cursor,
    HLSLGeometryFlowInterfaceKind kind, int field_index) {
  if (*index >= inventory->interface_record_count) return false;
  const HLSLGeometryFlowInterfaceRecord *record = &inventory->interface_records[(*index)++];
  if (record->kind != kind || record->field_index != field_index ||
      record->source_begin != *cursor || record->source_end <= *cursor ||
      record->source_end > ctx->sb->len || record->source_digest !=
          syntax_digest(ctx->sb->buf, record->source_begin, record->source_end))
    return false;
  *cursor = record->source_end;
  return true;
}

static bool interface_receipts_complete(const HLSLEmitterContext *ctx,
                                        const HLSLGeometryFlowSourceInventory *inventory) {
  if (inventory->interface_record_count > 72 || inventory->interface_begin > ctx->sb->len)
    return false;
  size_t index = 0, cursor = inventory->interface_begin;
  for (int field = 0; field < ctx->program->input_count; ++field)
    if (!interface_receipt_matches(ctx, inventory, &index, &cursor,
        HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_FIELD, field)) return false;
  if (!interface_receipt_matches(ctx, inventory, &index, &cursor,
      HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_END, -1)) return false;
  for (int field = 0; field < ctx->program->output_count; ++field)
    if (!interface_receipt_matches(ctx, inventory, &index, &cursor,
        HLSL_GEOMETRY_FLOW_INTERFACE_OUTPUT_FIELD, field)) return false;
  const HLSLGeometryFlowInterfaceKind remaining[] = {
      HLSL_GEOMETRY_FLOW_INTERFACE_OUTPUT_END,
      HLSL_GEOMETRY_FLOW_INTERFACE_ATTRIBUTE,
      HLSL_GEOMETRY_FLOW_INTERFACE_INPUT_PARAMETER,
      HLSL_GEOMETRY_FLOW_INTERFACE_STREAM_PARAMETER,
      HLSL_GEOMETRY_FLOW_INTERFACE_FUNCTION_OPEN,
      HLSL_GEOMETRY_FLOW_INTERFACE_RESULT_LOCAL};
  for (size_t kind = 0; kind < sizeof(remaining) / sizeof(remaining[0]); ++kind)
    if (!interface_receipt_matches(ctx, inventory, &index, &cursor, remaining[kind], -1))
      return false;
  return index == inventory->interface_record_count && cursor == inventory->source_begin;
}

static uint8_t syntax_destination_lanes(const USILProgram *program,
                                       const USILInstruction *owner) {
  USILOperandUseInfo use;
  if (owner->operand_count &&
      usil_instruction_operand_use(program, owner, 0, &use) &&
      use.use == USIL_OPERAND_USE_DESTINATION)
    return usil_operand_destination_lane_mask(&owner->operands[0]);
  return 0;
}

static HLSLGeometryFlowSyntaxKind syntax_kind(const Plan *plan,
                                             const USILInstruction *owner,
                                             int instruction) {
  if (instruction == plan->initial || instruction == plan->loop ||
      instruction == plan->compare || instruction == plan->test ||
      instruction == plan->increment)
    return HLSL_GEOMETRY_FLOW_SYNTAX_LOOP_HEADER;
  switch (owner->opcode) {
  case USIL_OP_IF: return HLSL_GEOMETRY_FLOW_SYNTAX_IF;
  case USIL_OP_ELSE: return HLSL_GEOMETRY_FLOW_SYNTAX_ELSE;
  case USIL_OP_ENDIF: return HLSL_GEOMETRY_FLOW_SYNTAX_ENDIF;
  case USIL_OP_ENDLOOP: return HLSL_GEOMETRY_FLOW_SYNTAX_ENDLOOP;
  case USIL_OP_GEOMETRY_APPEND:
  case USIL_OP_GEOMETRY_RESTART_STRIP:
    return HLSL_GEOMETRY_FLOW_SYNTAX_EFFECT;
  case USIL_OP_RET: return HLSL_GEOMETRY_FLOW_SYNTAX_RETURN;
  case USIL_OP_NOP: return HLSL_GEOMETRY_FLOW_SYNTAX_NOP;
  default: return HLSL_GEOMETRY_FLOW_SYNTAX_EXPRESSION;
  }
}

static bool inventory_begin(HLSLEmitterContext *ctx, const Plan *plan) {
  if (!ctx->source_quality_analysis) return true;
  if (!sb_ok(ctx->sb)) return false;
  HLSLGeometryFlowSourceInventory *inventory = ctx->geometry_flow_source_inventory;
  if (!inventory || inventory->source_begin != SIZE_MAX ||
      inventory->instruction_count != ctx->program->instruction_count) return false;
  inventory->instruction_count = ctx->program->instruction_count;
  inventory->loop = plan->loop;
  inventory->end = plan->end;
  inventory->compare = plan->compare;
  inventory->test = plan->test;
  inventory->increment = plan->increment;
  inventory->initial = plan->initial;
  inventory->source_begin = ctx->sb->len;
  ctx->geometry_flow_source_inventory = inventory;
  return true;
}

static bool record_syntax(HLSLEmitterContext *ctx, int instruction,
                          HLSLGeometryFlowSyntaxKind kind, size_t begin) {
  if (!ctx->source_quality_analysis) return true;
  HLSLGeometryFlowSourceInventory *inventory = ctx->geometry_flow_source_inventory;
  if (!inventory || instruction < 0 || instruction >= inventory->instruction_count ||
      kind == HLSL_GEOMETRY_FLOW_SYNTAX_NONE || !sb_ok(ctx->sb) ||
      begin > ctx->sb->len ||
      (kind != HLSL_GEOMETRY_FLOW_SYNTAX_NOP && begin == ctx->sb->len) ||
      (inventory->recorded_instructions & (UINT64_C(1) << instruction)))
    return false;
  const USILInstruction *owner = &ctx->program->instructions[instruction];
  HLSLGeometryFlowSyntaxRecord *record = &inventory->records[instruction];
  record->kind = kind;
  record->source_instruction_index = owner->source_instruction_index;
  record->destination_lanes = syntax_destination_lanes(ctx->program, owner);
  record->source_begin = begin;
  record->source_end = ctx->sb->len;
  record->source_digest = syntax_digest(ctx->sb->buf, begin, ctx->sb->len);
  inventory->recorded_instructions |= UINT64_C(1) << instruction;
  return true;
}

/* A root is counted only after the observer accepted the AST and its formatter
 * appended actual source. Folded induction owners share the proved loop header;
 * only its bound expression has an AST observation. */
static bool record_expression(HLSLEmitterContext *ctx, int instruction,
                              size_t begin) {
  if (!ctx->source_quality_analysis) return true;
  HLSLGeometryFlowSourceInventory *inventory = ctx->geometry_flow_source_inventory;
  if (!inventory || instruction < 0 || instruction >= inventory->instruction_count ||
      !sb_ok(ctx->sb) || ctx->sb->len <= begin ||
      (inventory->emitted_expressions & (UINT64_C(1) << instruction)))
    return false;
  inventory->emitted_expressions |= UINT64_C(1) << instruction;
  return true;
}

bool hlsl_geometry_control_flow_record_return(HLSLEmitterContext *ctx,
                                              size_t source_begin) {
  if (!ctx || !ctx->geometry_flow_source_inventory) return true;
  return record_syntax(ctx, ctx->program->instruction_count - 1,
                       HLSL_GEOMETRY_FLOW_SYNTAX_RETURN, source_begin);
}

void hlsl_geometry_control_flow_inventory_free(HLSLEmitterContext *ctx) {
  if (!ctx) return;
  free(ctx->geometry_flow_source_inventory);
  ctx->geometry_flow_source_inventory = NULL;
}

bool hlsl_geometry_control_flow_inventory_complete(HLSLEmitterContext *ctx) {
  if (!ctx || !ctx->program || !ctx->geometry_flow_source_inventory || !sb_ok(ctx->sb))
    return false;
  const HLSLGeometryFlowSourceInventory *inventory = ctx->geometry_flow_source_inventory;
  Plan plan = {0};
  bool valid = false;
  if (!build_plan(ctx, &plan) || inventory->instruction_count != ctx->program->instruction_count ||
      inventory->loop != plan.loop || inventory->end != plan.end ||
      inventory->compare != plan.compare || inventory->test != plan.test ||
      inventory->increment != plan.increment || inventory->initial != plan.initial)
    goto cleanup;
  const uint64_t all = inventory->instruction_count == 64 ? UINT64_MAX :
      (UINT64_C(1) << inventory->instruction_count) - 1u;
  if (inventory->recorded_instructions != all || inventory->source_begin > ctx->sb->len ||
      !interface_receipts_complete(ctx, inventory)) goto cleanup;
  const HLSLGeometryFlowSyntaxRecord *header = &inventory->records[plan.loop];
  uint64_t expected_expressions = UINT64_C(1) << plan.compare;
  size_t cursor = inventory->source_begin;
  for (int instruction = 0; instruction < inventory->instruction_count; ++instruction) {
    const USILInstruction *owner = &ctx->program->instructions[instruction];
    const HLSLGeometryFlowSyntaxRecord *record = &inventory->records[instruction];
    const HLSLGeometryFlowSyntaxKind expected = syntax_kind(&plan, owner, instruction);
    if (record->kind != expected ||
        record->source_instruction_index != owner->source_instruction_index ||
        record->destination_lanes != syntax_destination_lanes(ctx->program, owner) ||
        record->source_begin > record->source_end || record->source_end > ctx->sb->len ||
        record->source_digest != syntax_digest(ctx->sb->buf, record->source_begin, record->source_end))
      goto cleanup;
    if (expected == HLSL_GEOMETRY_FLOW_SYNTAX_LOOP_HEADER && instruction != plan.loop) {
      if (record->source_begin != header->source_begin || record->source_end != header->source_end)
        goto cleanup;
      continue;
    }
    /* Every emitted body byte, including indentation and braces, belongs to a
     * consecutive syntax receipt. A NOP contributes an explicit empty receipt. */
    if (record->source_begin != cursor ||
        (expected == HLSL_GEOMETRY_FLOW_SYNTAX_NOP
             ? record->source_end != cursor : record->source_end == cursor))
      goto cleanup;
    cursor = record->source_end;
    if (expected == HLSL_GEOMETRY_FLOW_SYNTAX_EXPRESSION ||
        expected == HLSL_GEOMETRY_FLOW_SYNTAX_IF)
      expected_expressions |= UINT64_C(1) << instruction;
  }
  valid = cursor == ctx->sb->len && inventory->emitted_expressions == expected_expressions;
cleanup:
  free(plan.variable_value);
  free(plan.variable_component);
  return valid;
}

static ASTExpr *owned(ASTExpr *expression, const HLSLEmitterContext *ctx,
                      int instruction, ASTScalarType type, int components,
                      uint64_t identity, bool projection) {
  if (!expression)
    return NULL;
  ASTLogicalValueOrigin origin;
  ast_logical_value_origin_init(&origin);
  origin.complete = true;
  origin.scalar_type = type;
  origin.components = (uint8_t)components;
  origin.logical_value_id = identity;
  origin.instruction_index = instruction;
  origin.source_instruction_index =
      ctx->program->instructions[instruction].source_instruction_index;
  const USILInstruction *owner = &ctx->program->instructions[instruction];
  if (owner->operand_count && owner->opcode != USIL_OP_IF)
    origin.destination_lanes =
        usil_operand_destination_lane_mask(&owner->operands[0]);
  origin.semantic_projection = projection;
  if (!ast_set_logical_value_origin(expression, &origin)) {
    ast_free_expr(expression);
    return NULL;
  }
  return expression;
}
static ASTExpr *join_expressions(ASTExpr **args, int count, ASTScalarType type,
                                 const HLSLEmitterContext *ctx, int instruction,
                                 uint64_t identity) {
  char name[16];
  snprintf(name, sizeof(name), "%s%d",
           type == AST_SCALAR_SINT32 ? "int" : "float", count);
  ASTExpr *result = ast_create_call(name, args, count);
  if (!result) {
    for (int a = 0; a < count; ++a)
      ast_free_expr(args[a]);
    return NULL;
  }
  return owned(result, ctx, instruction, type, count, identity, false);
}
static ASTExpr *source_expression(HLSLEmitterContext *ctx, Plan *plan,
                                  int index, int op, uint8_t mask,
                                  ASTScalarType expected) {
  const DXBCOperand *source = &ctx->program->instructions[index].operands[op];
  const uint64_t identity = UINT64_C(0x4000000000000000) |
                            ((uint64_t)index << 8) | ((unsigned)op << 4);
  int demanded[4], count = 0;
  for (int lane = 0; lane < 4; ++lane)
    if (mask & (1u << lane))
      demanded[count++] = lane;
  if (!count)
    return NULL;
  if (source->type == OPERAND_TYPE_TEMP) {
    ASTExpr *args[4] = {0};
    int groups[4], components[4];
    bool same = true, repeated = true;
    for (int c = 0; c < count; ++c) {
      int ssa = variable(ctx, index, op, demanded[c]);
      if (ssa == plan->counter_phi) {
        if (index <= plan->loop || index >= plan->end ||
            !hlsl_cfg_dominates(&ctx->cfg, plan->body_block,
                                ctx->cfg.instruction_block[index]))
          return NULL;
        groups[c] = -2;
        components[c] = 0;
      } else if (ssa >= 0 && ssa < ctx->ssa.ssa_var_count &&
                 plan->variable_value[ssa] >= 0) {
        groups[c] = plan->variable_value[ssa];
        components[c] = plan->variable_component[ssa];
        if (groups[c] == plan->initial || groups[c] == plan->increment ||
            groups[c] == plan->compare)
          return NULL;
        const Value *value = &plan->values[groups[c]];
        if (value->type != expected ||
            !hlsl_cfg_dominates(&ctx->cfg, value->block,
                                ctx->cfg.instruction_block[index]))
          return NULL;
      } else
        return NULL;
      if (c) {
        same = same && groups[c] == groups[0];
        repeated = repeated && groups[c] == groups[0] &&
                   components[c] == components[0];
      }
    }
    if (groups[0] == -2 && expected != AST_SCALAR_SINT32)
      return NULL;
    int selected_count = repeated ? 1 : count;
    if (same) {
      const Value *value = groups[0] >= 0 ? &plan->values[groups[0]] : NULL;
      ASTExpr *base =
          ast_create_var(-1, source->register_index, OPERAND_TYPE_TEMP,
                         value ? value->name : plan->counter_name);
      int natural = value ? value->width : 1;
      base = owned(base, ctx, index, expected, natural,
                   groups[0] >= 0
                       ? (uint64_t)groups[0] + 1u
                       : UINT64_C(0x2000000000000000) | (unsigned)plan->loop,
                   false);
      bool identity_selection = selected_count == natural;
      for (int c = 0; c < selected_count; ++c)
        identity_selection = identity_selection && components[c] == c;
      if (identity_selection)
        return base;
      ASTExpr *selection = ast_create_swizzle(base, components, selected_count);
      if (!selection) {
        ast_free_expr(base);
        return NULL;
      }
      bool semantic = true;
      for (int c = 1; c < selected_count; ++c)
        semantic = semantic && components[c] > components[c - 1];
      return owned(selection, ctx, index, expected, selected_count, identity,
                   semantic);
    }
    for (int c = 0; c < count; ++c) {
      uint8_t lane = (uint8_t)(1u << demanded[c]);
      args[c] = source_expression(ctx, plan, index, op, lane, expected);
      if (!args[c]) {
        for (int a = 0; a < c; ++a)
          ast_free_expr(args[a]);
        return NULL;
      }
    }
    return join_expressions(args, count, expected, ctx, index, identity);
  }
  if (source->type == OPERAND_TYPE_IMMEDIATE32) {
    uint32_t bits[4];
    bool same = true;
    for (int c = 0; c < count; ++c) {
      int selected = source->imm_value_count == 1
                         ? 0
                         : usil_operand_source_component(source, demanded[c]);
      if (selected < 0 || selected >= source->imm_value_count)
        return NULL;
      bits[c] = source->imm_values[selected];
      if (c)
        same = same && bits[c] == bits[0];
      if (expected == AST_SCALAR_FLOAT32) {
        float number;
        memcpy(&number, &bits[c], 4);
        if (!isfinite(number))
          return NULL;
      }
    }
    count = same ? 1 : count;
    return owned(ast_create_literal_bits(bits, count, expected), ctx, index,
                 expected, count, identity, false);
  }
  ASTOperandProvenance provenance;
  ast_operand_provenance_init(&provenance);
  if (source->type == OPERAND_TYPE_INPUT) {
    if (expected != AST_SCALAR_FLOAT32 ||
        !hlsl_high_level_input_provenance(ctx, source, mask, &provenance))
      return NULL;
  } else if (source->type == OPERAND_TYPE_CONSTANT_BUFFER) {
    const TempVariable *field = buffer_field(ctx, source, demanded[0]);
    if (!field || (expected == AST_SCALAR_FLOAT32  ? field->type != 0
                   : expected == AST_SCALAR_SINT32 ? field->type != 1
                                                   : true))
      return NULL;
    provenance.complete = true;
    provenance.value_role = AST_OPERAND_VALUE_LOGICAL;
    provenance.logical_value_id =
        UINT64_C(0x8000000000000000) |
        ((uint64_t)(unsigned)source->register_index << 32) | field->byte_offset;
    provenance.natural_components = (uint8_t)field->dim;
    provenance.result_components = (uint8_t)count;
    bool same = true, identity_selection = count == (int)field->dim;
    for (int c = 0; c < count; ++c) {
      if (buffer_field(ctx, source, demanded[c]) != field)
        return NULL;
      uint64_t offset =
          source->index_values[1] * 16u +
          (unsigned)usil_operand_source_component(source, demanded[c]) * 4u;
      provenance.selected_components[c] =
          (uint8_t)((offset - field->byte_offset) / 4u);
      if (c)
        same = same && provenance.selected_components[c] ==
                           provenance.selected_components[0];
      identity_selection =
          identity_selection && provenance.selected_components[c] == c;
    }
    if (same)
      provenance.result_components = 1;
    bool semantic = true;
    for (int c = 1; c < provenance.result_components; ++c)
      semantic = semantic && provenance.selected_components[c] >
                                 provenance.selected_components[c - 1];
    provenance.selection_role = identity_selection
                                    ? AST_COMPONENT_SELECTION_NONE
                                : semantic ? AST_COMPONENT_SELECTION_SEMANTIC
                                           : AST_COMPONENT_SELECTION_TRANSPORT;
    provenance.instruction_index = index;
    provenance.source_instruction_index =
        ctx->program->instructions[index].source_instruction_index;
    provenance.operand_index = op;
    provenance.destination_lanes = mask;
  } else
    return NULL;
  StringBuilder text;
  sb_init(&text);
  bool formatted =
      format_operand_hlsl_sb(ctx, source, expected == AST_SCALAR_SINT32, false,
                             mask << 4, true, &text);
  ASTExpr *expression =
      formatted && sb_ok(&text)
          ? ast_create_emitter_operand_with_provenance(text.buf, &provenance)
          : NULL;
  sb_free(&text);
  return expression;
}
static ASTExpr *instruction_expression(HLSLEmitterContext *ctx, Plan *plan,
                                       int index) {
  const USILInstruction *owner = &ctx->program->instructions[index];
  uint8_t mask = usil_operand_destination_lane_mask(&owner->operands[0]);
  ASTScalarType result = owner->operands[0].type == OPERAND_TYPE_OUTPUT
                             ? AST_SCALAR_FLOAT32
                             : plan->values[index].type;
  ASTScalarType source_type =
      signed_operation(owner->opcode) || owner->opcode == USIL_OP_ITOF
          ? AST_SCALAR_SINT32
      : result == AST_SCALAR_BOOL ? AST_SCALAR_FLOAT32
                                  : result;
  ASTExpr *a = source_expression(ctx, plan, index, 1, mask, source_type);
  if (!a)
    return NULL;
  if (owner->opcode == USIL_OP_MOV)
    return a;
  ASTExpr *expression = NULL;
  if (owner->opcode == USIL_OP_ITOF) {
    char type[16];
    snprintf(type, sizeof(type), "float%s", width(mask) == 1 ? "" : "4");
    if (width(mask) != 1) {
      ast_free_expr(a);
      return NULL;
    }
    expression = ast_create_cast(type, a);
    if (!expression) {
      ast_free_expr(a);
      return NULL;
    }
  } else {
    ASTExpr *b = source_expression(ctx, plan, index, 2, mask, source_type);
    if (!b) {
      ast_free_expr(a);
      return NULL;
    }
    if (owner->opcode == USIL_OP_GE || owner->opcode == USIL_OP_IGE)
      expression = ast_create_comparison(owner->opcode, a, b);
    else if (owner->opcode == USIL_OP_IMAX || owner->opcode == USIL_OP_IMIN) {
      ASTExpr *args[] = {a, b};
      expression = ast_create_call(
          owner->opcode == USIL_OP_IMAX ? "max" : "min", args, 2);
    } else if (owner->opcode == USIL_OP_MAD) {
      ASTExpr *product = ast_create_binary(USIL_OP_MUL, a, b);
      if (!product) {
        ast_free_expr(a);
        ast_free_expr(b);
        return NULL;
      }
      product = owned(product, ctx, index, AST_SCALAR_FLOAT32, width(mask),
                      UINT64_C(0x1000000000000000) | (unsigned)index, false);
      ASTExpr *c =
          source_expression(ctx, plan, index, 3, mask, AST_SCALAR_FLOAT32);
      if (!c) {
        ast_free_expr(product);
        return NULL;
      }
      expression = ast_create_binary(USIL_OP_ADD, product, c);
      if (!expression) {
        ast_free_expr(product);
        ast_free_expr(c);
        return NULL;
      }
    } else
      expression = ast_create_binary(owner->opcode, a, b);
    if (!expression && owner->opcode != USIL_OP_MAD) {
      ast_free_expr(a);
      ast_free_expr(b);
      return NULL;
    }
  }
  return owned(expression, ctx, index, result, width(mask),
               (uint64_t)index + 1u, false);
}

/* Every operation remains at its decoded position. Folding is limited to the
 * independently proved scalar induction header; Append snapshots a persistent
 * output tuple, and partial writes target semantic fields rather than banks. */
bool hlsl_geometry_control_flow_emit(HLSLEmitterContext *ctx) {
  Plan plan = {0};
  bool success = false;
  if (!build_plan(ctx, &plan))
    goto cleanup;
  if (!inventory_begin(ctx, &plan)) goto cleanup;
  hlsl_expression_source_map_begin(ctx);
  HLSLExpressionSourceMap *map = ctx->expression_source_map;
  for (int i = 0; i < ctx->program->instruction_count; ++i) {
    const USILInstruction *owner = &ctx->program->instructions[i];
    ctx->current_instruction_index = i;
    if (i == plan.initial || i == plan.compare || i == plan.test ||
        i == plan.increment)
      continue;
    if (owner->opcode == USIL_OP_NOP) {
      if (!record_syntax(ctx, i, HLSL_GEOMETRY_FLOW_SYNTAX_NOP, ctx->sb->len))
        goto cleanup;
      continue;
    }
    if (owner->opcode == USIL_OP_RET) continue;
    const size_t statement_begin = ctx->sb->len;
    if (owner->opcode == USIL_OP_ENDLOOP || owner->opcode == USIL_OP_ENDIF ||
        owner->opcode == USIL_OP_ELSE)
      ctx->indent -= 4;
    if (owner->opcode != USIL_OP_GEOMETRY_APPEND &&
        owner->opcode != USIL_OP_GEOMETRY_RESTART_STRIP)
      sb_append_spaces(ctx->sb, ctx->indent);
    if (owner->opcode == USIL_OP_LOOP) {
      int lane = single_lane(usil_operand_destination_lane_mask(
          &ctx->program->instructions[plan.compare].operands[0]));
      ASTExpr *bound =
          source_expression(ctx, &plan, plan.compare, 2, (uint8_t)(1u << lane),
                            AST_SCALAR_SINT32);
      if (!bound)
        goto cleanup;
      sb_appendf(ctx->sb, "[loop] for (int %s = %d; %s < ", plan.counter_name,
                 plan.initial_integer, plan.counter_name);
      if (!hlsl_source_quality_observe_expression(ctx, bound, plan.compare)) {
        ast_free_expr(bound);
        goto cleanup;
      }
      const size_t bound_begin = ctx->sb->len;
      ast_format_expr(bound, ctx->sb);
      ast_free_expr(bound);
      if (!record_expression(ctx, plan.compare, bound_begin)) goto cleanup;
      sb_appendf(ctx->sb, "; ++%s) {\n", plan.counter_name);
      ctx->indent += 4;
      const int owners[] = {plan.initial, plan.loop, plan.compare, plan.test,
                            plan.increment};
      for (size_t owner_index = 0;
           owner_index < sizeof(owners) / sizeof(owners[0]); ++owner_index) {
        const int header_owner = owners[owner_index];
        hlsl_source_quality_emission(ctx, 0, true, header_owner);
        if (!record_syntax(ctx, header_owner, HLSL_GEOMETRY_FLOW_SYNTAX_LOOP_HEADER,
                           statement_begin)) goto cleanup;
        if (map) {
          map->origins[header_owner].kind = HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL;
          map->origins[header_owner].source_begin = statement_begin;
          map->origins[header_owner].source_end = ctx->sb->len;
        }
      }
    } else if (owner->opcode == USIL_OP_IF) {
      ASTExpr *condition =
          source_expression(ctx, &plan, i, 0, 1, AST_SCALAR_BOOL);
      if (!condition)
        goto cleanup;
      if (!hlsl_source_quality_observe_expression(ctx, condition, i)) {
        ast_free_expr(condition);
        goto cleanup;
      }
      sb_append(ctx->sb, "[branch] if (");
      const size_t condition_begin = ctx->sb->len;
      ast_format_expr(condition, ctx->sb);
      ast_free_expr(condition);
      if (!record_expression(ctx, i, condition_begin)) goto cleanup;
      sb_append(ctx->sb, ") {\n");
      ctx->indent += 4;
      hlsl_source_quality_emission(ctx, 0, true, i);
      if (map) {
        map->origins[i].kind = HLSL_EXPRESSION_ORIGIN_CONTROL;
        map->origins[i].source_begin = statement_begin;
        map->origins[i].source_end = ctx->sb->len;
      }
    } else if (owner->opcode == USIL_OP_ENDLOOP ||
               owner->opcode == USIL_OP_ENDIF) {
      sb_append(ctx->sb, "}\n");
      hlsl_source_quality_emission(ctx, 0, true, i);
      if (map) {
        map->origins[i].kind = owner->opcode == USIL_OP_ENDLOOP
                                   ? HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL
                                   : HLSL_EXPRESSION_ORIGIN_CONTROL;
        map->origins[i].source_begin = statement_begin;
        map->origins[i].source_end = ctx->sb->len;
      }
    } else if (owner->opcode == USIL_OP_ELSE) {
      sb_append(ctx->sb, "} else {\n");
      ctx->indent += 4;
      hlsl_source_quality_emission(ctx, 0, true, i);
      if (map) {
        map->origins[i].kind = HLSL_EXPRESSION_ORIGIN_CONTROL;
        map->origins[i].source_begin = statement_begin;
        map->origins[i].source_end = ctx->sb->len;
      }
    } else if (owner->opcode == USIL_OP_GEOMETRY_APPEND ||
               owner->opcode == USIL_OP_GEOMETRY_RESTART_STRIP) {
      /* The ordinary effect helper emits its own indentation. */
      if (!hlsl_emit_high_level_geometry_effect(ctx, i))
        goto cleanup;
      if (map) {
        map->origins[i].kind = HLSL_EXPRESSION_ORIGIN_EFFECT;
        map->origins[i].source_begin = statement_begin;
        map->origins[i].source_end = ctx->sb->len;
      }
    } else {
      ASTExpr *expression = instruction_expression(ctx, &plan, i);
      if (!expression)
        goto cleanup;
      const DXBCOperand *destination = &owner->operands[0];
      uint8_t mask = usil_operand_destination_lane_mask(destination);
      if (destination->type == OPERAND_TYPE_OUTPUT) {
        const char *name =
            hlsl_high_level_output_name(ctx, destination->register_index);
        const DXBCSignatureElement *field = NULL;
        for (int f = 0; f < ctx->program->output_count; ++f)
          if (ctx->program->outputs[f].register_id ==
              (uint32_t)destination->register_index)
            field = &ctx->program->outputs[f];
        sb_appendf(ctx->sb, "%s.%s", ctx->high_level_output_variable, name);
        if (mask != field->mask) {
          sb_append_char(ctx->sb, '.');
          for (int lane = 0; lane < 4; ++lane)
            if (mask & (1u << lane))
              sb_append_char(ctx->sb, "xyzw"[lane]);
        }
      } else {
        const Value *value = &plan.values[i];
        const char *type = value->type == AST_SCALAR_BOOL     ? "bool"
                           : value->type == AST_SCALAR_SINT32 ? "int"
                                                              : "float";
        sb_appendf(ctx->sb, "const %s", type);
        if (value->width > 1)
          sb_appendf(ctx->sb, "%d", value->width);
        sb_appendf(ctx->sb, " %s", value->name);
      }
      if (!hlsl_source_quality_observe_expression(ctx, expression, i)) {
        ast_free_expr(expression);
        goto cleanup;
      }
      sb_append(ctx->sb, " = ");
      const size_t expression_begin = ctx->sb->len;
      ast_format_expr(expression, ctx->sb);
      if (map) {
        map->origins[i].kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
        map->origins[i].source_begin = expression_begin;
        map->origins[i].source_end = ctx->sb->len;
      }
      ast_free_expr(expression);
      if (!record_expression(ctx, i, expression_begin)) goto cleanup;
      sb_append(ctx->sb, ";\n");
      hlsl_source_quality_emission(ctx, 0, true, i);
    }
    if (owner->opcode != USIL_OP_LOOP &&
        !record_syntax(ctx, i, syntax_kind(&plan, owner, i), statement_begin))
      goto cleanup;
    if (!sb_ok(ctx->sb)) goto cleanup;
  }
  /* The complete stage-entry syntax/declaration inventory remains separate
   * from these instruction-owned source spans. Neither certifies execution. */
  success = sb_ok(ctx->sb);
cleanup:
  free(plan.variable_value);
  free(plan.variable_component);
  return success;
}

/* A source map is structural provenance. This matcher reconstructs the CFG/SSA
 * induction owners and validates their shared header span, but cannot replace
 * the metadata type/range proof or the exact compiler comparison. */
bool hlsl_geometry_control_flow_source_map_matches(
    const HLSLExpressionSourceMap *map, const USILProgram *program,
    const char *source) {
  if (!map || !map->complete || !source ||
      !hlsl_geometry_control_flow_admission(
          program, HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE) ||
      map->count != (size_t)program->instruction_count)
    return false;
  for (int instruction = 0; instruction < program->instruction_count;
       ++instruction) {
    const USILInstruction *owner = &program->instructions[instruction];
    if (owner->precise_mask || owner->saturate ||
        !usil_instruction_shape_valid(program, owner) ||
        (instruction &&
         owner->source_instruction_index <=
             program->instructions[instruction - 1].source_instruction_index))
      return false;
    for (int operand = 0; operand < owner->operand_count; ++operand)
      if (!hlsl_lift_operand_is_plain(&owner->operands[operand]) ||
          (owner->operands[operand].type == OPERAND_TYPE_OUTPUT &&
           (owner->operands[operand].register_index < 0 ||
            owner->operands[operand].register_index >= 32)))
        return false;
  }
  HLSLEmitterContext *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return false;
  ctx->program = program;
  ctx->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
  ctx->high_level_geometry = true;
  bool valid = false;
  Plan plan = {.loop = -1};
  if (!build_control_flow_graph(ctx) || !compute_dominance(&ctx->cfg) ||
      !build_hlsl_ssa_graph(ctx))
    goto cleanup;
  for (int instruction = 0; instruction < program->instruction_count;
       ++instruction)
    if (program->instructions[instruction].opcode == USIL_OP_LOOP &&
        !match_loop(ctx, &plan, instruction, false))
      goto cleanup;
  if (plan.loop < 0 || !output_initialized_at_appends(ctx))
    goto cleanup;
  const size_t source_length = strlen(source);
  const HLSLExpressionOrigin *header = &map->origins[plan.loop];
  for (int instruction = 0; instruction < program->instruction_count;
       ++instruction) {
    const USILInstruction *owner = &program->instructions[instruction];
    const HLSLExpressionOrigin *origin = &map->origins[instruction];
    USILOperandUseInfo use;
    uint8_t lanes = 0;
    if (owner->operand_count &&
        usil_instruction_operand_use(program, owner, 0, &use) &&
        use.use == USIL_OPERAND_USE_DESTINATION)
      lanes = usil_operand_destination_lane_mask(&owner->operands[0]);
    if (origin->instruction_index != instruction ||
        origin->source_instruction_index != owner->source_instruction_index ||
        origin->destination_lanes != lanes ||
        !hlsl_expression_origin_ranges_valid(origin, source_length))
      goto cleanup;
    if (instruction == plan.initial || instruction == plan.loop ||
        instruction == plan.compare || instruction == plan.test ||
        instruction == plan.increment) {
      if (origin->kind != HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL ||
          origin->source_begin != header->source_begin ||
          origin->source_end != header->source_end)
        goto cleanup;
      continue;
    }
    switch (owner->opcode) {
    case USIL_OP_MOV:
    case USIL_OP_ADD:
    case USIL_OP_MUL:
    case USIL_OP_MAD:
    case USIL_OP_DIV:
    case USIL_OP_IMAX:
    case USIL_OP_IMIN:
    case USIL_OP_ITOF:
    case USIL_OP_GE:
    case USIL_OP_IGE:
      if (!lanes || origin->kind != HLSL_EXPRESSION_ORIGIN_EXPRESSION)
        goto cleanup;
      break;
    case USIL_OP_IF:
    case USIL_OP_ELSE:
    case USIL_OP_ENDIF:
      if (origin->kind != HLSL_EXPRESSION_ORIGIN_CONTROL)
        goto cleanup;
      break;
    case USIL_OP_ENDLOOP:
      if (instruction != plan.end ||
          origin->kind != HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL)
        goto cleanup;
      break;
    case USIL_OP_GEOMETRY_APPEND:
    case USIL_OP_GEOMETRY_RESTART_STRIP:
      if (origin->kind != HLSL_EXPRESSION_ORIGIN_EFFECT || lanes ||
          !hlsl_high_level_geometry_effect_supported(ctx, instruction))
        goto cleanup;
      break;
    case USIL_OP_NOP:
      if (origin->kind != HLSL_EXPRESSION_ORIGIN_NOP)
        goto cleanup;
      break;
    case USIL_OP_RET:
      if (instruction != program->instruction_count - 1 ||
          origin->kind != HLSL_EXPRESSION_ORIGIN_RETURN)
        goto cleanup;
      break;
    default:
      goto cleanup;
    }
  }
  valid = true;
cleanup:
  free_hlsl_ssa_graph(ctx);
  free_control_flow_graph(ctx);
  free(ctx);
  return valid;
}
