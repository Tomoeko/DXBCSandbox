#include "translation/hlsl_emitter_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,     \
              #condition);                                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static void init_register_operand(DXBCOperand *operand, DXBCOperandType type,
                                  int register_index) {
  memset(operand, 0, sizeof(*operand));
  operand->type = type;
  operand->register_index = register_index;
  operand->register_index_dim = 1;
  operand->index_has_immediate[0] = true;
  operand->index_values[0] = (uint32_t)register_index;
  operand->swizzle_mode = 1;
  operand->swizzle[0] = 0;
  operand->swizzle[1] = 1;
  operand->swizzle[2] = 2;
  operand->swizzle[3] = 3;
  operand->destination_mask = 0xf0;
}

static void init_immediate_operand(DXBCOperand *operand) {
  memset(operand, 0, sizeof(*operand));
  operand->type = OPERAND_TYPE_IMMEDIATE32;
  operand->imm_value_count = 4;
  operand->imm_values[0] = 0x3f800000u;
  operand->imm_values[1] = 0x40000000u;
  operand->imm_values[2] = 0x40400000u;
  operand->imm_values[3] = 0x40800000u;
}

static void init_valid_program(USILProgram *program,
                               USILInstruction *instruction,
                               DXBCSignatureElement *output) {
  memset(program, 0, sizeof(*program));
  memset(instruction, 0, sizeof(*instruction));
  memset(output, 0, sizeof(*output));
  snprintf(program->shader_type_model, sizeof(program->shader_type_model),
           "ps_5_0");
  program->program_type = DXBC_PROGRAM_TYPE_PIXEL;
  program->outputs = output;
  program->output_count = 1;
  program->output_alloc = 1;
  snprintf(output->semantic_name, sizeof(output->semantic_name),
           "SV_Target");
  output->system_value = 64u;
  output->register_id = 0;
  output->mask = 0xfu;
  output->component_type = 3u;
  program->instructions = instruction;
  program->instruction_count = 1;
  program->instruction_alloc = 1;
  instruction->opcode = USIL_OP_MOV;
  instruction->operand_count = 2;
  instruction->source_instruction_index = 37u;
  snprintf(instruction->original_asm, sizeof(instruction->original_asm),
           "mov o0, l(1, 2, 3, 4)");
  init_register_operand(&instruction->operands[0], OPERAND_TYPE_OUTPUT, 0);
  init_immediate_operand(&instruction->operands[1]);
}

static int verify_initialization_and_stable_names(void) {
  HLSLEmitDiagnostic diagnostic;
  memset(&diagnostic, 0x5a, sizeof(diagnostic));
  hlsl_emit_diagnostic_init(&diagnostic);
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_OK);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_NONE);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_NONE);
  CHECK(diagnostic.instruction_index == -1);
  CHECK(diagnostic.source_instruction_index == UINT32_MAX);
  CHECK(diagnostic.opcode == -1);
  CHECK(diagnostic.operand_index == -1);
  CHECK(diagnostic.metadata.record_index == -1);
  CHECK(diagnostic.related_metadata.register_index == -1);
  CHECK(strcmp(hlsl_emit_status_name(HLSL_EMIT_STATUS_INVALID_METADATA),
               "invalid-metadata") == 0);
  CHECK(strcmp(hlsl_emit_phase_name(HLSL_EMIT_PHASE_INSTRUCTION_EMISSION),
               "instruction-emission") == 0);
  CHECK(strcmp(hlsl_emit_reason_name(HLSL_EMIT_REASON_UNSUPPORTED_OPCODE),
               "unsupported-opcode") == 0);
  CHECK(strcmp(hlsl_emit_metadata_kind_name(HLSL_EMIT_METADATA_CBUFFER),
               "constant-buffer") == 0);
  CHECK(strcmp(hlsl_emit_opcode_name(USIL_OP_SINCOS), "sincos") == 0);
  CHECK(strcmp(hlsl_emit_opcode_name(-1), "unknown") == 0);
  return 0;
}

static int verify_invalid_mode_and_legacy_wrapper(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output_signature;
  init_valid_program(&program, &instruction, &output_signature);
  HLSLEmitOptions options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  options.mode = (HLSLEmitMode)99;

  StringBuilder output;
  HLSLEmitDiagnostic diagnostic;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, &options, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_ARGUMENT_VALIDATION);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_INVALID_MODE);
  CHECK(!sb_ok(&output));
  sb_free(&output);

  sb_init(&output);
  CHECK(!hlsl_emit_with_options(&program, &output, NULL, NULL, NULL,
                                &options));
  CHECK(!sb_ok(&output));
  sb_free(&output);
  return 0;
}

static int verify_reserved_identifier_option_shape(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output_signature;
  init_valid_program(&program, &instruction, &output_signature);
  HLSLEmitOptions options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  StringBuilder output;
  HLSLEmitDiagnostic diagnostic;

  options.reserved_preprocessor_identifier_count = 1;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, &options, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_ARGUMENT_VALIDATION);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_INVALID_ARGUMENT);
  sb_free(&output);

  const char *valid[] = {"VALID_KEYWORD", "VALID_KEYWORD"};
  options.reserved_preprocessor_identifiers = valid;
  options.reserved_preprocessor_identifier_count = 0;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, &options, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_ARGUMENT_VALIDATION);
  sb_free(&output);

  const char *null_identifier[] = {NULL};
  options.reserved_preprocessor_identifiers = null_identifier;
  options.reserved_preprocessor_identifier_count = 1;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, &options, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_ARGUMENT_VALIDATION);
  sb_free(&output);

  const char *invalid_identifier[] = {"NOT-A-PREPROCESSOR-NAME"};
  options.reserved_preprocessor_identifiers = invalid_identifier;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, &options, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_ARGUMENT);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_ARGUMENT_VALIDATION);
  sb_free(&output);

  /* The serialized keyword table is indexed authority and may contain the
   * same spelling at distinct indices. Duplicates remain valid reservations. */
  options.reserved_preprocessor_identifiers = valid;
  options.reserved_preprocessor_identifier_count = 2;
  sb_init(&output);
  CHECK(hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, &options, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_OK);
  CHECK(sb_ok(&output));
  sb_free(&output);
  return 0;
}

static int verify_prefailed_output(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output_signature;
  init_valid_program(&program, &instruction, &output_signature);
  StringBuilder output;
  HLSLEmitDiagnostic diagnostic;
  sb_init(&output);
  output.failed = true;
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, NULL, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_OUTPUT_FAILED);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_ARGUMENT_VALIDATION);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_OUTPUT_ALREADY_FAILED);
  sb_free(&output);
  return 0;
}

static int verify_unsupported_opcode_instruction_context(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output_signature;
  init_valid_program(&program, &instruction, &output_signature);
  instruction.opcode = USIL_OP_NOP;
  instruction.operand_count = 0;

  StringBuilder output;
  HLSLEmitDiagnostic first;
  HLSLEmitDiagnostic second;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, NULL, &first));
  CHECK(first.status == HLSL_EMIT_STATUS_UNSUPPORTED);
  CHECK(first.phase == HLSL_EMIT_PHASE_PROGRAM_VALIDATION);
  CHECK(first.reason == HLSL_EMIT_REASON_UNSUPPORTED_OPCODE);
  CHECK(first.instruction_index == 0);
  CHECK(first.source_instruction_index == 37u);
  CHECK(first.opcode == USIL_OP_NOP);
  CHECK(first.operand_index == -1);
  sb_free(&output);

  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, NULL, &second));
  CHECK(memcmp(&first, &second, sizeof(first)) == 0);
  sb_free(&output);
  return 0;
}

static int verify_missing_resource_binding_context(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output_signature;
  init_valid_program(&program, &instruction, &output_signature);
  instruction.opcode = USIL_OP_SAMPLEINFO;
  instruction.operand_count = 2;
  snprintf(instruction.original_asm, sizeof(instruction.original_asm),
           "sampleinfo o0, t3");
  init_register_operand(&instruction.operands[0], OPERAND_TYPE_OUTPUT, 0);
  init_register_operand(&instruction.operands[1], OPERAND_TYPE_RESOURCE, 3);

  StringBuilder output;
  HLSLEmitDiagnostic diagnostic;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, NULL, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_PROGRAM);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_PROGRAM_VALIDATION);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_INVALID_OPERAND);
  CHECK(diagnostic.instruction_index == 0);
  CHECK(diagnostic.opcode == USIL_OP_SAMPLEINFO);
  CHECK(diagnostic.operand_index == 1);
  CHECK(diagnostic.metadata.source == HLSL_EMIT_METADATA_SOURCE_PROGRAM);
  CHECK(diagnostic.metadata.kind == HLSL_EMIT_METADATA_TEXTURE);
  CHECK(diagnostic.metadata.record_index == -1);
  CHECK(diagnostic.metadata.register_index == 3);
  sb_free(&output);
  return 0;
}

static int verify_conflicting_metadata_locations(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output_signature;
  USILConstantBuffer cbuffers[2];
  init_valid_program(&program, &instruction, &output_signature);
  memset(cbuffers, 0, sizeof(cbuffers));
  cbuffers[0].reg_idx = 2;
  cbuffers[0].size = 1;
  cbuffers[1].reg_idx = 2;
  cbuffers[1].size = 1;
  program.cbuffers = cbuffers;
  program.cbuffer_count = 2;
  program.cbuffer_alloc = 2;

  StringBuilder output;
  HLSLEmitDiagnostic diagnostic;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, NULL, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_INVALID_PROGRAM);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_PROGRAM_VALIDATION);
  CHECK(diagnostic.reason ==
        HLSL_EMIT_REASON_CONFLICTING_METADATA_AUTHORITY);
  CHECK(diagnostic.metadata.kind == HLSL_EMIT_METADATA_CBUFFER);
  CHECK(diagnostic.metadata.record_index == 1);
  CHECK(diagnostic.metadata.register_index == 2);
  CHECK(diagnostic.related_metadata.kind == HLSL_EMIT_METADATA_CBUFFER);
  CHECK(diagnostic.related_metadata.record_index == 0);
  CHECK(diagnostic.related_metadata.register_index == 2);
  sb_free(&output);
  return 0;
}

static int verify_success_leaves_ok_diagnostic(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output_signature;
  init_valid_program(&program, &instruction, &output_signature);
  StringBuilder output;
  HLSLEmitDiagnostic diagnostic;
  sb_init(&output);
  CHECK(hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, NULL, &diagnostic));
  CHECK(sb_ok(&output));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_OK);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_NONE);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_NONE);
  CHECK(diagnostic.instruction_index == -1);
  CHECK(diagnostic.metadata.kind == HLSL_EMIT_METADATA_NONE);
  CHECK(strstr(output.buf, "return ") != NULL);
  sb_free(&output);
  return 0;
}

static int verify_instruction_emission_binding_context(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement output_signature;
  USILTexture texture;
  init_valid_program(&program, &instruction, &output_signature);
  memset(&texture, 0, sizeof(texture));
  texture.reg_idx = 1;
  snprintf(texture.dimension, sizeof(texture.dimension), "buffer");
  texture.return_types[0] = 5;
  texture.return_types[1] = 9;
  texture.return_types[2] = 9;
  texture.return_types[3] = 9;
  program.textures = &texture;
  program.texture_count = 1;
  program.texture_alloc = 1;
  instruction.opcode = USIL_OP_RESINFO;
  instruction.operand_count = 3;
  snprintf(instruction.original_asm, sizeof(instruction.original_asm),
           "resinfo o0, l(0), t1");
  init_register_operand(&instruction.operands[0], OPERAND_TYPE_OUTPUT, 0);
  init_immediate_operand(&instruction.operands[1]);
  instruction.operands[1].imm_value_count = 1;
  instruction.operands[1].imm_values[0] = 0;
  init_register_operand(&instruction.operands[2], OPERAND_TYPE_RESOURCE, 1);

  StringBuilder output;
  HLSLEmitDiagnostic diagnostic;
  sb_init(&output);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &output, NULL, NULL, NULL, NULL, &diagnostic));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_UNSUPPORTED);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_INSTRUCTION_EMISSION);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_UNSUPPORTED_FEATURE);
  CHECK(diagnostic.instruction_index == 0);
  CHECK(diagnostic.source_instruction_index == 37u);
  CHECK(diagnostic.opcode == USIL_OP_RESINFO);
  CHECK(diagnostic.operand_index == 2);
  CHECK(diagnostic.metadata.source == HLSL_EMIT_METADATA_SOURCE_PROGRAM);
  CHECK(diagnostic.metadata.kind == HLSL_EMIT_METADATA_TEXTURE);
  CHECK(diagnostic.metadata.record_index == 0);
  CHECK(diagnostic.metadata.register_index == 1);
  sb_free(&output);
  return 0;
}

static int verify_cbuffer_helper_metadata_location(void) {
  USILProgram program;
  USILConstantBuffer program_buffer;
  HLSLEmitterContext context;
  TempVariable variable;
  SerializedVariable serialized_variable;
  SerializedConstantBuffer serialized_buffer;
  SerializedProgramParameters parameters;
  StringBuilder output;
  HLSLEmitDiagnostic diagnostic;
  memset(&program, 0, sizeof(program));
  memset(&program_buffer, 0, sizeof(program_buffer));
  memset(&context, 0, sizeof(context));
  memset(&variable, 0, sizeof(variable));
  memset(&serialized_variable, 0, sizeof(serialized_variable));
  memset(&serialized_buffer, 0, sizeof(serialized_buffer));
  memset(&parameters, 0, sizeof(parameters));

  program_buffer.reg_idx = 2;
  program_buffer.size = 1;
  program.cbuffers = &program_buffer;
  program.cbuffer_count = 1;
  program.cbuffer_alloc = 1;
  serialized_variable.name = "badWidth";
  serialized_buffer.variables = &serialized_variable;
  serialized_buffer.var_count = 1;
  parameters.constant_buffers = &serialized_buffer;
  parameters.cb_count = 1;

  variable.name = serialized_variable.name;
  variable.type = 0;
  variable.rows = 1;
  variable.dim = 5;
  variable.reg_offset = 0;
  variable.byte_offset = 0;
  variable.byte_size = 20;
  variable.authority = 1; /* stage-specific compiled parameters */

  sb_init(&output);
  hlsl_emit_diagnostic_init(&diagnostic);
  context.program = &program;
  context.params = &parameters;
  context.sb = &output;
  context.diagnostic = &diagnostic;
  context.cbuffer_layout_count = 1;
  context.cbuffer_layouts[0].reg = 2;
  context.cbuffer_layouts[0].variables = &variable;
  context.cbuffer_layouts[0].variable_count = 1;
  emit_cbuffer_helpers(&context);

  CHECK(!sb_ok(&output));
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_UNSUPPORTED);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_CBUFFER_HELPER_EMISSION);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
  CHECK(diagnostic.metadata.source ==
        HLSL_EMIT_METADATA_SOURCE_STAGE_PARAMETERS);
  CHECK(diagnostic.metadata.kind == HLSL_EMIT_METADATA_CBUFFER_VARIABLE);
  CHECK(diagnostic.metadata.record_index == 0);
  CHECK(diagnostic.metadata.member_index == 0);
  CHECK(diagnostic.metadata.register_index == 2);
  sb_free(&output);
  return 0;
}

int main(void) {
  CHECK(verify_initialization_and_stable_names() == 0);
  CHECK(verify_invalid_mode_and_legacy_wrapper() == 0);
  CHECK(verify_reserved_identifier_option_shape() == 0);
  CHECK(verify_prefailed_output() == 0);
  CHECK(verify_unsupported_opcode_instruction_context() == 0);
  CHECK(verify_missing_resource_binding_context() == 0);
  CHECK(verify_conflicting_metadata_locations() == 0);
  CHECK(verify_success_leaves_ok_diagnostic() == 0);
  CHECK(verify_instruction_emission_binding_context() == 0);
  CHECK(verify_cbuffer_helper_metadata_location() == 0);
  puts("hlsl diagnostic unit tests passed");
  return 0;
}
