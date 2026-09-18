#include "common/string_builder.h"
#include "common/file_io.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil.h"
#include "test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DXBC_WIREFRAME_GS_FIXTURE
#define DXBC_WIREFRAME_GS_FIXTURE \
  "tests/fixtures/spatial_mapping_wireframe_gs.dxbc.b64"
#endif

#ifndef DXBC_LIMIT_TEST2_GS_FIXTURE
#define DXBC_LIMIT_TEST2_GS_FIXTURE \
  "tests/fixtures/limit_test2_extrusion_gs.dxbc.b64"
#endif

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,     \
              #condition);                                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static void init_signature(DXBCSignatureElement *element, const char *semantic,
                           uint32_t semantic_index, uint32_t component_type,
                           uint32_t register_id, uint8_t mask) {
  memset(element, 0, sizeof(*element));
  snprintf(element->semantic_name, sizeof(element->semantic_name), "%s",
           semantic);
  element->semantic_index = semantic_index;
  if (strcmp(semantic, "SV_POSITION") == 0) {
    element->system_value = 1u;
  } else if (strcmp(semantic, "SV_RenderTargetArrayIndex") == 0) {
    element->system_value = 4u;
  } else if (strcmp(semantic, "SV_Target") == 0) {
    element->system_value = 64u;
  }
  element->component_type = component_type;
  element->register_id = register_id;
  element->mask = mask;
  element->rw_mask = mask;
}

static void init_register(DXBCOperand *operand, DXBCOperandType type,
                          int register_index, uint8_t destination_mask) {
  memset(operand, 0, sizeof(*operand));
  operand->type = type;
  operand->register_index = register_index;
  operand->register_index_dim = 1;
  operand->index_has_immediate[0] = true;
  operand->index_values[0] = (uint32_t)register_index;
  operand->destination_mask = destination_mask;
  operand->swizzle_mode = 1;
  operand->swizzle[0] = 0;
  operand->swizzle[1] = 1;
  operand->swizzle[2] = 2;
  operand->swizzle[3] = 3;
}

static void init_geometry_input(DXBCOperand *operand, int vertex,
                                int register_index, uint8_t mask) {
  init_register(operand, OPERAND_TYPE_INPUT, vertex, mask);
  operand->register_index_dim = 2;
  operand->index_has_immediate[1] = true;
  operand->index_values[1] = (uint32_t)register_index;
  operand->rel_offset0 = register_index;
}

static void init_instruction(DXBCInstruction *instruction, uint32_t opcode,
                             int operand_count) {
  memset(instruction, 0, sizeof(*instruction));
  instruction->opcode = opcode;
  instruction->operand_count = operand_count;
}

static bool build_copy_geometry(DXBCContainer *container,
                                DXBCStageContract *contract,
                                DXBCSignatureElement inputs[2],
                                DXBCSignatureElement outputs[3],
                                DXBCInstruction instructions[7]) {
  memset(container, 0, sizeof(*container));
  dxbc_stage_contract_init(contract);
  snprintf(container->shader_type_model, sizeof(container->shader_type_model),
           "gs_4_0");
  container->has_executable_program = true;
  container->program_type = DXBC_PROGRAM_TYPE_GEOMETRY;
  container->major_version = 4;
  container->minor_version = 0;
  init_signature(&inputs[0], "SV_POSITION", 0, 3, 0, 0xf);
  init_signature(&inputs[1], "TEXCOORD", 0, 3, 1, 0x3);
  container->input_signature = inputs;
  container->input_signature_count = 2;
  container->input_signature_alloc = 2;
  init_signature(&outputs[0], "SV_POSITION", 0, 3, 0, 0xf);
  init_signature(&outputs[1], "TEXCOORD", 0, 3, 1, 0x3);
  init_signature(&outputs[2], "SV_RenderTargetArrayIndex", 0, 1, 2, 0x1);
  container->output_signature = outputs;
  container->output_signature_count = 3;
  container->output_signature_alloc = 3;
  container->instructions = instructions;
  container->instruction_count = 7;
  container->instruction_alloc = 7;

  init_instruction(&instructions[0], 54, 2); /* mov */
  init_register(&instructions[0].operands[0], OPERAND_TYPE_OUTPUT, 0, 0xf0);
  init_geometry_input(&instructions[0].operands[1], 0, 0, 0xf0);
  init_instruction(&instructions[1], 54, 2); /* mov */
  init_register(&instructions[1].operands[0], OPERAND_TYPE_OUTPUT, 1, 0x30);
  init_geometry_input(&instructions[1].operands[1], 0, 1, 0x30);
  instructions[1].operands[1].swizzle[2] = 0;
  instructions[1].operands[1].swizzle[3] = 0;
  init_instruction(&instructions[2], 28, 2); /* ftou */
  init_register(&instructions[2].operands[0], OPERAND_TYPE_TEMP, 0, 0x10);
  memset(&instructions[2].operands[1], 0,
         sizeof(instructions[2].operands[1]));
  instructions[2].operands[1].type = OPERAND_TYPE_IMMEDIATE32;
  instructions[2].operands[1].imm_value_count = 1;
  instructions[2].operands[1].imm_values[0] = UINT32_C(0x3fc00000);
  instructions[2].operands[1].immediate_word_count = 1;
  instructions[2].operands[1].immediate_words[0] = UINT32_C(0x3fc00000);
  init_instruction(&instructions[3], 54, 2); /* mov */
  init_register(&instructions[3].operands[0], OPERAND_TYPE_OUTPUT, 2, 0x10);
  init_register(&instructions[3].operands[1], OPERAND_TYPE_TEMP, 0, 0x10);
  instructions[3].operands[1].swizzle_mode = 2;
  instructions[3].operands[1].swizzle[0] = 0;
  init_instruction(&instructions[4], 19, 0); /* emit */
  init_instruction(&instructions[5], 9, 0);  /* cut */
  init_instruction(&instructions[6], 62, 0); /* ret */

  contract->program_type = DXBC_PROGRAM_TYPE_GEOMETRY;
  contract->shader_model_major = 4;
  contract->shader_model_minor = 0;
  contract->has_input_primitive = true;
  contract->input_primitive = DXBC_INPUT_PRIMITIVE_TRIANGLE;
  contract->has_output_topology = true;
  contract->output_topology = DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP;
  contract->has_max_output_vertex_count = true;
  contract->max_output_vertex_count = 3;
  contract->referenced_stream_mask = 1;
  contract->geometry_effect_count = 2;
  contract->geometry_effect_capacity = 2;
  contract->geometry_effects = mem_alloc(
      2 * sizeof(*contract->geometry_effects));
  if (!contract->geometry_effects) return false;
  memset(contract->geometry_effects, 0,
         2 * sizeof(*contract->geometry_effects));
  contract->geometry_effects[0].kind = DXBC_GEOMETRY_EFFECT_APPEND;
  contract->geometry_effects[0].instruction_index = 4;
  contract->geometry_effects[1].kind =
      DXBC_GEOMETRY_EFFECT_RESTART_STRIP;
  contract->geometry_effects[1].instruction_index = 5;
  return true;
}

static int verify_stage_aware_geometry_projection_and_emission(void) {
  DXBCContainer container;
  DXBCStageContract contract;
  DXBCSignatureElement inputs[2];
  DXBCSignatureElement outputs[3];
  DXBCInstruction instructions[7];
  CHECK(build_copy_geometry(&container, &contract, inputs, outputs,
                            instructions));

  USILProgram without_contract;
  CHECK(!usil_translate(&without_contract, &container));

  USILProgram program;
  CHECK(usil_translate_with_stage_contract(&program, &container, &contract));
  CHECK(program.has_stage_contract);
  CHECK(program.program_type == DXBC_PROGRAM_TYPE_GEOMETRY);
  CHECK(program.geometry.valid);
  CHECK(program.geometry.input_primitive == DXBC_INPUT_PRIMITIVE_TRIANGLE);
  CHECK(program.geometry.output_topology ==
        DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP);
  CHECK(program.geometry.input_vertex_count == 3);
  CHECK(program.geometry.max_output_vertex_count == 3);
  CHECK(program.geometry.effect_count == 2);
  CHECK(program.geometry.output_tuple_state_persists);
  CHECK(program.instructions[0].operands[1].register_index_dim == 2);
  CHECK(program.instructions[0].operands[1].index_values[0] == 0);
  CHECK(program.instructions[0].operands[1].index_values[1] == 0);
  CHECK(program.instructions[4].geometry_effect ==
        USIL_GEOMETRY_EFFECT_APPEND);
  CHECK(program.instructions[5].geometry_effect ==
        USIL_GEOMETRY_EFFECT_RESTART_STRIP);
  CHECK(program.instructions[4].source_instruction_index == 4);
  CHECK(program.instructions[5].source_instruction_index == 5);

  HLSLEmitNames names = {"geom", "gs_input", "gs_output"};
  StringBuilder hlsl;
  sb_init(&hlsl);
  CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, &names));
  CHECK(strstr(hlsl.buf, "[maxvertexcount(3)]") != NULL);
  CHECK(strstr(hlsl.buf, "triangle gs_input input[3]") != NULL);
  CHECK(strstr(hlsl.buf, "TriangleStream<gs_output>") != NULL);
  CHECK(strstr(hlsl.buf, "float4 o2;") != NULL);
  CHECK(strstr(hlsl.buf, "float4 o2 = (float4)0") == NULL);
  CHECK(strstr(hlsl.buf, "output.o2 = asuint(o2.x);") != NULL);
  const char *append = strstr(hlsl.buf, "dxbc_stream.Append(output);");
  const char *restart = strstr(hlsl.buf, "dxbc_stream.RestartStrip();");
  CHECK(append != NULL && restart != NULL && append < restart);
  sb_free(&hlsl);

  /* Explicit/multi-stream geometry is retained by USIL but intentionally
   * rejected by the bounded recompile surface. */
  program.geometry.declared_stream_mask = 1;
  StringBuilder rejected_stream;
  sb_init(&rejected_stream);
  CHECK(!hlsl_emit(&program, &rejected_stream, NULL, NULL, &names));
  sb_free(&rejected_stream);
  program.geometry.declared_stream_mask = 0;

  /* The formatter consumes the two immediate indices directly.  Conflicting
   * cached fields or a relative-index representation are ambiguous and must
   * fail before HLSL emission. */
  DXBCOperand *indexed_input = &program.instructions[0].operands[1];
  indexed_input->index_representations[1] = 3;
  StringBuilder rejected_index;
  sb_init(&rejected_index);
  CHECK(!hlsl_emit(&program, &rejected_index, NULL, NULL, &names));
  sb_free(&rejected_index);
  indexed_input->index_representations[1] = 0;

  /* Arithmetic remains lossless in USIL, but it must not silently enter the
   * byte-exact geometry path until expression-level inversion is proven. */
  const USILOpcode original_opcode = program.instructions[0].opcode;
  program.instructions[0].opcode = USIL_OP_ADD;
  StringBuilder rejected_arithmetic;
  sb_init(&rejected_arithmetic);
  CHECK(!hlsl_emit(&program, &rejected_arithmetic, NULL, NULL, &names));
  sb_free(&rejected_arithmetic);
  program.instructions[0].opcode = original_opcode;

  usil_free(&program);
  dxbc_stage_contract_free(&contract);
  return 0;
}

static int verify_non_geometry_compatibility(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output;
  memset(&program, 0, sizeof(program));
  memset(&instruction, 0, sizeof(instruction));
  init_signature(&output, "SV_Target", 0, 3, 0, 0xf);
  snprintf(program.shader_type_model, sizeof(program.shader_type_model),
           "ps_4_0");
  program.program_type = DXBC_PROGRAM_TYPE_INVALID;
  program.outputs = &output;
  program.output_count = 1;
  program.output_alloc = 1;
  program.instructions = &instruction;
  program.instruction_count = 1;
  program.instruction_alloc = 1;
  instruction.opcode = USIL_OP_MOV;
  instruction.operand_count = 2;
  init_register(&instruction.operands[0], OPERAND_TYPE_OUTPUT, 0, 0xf0);
  memset(&instruction.operands[1], 0, sizeof(instruction.operands[1]));
  instruction.operands[1].type = OPERAND_TYPE_IMMEDIATE32;
  instruction.operands[1].imm_value_count = 4;
  instruction.operands[1].immediate_word_count = 4;
  instruction.operands[1].imm_values[3] = UINT32_C(0x3f800000);
  instruction.operands[1].immediate_words[3] = UINT32_C(0x3f800000);
  StringBuilder hlsl;
  sb_init(&hlsl);
  CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, NULL));
  CHECK(strstr(hlsl.buf, "SV_Target") != NULL);
  sb_free(&hlsl);

  program.has_global_flags = true;
  program.global_flags = 4; /* FORCE_EARLY_DEPTH_STENCIL */
  StringBuilder rejected_flags;
  sb_init(&rejected_flags);
  CHECK(!hlsl_emit(&program, &rejected_flags, NULL, NULL, NULL));
  sb_free(&rejected_flags);
  return 0;
}

static int verify_triangle_edge_distance_structural_lift(void) {
  uint8_t *bytes = NULL;
  size_t size = 0;
  CHECK(test_fixture_decode_base64(DXBC_WIREFRAME_GS_FIXTURE, &bytes,
                                   &size));

  DXBCDocument document;
  DXBCDocumentDiagnostic document_diagnostic;
  DXBCStageContract contract;
  DXBCStageContractDiagnostic contract_diagnostic;
  DXBCContainer semantic;
  dxbc_document_init(&document);
  dxbc_stage_contract_init(&contract);
  memset(&semantic, 0, sizeof(semantic));
  CHECK(dxbc_document_parse(&document, bytes, size, &document_diagnostic));
  CHECK(dxbc_document_decode_semantic(&document, &semantic));
  CHECK(dxbc_stage_contract_decode(&document, &semantic, &contract,
                                   &contract_diagnostic));

  USILProgram program;
  CHECK(usil_translate_with_stage_contract(&program, &semantic, &contract));
  CHECK(hlsl_triangle_edge_distance_lift_matches(&program));
  HLSLEmitNames names = {"geom", "gs_input", "gs_output"};
  StringBuilder hlsl;
  sb_init(&hlsl);
  CHECK(hlsl_emit(&program, &hlsl, NULL, NULL, &names));
  CHECK(strstr(hlsl.buf, "float2 dxbc_edge0 = dxbc_p2 - dxbc_p1;") != NULL);
  CHECK(strstr(hlsl.buf, "dxbc_area / length(dxbc_edge2)") != NULL);
  sb_free(&hlsl);

  /* One opcode mutation invalidates the complete compiler-inverse proof. The
   * ordinary per-instruction arithmetic emitter remains outside the exact GS
   * surface, so the transaction must fail rather than emit a near match. */
  const USILOpcode original = program.instructions[10].opcode;
  program.instructions[10].opcode = USIL_OP_ADD;
  CHECK(!hlsl_triangle_edge_distance_lift_matches(&program));
  StringBuilder rejected;
  sb_init(&rejected);
  CHECK(!hlsl_emit(&program, &rejected, NULL, NULL, &names));
  sb_free(&rejected);
  program.instructions[10].opcode = original;

  usil_free(&program);
  dxbc_free(&semantic);
  dxbc_stage_contract_free(&contract);
  dxbc_document_free(&document);
  mem_free(bytes, size);
  return 0;
}

static void init_serialized_vector(SerializedVariable *variable,
                                   const char *name, uint32_t byte_offset,
                                   uint32_t columns) {
  memset(variable, 0, sizeof(*variable));
  variable->name = name;
  variable->layout[0] = byte_offset;
  variable->layout[3] = columns;
}

static void init_serialized_matrix(SerializedVariable *variable,
                                   const char *name, uint32_t byte_offset) {
  init_serialized_vector(variable, name, byte_offset, 4);
  variable->layout[4] = 1;
}

static int verify_limit_test2_extrusion_structural_lift(void) {
  uint8_t *bytes = NULL;
  size_t size = 0;
  CHECK(test_fixture_decode_base64(DXBC_LIMIT_TEST2_GS_FIXTURE, &bytes,
                                   &size));
  CHECK(size == 1812);

  DXBCDocument document;
  DXBCDocumentDiagnostic document_diagnostic;
  DXBCStageContract contract;
  DXBCStageContractDiagnostic contract_diagnostic;
  DXBCContainer semantic;
  dxbc_document_init(&document);
  dxbc_stage_contract_init(&contract);
  memset(&semantic, 0, sizeof(semantic));
  CHECK(dxbc_document_parse(&document, bytes, size, &document_diagnostic));
  CHECK(dxbc_document_decode_semantic(&document, &semantic));
  CHECK(dxbc_stage_contract_decode(&document, &semantic, &contract,
                                   &contract_diagnostic));

  USILProgram program;
  CHECK(usil_translate_with_stage_contract(&program, &semantic, &contract));
  CHECK(program.geometry.declared_stream_mask == 1);
  CHECK(program.geometry.referenced_stream_mask == 1);
  CHECK(hlsl_extruded_triangle_lift_matches(&program));

  SerializedVariable variables[3];
  init_serialized_vector(&variables[0], "_Extrusion", 32, 1);
  init_serialized_matrix(&variables[1], "unity_ObjectToWorld", 0);
  init_serialized_matrix(&variables[2], "unity_MatrixVP", 272);
  SerializedConstantBuffer buffers[3];
  memset(buffers, 0, sizeof(buffers));
  buffers[0].name = "$Globals";
  buffers[0].size = 48;
  buffers[0].var_count = 1;
  buffers[0].variables = &variables[0];
  buffers[1].name = "UnityPerDraw";
  buffers[1].size = 64;
  buffers[1].var_count = 1;
  buffers[1].variables = &variables[1];
  buffers[2].name = "UnityPerFrame";
  buffers[2].size = 336;
  buffers[2].var_count = 1;
  buffers[2].variables = &variables[2];
  SerializedResourceParam bindings[3];
  memset(bindings, 0, sizeof(bindings));
  for (uint32_t index = 0; index < 3; ++index) {
    bindings[index].name = buffers[index].name;
    bindings[index].bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER;
    bindings[index].bind_index = index;
  }
  SerializedProgramParameters parameters;
  memset(&parameters, 0, sizeof(parameters));
  parameters.cb_count = 3;
  parameters.constant_buffers = buffers;
  parameters.res_count = 3;
  parameters.resources = bindings;

  HLSLEmitNames names = {"geom", "gs_input", "gs_output"};
  HLSLEmitOptions options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  options.omit_unity_builtin_declarations = true;
  StringBuilder hlsl;
  sb_init(&hlsl);
  CHECK(hlsl_emit_with_options(&program, &hlsl, &parameters, NULL, &names,
                               &options));
  CHECK(strstr(hlsl.buf, "[maxvertexcount(6)]") != NULL);
  CHECK(strstr(hlsl.buf,
               "for (int dxbc_i = 0; dxbc_i < 3; ++dxbc_i)") != NULL);
  CHECK(strstr(hlsl.buf, "normalize(cross(input[1].v0 - input[0].v0") !=
        NULL);
  CHECK(strstr(hlsl.buf,
               "dxbc_center.xyz += dxbc_face_normal * _Extrusion;") !=
        NULL);
  CHECK(strstr(hlsl.buf, "dxbc_stream.RestartStrip();") != NULL);
  CHECK(strstr(hlsl.buf, "cbuffer UnityPerDraw") == NULL);
  CHECK(strstr(hlsl.buf, "cbuffer UnityPerFrame") == NULL);
  sb_free(&hlsl);

  /* Admission is a complete graph proof: a single arithmetic mutation or
   * loss of the explicit dcl_stream authority invalidates the lift. */
  const USILOpcode original_opcode = program.instructions[23].opcode;
  program.instructions[23].opcode = USIL_OP_ADD;
  CHECK(!hlsl_extruded_triangle_lift_matches(&program));
  StringBuilder rejected_opcode;
  sb_init(&rejected_opcode);
  CHECK(!hlsl_emit_with_options(&program, &rejected_opcode, &parameters,
                                NULL, &names, &options));
  sb_free(&rejected_opcode);
  program.instructions[23].opcode = original_opcode;

  program.geometry.declared_stream_mask = 0;
  CHECK(!hlsl_extruded_triangle_lift_matches(&program));
  StringBuilder rejected_stream;
  sb_init(&rejected_stream);
  CHECK(!hlsl_emit_with_options(&program, &rejected_stream, &parameters,
                                NULL, &names, &options));
  sb_free(&rejected_stream);
  program.geometry.declared_stream_mask = 1;

  usil_free(&program);
  dxbc_free(&semantic);
  dxbc_stage_contract_free(&contract);
  dxbc_document_free(&document);
  mem_free(bytes, size);
  return 0;
}

int main(void) {
  CHECK(verify_stage_aware_geometry_projection_and_emission() == 0);
  CHECK(verify_non_geometry_compatibility() == 0);
  CHECK(verify_triangle_edge_distance_structural_lift() == 0);
  CHECK(verify_limit_test2_extrusion_structural_lift() == 0);
  puts("geometry USIL tests passed");
  return 0;
}
