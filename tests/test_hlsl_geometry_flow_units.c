// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_geometry_flow.h"
#include "translation/hlsl_source_quality.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__,          \
              #condition);                                                     \
      return false;                                                            \
    }                                                                          \
  } while (0)
#define INSTRUCTION(opcode, length)                                            \
  ((uint32_t)(opcode) | (uint32_t)(length) << 24u)

static void write_u32(uint8_t *bytes, uint32_t value) {
  for (unsigned index = 0; index < 4; ++index)
    bytes[index] = (uint8_t)(value >> (8 * index));
}

static size_t write_signature(uint8_t *bytes, bool output) {
  memcpy(bytes, output ? "OSGN" : "ISGN", 4);
  const uint32_t count = output ? 2 : 1;
  const size_t payload_size = 8 + count * 24 + sizeof("SV_POSITION") +
                              (output ? sizeof("TEXCOORD") : 0);
  write_u32(bytes + 4, (uint32_t)payload_size);
  uint8_t *payload = bytes + 8;
  write_u32(payload, count);
  write_u32(payload + 4, 8);
  const uint32_t names = 8 + count * 24;
  for (unsigned index = 0; index < count; ++index) {
    uint8_t *element = payload + 8 + index * 24;
    write_u32(element, names + (index ? (uint32_t)sizeof("SV_POSITION") : 0));
    write_u32(element + 4, 0);
    write_u32(element + 8, index ? 0 : 1);
    write_u32(element + 12, 3);
    write_u32(element + 16, index);
    const uint32_t mask = index ? 3u : 15u;
    write_u32(element + 20,
              output ? mask | ((15u & ~mask) << 8u) : mask | mask << 8u);
  }
  memcpy(payload + names, "SV_POSITION", sizeof("SV_POSITION"));
  if (output)
    memcpy(payload + names + sizeof("SV_POSITION"), "TEXCOORD",
           sizeof("TEXCOORD"));
  return 8 + payload_size;
}

/* A generic controlled token program, independent of captured fixture bytes:
 * signed clamped bound, scalar predicate, partial semantic field writes and
 * one anchored Append/Cut per iteration. Parsed authority is never forged. */
static uint8_t *controlled_dxbc(int variant, size_t *out_size) {
  uint32_t words[192];
  size_t count = 0;
#define WORD(value) words[count++] = (value)
  WORD(INSTRUCTION(97, 5));
  WORD(0x002010f2);
  WORD(1);
  WORD(0);
  WORD(1);
  WORD(INSTRUCTION(93, 1) | 1u << 11u);
  WORD(INSTRUCTION(143, 3));
  WORD(0x00110000);
  WORD(0);
  WORD(INSTRUCTION(92, 1) | 5u << 11u);
  WORD(INSTRUCTION(103, 4));
  WORD(0x001020f2);
  WORD(0);
  WORD(1);
  WORD(INSTRUCTION(101, 3));
  WORD(0x00102032);
  WORD(1);
  WORD(INSTRUCTION(94, 2));
  WORD(2);
  WORD(INSTRUCTION(104, 2));
  WORD(3);
  WORD(INSTRUCTION(29, 8));
  WORD(0x00100012);
  WORD(2);
  WORD(0x0020103a);
  WORD(0);
  WORD(0);
  WORD(0x00004001);
  WORD(0);
  WORD(INSTRUCTION(31, 3) | 1u << 18u);
  WORD(0x0010000a);
  WORD(2);
  WORD(INSTRUCTION(36, 7));
  WORD(0x00100022);
  WORD(2);
  WORD(0x00004001);
  WORD(2);
  WORD(0x00004001);
  WORD(0);
  WORD(INSTRUCTION(37, 7));
  WORD(0x00100022);
  WORD(2);
  WORD(0x0010001a);
  WORD(2);
  WORD(0x00004001);
  WORD(2);
  WORD(INSTRUCTION(54, 5));
  WORD(0x00100012);
  WORD(0);
  WORD(0x00004001);
  WORD(0);
  WORD(INSTRUCTION(48, 1));
  WORD(INSTRUCTION(33, 7));
  WORD(0x00100022);
  WORD(0);
  WORD(0x0010000a);
  WORD(0);
  WORD(0x0010001a);
  WORD(2);
  WORD(INSTRUCTION(3, 3) | 1u << 18u);
  WORD(0x0010001a);
  WORD(0);
  WORD(INSTRUCTION(43, 5));
  WORD(0x00100042);
  WORD(0);
  WORD(0x0010000a);
  WORD(0);
  WORD(INSTRUCTION(56, 8));
  WORD(0x00100032);
  WORD(1);
  WORD(0x00201e46);
  WORD(0);
  WORD(0);
  WORD(0x00004001);
  WORD(0x3f000000);
  WORD(INSTRUCTION(0, 7));
  WORD(0x00100012);
  WORD(1);
  WORD(0x0010000a);
  WORD(1);
  WORD(0x0010002a);
  WORD(0);
  WORD(INSTRUCTION(54, 5));
  WORD(0x00102032);
  WORD(0);
  WORD(0x00100e46);
  WORD(1);
  WORD(INSTRUCTION(54, 6));
  WORD(0x001020c2);
  WORD(0);
  WORD(0x00201e46);
  WORD(0);
  WORD(0);
  WORD(INSTRUCTION(54, 8));
  WORD(0x00102032);
  WORD(1);
  WORD(0x00004002);
  WORD(0);
  WORD(0x3f800000);
  WORD(0);
  WORD(0);
  WORD(INSTRUCTION(117, 3));
  WORD(0x00110000);
  WORD(0);
  WORD(INSTRUCTION(118, 3));
  WORD(0x00110000);
  WORD(0);
  WORD(INSTRUCTION(30, 7));
  WORD(0x00100012);
  WORD(0);
  WORD(0x0010000a);
  WORD(0);
  WORD(0x00004001);
  WORD(1);
  WORD(INSTRUCTION(22, 1));
  if (variant == 1) {
    WORD(INSTRUCTION(18, 1));
    WORD(INSTRUCTION(58, 1));
  }
  WORD(INSTRUCTION(21, 1));
  WORD(INSTRUCTION(62, 1));
#undef WORD
  uint8_t bytes[1536] = {0};
  memcpy(bytes, "DXBC", 4);
  write_u32(bytes + 20, 1);
  write_u32(bytes + 28, 3);
  size_t offset = 44;
  for (unsigned signature = 0; signature < 2; ++signature) {
    write_u32(bytes + 32 + signature * 4, (uint32_t)offset);
    offset += write_signature(bytes + offset, signature != 0);
    offset = (offset + 3u) & ~(size_t)3u;
  }
  write_u32(bytes + 40, (uint32_t)offset);
  memcpy(bytes + offset, "SHEX", 4);
  write_u32(bytes + offset + 4, (uint32_t)(count + 2) * 4);
  write_u32(bytes + offset + 8, 0x00020050);
  write_u32(bytes + offset + 12, (uint32_t)count + 2);
  for (size_t index = 0; index < count; ++index)
    write_u32(bytes + offset + 16 + index * 4, words[index]);
  const size_t size = offset + 16 + count * 4;
  write_u32(bytes + 24, (uint32_t)size);
  if (!dxbc_compute_hash(bytes, size, bytes + 4))
    return NULL;
  uint8_t *result = malloc(size);
  if (!result)
    return NULL;
  memcpy(result, bytes, size);
  *out_size = size;
  return result;
}

typedef struct {
  DXBCDocument document;
  DXBCContainer semantic;
  DXBCStageContract contract;
  USILProgram program;
  HLSLExpressionSourceMap map;
  HLSLSourceQualityResult quality;
  bool reject_expression;
  bool reject_emission;
  int rejected_instruction;
} Fixture;

static bool fixture_init_variant(Fixture *fixture, int variant) {
  memset(fixture, 0, sizeof(*fixture));
  dxbc_document_init(&fixture->document);
  dxbc_stage_contract_init(&fixture->contract);
  size_t size = 0;
  uint8_t *bytes = controlled_dxbc(variant, &size);
  CHECK(bytes);
  DXBCDocumentDiagnostic document_diagnostic;
  DXBCStageContractDiagnostic contract_diagnostic;
  CHECK(dxbc_document_parse(&fixture->document, bytes, size,
                            &document_diagnostic));
  free(bytes);
  CHECK(dxbc_document_decode_semantic(&fixture->document, &fixture->semantic));
  CHECK(dxbc_stage_contract_decode(&fixture->document, &fixture->semantic,
                                   &fixture->contract, &contract_diagnostic));
  CHECK(usil_translate_with_stage_contract(
      &fixture->program, &fixture->semantic, &fixture->contract));
  CHECK(fixture->program.instruction_count == (variant == 1 ? 22 : 20));
  return true;
}
static bool fixture_init(Fixture *fixture) {
  return fixture_init_variant(fixture, 0);
}
static void fixture_free(Fixture *fixture) {
  usil_free(&fixture->program);
  dxbc_stage_contract_free(&fixture->contract);
  dxbc_free(&fixture->semantic);
  dxbc_document_free(&fixture->document);
}
static bool observe(void *context,
                    const HLSLSourceQualityObservation *observation) {
  const Fixture *fixture = context;
  const HLSLSourceQualityFacts *facts = &observation->facts;
  if (facts->instruction_index >= 0 &&
      (facts->instruction_index >= fixture->program.instruction_count ||
       facts->source_instruction_index !=
           fixture->program.instructions[facts->instruction_index]
               .source_instruction_index))
    return false;
  if (fixture->reject_emission && observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION &&
      facts->instruction_index == fixture->rejected_instruction) return false;
  return !fixture->reject_expression ||
         observation->kind != HLSL_SOURCE_OBSERVATION_EXPRESSION;
}

static bool emit(Fixture *fixture, StringBuilder *source) {
  sb_init(source);
  HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
  options.source_quality = &fixture->quality;
  options.expression_source_map = &fixture->map;
  options.source_quality_observer = observe;
  options.source_quality_observer_context = fixture;
  return hlsl_emit_with_options(&fixture->program, source, NULL, NULL, NULL,
                                &options);
}
static bool typed_flow_and_owners(void) {
  Fixture fixture;
  CHECK(fixture_init(&fixture));
  StringBuilder source;
  CHECK(emit(&fixture, &source));
  CHECK(strstr(source.buf, "const bool value_i0") &&
        strstr(source.buf, "const int value_i2 = max(") &&
        strstr(
            source.buf,
            "[loop] for (int index_i5 = 0; index_i5 < value_i3; ++index_i5)") &&
        strstr(source.buf, "output.clipPosition_1.xy") &&
        strstr(source.buf, "output.texcoord0") &&
        strstr(source.buf, "stream.Append(output);") &&
        !strstr(source.buf, " r0") && !strstr(source.buf, " o0"));
  CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
        fixture.quality.counts.incomplete_units == 0 &&
        !fixture.quality.counts.unknown_provenance &&
        !fixture.quality.counts.residual_total);
  CHECK(fixture.map.complete &&
        hlsl_expression_source_map_matches(&fixture.map, &fixture.program,
                                           source.buf));
  StringBuilder unobserved;
  sb_init(&unobserved);
  HLSLEmitOptions unobserved_options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
  CHECK(hlsl_emit_with_options(&fixture.program, &unobserved, NULL, NULL, NULL,
                              &unobserved_options));
  CHECK(strcmp(source.buf, unobserved.buf) == 0);
  sb_free(&unobserved);
  const int header[] = {4, 5, 6, 7, 16};
  for (size_t i = 0; i < sizeof(header) / sizeof(header[0]); ++i)
    CHECK(fixture.map.origins[header[i]].kind ==
              HLSL_EXPRESSION_ORIGIN_LOOP_CONTROL &&
          fixture.map.origins[header[i]].source_begin ==
              fixture.map.origins[5].source_begin &&
          fixture.map.origins[header[i]].source_end ==
              fixture.map.origins[5].source_end);
  for (int mutation = 0; mutation < 5; ++mutation) {
    HLSLExpressionSourceMap changed = fixture.map;
    if (mutation == 0)
      changed.origins[6].kind = HLSL_EXPRESSION_ORIGIN_EXPRESSION;
    if (mutation == 1)
      ++changed.origins[16].source_begin;
    if (mutation == 2)
      changed.origins[14].kind = HLSL_EXPRESSION_ORIGIN_DEAD;
    if (mutation == 3)
      ++changed.origins[15].source_instruction_index;
    if (mutation == 4)
      changed.origins[12].destination_lanes = 3;
    CHECK(!hlsl_expression_source_map_matches(&changed, &fixture.program,
                                              source.buf));
  }
  sb_free(&source);
  fixture.reject_expression = true;
  CHECK(!emit(&fixture, &source) && !fixture.map.complete &&
        fixture.quality.classification == HLSL_SOURCE_QUALITY_FAILED);
  sb_free(&source);
  fixture_free(&fixture);
  return true;
}
static bool flow_rejections(void) {
  for (int mutation = 0; mutation < 16; ++mutation) {
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    USILInstruction *instructions = fixture.program.instructions;
    if (mutation == 0) {
      instructions[2].operands[1].imm_values[0] = 3;
      instructions[3].operands[2].imm_values[0] = 3;
    }
    if (mutation == 1)
      instructions[16].operands[2].imm_values[0] = 2;
    if (mutation == 2)
      instructions[4].operands[1].imm_values[0] = UINT32_MAX;
    if (mutation == 3)
      instructions[6].operands[1] = instructions[6].operands[2];
    if (mutation == 4)
      instructions[7].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
    if (mutation == 5)
      instructions[12].opcode = USIL_OP_NOP, instructions[12].operand_count = 0;
    if (mutation == 6)
      instructions[11].operands[1] = instructions[1].operands[0];
    if (mutation == 7)
      instructions[6].operand_count = 1;
    if (mutation == 8)
      instructions[16].operand_count = 1;
    if (mutation == 9)
      instructions[14].source_instruction_index++;
    if (mutation == 10) instructions[7].opcode = USIL_OP_BREAK, instructions[7].operand_count = 0;
    if (mutation == 11) instructions[2].opcode = USIL_OP_SWITCH, instructions[2].operand_count = 1;
    if (mutation == 12) instructions[2].opcode = USIL_OP_LOOP, instructions[2].operand_count = 0;
    if (mutation == 13) instructions[2].opcode = USIL_OP_CONTINUEC, instructions[2].operand_count = 1;
    if (mutation == 14) instructions[1].condition_test = DXBC_INSTRUCTION_TEST_ZERO;
    if (mutation == 15) instructions[2].opcode = USIL_OP_RET, instructions[2].operand_count = 0;
    StringBuilder source;
    const bool emitted = emit(&fixture, &source);
    if (emitted)
      fprintf(stderr, "Mutation %d unexpectedly emitted.\n", mutation);
    CHECK(!emitted);
    CHECK(!fixture.map.complete &&
          fixture.quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    fixture_free(&fixture);
  }
  return true;
}
/* Exercise the private receipts before finalization without adding a
 * production mutation hook. All semantic authority comes from the parsed
 * controlled DXBC above and the same CFG/SSA/value/interface modules. */
static HLSLEmitterContext *inventory_context(Fixture *fixture, StringBuilder *source,
                                            HLSLEmitDiagnostic *diagnostic) {
  HLSLEmitterContext *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  sb_init(source);
  hlsl_emit_diagnostic_init(diagnostic);
  ctx->program = &fixture->program;
  ctx->diagnostic = diagnostic;
  ctx->emit_mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
  ctx->is_geometry = true;
  ctx->high_level_geometry = true;
  ctx->high_level_interface = true;
  ctx->preferred_input_struct_name = "appdata";
  ctx->preferred_output_struct_name = "v2f";
  ctx->entry_point_name = "main";
  ctx->indent = 4;
  ctx->sb = source;
  if (!build_control_flow_graph(ctx) || !compute_dominance(&ctx->cfg) ||
      !build_cbuffer_emission_layouts(ctx) || !analyze_block_nesting(ctx) ||
      !build_hlsl_ssa_graph(ctx) || !build_component_provenance(ctx) ||
      !build_hlsl_use_def_graph(ctx) || !analyze_lane_value_types(ctx) ||
      !hlsl_prepare_high_level_interface(ctx) ||
      !hlsl_prepare_high_level_functions(ctx)) {
    fputs("Could not prepare inventory context.\n", stderr);
    abort();
  }
  HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
  options.source_quality = &fixture->quality;
  options.source_quality_observer = observe;
  options.source_quality_observer_context = fixture;
  if (!hlsl_source_quality_initialize(ctx, &options)) abort();
  return ctx;
}

static void inventory_context_free(HLSLEmitterContext *ctx) {
  hlsl_source_quality_finish_emission(ctx);
  hlsl_geometry_control_flow_inventory_free(ctx);
  free_d3dcompiler_model(ctx);
  free_cbuffer_emission_layouts(ctx);
  free_lane_value_types(ctx);
  free_hlsl_use_def_graph(ctx);
  free_component_provenance(ctx);
  free_hlsl_ssa_graph(ctx);
  free_control_flow_graph(ctx);
  free(ctx);
}

static bool inventory_emit(HLSLEmitterContext *ctx) {
  CHECK(hlsl_source_quality_inventory_supported(ctx));
  CHECK(hlsl_source_quality_begin_entry(ctx, true));
  /* Include every active prelude/function/declaration path in the comparison;
   * no resource/CB/helper dependency is supplied by this controlled program. */
  emit_comments_and_icb(ctx);
  hlsl_source_quality_emission(ctx, 0, false, -1);
  emit_cbuffers(ctx);
  emit_resources(ctx);
  emit_exact_structural_helpers(ctx);
  CHECK(emit_high_level_functions(ctx));
  emit_io_structs(ctx, "appdata", "v2f");
  bool inputs_used[HLSL_SM5_IO_REGISTER_COUNT] = {false};
  bool outputs_used[HLSL_SM5_IO_REGISTER_COUNT] = {false};
  emit_entry_point_declarations(ctx, "main", "appdata", "v2f", inputs_used, outputs_used);
  CHECK(hlsl_geometry_control_flow_emit(ctx));
  emit_return_block(ctx);
  CHECK(sb_ok(ctx->sb));
  CHECK(hlsl_source_quality_interface_inventory_complete(ctx));
  CHECK(hlsl_geometry_control_flow_inventory_complete(ctx));
  return true;
}

static bool empty_else_and_nop_inventory(void) {
  Fixture fixture;
  CHECK(fixture_init_variant(&fixture, 1));
  StringBuilder source;
  HLSLEmitDiagnostic diagnostic;
  HLSLEmitterContext *ctx = inventory_context(&fixture, &source, &diagnostic);
  CHECK(ctx && inventory_emit(ctx));
  HLSLGeometryFlowSourceInventory *inventory = ctx->geometry_flow_source_inventory;
  CHECK(inventory->records[18].kind == HLSL_GEOMETRY_FLOW_SYNTAX_ELSE);
  CHECK(inventory->records[19].kind == HLSL_GEOMETRY_FLOW_SYNTAX_NOP &&
        inventory->records[19].source_begin == inventory->records[19].source_end);
  CHECK(strstr(source.buf, "} else {\n"));
  hlsl_source_quality_finish_emission(ctx);
  CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
        !fixture.quality.counts.incomplete_units && !fixture.quality.counts.unknown_provenance &&
        !fixture.quality.counts.residual_total);
  inventory_context_free(ctx);
  sb_free(&source);
  fixture_free(&fixture);
  return true;
}

static bool syntax_ledger_mutations(void) {
  /* Missing owners, altered kinds/raw ownership/lanes/ranges, folded header
   * overlap, absent AST observations and mutated source cannot close a unit. */
  for (int mutation = -1; mutation < 35; ++mutation) {
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    StringBuilder source;
    HLSLEmitDiagnostic diagnostic;
    HLSLEmitterContext *ctx = inventory_context(&fixture, &source, &diagnostic);
    CHECK(ctx && inventory_emit(ctx));
    HLSLGeometryFlowSourceInventory *inventory = ctx->geometry_flow_source_inventory;
    CHECK(inventory && inventory->recorded_instructions == ((UINT64_C(1) << 20) - 1u));
    if (mutation == 0) inventory->recorded_instructions &= ~(UINT64_C(1) << 1);
    if (mutation == 1) inventory->records[1].kind = HLSL_GEOMETRY_FLOW_SYNTAX_EXPRESSION;
    if (mutation == 2) ++inventory->records[14].source_instruction_index;
    if (mutation == 3) inventory->records[12].destination_lanes = 3;
    if (mutation == 4) ++inventory->records[1].source_begin;
    if (mutation == 5) inventory->records[18].source_end = source.len + 1;
    if (mutation == 6) ++inventory->records[16].source_begin;
    if (mutation == 7) inventory->emitted_expressions &= ~(UINT64_C(1) << 1);
    if (mutation == 8) inventory->emitted_expressions |= UINT64_C(1) << 14;
    if (mutation == 9) ++inventory->instruction_count;
    if (mutation == 10) ++inventory->increment;
    if (mutation == 11) source.buf[inventory->records[1].source_begin] = '!';
    if (mutation == 12) ++inventory->source_begin;
    if (mutation == 13) inventory->records[14].source_begin = inventory->records[13].source_begin;
    if (mutation == 14) inventory->records[19].source_end = inventory->records[19].source_begin;
    if (mutation == 15) inventory->recorded_instructions |= UINT64_C(1) << 20;
    if (mutation == 16) ctx->high_level_geometry_attribute_emitted = false;
    if (mutation == 17) ctx->high_level_geometry_input_struct_emitted = false;
    if (mutation == 18) ctx->high_level_output_struct_emitted = false;
    if (mutation == 19) ctx->high_level_result_local_emitted = false;
    if (mutation == 20) ctx->high_level_geometry_statements_emitted &= ~(UINT64_C(1) << 12);
    if (mutation == 21) ctx->high_level_return_block_emitted = false;
    if (mutation == 22) --inventory->interface_record_count;
    if (mutation == 23) inventory->interface_records[0].kind = HLSL_GEOMETRY_FLOW_INTERFACE_OUTPUT_FIELD;
    if (mutation == 24) ++inventory->interface_records[2].field_index;
    if (mutation == 25) ++inventory->interface_records[8].source_begin;
    if (mutation == 26) inventory->interface_records[7].source_end = source.len + 1;
    if (mutation == 27) source.buf[inventory->interface_records[5].source_begin] = '!';
    if (mutation == 28) inventory->interface_record_count = 73;
    if (mutation == 29) ++inventory->interface_begin;
    if (mutation == 30) inventory->interface_records[8].source_end = inventory->interface_records[8].source_begin;
    if (mutation == 31) inventory->interface_records[2].field_index = 1;
    if (mutation == 32) ++inventory->interface_records[7].source_digest;
    if (mutation == 33) {
      HLSLGeometryFlowInterfaceRecord saved = inventory->interface_records[5];
      inventory->interface_records[5] = inventory->interface_records[6];
      inventory->interface_records[6] = saved;
    }
    if (mutation == 34) {
      memmove(inventory->interface_records, inventory->interface_records + 1,
              (--inventory->interface_record_count) * sizeof(*inventory->interface_records));
    }
    CHECK((hlsl_geometry_control_flow_inventory_complete(ctx) &&
           hlsl_source_quality_interface_inventory_complete(ctx)) == (mutation < 0));
    hlsl_source_quality_finish_emission(ctx);
    CHECK(fixture.quality.classification ==
          (mutation < 0 ? HLSL_SOURCE_QUALITY_CLEAN : HLSL_SOURCE_QUALITY_MIXED));
    CHECK(fixture.quality.counts.incomplete_units == (size_t)(mutation >= 0));
    CHECK(!fixture.quality.counts.unknown_provenance && !fixture.quality.counts.residual_total);
    inventory_context_free(ctx);
    sb_free(&source);
    fixture_free(&fixture);
  }
  return true;
}

static bool inactive_compiler_plans(void) {
  Fixture fixture;
  CHECK(fixture_init(&fixture));
  StringBuilder original;
  CHECK(emit(&fixture, &original));
  CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
  for (int mutation = 0; mutation < 3; ++mutation) {
    StringBuilder source;
    HLSLEmitDiagnostic diagnostic;
    HLSLEmitterContext *ctx = inventory_context(&fixture, &source, &diagnostic);
    CHECK(ctx);
    const size_t count = (size_t)fixture.program.instruction_count;
    ctx->compiler_model.swap_binary_operands = calloc(count, sizeof(signed char));
    ctx->compiler_model.preserve_vector_output = calloc(count, sizeof(unsigned char));
    ctx->compiler_model.claim_owner = calloc(count, sizeof(int));
    ctx->compiler_model.tangent_frames = calloc(count, sizeof(HLSLCompilerTangentFrame));
    ctx->compiler_model.screen_positions = calloc(count, sizeof(HLSLCompilerScreenPosition));
    ctx->compiler_model.interleaved_projection_packs =
        calloc(count, sizeof(HLSLCompilerInterleavedProjectionPack));
    ctx->compiler_model.split_matrix_transforms =
        calloc(count, sizeof(HLSLCompilerSplitMatrixTransform));
    CHECK(ctx->compiler_model.swap_binary_operands && ctx->compiler_model.preserve_vector_output &&
          ctx->compiler_model.claim_owner && ctx->compiler_model.tangent_frames &&
          ctx->compiler_model.screen_positions && ctx->compiler_model.interleaved_projection_packs &&
          ctx->compiler_model.split_matrix_transforms);
    for (size_t instruction = 0; instruction < count; ++instruction) {
      ctx->compiler_model.swap_binary_operands[instruction] = (signed char)(mutation & 1);
      ctx->compiler_model.preserve_vector_output[instruction] = (unsigned char)mutation;
      ctx->compiler_model.claim_owner[instruction] = mutation ? (int)instruction : -1;
      ctx->compiler_model.tangent_frames[instruction].valid = mutation != 0;
      ctx->compiler_model.screen_positions[instruction].valid = mutation != 0;
      ctx->compiler_model.interleaved_projection_packs[instruction].valid = mutation != 0;
      ctx->compiler_model.split_matrix_transforms[instruction].valid = mutation != 0;
    }
    ctx->compiler_model.replacement_count = mutation ? 37 : 0;
    CHECK(inventory_emit(ctx));
    CHECK(strcmp(original.buf, source.buf) == 0);
    hlsl_source_quality_finish_emission(ctx);
    CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    inventory_context_free(ctx);
    sb_free(&source);
  }
  sb_free(&original);
  fixture_free(&fixture);
  return true;
}

static bool dependencies_and_callback_rejection(void) {
  for (int mutation = 0; mutation < 7; ++mutation) {
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    StringBuilder source;
    HLSLEmitDiagnostic diagnostic;
    HLSLEmitterContext *ctx = inventory_context(&fixture, &source, &diagnostic);
    CHECK(ctx && hlsl_source_quality_inventory_supported(ctx));
    if (mutation == 0) ctx->unity_uv_helper = true;
    if (mutation == 1) ctx->readable_screen_pos_helper = "ExternalScreenPosition";
    if (mutation == 2) ctx->float4_functions.use_count[0] = 2;
    if (mutation == 3) ctx->cbuffer_layouts_built = false;
    if (mutation == 4) ctx->high_level_geometry_stream_parameter_emitted = true,
                       ctx->high_level_interface_prepared = false;
    if (mutation == 5) ctx->indexed_face_basis.valid = true;
    if (mutation == 6) ctx->use_uint_temps = true;
    CHECK(!hlsl_source_quality_inventory_supported(ctx));
    inventory_context_free(ctx);
    sb_free(&source);
    fixture_free(&fixture);
  }
  const int rejected_owners[] = {1, 5, 14, 19};
  for (size_t index = 0; index < sizeof(rejected_owners) / sizeof(rejected_owners[0]); ++index) {
    Fixture fixture;
    CHECK(fixture_init(&fixture));
    fixture.reject_emission = true;
    fixture.rejected_instruction = rejected_owners[index];
    StringBuilder source;
    CHECK(!emit(&fixture, &source));
    CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_FAILED && !fixture.map.complete);
    sb_free(&source);
    fixture_free(&fixture);
  }
  return true;
}

static bool scalar_comparison_ownership(void) {
  const uint32_t zero = 0, one = 1;
  ASTExpr *left = ast_create_literal_bits(&zero, 1, AST_SCALAR_SINT32);
  ASTExpr *right = ast_create_literal_bits(&one, 1, AST_SCALAR_SINT32);
  CHECK(left && right);
  CHECK(!ast_create_comparison(USIL_OP_ADD, left, right));
  CHECK(!ast_create_comparison(USIL_OP_IGE, left, left));
  ASTExpr *comparison = ast_create_comparison(USIL_OP_IGE, left, right);
  CHECK(comparison);
  ASTLogicalValueOrigin origin;
  ast_logical_value_origin_init(&origin);
  origin.complete = true;
  origin.logical_value_id = 42;
  origin.scalar_type = AST_SCALAR_BOOL;
  origin.components = 2;
  CHECK(!ast_set_logical_value_origin(comparison, &origin));
  origin.components = 1;
  origin.scalar_type = AST_SCALAR_SINT32;
  CHECK(!ast_set_logical_value_origin(comparison, &origin));
  origin.scalar_type = AST_SCALAR_BOOL;
  CHECK(ast_set_logical_value_origin(comparison, &origin));
  StringBuilder source;
  sb_init(&source);
  ast_format_expr(comparison, &source);
  CHECK(sb_ok(&source) && strcmp(source.buf, "(0 >= 1)") == 0);
  ast_free_expr(comparison);
  sb_free(&source);
  return true;
}

int main(void) {
  const size_t allocations = g_allocations_count, bytes = g_allocated_bytes;
  if (!scalar_comparison_ownership() || !typed_flow_and_owners() ||
      !flow_rejections() || !empty_else_and_nop_inventory() || !syntax_ledger_mutations() ||
      !inactive_compiler_plans() || !dependencies_and_callback_rejection())
    return 1;
  if (g_allocations_count != allocations || g_allocated_bytes != bytes) {
    fputs("Flow unit leaked tracked allocations.\n", stderr);
    return 1;
  }
  puts("Geometry flow units passed.");
  return 0;
}
