// SPDX-License-Identifier: GPL-3.0-only
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter_internal.h"
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
static uint8_t *controlled_dxbc(size_t *out_size) {
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
} Fixture;

static bool fixture_init(Fixture *fixture) {
  memset(fixture, 0, sizeof(*fixture));
  dxbc_document_init(&fixture->document);
  dxbc_stage_contract_init(&fixture->contract);
  size_t size = 0;
  uint8_t *bytes = controlled_dxbc(&size);
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
  CHECK(fixture->program.instruction_count == 20);
  return true;
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
  CHECK(fixture.quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
        fixture.quality.counts.incomplete_units == 1 &&
        !fixture.quality.counts.unknown_provenance &&
        !fixture.quality.counts.residual_total);
  CHECK(fixture.map.complete &&
        hlsl_expression_source_map_matches(&fixture.map, &fixture.program,
                                           source.buf));
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
  for (int mutation = 0; mutation < 10; ++mutation) {
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
      !flow_rejections())
    return 1;
  if (g_allocations_count != allocations || g_allocated_bytes != bytes) {
    fputs("Flow unit leaked tracked allocations.\n", stderr);
    return 1;
  }
  puts("Geometry flow units passed.");
  return 0;
}
