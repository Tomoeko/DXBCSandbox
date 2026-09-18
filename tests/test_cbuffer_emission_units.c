#include "translation/hlsl_emitter_internal.h"

#include <stdio.h>
#include <stdlib.h>
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

static void init_cbuffer_operand(DXBCOperand *operand, int buffer_register,
                                 int row) {
  init_register_operand(operand, OPERAND_TYPE_CONSTANT_BUFFER,
                        buffer_register);
  operand->register_index_dim = 2;
  operand->index_has_immediate[1] = true;
  operand->index_values[1] = (uint32_t)row;
  operand->rel_offset0 = row;
}

static void init_program(USILProgram *program, USILInstruction *instruction,
                         USILConstantBuffer *cbuffer,
                         DXBCSignatureElement *output, int row_count) {
  memset(program, 0, sizeof(*program));
  memset(instruction, 0, sizeof(*instruction));
  memset(cbuffer, 0, sizeof(*cbuffer));
  memset(output, 0, sizeof(*output));
  snprintf(program->shader_type_model, sizeof(program->shader_type_model),
           "ps_5_0");
  program->outputs = output;
  program->output_count = 1;
  program->output_alloc = 1;
  snprintf(program->outputs[0].semantic_name,
           sizeof(program->outputs[0].semantic_name), "SV_Target");
  program->outputs[0].system_value = 64u;
  program->outputs[0].register_id = 0;
  program->outputs[0].mask = 0xf;
  program->outputs[0].component_type = 3;
  cbuffer->reg_idx = 0;
  cbuffer->size = row_count;
  program->cbuffers = cbuffer;
  program->cbuffer_count = 1;
  program->cbuffer_alloc = 1;
  instruction->opcode = USIL_OP_MOV;
  instruction->operand_count = 2;
  snprintf(instruction->original_asm, sizeof(instruction->original_asm),
           "mov o0, cb0[0]");
  init_register_operand(&instruction->operands[0], OPERAND_TYPE_OUTPUT, 0);
  init_cbuffer_operand(&instruction->operands[1], 0, 0);
  program->instructions = instruction;
  program->instruction_count = 1;
  program->instruction_alloc = 1;
}

static void init_params(SerializedProgramParameters *parameters,
                        SerializedConstantBuffer *buffer,
                        SerializedResourceParam *resource,
                        SerializedVariable *variables, int variable_count,
                        const char *cb_name) {
  memset(parameters, 0, sizeof(*parameters));
  memset(buffer, 0, sizeof(*buffer));
  memset(resource, 0, sizeof(*resource));
  parameters->constant_buffers = buffer;
  parameters->cb_count = 1;
  parameters->resources = resource;
  parameters->res_count = 1;
  buffer->variables = variables;
  buffer->var_count = variable_count;
  buffer->name = cb_name;
  resource->name = cb_name;
  resource->bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER;
  resource->bind_index = 0;
}

static void init_vector(SerializedVariable *variable, const char *name,
                        uint32_t byte_offset, uint32_t columns,
                        uint32_t array_size) {
  memset(variable, 0, sizeof(*variable));
  variable->name = name;
  variable->layout[0] = byte_offset;
  variable->layout[1] = array_size;
  variable->layout[2] = 0;
  variable->layout[3] = columns;
  variable->layout[4] = 0;
}

static void init_matrix(SerializedVariable *variable, const char *name,
                        uint32_t byte_offset, uint32_t rows,
                        uint32_t array_size) {
  init_vector(variable, name, byte_offset, rows, array_size);
  variable->layout[3] = rows;
  variable->layout[4] = 1;
}

static int verify_authority_precedence_and_projection(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 2);

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable stage_variables[1];
  init_vector(&stage_variables[0], "stageValue", 0, 4, 0);
  init_params(&stage, &stage_buffer, &stage_resource, stage_variables, 1,
              "$Globals");

  SerializedProgramParameters common;
  SerializedConstantBuffer common_buffer;
  SerializedResourceParam common_resource;
  SerializedVariable common_variables[2];
  init_vector(&common_variables[0], "commonShadow", 0, 4, 0);
  init_vector(&common_variables[1], "commonUnused", 16, 4, 0);
  init_params(&common, &common_buffer, &common_resource, common_variables, 2,
              "$Globals");

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, &common, NULL));
  CHECK(strstr(output.buf, "float4 stageValue;") != NULL);
  CHECK(strstr(output.buf, "return stageValue;") != NULL);
  CHECK(strstr(output.buf, "commonShadow") == NULL);
  CHECK(strstr(output.buf, "commonUnused") == NULL);
  sb_free(&output);
  return 0;
}

/* A Unity player parameter blob begins with a loose-parameter area and then
 * stores named constant buffers.  Both may conventionally spell `$Globals`.
 * The sparse-domain fixture has an empty loose area, a 112-byte named stage shell, and a
 * 176-byte partial common-family record containing the active 48-byte field.
 * The family size must not replace or conflict with the selected shell. */
static int verify_loose_globals_and_partial_family_shell(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 3);
  instruction.operands[1].index_values[1] = 2U;
  instruction.operands[1].rel_offset0 = 2;

  SerializedConstantBuffer stage_buffers[2];
  memset(stage_buffers, 0, sizeof(stage_buffers));
  stage_buffers[0].name = "$Globals";
  stage_buffers[0].role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS;
  stage_buffers[1].name = "$Globals";
  stage_buffers[1].role = SERIALIZED_CBUFFER_NAMED;
  stage_buffers[1].size = 112U;
  SerializedResourceParam stage_resource;
  memset(&stage_resource, 0, sizeof(stage_resource));
  stage_resource.name = "$Globals";
  stage_resource.bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER;
  stage_resource.bind_index = 0U;
  SerializedProgramParameters stage;
  memset(&stage, 0, sizeof(stage));
  stage.is_binary = true;
  stage.cb_count = 2;
  stage.constant_buffers = stage_buffers;
  stage.res_count = 1;
  stage.resources = &stage_resource;

  SerializedVariable common_variable;
  init_vector(&common_variable, "_LightColor0", 32U, 4U, 0U);
  SerializedConstantBuffer common_buffer;
  memset(&common_buffer, 0, sizeof(common_buffer));
  common_buffer.name = "$Globals";
  common_buffer.role = SERIALIZED_CBUFFER_NAMED;
  common_buffer.size = 176U;
  common_buffer.has_is_partial = true;
  common_buffer.is_partial = true;
  common_buffer.var_count = 1;
  common_buffer.variables = &common_variable;
  SerializedProgramParameters common;
  memset(&common, 0, sizeof(common));
  common.cb_count = 1;
  common.constant_buffers = &common_buffer;

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, &common, NULL));
  CHECK(strstr(output.buf, "float4 _LightColor0;") != NULL);
  CHECK(strstr(output.buf, "return _LightColor0;") != NULL);
  CHECK(strstr(output.buf, "float4 cb0_6;") != NULL);
  CHECK(strstr(output.buf, "case 3:") == NULL);
  CHECK(strstr(output.buf, "cb0_data") == NULL);
  sb_free(&output);
  return 0;
}

static int verify_raw_register_authority(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 2);

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
  CHECK(strstr(output.buf, "float4 cb0_data[2];") != NULL);
  CHECK(strstr(output.buf, "return cb0_data[idx];") != NULL);
  CHECK(strstr(output.buf, "get_cb0(0)") != NULL);
  sb_free(&output);
  return 0;
}

static int verify_builtins_are_readable_only(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 9);
  instruction.operands[1].index_values[1] = 4;
  instruction.operands[1].rel_offset0 = 4;
  instruction.operands[0].destination_mask = 0x70;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  init_params(&stage, &stage_buffer, &stage_resource, NULL, 0,
              "UnityPerCamera");

  StringBuilder exact;
  sb_init(&exact);
  CHECK(hlsl_emit(&program, &exact, &stage, NULL, NULL));
  CHECK(strstr(exact.buf, "_WorldSpaceCameraPos") == NULL);
  CHECK(strstr(exact.buf, "float4 cb0_data[9];") != NULL);

  HLSLEmitOptions readable_options = HLSL_EMIT_READABLE_OPTIONS_INIT;
  StringBuilder readable;
  sb_init(&readable);
  CHECK(hlsl_emit_with_options(&program, &readable, &stage, NULL, NULL,
                               &readable_options));
  CHECK(strstr(readable.buf, "float3 _WorldSpaceCameraPos") != NULL);
  CHECK(strstr(readable.buf, "float4 cb0_data[9]") == NULL);
  sb_free(&readable);
  sb_free(&exact);
  return 0;
}

static int verify_builtin_omission_is_per_emission(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 4);

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable stage_variable;
  init_matrix(&stage_variable, "unity_ObjectToWorld", 0, 4, 0);
  init_params(&stage, &stage_buffer, &stage_resource, &stage_variable, 1,
              "UnityPerDraw");

  StringBuilder standalone_before;
  sb_init(&standalone_before);
  CHECK(hlsl_emit(&program, &standalone_before, &stage, NULL, NULL));
  CHECK(strstr(standalone_before.buf, "cbuffer UnityPerDraw") != NULL);

  HLSLEmitOptions unity_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  unity_options.omit_unity_builtin_declarations = true;
  StringBuilder unity;
  sb_init(&unity);
  CHECK(hlsl_emit_with_options(&program, &unity, &stage, NULL, NULL,
                               &unity_options));
  CHECK(strstr(unity.buf, "cbuffer UnityPerDraw") == NULL);
  CHECK(strstr(unity.buf, "unity_ObjectToWorld") != NULL);
  CHECK(strstr(unity.buf, "transpose(unity_ObjectToWorld)[0]") != NULL);

  /* A similarly named or layout-incompatible field is not silently
   * attributed to UnityShaderVariables.cginc. */
  stage_variable.layout[3] = 3;
  StringBuilder wrong_shape;
  sb_init(&wrong_shape);
  CHECK(!hlsl_emit_with_options(&program, &wrong_shape, &stage, NULL, NULL,
                                &unity_options));
  CHECK(wrong_shape.failed);
  sb_free(&wrong_shape);
  stage_variable.layout[3] = 4;

  stage_variable.name = "unity_ObjectToWorld_alias";
  StringBuilder wrong_name;
  sb_init(&wrong_name);
  CHECK(!hlsl_emit_with_options(&program, &wrong_name, &stage, NULL, NULL,
                                &unity_options));
  CHECK(wrong_name.failed);
  sb_free(&wrong_name);
  stage_variable.name = "unity_ObjectToWorld";

  /* A Unity-targeted emission must not leak its declaration policy into the
   * next standalone call, including when callers run concurrently. */
  StringBuilder standalone_after;
  sb_init(&standalone_after);
  CHECK(hlsl_emit(&program, &standalone_after, &stage, NULL, NULL));
  CHECK(strcmp(standalone_before.buf, standalone_after.buf) == 0);

  sb_free(&standalone_after);
  sb_free(&unity);
  sb_free(&standalone_before);
  return 0;
}

static int verify_urp_only_per_draw_field_is_not_legacy_include_owned(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 21);
  instruction.operands[1].index_values[1] = 17U;
  instruction.operands[1].rel_offset0 = 17;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable previous_matrix;
  init_matrix(&previous_matrix, "unity_MatrixPreviousM", 272U, 4U, 0U);
  init_params(&stage, &stage_buffer, &stage_resource, &previous_matrix, 1,
              "UnityPerDraw");

  StringBuilder standalone;
  sb_init(&standalone);
  CHECK(hlsl_emit(&program, &standalone, &stage, NULL, NULL));
  CHECK(strstr(standalone.buf, "float4x4 unity_MatrixPreviousM;") != NULL);
  sb_free(&standalone);

  /* Unity 2021.3's URP UnityInput.hlsl owns this field, but the generated
   * ShaderLab candidate includes only UnityShaderVariables.cginc.  The legacy
   * include's UnityPerDraw block ends at unity_RenderingLayer, so omission
   * must fail closed instead of leaving an undefined matrix reference. */
  HLSLEmitOptions unity_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  unity_options.omit_unity_builtin_declarations = true;
  StringBuilder unity;
  sb_init(&unity);
  CHECK(!hlsl_emit_with_options(&program, &unity, &stage, NULL, NULL,
                                &unity_options));
  CHECK(unity.failed);
  sb_free(&unity);
  return 0;
}

static int verify_flattened_globals_builtin_omission(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 21);
  instruction.operands[1].index_values[1] = 20U;
  instruction.operands[1].rel_offset0 = 20;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable builtin;
  init_vector(&builtin, "_ProjectionParams", 320U, 4U, 0U);
  init_params(&stage, &stage_buffer, &stage_resource, &builtin, 1,
              "$Globals");

  HLSLEmitOptions unity_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  unity_options.omit_unity_builtin_declarations = true;

  /* The first four flattened UnityPerCamera aliases occur in the
   * PostProcessing corpus.  unity_HalfStereoSeparation is owned by the same
   * Unity 2021.3 include when STEREO_CUBEMAP_RENDER_ON is active.  Standalone
   * HLSL owns each declaration, whereas ShaderLab emission must use the
   * include's exact float4 declaration and preserve the serialized target row
   * in get_cb0. */
  const char *vector_aliases[] = {
      "_ProjectionParams",
      "_ScreenParams",
      "_ZBufferParams",
      "unity_OrthoParams",
      "unity_HalfStereoSeparation",
  };
  for (size_t index = 0;
       index < sizeof(vector_aliases) / sizeof(vector_aliases[0]); ++index) {
    char declaration[96];
    char access[128];
    builtin.name = vector_aliases[index];
    snprintf(declaration, sizeof(declaration), "float4 %s;", builtin.name);
    snprintf(access, sizeof(access), "case 20: return %s;", builtin.name);

    StringBuilder standalone;
    sb_init(&standalone);
    CHECK(hlsl_emit(&program, &standalone, &stage, NULL, NULL));
    CHECK(strstr(standalone.buf, declaration) != NULL);
    CHECK(strstr(standalone.buf, access) != NULL);
    sb_free(&standalone);

    StringBuilder unity;
    sb_init(&unity);
    CHECK(hlsl_emit_with_options(&program, &unity, &stage, NULL, NULL,
                                 &unity_options));
    CHECK(strstr(unity.buf, declaration) == NULL);
    CHECK(strstr(unity.buf, access) != NULL);
    sb_free(&unity);
  }

  /* A reserved include spelling with a different serialized scalar type is
   * contradictory authority, not permission to emit an invalid duplicate. */
  builtin.name = "unity_HalfStereoSeparation";
  builtin.layout[2] = 1U;
  StringBuilder conflict;
  HLSLEmitDiagnostic conflict_diagnostic;
  sb_init(&conflict);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &conflict, &stage, NULL, NULL, &unity_options,
      &conflict_diagnostic));
  CHECK(conflict.failed);
  CHECK(conflict_diagnostic.status == HLSL_EMIT_STATUS_INVALID_METADATA);
  CHECK(conflict_diagnostic.phase == HLSL_EMIT_PHASE_CBUFFER_EMISSION);
  CHECK(conflict_diagnostic.reason ==
        HLSL_EMIT_REASON_BUILTIN_CONTRACT_MISMATCH);
  sb_free(&conflict);
  builtin.layout[2] = 0U;

  /* ScalableAO also serializes these UnityPerCameraRare matrices into
   * $Globals at a shader-specific offset.  Their include-owned declaration is
   * column-major, so physical DXBC row access must retain the transpose. */
  init_program(&program, &instruction, &cbuffer, &output_signature, 24);
  instruction.operands[1].index_values[1] = 20U;
  instruction.operands[1].rel_offset0 = 20;
  init_matrix(&builtin, "unity_CameraProjection", 320U, 4U, 0U);
  init_params(&stage, &stage_buffer, &stage_resource, &builtin, 1,
              "$Globals");

  const char *matrix_aliases[] = {
      "unity_CameraProjection",
      "unity_WorldToCamera",
  };
  for (size_t index = 0;
       index < sizeof(matrix_aliases) / sizeof(matrix_aliases[0]); ++index) {
    char declaration[96];
    char access[160];
    builtin.name = matrix_aliases[index];
    snprintf(declaration, sizeof(declaration), "float4x4 %s;", builtin.name);
    snprintf(access, sizeof(access), "case 20: return transpose(%s)[0];",
             builtin.name);

    StringBuilder standalone;
    sb_init(&standalone);
    CHECK(hlsl_emit(&program, &standalone, &stage, NULL, NULL));
    CHECK(strstr(standalone.buf, declaration) != NULL);
    CHECK(strstr(standalone.buf, access) != NULL);
    sb_free(&standalone);

    StringBuilder unity;
    sb_init(&unity);
    CHECK(hlsl_emit_with_options(&program, &unity, &stage, NULL, NULL,
                                 &unity_options));
    CHECK(strstr(unity.buf, declaration) == NULL);
    CHECK(strstr(unity.buf, access) != NULL);
    sb_free(&unity);
  }

  builtin.layout[3] = 3U;
  StringBuilder matrix_conflict;
  sb_init(&matrix_conflict);
  CHECK(!hlsl_emit_with_options(&program, &matrix_conflict, &stage, NULL, NULL,
                                &unity_options));
  CHECK(matrix_conflict.failed);
  sb_free(&matrix_conflict);
  builtin.layout[3] = 4U;
  return 0;
}

static int verify_stereo_builtin_matrix_array_omission(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 32);
  init_register_operand(&relative, OPERAND_TYPE_TEMP, 0);
  relative.swizzle_mode = 2;
  relative.swizzle[0] = 0;
  instruction.operands[1].index_representations[1] = 3;
  instruction.operands[1].index_has_immediate[1] = true;
  instruction.operands[1].index_values[1] = 24;
  instruction.operands[1].rel_offset0 = 24;
  instruction.operands[1].rel_op1 = &relative;
  program.temp_count = 1;
  cbuffer.dynamic_indexed = true;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable matrix;
  init_matrix(&matrix, "unity_StereoMatrixVP", 384, 4, 2);
  init_params(&stage, &stage_buffer, &stage_resource, &matrix, 1,
              "UnityStereoGlobals");

  HLSLEmitOptions unity_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  unity_options.omit_unity_builtin_declarations = true;
  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit_with_options(&program, &output, &stage, NULL, NULL,
                               &unity_options));
  CHECK(strstr(output.buf, "cbuffer UnityStereoGlobals") == NULL);
  CHECK(strstr(output.buf, "unity_StereoMatrixVP") != NULL);
  CHECK(strstr(output.buf, "get_cb0(asint(") != NULL);
  sb_free(&output);
  return 0;
}

static int verify_stereo_eye_index_builtin_omission(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 1);
  instruction.operands[0].destination_mask = 0x10;
  instruction.operands[1].swizzle_mode = 2;
  instruction.operands[1].swizzle[0] = 0;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable eye_index;
  init_vector(&eye_index, "unity_StereoEyeIndex", 0, 1, 0);
  eye_index.layout[2] = 1;
  init_params(&stage, &stage_buffer, &stage_resource, &eye_index, 1,
              "UnityStereoEyeIndex");

  HLSLEmitOptions unity_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  unity_options.omit_unity_builtin_declarations = true;
  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit_with_options(&program, &output, &stage, NULL, NULL,
                               &unity_options));
  CHECK(strstr(output.buf, "cbuffer UnityStereoEyeIndex") == NULL);
  CHECK(strstr(output.buf, "unity_StereoEyeIndex") != NULL);
  sb_free(&output);
  return 0;
}

static int verify_integer_vertex_output_keeps_native_backing(void) {
  USILProgram program;
  USILInstruction instruction;
  DXBCSignatureElement input;
  DXBCSignatureElement outputs[2];
  memset(&program, 0, sizeof(program));
  memset(&instruction, 0, sizeof(instruction));
  memset(&input, 0, sizeof(input));
  memset(outputs, 0, sizeof(outputs));
  snprintf(program.shader_type_model, sizeof(program.shader_type_model),
           "vs_4_0");

  snprintf(input.semantic_name, sizeof(input.semantic_name),
           "SV_InstanceID");
  input.system_value = 8;
  input.component_type = 1;
  input.register_id = 0;
  input.mask = 1;
  input.rw_mask = 1;
  program.inputs = &input;
  program.input_count = 1;
  program.input_alloc = 1;

  snprintf(outputs[0].semantic_name, sizeof(outputs[0].semantic_name),
           "SV_POSITION");
  outputs[0].system_value = 1;
  outputs[0].component_type = 3;
  outputs[0].register_id = 0;
  outputs[0].mask = 0xf;
  outputs[0].rw_mask = 0xf;
  snprintf(outputs[1].semantic_name, sizeof(outputs[1].semantic_name),
           "SV_RenderTargetArrayIndex");
  outputs[1].system_value = 4;
  outputs[1].component_type = 1;
  outputs[1].register_id = 1;
  outputs[1].mask = 1;
  outputs[1].rw_mask = 1;
  program.outputs = outputs;
  program.output_count = 2;
  program.output_alloc = 2;

  instruction.opcode = USIL_OP_MOV;
  instruction.operand_count = 2;
  snprintf(instruction.original_asm, sizeof(instruction.original_asm),
           "mov o1.x, v0.x");
  init_register_operand(&instruction.operands[0], OPERAND_TYPE_OUTPUT, 1);
  instruction.operands[0].destination_mask = 0x10;
  init_register_operand(&instruction.operands[1], OPERAND_TYPE_INPUT, 0);
  instruction.operands[1].swizzle_mode = 2;
  instruction.operands[1].swizzle[0] = 0;
  program.instructions = &instruction;
  program.instruction_count = 1;
  program.instruction_alloc = 1;

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
  CHECK(strstr(output.buf, "uint4 o1 = (uint4)0;") != NULL);
  CHECK(strstr(output.buf, "o1.x = v0.x;") != NULL);
  CHECK(strstr(output.buf, "output.o1 = o1.x;") != NULL);
  CHECK(strstr(output.buf, "o1.x = asfloat(") == NULL);
  sb_free(&output);
  return 0;
}

static int verify_builtin_resource_omission_contract(void) {
  USILProgram program;
  USILTexture texture;
  USILSampler sampler;
  memset(&program, 0, sizeof(program));
  memset(&texture, 0, sizeof(texture));
  memset(&sampler, 0, sizeof(sampler));
  snprintf(program.shader_type_model, sizeof(program.shader_type_model),
           "ps_4_0");
  texture.reg_idx = 1;
  snprintf(texture.dimension, sizeof(texture.dimension), "cube");
  memset(texture.return_types, 5, sizeof(texture.return_types));
  program.textures = &texture;
  program.texture_count = 1;
  program.texture_alloc = 1;
  sampler.reg_idx = 0;
  program.samplers = &sampler;
  program.sampler_count = 1;
  program.sampler_alloc = 1;

  SerializedProgramParameters parameters;
  SerializedResourceParam resource;
  memset(&parameters, 0, sizeof(parameters));
  memset(&resource, 0, sizeof(resource));
  resource.name = "unity_SpecCube0";
  resource.bind_type = SERIALIZED_RESOURCE_TEXTURE;
  resource.bind_index = 1;
  resource.sampler_index = 0;
  parameters.resources = &resource;
  parameters.res_count = 1;

  StringBuilder standalone;
  sb_init(&standalone);
  CHECK(hlsl_emit(&program, &standalone, &parameters, NULL, NULL));
  CHECK(strstr(standalone.buf,
               "TextureCube<float4> unity_SpecCube0 : register(t1)") !=
        NULL);
  CHECK(strstr(standalone.buf,
               "SamplerState samplerunity_SpecCube0 : register(s0)") !=
        NULL);
  sb_free(&standalone);

  HLSLEmitOptions unity_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  unity_options.omit_unity_builtin_declarations = true;
  StringBuilder unity;
  sb_init(&unity);
  CHECK(hlsl_emit_with_options(&program, &unity, &parameters, NULL, NULL,
                               &unity_options));
  CHECK(strstr(unity.buf, "TextureCube") == NULL);
  CHECK(strstr(unity.buf, "SamplerState") == NULL);
  sb_free(&unity);

  snprintf(texture.dimension, sizeof(texture.dimension), "2d");
  StringBuilder wrong_dimension;
  sb_init(&wrong_dimension);
  CHECK(!hlsl_emit_with_options(&program, &wrong_dimension, &parameters,
                                NULL, NULL, &unity_options));
  CHECK(wrong_dimension.failed);
  sb_free(&wrong_dimension);
  snprintf(texture.dimension, sizeof(texture.dimension), "cube");

  texture.return_types[3] = 4;
  StringBuilder wrong_return_type;
  sb_init(&wrong_return_type);
  CHECK(!hlsl_emit_with_options(&program, &wrong_return_type, &parameters,
                                NULL, NULL, &unity_options));
  CHECK(wrong_return_type.failed);
  sb_free(&wrong_return_type);
  texture.return_types[3] = 5;

  sampler.mode = 1;
  StringBuilder comparison_sampler;
  sb_init(&comparison_sampler);
  CHECK(!hlsl_emit_with_options(&program, &comparison_sampler, &parameters,
                                NULL, NULL, &unity_options));
  CHECK(comparison_sampler.failed);
  sb_free(&comparison_sampler);
  return 0;
}

static int verify_dynamic_uses_physical_rows_without_variable_metadata(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 4);
  init_register_operand(&relative, OPERAND_TYPE_TEMP, 0);
  instruction.operands[1].index_representations[1] = 3;
  instruction.operands[1].rel_op1 = &relative;
  program.temp_count = 1;
  cbuffer.dynamic_indexed = true;

  StringBuilder physical;
  sb_init(&physical);
  CHECK(hlsl_emit(&program, &physical, NULL, NULL, NULL));
  CHECK(strstr(physical.buf, "float4 cb0_data[4]") != NULL);
  CHECK(strstr(physical.buf, "return cb0_data[idx]") != NULL);
  CHECK(strstr(physical.buf, "get_cb0(asint(") != NULL);
  sb_free(&physical);

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable stage_variables[1];
  init_vector(&stage_variables[0], "values", 0, 4, 4);
  init_params(&stage, &stage_buffer, &stage_resource, stage_variables, 1,
              "$Globals");

  StringBuilder authoritative;
  sb_init(&authoritative);
  CHECK(hlsl_emit(&program, &authoritative, &stage, NULL, NULL));
  CHECK(strstr(authoritative.buf, "float4 values[4]") != NULL);
  CHECK(strstr(authoritative.buf, "values[asint(") != NULL);
  sb_free(&authoritative);
  return 0;
}

static void init_row_struct_candidate(USILProgram *program,
                                      USILInstruction instructions[2],
                                      USILConstantBuffer *cbuffer,
                                      DXBCOperand *relative,
                                      DXBCSignatureElement *output_signature,
                                      int row_count, uint32_t shift_amount,
                                      int row_offset) {
  init_program(program, &instructions[0], cbuffer, output_signature,
               row_count);
  instructions[1] = instructions[0];
  snprintf(instructions[1].original_asm,
           sizeof(instructions[1].original_asm),
           "mov o0, cb0[r0.x + %d]", row_offset);
  init_register_operand(relative, OPERAND_TYPE_TEMP, 0);
  relative->swizzle_mode = 2;
  relative->swizzle[0] = 0;
  instructions[1].operands[1].index_representations[1] = 3;
  instructions[1].operands[1].index_has_immediate[1] = true;
  instructions[1].operands[1].index_values[1] = (uint32_t)row_offset;
  instructions[1].operands[1].rel_offset0 = row_offset;
  instructions[1].operands[1].rel_op1 = relative;

  memset(&instructions[0], 0, sizeof(instructions[0]));
  instructions[0].opcode = USIL_OP_ISHL;
  instructions[0].operand_count = 3;
  snprintf(instructions[0].original_asm,
           sizeof(instructions[0].original_asm),
           "ishl r0.x, r0.x, l(%u)", shift_amount);
  init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 0);
  instructions[0].operands[0].swizzle_mode = 0;
  instructions[0].operands[0].destination_mask = 0x10;
  init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 0);
  instructions[0].operands[1].swizzle_mode = 2;
  instructions[0].operands[1].swizzle[0] = 0;
  memset(&instructions[0].operands[2], 0,
         sizeof(instructions[0].operands[2]));
  instructions[0].operands[2].type = OPERAND_TYPE_IMMEDIATE32;
  instructions[0].operands[2].imm_value_count = 1;
  instructions[0].operands[2].immediate_word_count = 1;
  instructions[0].operands[2].imm_values[0] = shift_amount;
  instructions[0].operands[2].immediate_words[0] = shift_amount;

  program->instructions = instructions;
  program->instruction_count = 2;
  program->instruction_alloc = 2;
  program->temp_count = 1;
  cbuffer->dynamic_indexed = true;
}

static int verify_power_of_two_row_struct_inverse(void) {
  USILProgram program;
  USILInstruction instructions[3];
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_row_struct_candidate(&program, instructions, &cbuffer, &relative,
                            &output_signature, 15, 3, 6);
  /* Relative-only zero and immediate-plus-relative nonzero are the two exact
   * DXBC encodings used by the same flattened row family. */
  instructions[2] = instructions[1];
  snprintf(instructions[2].original_asm,
           sizeof(instructions[2].original_asm), "mov o0, cb0[r0.x]");
  instructions[2].operands[1].index_representations[1] = 2;
  instructions[2].operands[1].index_has_immediate[1] = false;
  instructions[2].operands[1].index_values[1] = 0;
  instructions[2].operands[1].rel_offset0 = 0;
  program.instruction_count = 3;
  program.instruction_alloc = 3;

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
  CHECK(strstr(output.buf, "struct {\n") != NULL);
  CHECK(strstr(output.buf, "float4 row0;") != NULL);
  CHECK(strstr(output.buf, "float4 row7;") != NULL);
  CHECK(strstr(output.buf, "} cb0_rows[2];") != NULL);
  CHECK(strstr(output.buf,
               "int dxbc_row_index_i0 = asint(r0.x);") != NULL);
  CHECK(strstr(output.buf,
               "cb0_rows[dxbc_row_index_i0].row6") != NULL);
  CHECK(strstr(output.buf,
               "cb0_rows[dxbc_row_index_i0].row0") != NULL);
  CHECK(strstr(output.buf,
               "r0.x = asuint(asint(r0.x)) <<") == NULL);
  CHECK(strstr(output.buf, "get_cb0(") == NULL);
  CHECK(strstr(output.buf, "cb0_data") == NULL);
  sb_free(&output);

  /* A serialized 15-row reflection shell rules out the inferred two-by-eight
   * source struct. Preserve the exact shell with flat physical rows instead
   * of failing the whole emission or silently expanding it to 16 rows. */
  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  init_params(&stage, &stage_buffer, &stage_resource, NULL, 0, "ProbeCB");
  stage_buffer.size = 15U * 16U;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf, "float4 cb0_data[15]") != NULL);
  CHECK(strstr(output.buf, "cb0_rows") == NULL);
  sb_free(&output);
  return 0;
}

static int verify_serialized_row_struct_topology(void) {
  USILProgram program;
  USILInstruction instructions[3];
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_row_struct_candidate(&program, instructions, &cbuffer, &relative,
                            &output_signature, 15, 3, 6);
  /* Standard's serialized PerDraw0 form uses an SSA-like scale destination:
   *   ishl r0.y, r0.x, l(3)
   *   ... cb0[r0.y + k]
   * The source lane is the element index; the destination lane is only its
   * flattened row address. */
  instructions[0].operands[0].destination_mask = 0x20;
  relative.swizzle[0] = 1;
  snprintf(instructions[0].original_asm,
           sizeof(instructions[0].original_asm),
           "ishl r0.y, r0.x, l(3)");
  snprintf(instructions[1].original_asm,
           sizeof(instructions[1].original_asm),
           "mov o0, cb0[r0.y + 6]");
  instructions[2] = instructions[1];
  snprintf(instructions[2].original_asm,
           sizeof(instructions[2].original_asm), "mov o0, cb0[r0.y + 1]");
  instructions[2].operands[1].index_values[1] = 1U;
  instructions[2].operands[1].rel_offset0 = 1;
  program.instruction_count = 3;
  program.instruction_alloc = 3;

  SerializedVariable members[2];
  memset(members, 0, sizeof(members));
  members[0].name = "unity_ObjectToWorldArray";
  memcpy(members[0].layout,
         (const uint32_t[]){0U, 4U, 4U, 1U, 0U, 0U},
         sizeof(members[0].layout));
  members[1].name = "unity_WorldToObjectArray";
  memcpy(members[1].layout,
         (const uint32_t[]){0U, 4U, 4U, 1U, 0U, 64U},
         sizeof(members[1].layout));
  SerializedStructParam structure = {
      .name = "unity_Builtins0Array",
      .layout = {0U, 2U, 128U},
      .member_count = 2,
      .members = members,
  };
  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  init_params(&stage, &stage_buffer, &stage_resource, NULL, 0,
              "UnityInstancing_PerDraw0");
  stage.is_binary = true;
  stage_buffer.size = 256U;
  stage_buffer.struct_count = 1;
  stage_buffer.struct_params = &structure;

  StringBuilder exact;
  sb_init(&exact);
  CHECK(hlsl_emit(&program, &exact, &stage, NULL, NULL));
  CHECK(strstr(exact.buf, "float4x4 unity_ObjectToWorldArray;") != NULL);
  CHECK(strstr(exact.buf, "float4x4 unity_WorldToObjectArray;") != NULL);
  CHECK(strstr(exact.buf, "} unity_Builtins0Array[2];") != NULL);
  CHECK(strstr(exact.buf, "float4 row0;") == NULL);
  CHECK(strstr(exact.buf,
               "int dxbc_row_index_i0 = asint(r0.x);") != NULL);
  CHECK(strstr(exact.buf,
               "transpose(unity_Builtins0Array[dxbc_row_index_i0]."
               "unity_WorldToObjectArray)[2]") != NULL);
  CHECK(strstr(exact.buf,
               "transpose(unity_Builtins0Array[dxbc_row_index_i0]."
               "unity_ObjectToWorldArray)[1]") != NULL);
  CHECK(strstr(exact.buf,
               "r0.y = asuint(asint(r0.x)) <<") == NULL);
  sb_free(&exact);

  /* Every identity-bearing dimension must agree with the executable proof.
   * These near misses are rejected instead of falling back to anonymous rows
   * whose D3D reflection would have different names and types. */
  structure.layout[2] = 112U;
  StringBuilder wrong_stride;
  sb_init(&wrong_stride);
  CHECK(!hlsl_emit(&program, &wrong_stride, &stage, NULL, NULL));
  CHECK(wrong_stride.failed);
  sb_free(&wrong_stride);
  structure.layout[2] = 128U;

  structure.layout[1] = 3U;
  StringBuilder wrong_count;
  sb_init(&wrong_count);
  CHECK(!hlsl_emit(&program, &wrong_count, &stage, NULL, NULL));
  CHECK(wrong_count.failed);
  sb_free(&wrong_count);
  structure.layout[1] = 2U;

  members[1].layout[5] = 80U;
  StringBuilder member_gap;
  sb_init(&member_gap);
  CHECK(!hlsl_emit(&program, &member_gap, &stage, NULL, NULL));
  CHECK(member_gap.failed);
  sb_free(&member_gap);
  members[1].layout[5] = 64U;

  SerializedStructParam ambiguous[2] = {structure, structure};
  stage_buffer.struct_count = 2;
  stage_buffer.struct_params = ambiguous;
  StringBuilder duplicate_topology;
  sb_init(&duplicate_topology);
  CHECK(!hlsl_emit(&program, &duplicate_topology, &stage, NULL, NULL));
  CHECK(duplicate_topology.failed);
  sb_free(&duplicate_topology);
  stage_buffer.struct_count = 1;
  stage_buffer.struct_params = &structure;

  members[0].layout[0] = 1U;
  StringBuilder non_float_matrix;
  sb_init(&non_float_matrix);
  CHECK(!hlsl_emit(&program, &non_float_matrix, &stage, NULL, NULL));
  CHECK(non_float_matrix.failed);
  sb_free(&non_float_matrix);
  members[0].layout[0] = 0U;
  return 0;
}

static void init_row_struct_imul_candidate(
    USILProgram *program, USILInstruction instructions[2],
    USILConstantBuffer *cbuffer, DXBCOperand *relative,
    DXBCSignatureElement *output_signature, int row_count,
    uint32_t multiplier, int row_offset) {
  init_row_struct_candidate(program, instructions, cbuffer, relative,
                            output_signature, row_count, 1, row_offset);
  memset(&instructions[0], 0, sizeof(instructions[0]));
  instructions[0].opcode = USIL_OP_IMUL;
  instructions[0].operand_count = 4;
  snprintf(instructions[0].original_asm,
           sizeof(instructions[0].original_asm),
           "imul null, r0.x, r0.x, l(%u)", multiplier);
  instructions[0].operands[0].type = OPERAND_TYPE_NULL;
  init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 0);
  instructions[0].operands[1].swizzle_mode = 0;
  instructions[0].operands[1].destination_mask = 0x10;
  init_register_operand(&instructions[0].operands[2], OPERAND_TYPE_TEMP, 0);
  instructions[0].operands[2].swizzle_mode = 2;
  instructions[0].operands[2].swizzle[0] = 0;
  memset(&instructions[0].operands[3], 0,
         sizeof(instructions[0].operands[3]));
  instructions[0].operands[3].type = OPERAND_TYPE_IMMEDIATE32;
  instructions[0].operands[3].imm_value_count = 1;
  instructions[0].operands[3].immediate_word_count = 1;
  instructions[0].operands[3].imm_values[0] = multiplier;
  instructions[0].operands[3].immediate_words[0] = multiplier;
}

static void init_test_if_nonzero(USILInstruction *instruction,
                                 int register_index) {
  memset(instruction, 0, sizeof(*instruction));
  instruction->opcode = USIL_OP_IF;
  instruction->operand_count = 1;
  instruction->condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
  snprintf(instruction->original_asm, sizeof(instruction->original_asm),
           "if_nz r%d.x", register_index);
  init_register_operand(&instruction->operands[0], OPERAND_TYPE_TEMP,
                        register_index);
  instruction->operands[0].swizzle_mode = 2;
  instruction->operands[0].swizzle[0] = 0;
}

static void init_test_flow_marker(USILInstruction *instruction,
                                  USILOpcode opcode, const char *text) {
  memset(instruction, 0, sizeof(*instruction));
  instruction->opcode = opcode;
  snprintf(instruction->original_asm, sizeof(instruction->original_asm),
           "%s", text);
}

static void init_test_scalar_move(USILInstruction *instruction,
                                  int destination_register,
                                  int source_register) {
  memset(instruction, 0, sizeof(*instruction));
  instruction->opcode = USIL_OP_MOV;
  instruction->operand_count = 2;
  snprintf(instruction->original_asm, sizeof(instruction->original_asm),
           "mov r%d.x, r%d.x", destination_register, source_register);
  init_register_operand(&instruction->operands[0], OPERAND_TYPE_TEMP,
                        destination_register);
  instruction->operands[0].swizzle_mode = 0;
  instruction->operands[0].destination_mask = 0x10;
  init_register_operand(&instruction->operands[1], OPERAND_TYPE_TEMP,
                        source_register);
  instruction->operands[1].swizzle_mode = 2;
  instruction->operands[1].swizzle[0] = 0;
}

static int verify_low_imul_row_struct_topology(void) {
  USILProgram program;
  USILInstruction instructions[8];
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_row_struct_imul_candidate(&program, instructions, &cbuffer, &relative,
                                 &output_signature, 14, 7, 6);
  instructions[2] = instructions[1];
  snprintf(instructions[2].original_asm,
           sizeof(instructions[2].original_asm),
           "mov o0, cb0[r0.x + 3]");
  instructions[2].operands[1].index_values[1] = 3U;
  instructions[2].operands[1].rel_offset0 = 3;
  program.instruction_count = 3;
  program.instruction_alloc = 3;
  const USILInstruction exact_scale = instructions[0];
  const USILInstruction exact_row_six_access = instructions[1];

  static const char *member_names[7] = {
      "unity_SHArArray", "unity_SHAgArray", "unity_SHAbArray",
      "unity_SHBrArray", "unity_SHBgArray", "unity_SHBbArray",
      "unity_SHCArray"};
  SerializedVariable members[7];
  memset(members, 0, sizeof(members));
  for (int member_index = 0; member_index < 7; ++member_index) {
    members[member_index].name = member_names[member_index];
    members[member_index].layout[0] = 0U;
    members[member_index].layout[1] = 1U;
    members[member_index].layout[2] = 4U;
    members[member_index].layout[3] = 0U;
    members[member_index].layout[4] = 0U;
    members[member_index].layout[5] = (uint32_t)member_index * 16U;
  }
  SerializedStructParam structure = {
      .name = "unity_Builtins2Array",
      .layout = {0U, 2U, 112U},
      .member_count = 7,
      .members = members,
  };
  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  init_params(&stage, &stage_buffer, &stage_resource, NULL, 0,
              "UnityInstancing_PerDraw2");
  stage.is_binary = true;
  stage_buffer.size = 224U;
  stage_buffer.struct_count = 1;
  stage_buffer.struct_params = &structure;

  StringBuilder exact;
  sb_init(&exact);
  CHECK(hlsl_emit(&program, &exact, &stage, NULL, NULL));
  CHECK(strstr(exact.buf, "float4 unity_SHArArray;") != NULL);
  CHECK(strstr(exact.buf, "float4 unity_SHCArray;") != NULL);
  CHECK(strstr(exact.buf, "} unity_Builtins2Array[2];") != NULL);
  CHECK(strstr(exact.buf,
               "int dxbc_row_index_i0 = asint(r0.x);") != NULL);
  CHECK(strstr(exact.buf,
               "unity_Builtins2Array[dxbc_row_index_i0].unity_SHCArray") !=
        NULL);
  CHECK(strstr(exact.buf,
               "unity_Builtins2Array[dxbc_row_index_i0].unity_SHBrArray") !=
        NULL);
  CHECK(strstr(exact.buf, "r0.x = asuint(asint(r0.x) *") == NULL);
  sb_free(&exact);

  /* The compiler may retain the complete serialized 2x7-row shell even
   * when this variant references only fields 0..2.  Full or final-tail-
   * trimmed are the only exact extents; an intermediate declaration fails. */
  instructions[1].operands[1].index_values[1] = 2U;
  instructions[1].operands[1].rel_offset0 = 2;
  snprintf(instructions[1].original_asm,
           sizeof(instructions[1].original_asm),
           "mov o0, cb0[r0.x + 2]");
  instructions[2] = instructions[1];
  instructions[2].operands[1].index_representations[1] = 2;
  instructions[2].operands[1].index_has_immediate[1] = false;
  instructions[2].operands[1].index_values[1] = 0U;
  instructions[2].operands[1].rel_offset0 = 0;
  snprintf(instructions[2].original_asm,
           sizeof(instructions[2].original_asm),
           "mov o0, cb0[r0.x]");
  StringBuilder full_shell;
  sb_init(&full_shell);
  CHECK(hlsl_emit(&program, &full_shell, &stage, NULL, NULL));
  CHECK(strstr(full_shell.buf,
               "unity_Builtins2Array[dxbc_row_index_i0].unity_SHAbArray") !=
        NULL);
  CHECK(strstr(full_shell.buf,
               "unity_Builtins2Array[dxbc_row_index_i0].unity_SHArArray") !=
        NULL);
  sb_free(&full_shell);

  cbuffer.size = 13;
  StringBuilder intermediate_shell;
  sb_init(&intermediate_shell);
  CHECK(!hlsl_emit(&program, &intermediate_shell, &stage, NULL, NULL));
  CHECK(intermediate_shell.failed);
  sb_free(&intermediate_shell);
  cbuffer.size = 14;
  instructions[0] = exact_scale;
  instructions[1] = exact_row_six_access;
  instructions[2] = exact_row_six_access;
  instructions[2].operands[1].index_values[1] = 3U;
  instructions[2].operands[1].rel_offset0 = 3;
  snprintf(instructions[2].original_asm,
           sizeof(instructions[2].original_asm),
           "mov o0, cb0[r0.x + 3]");

  /* A live high product is an observable result.  It cannot be erased while
   * reconstructing the low-result row scale, so the serialized struct
   * topology fails closed with the cbuffer's typed metadata location. */
  init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 1);
  instructions[0].operands[0].swizzle_mode = 0;
  instructions[0].operands[0].destination_mask = 0x10;
  program.temp_count = 2;
  StringBuilder live_high;
  HLSLEmitDiagnostic diagnostic;
  sb_init(&live_high);
  CHECK(!hlsl_emit_with_options_diagnostic(
      &program, &live_high, &stage, NULL, NULL, NULL, &diagnostic));
  CHECK(live_high.failed);
  CHECK(diagnostic.status == HLSL_EMIT_STATUS_UNSUPPORTED);
  CHECK(diagnostic.phase == HLSL_EMIT_PHASE_CBUFFER_LAYOUT);
  CHECK(diagnostic.reason == HLSL_EMIT_REASON_UNREPRESENTABLE_LAYOUT);
  CHECK(diagnostic.metadata.source ==
        HLSL_EMIT_METADATA_SOURCE_STAGE_PARAMETERS);
  CHECK(diagnostic.metadata.kind == HLSL_EMIT_METADATA_CBUFFER);
  CHECK(diagnostic.metadata.record_index == 0);
  CHECK(diagnostic.metadata.member_index == -1);
  CHECK(diagnostic.metadata.register_index == 0);
  sb_free(&live_high);

  /* A scale inside one side of a branch is valid when that exact CFG block
   * dominates every row access.  Adding a target access on the other side is
   * a deterministic non-dominance near miss. */
  init_test_if_nonzero(&instructions[0], 1);
  init_test_flow_marker(&instructions[1], USIL_OP_ELSE, "else");
  instructions[2] = exact_scale;
  instructions[3] = exact_row_six_access;
  instructions[3].operands[1].index_values[1] = 2U;
  instructions[3].operands[1].rel_offset0 = 2;
  snprintf(instructions[3].original_asm,
           sizeof(instructions[3].original_asm),
           "mov o0, cb0[r0.x + 2]");
  init_test_flow_marker(&instructions[4], USIL_OP_ENDIF, "endif");
  program.instruction_count = 5;
  program.instruction_alloc = 5;
  program.temp_count = 2;
  StringBuilder dominated_branch;
  sb_init(&dominated_branch);
  CHECK(hlsl_emit(&program, &dominated_branch, &stage, NULL, NULL));
  CHECK(strstr(dominated_branch.buf,
               "int dxbc_row_index_i2 = asint(r0.x);") != NULL);
  CHECK(strstr(dominated_branch.buf,
               "unity_Builtins2Array[dxbc_row_index_i2].unity_SHAbArray") !=
        NULL);
  sb_free(&dominated_branch);

  instructions[5] = instructions[4];
  instructions[4] = instructions[3];
  instructions[3] = instructions[2];
  instructions[2] = instructions[1];
  instructions[1] = exact_row_six_access;
  program.instruction_count = 6;
  program.instruction_alloc = 6;
  StringBuilder nondominating_branch;
  sb_init(&nondominating_branch);
  CHECK(!hlsl_emit(&program, &nondominating_branch, &stage, NULL, NULL));
  CHECK(nondominating_branch.failed);
  sb_free(&nondominating_branch);
  return 0;
}

static int verify_identity_row_struct_topology(void) {
  USILProgram program;
  USILInstruction instructions[2];
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_row_struct_candidate(&program, instructions, &cbuffer, &relative,
                            &output_signature, 2, 1, 0);
  memset(&instructions[0], 0, sizeof(instructions[0]));
  instructions[0].opcode = USIL_OP_IADD;
  instructions[0].operand_count = 3;
  snprintf(instructions[0].original_asm,
           sizeof(instructions[0].original_asm),
           "iadd r0.x, r1.x, l(1)");
  init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 0);
  instructions[0].operands[0].swizzle_mode = 0;
  instructions[0].operands[0].destination_mask = 0x10;
  init_register_operand(&instructions[0].operands[1], OPERAND_TYPE_TEMP, 1);
  instructions[0].operands[1].swizzle_mode = 2;
  instructions[0].operands[1].swizzle[0] = 0;
  memset(&instructions[0].operands[2], 0,
         sizeof(instructions[0].operands[2]));
  instructions[0].operands[2].type = OPERAND_TYPE_IMMEDIATE32;
  instructions[0].operands[2].imm_value_count = 1;
  instructions[0].operands[2].immediate_word_count = 1;
  instructions[0].operands[2].imm_values[0] = 1U;
  instructions[0].operands[2].immediate_words[0] = 1U;
  program.temp_count = 2;

  SerializedVariable member;
  memset(&member, 0, sizeof(member));
  member.name = "unity_LightmapSTArray";
  memcpy(member.layout,
         (const uint32_t[]){0U, 1U, 4U, 0U, 0U, 0U},
         sizeof(member.layout));
  SerializedStructParam structure = {
      .name = "unity_Builtins2Array",
      .layout = {0U, 2U, 16U},
      .member_count = 1,
      .members = &member,
  };
  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  init_params(&stage, &stage_buffer, &stage_resource, NULL, 0,
              "UnityInstancing_PerDraw2");
  stage.is_binary = true;
  stage_buffer.size = 32U;
  stage_buffer.struct_count = 1;
  stage_buffer.struct_params = &structure;

  StringBuilder exact;
  sb_init(&exact);
  CHECK(hlsl_emit(&program, &exact, &stage, NULL, NULL));
  CHECK(strstr(exact.buf, "float4 unity_LightmapSTArray;") != NULL);
  CHECK(strstr(exact.buf, "} unity_Builtins2Array[2];") != NULL);
  CHECK(strstr(exact.buf,
               "unity_Builtins2Array[asint(r0.x)]."
               "unity_LightmapSTArray") != NULL);
  CHECK(strstr(exact.buf, "int dxbc_row_index_") == NULL);
  CHECK(strstr(exact.buf, "r0.x =") != NULL);
  sb_free(&exact);

  /* A fixed row bias is not an identity-stride array access. */
  instructions[1].operands[1].index_representations[1] = 3;
  instructions[1].operands[1].index_has_immediate[1] = true;
  instructions[1].operands[1].index_values[1] = 1U;
  instructions[1].operands[1].rel_offset0 = 1;
  StringBuilder biased;
  sb_init(&biased);
  CHECK(!hlsl_emit(&program, &biased, &stage, NULL, NULL));
  CHECK(biased.failed);
  sb_free(&biased);
  return 0;
}

static int verify_branching_row_struct_scale(void) {
  USILProgram program;
  USILInstruction instructions[7];
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_row_struct_imul_candidate(&program, instructions, &cbuffer, &relative,
                                 &output_signature, 14, 7, 6);
  instructions[2] = instructions[1];
  memset(&instructions[1], 0, sizeof(instructions[1]));
  instructions[1].opcode = USIL_OP_IF;
  instructions[1].operand_count = 1;
  instructions[1].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
  snprintf(instructions[1].original_asm,
           sizeof(instructions[1].original_asm), "if_nz r1.x");
  init_register_operand(&instructions[1].operands[0], OPERAND_TYPE_TEMP, 1);
  instructions[1].operands[0].swizzle_mode = 2;
  instructions[1].operands[0].swizzle[0] = 0;
  memset(&instructions[3], 0, sizeof(instructions[3]));
  instructions[3].opcode = USIL_OP_ELSE;
  snprintf(instructions[3].original_asm,
           sizeof(instructions[3].original_asm), "else");
  instructions[4] = instructions[2];
  snprintf(instructions[4].original_asm,
           sizeof(instructions[4].original_asm), "mov o0, cb0[r0.x]");
  instructions[4].operands[1].index_representations[1] = 2;
  instructions[4].operands[1].index_has_immediate[1] = false;
  instructions[4].operands[1].index_values[1] = 0U;
  instructions[4].operands[1].rel_offset0 = 0;
  memset(&instructions[5], 0, sizeof(instructions[5]));
  instructions[5].opcode = USIL_OP_ENDIF;
  snprintf(instructions[5].original_asm,
           sizeof(instructions[5].original_asm), "endif");
  program.instructions = instructions;
  program.instruction_count = 6;
  program.instruction_alloc = 6;
  program.temp_count = 2;

  StringBuilder branching;
  sb_init(&branching);
  CHECK(hlsl_emit(&program, &branching, NULL, NULL, NULL));
  CHECK(strstr(branching.buf,
               "int dxbc_row_index_i0 = asint(r0.x);") != NULL);
  CHECK(strstr(branching.buf,
               "cb0_rows[dxbc_row_index_i0].row6") != NULL);
  CHECK(strstr(branching.buf,
               "cb0_rows[dxbc_row_index_i0].row0") != NULL);
  CHECK(strstr(branching.buf, "if (") != NULL);
  sb_free(&branching);

  /* A write kills the old scaled value only on paths that execute it.  Reads
   * dominated by that branch-local kill are ordinary reads of the new value;
   * the untouched path may retain the old value when it has no later read. */
  USILInstruction killed_in_branch[6];
  DXBCOperand killed_relative;
  init_row_struct_imul_candidate(
      &program, killed_in_branch, &cbuffer, &killed_relative,
      &output_signature, 14, 7, 6);
  const USILInstruction killed_access = killed_in_branch[1];
  init_test_if_nonzero(&killed_in_branch[1], 1);
  killed_in_branch[2] = killed_access;
  init_test_scalar_move(&killed_in_branch[3], 0, 1);
  init_test_scalar_move(&killed_in_branch[4], 2, 0);
  init_test_flow_marker(&killed_in_branch[5], USIL_OP_ENDIF, "endif");
  program.instruction_count = 6;
  program.instruction_alloc = 6;
  program.temp_count = 3;
  StringBuilder branch_local_kill;
  sb_init(&branch_local_kill);
  CHECK(hlsl_emit(&program, &branch_local_kill, NULL, NULL, NULL));
  CHECK(strstr(branch_local_kill.buf,
               "int dxbc_row_index_i0 = asint(r0.x);") != NULL);
  CHECK(strstr(branch_local_kill.buf,
               "cb0_rows[dxbc_row_index_i0].row6") != NULL);
  sb_free(&branch_local_kill);

  const USILInstruction branch_kill = killed_in_branch[3];
  killed_in_branch[3] = killed_in_branch[4];
  killed_in_branch[4] = branch_kill;
  StringBuilder branch_read_before_kill;
  sb_init(&branch_read_before_kill);
  CHECK(hlsl_emit(&program, &branch_read_before_kill, NULL, NULL, NULL));
  CHECK(strstr(branch_read_before_kill.buf, "float4 cb0_data[14]") != NULL);
  CHECK(strstr(branch_read_before_kill.buf, "cb0_rows") == NULL);
  sb_free(&branch_read_before_kill);

  program.instructions = instructions;
  program.instruction_count = 6;
  program.instruction_alloc = 6;
  program.temp_count = 2;

  /* A branch-local rewrite means the post-merge cbuffer read does not have
   * one scale definition on every path.  It must not adopt row topology. */
  instructions[6] = instructions[4];
  memset(&instructions[4], 0, sizeof(instructions[4]));
  instructions[4].opcode = USIL_OP_MOV;
  instructions[4].operand_count = 2;
  snprintf(instructions[4].original_asm,
           sizeof(instructions[4].original_asm), "mov r0.x, r1.x");
  init_register_operand(&instructions[4].operands[0], OPERAND_TYPE_TEMP, 0);
  instructions[4].operands[0].swizzle_mode = 0;
  instructions[4].operands[0].destination_mask = 0x10;
  init_register_operand(&instructions[4].operands[1], OPERAND_TYPE_TEMP, 1);
  instructions[4].operands[1].swizzle_mode = 2;
  instructions[4].operands[1].swizzle[0] = 0;
  program.instruction_count = 7;
  program.instruction_alloc = 7;
  StringBuilder branch_rewrite;
  sb_init(&branch_rewrite);
  CHECK(hlsl_emit(&program, &branch_rewrite, NULL, NULL, NULL));
  CHECK(strstr(branch_rewrite.buf, "float4 cb0_data[14]") != NULL);
  CHECK(strstr(branch_rewrite.buf, "cb0_rows") == NULL);
  sb_free(&branch_rewrite);
  return 0;
}

static int verify_discard_before_row_struct_scale(void) {
  USILProgram program;
  USILInstruction instructions[3];
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_row_struct_imul_candidate(&program, instructions, &cbuffer, &relative,
                                 &output_signature, 14, 7, 6);
  instructions[2] = instructions[1];
  instructions[1] = instructions[0];
  memset(&instructions[0], 0, sizeof(instructions[0]));
  instructions[0].opcode = USIL_OP_DISCARD;
  instructions[0].operand_count = 1;
  instructions[0].condition_test = DXBC_INSTRUCTION_TEST_NONZERO;
  snprintf(instructions[0].original_asm,
           sizeof(instructions[0].original_asm), "discard_nz r1.x");
  init_register_operand(&instructions[0].operands[0], OPERAND_TYPE_TEMP, 1);
  instructions[0].operands[0].swizzle_mode = 2;
  instructions[0].operands[0].swizzle[0] = 0;
  program.instructions = instructions;
  program.instruction_count = 3;
  program.instruction_alloc = 3;
  program.temp_count = 2;

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
  CHECK(strstr(output.buf,
               "int dxbc_row_index_i1 = asint(r0.x);") != NULL);
  CHECK(strstr(output.buf,
               "cb0_rows[dxbc_row_index_i1].row6") != NULL);
  CHECK(strstr(output.buf, "clip(asuint(") != NULL);
  sb_free(&output);
  return 0;
}

static int verify_row_struct_near_misses_keep_raw_rows(void) {
  USILProgram program;
  USILInstruction instructions[2];
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;

  /* The constant row is outside the inferred eight-row element. */
  init_row_struct_candidate(&program, instructions, &cbuffer, &relative,
                            &output_signature, 16, 3, 8);
  StringBuilder out_of_range;
  sb_init(&out_of_range);
  CHECK(hlsl_emit(&program, &out_of_range, NULL, NULL, NULL));
  CHECK(strstr(out_of_range.buf, "float4 cb0_data[16]") != NULL);
  CHECK(strstr(out_of_range.buf, "cb0_rows") == NULL);
  sb_free(&out_of_range);

  /* A declaration ending before the highest possible referenced field does
   * not match D3DCompiler's trimmed-final-element layout. */
  init_row_struct_candidate(&program, instructions, &cbuffer, &relative,
                            &output_signature, 14, 3, 6);
  StringBuilder partial_element;
  sb_init(&partial_element);
  CHECK(hlsl_emit(&program, &partial_element, NULL, NULL, NULL));
  CHECK(strstr(partial_element.buf, "float4 cb0_data[14]") != NULL);
  CHECK(strstr(partial_element.buf, "cb0_rows") == NULL);
  sb_free(&partial_element);

  /* The scale must define the exact lane used by every relative access. */
  init_row_struct_candidate(&program, instructions, &cbuffer, &relative,
                            &output_signature, 15, 3, 6);
  instructions[0].operands[0].destination_mask = 0x20;
  StringBuilder different_destination;
  sb_init(&different_destination);
  CHECK(hlsl_emit(&program, &different_destination, NULL, NULL, NULL));
  CHECK(strstr(different_destination.buf, "float4 cb0_data[15]") != NULL);
  CHECK(strstr(different_destination.buf, "cb0_rows") == NULL);
  sb_free(&different_destination);

  /* A scaled lane with an independent arithmetic use cannot be folded into
   * a source array index, even when its cbuffer accesses otherwise fit. */
  USILInstruction observed_instructions[3];
  DXBCOperand observed_relative;
  init_row_struct_candidate(&program, observed_instructions, &cbuffer,
                            &observed_relative, &output_signature, 15, 3, 6);
  observed_instructions[2] = observed_instructions[1];
  memset(&observed_instructions[1], 0, sizeof(observed_instructions[1]));
  observed_instructions[1].opcode = USIL_OP_MOV;
  observed_instructions[1].operand_count = 2;
  snprintf(observed_instructions[1].original_asm,
           sizeof(observed_instructions[1].original_asm),
           "mov r1.x, r0.x");
  init_register_operand(&observed_instructions[1].operands[0],
                        OPERAND_TYPE_TEMP, 1);
  observed_instructions[1].operands[0].swizzle_mode = 0;
  observed_instructions[1].operands[0].destination_mask = 0x10;
  init_register_operand(&observed_instructions[1].operands[1],
                        OPERAND_TYPE_TEMP, 0);
  observed_instructions[1].operands[1].swizzle_mode = 2;
  observed_instructions[1].operands[1].swizzle[0] = 0;
  program.instruction_count = 3;
  program.instruction_alloc = 3;
  program.temp_count = 2;
  StringBuilder independently_observed;
  sb_init(&independently_observed);
  CHECK(hlsl_emit(&program, &independently_observed, NULL, NULL, NULL));
  CHECK(strstr(independently_observed.buf, "float4 cb0_data[15]") != NULL);
  CHECK(strstr(independently_observed.buf, "cb0_rows") == NULL);
  sb_free(&independently_observed);

  /* Every relative use must share the proven address lane. */
  USILInstruction mixed_instructions[3];
  DXBCOperand relative_x;
  DXBCOperand relative_y;
  init_row_struct_candidate(&program, mixed_instructions, &cbuffer,
                            &relative_x, &output_signature, 15, 3, 6);
  mixed_instructions[2] = mixed_instructions[1];
  init_register_operand(&relative_y, OPERAND_TYPE_TEMP, 0);
  relative_y.swizzle_mode = 2;
  relative_y.swizzle[0] = 1;
  mixed_instructions[2].operands[1].rel_op1 = &relative_y;
  program.instruction_count = 3;
  program.instruction_alloc = 3;
  StringBuilder mixed_relative_bases;
  sb_init(&mixed_relative_bases);
  CHECK(hlsl_emit(&program, &mixed_relative_bases, NULL, NULL, NULL));
  CHECK(strstr(mixed_relative_bases.buf, "float4 cb0_data[15]") != NULL);
  CHECK(strstr(mixed_relative_bases.buf, "cb0_rows") == NULL);
  sb_free(&mixed_relative_bases);
  return 0;
}

static int verify_dynamic_matrix_array_uses_row_exact_helper(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 8);
  init_register_operand(&relative, OPERAND_TYPE_TEMP, 0);
  instruction.operands[1].index_representations[1] = 3;
  instruction.operands[1].index_has_immediate[1] = true;
  instruction.operands[1].index_values[1] = 1;
  instruction.operands[1].rel_offset0 = 1;
  instruction.operands[1].rel_op1 = &relative;
  program.temp_count = 1;
  cbuffer.dynamic_indexed = true;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable matrix;
  init_matrix(&matrix, "matrixValues", 0, 4, 2);
  init_params(&stage, &stage_buffer, &stage_resource, &matrix, 1,
              "$Globals");

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf, "float4x4 matrixValues[2]") != NULL);
  CHECK(strstr(output.buf, "row_major float4x4 matrixValues[2]") == NULL);
  CHECK(strstr(output.buf, "get_cb0(asint(") != NULL);
  CHECK(strstr(output.buf, ") + 1)") != NULL);
  CHECK(strstr(output.buf,
               "case 1: return transpose(matrixValues[0])[1]") != NULL);
  CHECK(strstr(output.buf, "matrixValues[asint(") == NULL);
  sb_free(&output);
  return 0;
}

static int verify_dynamic_matrix_array_recovers_shift_source(void) {
  USILProgram program;
  USILInstruction instructions[2];
  USILConstantBuffer cbuffer;
  DXBCOperand relative;
  DXBCSignatureElement output_signature;
  init_row_struct_candidate(&program, instructions, &cbuffer, &relative,
                            &output_signature, 8, 2, 1);
  instructions[0].operands[1].swizzle[0] = 1;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable matrix;
  init_matrix(&matrix, "matrixValues", 0, 4, 2);
  init_params(&stage, &stage_buffer, &stage_resource, &matrix, 1,
              "$Globals");

  StringBuilder exact;
  sb_init(&exact);
  CHECK(hlsl_emit(&program, &exact, &stage, NULL, NULL));
  CHECK(strstr(exact.buf,
               "transpose(matrixValues[asint(asint(r0.y))])[1]") != NULL);
  CHECK(strstr(exact.buf,
               "get_cb0(asint(asint(r0.x)) + 1)") == NULL);
  sb_free(&exact);

  /* A different stride is not the compiler inverse of a four-row matrix.
   * Keep the row-exact fallback instead of inventing an array index. */
  instructions[0].operands[2].imm_values[0] = 1;
  instructions[0].operands[2].immediate_words[0] = 1;
  StringBuilder wrong_stride;
  sb_init(&wrong_stride);
  CHECK(hlsl_emit(&program, &wrong_stride, &stage, NULL, NULL));
  CHECK(strstr(wrong_stride.buf,
               "get_cb0(asint(asint(r0.x)) + 1)") != NULL);
  CHECK(strstr(wrong_stride.buf,
               "transpose(matrixValues[asint(asint(r0.y))])[1]") == NULL);
  sb_free(&wrong_stride);
  return 0;
}

static int verify_pruned_tail_keeps_full_declaration(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 3);
  instruction.operands[0].destination_mask = 0x10;
  instruction.operands[1].index_values[1] = 1;
  instruction.operands[1].rel_offset0 = 1;
  instruction.operands[1].swizzle_mode = 2;
  instruction.operands[1].swizzle[0] = 0;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable variable;
  init_matrix(&variable, "prunedMatrix", 16, 4, 0);
  init_params(&stage, &stage_buffer, &stage_resource, &variable, 1,
              "$Globals");

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf, "float4x4 prunedMatrix;") != NULL);
  CHECK(strstr(output.buf, "row_major float4x4 prunedMatrix;") == NULL);
  CHECK(strstr(output.buf, "transpose(prunedMatrix)[0]") != NULL);
  sb_free(&output);
  return 0;
}

static int verify_serialized_shell_tail_does_not_expand_active_rows(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 1);

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable variable;
  init_vector(&variable, "ProbeValue", 0, 4, 0);
  init_params(&stage, &stage_buffer, &stage_resource, &variable, 1,
              "ProbeCB");
  stage_buffer.size = 64U;

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf,
               "float4 _cb0_pad_end : packoffset(c3);") != NULL);
  CHECK(strstr(output.buf, "case 0: return ProbeValue;") != NULL);
  CHECK(strstr(output.buf, "case 1:") == NULL);
  CHECK(strstr(output.buf, "cb0_data[4]") == NULL);
  sb_free(&output);

  /* A nonzero serialized shell is an exact aligned authority, never an
   * upper-bound hint that can truncate the DXBC-active declaration. */
  stage_buffer.size = 12U;
  sb_init(&output);
  CHECK(!hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(output.failed);
  sb_free(&output);
  return 0;
}

static int verify_vector_array_tail_and_bool_layout(void) {
  USILProgram program;
  USILInstruction instructions[2];
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instructions[0], &cbuffer, &output_signature, 4);
  instructions[0].operands[0].destination_mask = 0x10;
  instructions[0].operands[1].swizzle_mode = 2;
  instructions[0].operands[1].swizzle[0] = 0;

  instructions[1] = instructions[0];
  snprintf(instructions[1].original_asm,
           sizeof(instructions[1].original_asm), "mov o0.y, cb0[3].y");
  instructions[1].operands[0].destination_mask = 0x20;
  instructions[1].operands[1].index_values[1] = 3;
  instructions[1].operands[1].rel_offset0 = 3;
  instructions[1].operands[1].swizzle[0] = 1;
  program.instructions = instructions;
  program.instruction_count = 2;
  program.instruction_alloc = 2;

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable variables[2];
  init_vector(&variables[0], "_Rect", 0, 1, 4);
  init_vector(&variables[1], "_SmoothCorners", 52, 1, 0);
  variables[1].layout[2] = 2;
  init_params(&stage, &stage_buffer, &stage_resource, variables, 2,
              "$Globals");

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf, "float _Rect[4];") != NULL);
  CHECK(strstr(output.buf, "bool _SmoothCorners;") != NULL);
  CHECK(strstr(output.buf, "_Rect[3]") != NULL);
  CHECK(strstr(output.buf, "_SmoothCorners") != NULL);
  CHECK(strstr(output.buf,
               "asfloat((_SmoothCorners) ? 1u : 0u)") != NULL);
  sb_free(&output);
  return 0;
}

static int verify_long_authoritative_helper_expression(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 1);

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable variables[4];
  char names[4][160];
  for (int component = 0; component < 4; ++component) {
    memset(names[component], 'a' + component, sizeof(names[component]) - 1);
    names[component][0] = 'v';
    names[component][1] = (char)('0' + component);
    names[component][sizeof(names[component]) - 1] = '\0';
    init_vector(&variables[component], names[component],
                (uint32_t)component * 4u, 1, 0);
  }
  char cbuffer_name[180];
  memset(cbuffer_name, 'c', sizeof(cbuffer_name) - 1U);
  cbuffer_name[0] = 'C';
  cbuffer_name[sizeof(cbuffer_name) - 1U] = '\0';
  init_params(&stage, &stage_buffer, &stage_resource, variables, 4,
              cbuffer_name);

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf, cbuffer_name) != NULL);
  for (int component = 0; component < 4; ++component) {
    CHECK(strstr(output.buf, names[component]) != NULL);
  }
  const char *helper_case = strstr(output.buf, "case 0: return float4(");
  CHECK(helper_case != NULL);
  const char *helper_end = strchr(helper_case, '\n');
  CHECK(helper_end != NULL);
  CHECK((size_t)(helper_end - helper_case) > 256);
  sb_free(&output);
  return 0;
}

static int verify_dynamic_cbuffer_variable_capacity(void) {
  enum { variable_count = 129 };
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature,
               variable_count);
  instruction.operands[1].index_values[1] = variable_count - 1;
  instruction.operands[1].rel_offset0 = variable_count - 1;

  SerializedVariable *variables = calloc(
      variable_count, sizeof(*variables));
  char (*names)[32] = calloc(variable_count, sizeof(*names));
  CHECK(variables != NULL && names != NULL);
  for (int index = 0; index < variable_count; ++index) {
    snprintf(names[index], sizeof(names[index]), "capacityValue%d", index);
    init_vector(&variables[index], names[index], (uint32_t)index * 16u, 4,
                0);
  }
  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  init_params(&stage, &stage_buffer, &stage_resource, variables,
              variable_count, "$Globals");

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf, "float4 capacityValue128;") != NULL);
  CHECK(strstr(output.buf, "return capacityValue128;") != NULL);
  sb_free(&output);
  free(names);
  free(variables);
  return 0;
}

static int verify_long_cbuffer_declaration_name_is_exact(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 1);

  char serialized_name[514];
  serialized_name[0] = '$';
  for (size_t index = 1; index + 1u < sizeof(serialized_name); ++index)
    serialized_name[index] = (char)('A' + index % 26u);
  serialized_name[sizeof(serialized_name) - 1u] = '\0';

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable variable;
  init_vector(&variable, "longNameValue", 0, 4, 0);
  init_params(&stage, &stage_buffer, &stage_resource, &variable, 1,
              serialized_name);

  char expected[sizeof(serialized_name)];
  expected[0] = '_';
  memcpy(expected + 1u, serialized_name + 1u,
         sizeof(serialized_name) - 1u);

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf, expected) != NULL);
  CHECK(strlen(expected) == strlen(serialized_name));
  sb_free(&output);
  return 0;
}

static int verify_long_cbuffer_variable_reference_is_exact(void) {
  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 1);

  char authoritative_name[1602];
  authoritative_name[0] = 'v';
  for (size_t index = 1; index + 1u < sizeof(authoritative_name); ++index)
    authoritative_name[index] = (char)('a' + index % 26u);
  authoritative_name[sizeof(authoritative_name) - 1u] = '\0';
  CHECK(strlen(authoritative_name) > 1500u);

  SerializedProgramParameters stage;
  SerializedConstantBuffer stage_buffer;
  SerializedResourceParam stage_resource;
  SerializedVariable variable;
  init_vector(&variable, authoritative_name, 0, 4, 0);
  init_params(&stage, &stage_buffer, &stage_resource, &variable, 1,
              "$Globals");

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, &stage, NULL, NULL));
  CHECK(strstr(output.buf, authoritative_name) != NULL);

  StringBuilder expected_reference;
  sb_init(&expected_reference);
  sb_appendf(&expected_reference, "return %s;", authoritative_name);
  CHECK(sb_ok(&expected_reference));
  CHECK(strstr(output.buf, expected_reference.buf) != NULL);
  sb_free(&expected_reference);
  sb_free(&output);
  return 0;
}

static int verify_lossless_hlsl_float_literals(void) {
  char literal[64];
  CHECK(format_float_bits_hlsl(UINT32_C(0x3f800001), literal,
                               sizeof(literal)));
  CHECK(strcmp(literal, "1.00000012f") == 0);
  CHECK(format_float_bits_hlsl(UINT32_C(0x80000000), literal,
                               sizeof(literal)));
  CHECK(strcmp(literal, "-0.0f") == 0);
  CHECK(format_float_bits_hlsl(UINT32_C(0x7fc12345), literal,
                               sizeof(literal)));
  CHECK(strcmp(literal, "asfloat(0x7FC12345u)") == 0);
  CHECK(format_float_bits_hlsl(UINT32_C(0x00000001), literal,
                               sizeof(literal)));
  CHECK(strcmp(literal, "asfloat(0x00000001u)") == 0);

  USILProgram program;
  USILInstruction instruction;
  USILConstantBuffer cbuffer;
  DXBCSignatureElement output_signature;
  init_program(&program, &instruction, &cbuffer, &output_signature, 1);
  uint32_t icb_values[4];
  program.icb_values = icb_values;
  program.icb_value_count = 4;
  program.icb_value_alloc = 4;
  program.icb_values[0] = UINT32_C(0x3f800001);
  program.icb_values[1] = UINT32_C(0x80000000);
  program.icb_values[2] = UINT32_C(0x7fc12345);
  program.icb_values[3] = UINT32_C(0x00000001);

  StringBuilder output;
  sb_init(&output);
  CHECK(hlsl_emit(&program, &output, NULL, NULL, NULL));
  CHECK(strstr(output.buf,
               "float4(1.00000012f, -0.0f, asfloat(0x7FC12345u), "
               "asfloat(0x00000001u))") != NULL);
  sb_free(&output);

  StringBuilder failed;
  HLSLEmitterContext *context =
      (HLSLEmitterContext *)calloc(1, sizeof(HLSLEmitterContext));
  char too_small[4];
  CHECK(context != NULL);
  sb_init(&failed);
  context->sb = &failed;
  CHECK(!hlsl_format_checked(context, too_small, sizeof(too_small), "%s",
                             "authoritative_identifier"));
  CHECK(failed.failed);
  CHECK(too_small[0] == '\0');
  sb_free(&failed);
  free(context);
  return 0;
}

int main(void) {
  CHECK(verify_authority_precedence_and_projection() == 0);
  CHECK(verify_loose_globals_and_partial_family_shell() == 0);
  CHECK(verify_raw_register_authority() == 0);
  CHECK(verify_builtins_are_readable_only() == 0);
  CHECK(verify_builtin_omission_is_per_emission() == 0);
  CHECK(verify_urp_only_per_draw_field_is_not_legacy_include_owned() == 0);
  CHECK(verify_flattened_globals_builtin_omission() == 0);
  CHECK(verify_stereo_builtin_matrix_array_omission() == 0);
  CHECK(verify_stereo_eye_index_builtin_omission() == 0);
  CHECK(verify_integer_vertex_output_keeps_native_backing() == 0);
  CHECK(verify_builtin_resource_omission_contract() == 0);
  CHECK(verify_dynamic_uses_physical_rows_without_variable_metadata() == 0);
  CHECK(verify_power_of_two_row_struct_inverse() == 0);
  CHECK(verify_serialized_row_struct_topology() == 0);
  CHECK(verify_low_imul_row_struct_topology() == 0);
  CHECK(verify_identity_row_struct_topology() == 0);
  CHECK(verify_branching_row_struct_scale() == 0);
  CHECK(verify_discard_before_row_struct_scale() == 0);
  CHECK(verify_row_struct_near_misses_keep_raw_rows() == 0);
  CHECK(verify_dynamic_matrix_array_uses_row_exact_helper() == 0);
  CHECK(verify_dynamic_matrix_array_recovers_shift_source() == 0);
  CHECK(verify_pruned_tail_keeps_full_declaration() == 0);
  CHECK(verify_serialized_shell_tail_does_not_expand_active_rows() == 0);
  CHECK(verify_vector_array_tail_and_bool_layout() == 0);
  CHECK(verify_long_authoritative_helper_expression() == 0);
  CHECK(verify_dynamic_cbuffer_variable_capacity() == 0);
  CHECK(verify_long_cbuffer_declaration_name_is_exact() == 0);
  CHECK(verify_long_cbuffer_variable_reference_is_exact() == 0);
  CHECK(verify_lossless_hlsl_float_literals() == 0);
  puts("cbuffer emission tests passed");
  return 0;
}
