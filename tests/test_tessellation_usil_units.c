#include "common/file_io.h"
#include "common/string_builder.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_stage_contract.h"
#include "dxbc/usbd.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/usil.h"

#include <stdio.h>
#include <string.h>

#ifndef DXBC_LIMIT_TEST2_FIXTURE
#error DXBC_LIMIT_TEST2_FIXTURE must name the checked-in LimitTest2 USBD fixture
#endif

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,     \
              #condition);                                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef int (*StageVerifier)(USILProgram *program);

static int with_stage(const DXBCUSBDTableView *table, const char *name,
                      StageVerifier verifier) {
  DXBCUSBDRecordView record;
  bool found = false;
  for (uint32_t index = 0; index < table->record_count; ++index) {
    CHECK(dxbc_usbd_table_record(table, index, &record));
    if (record.name_size == strlen(name) &&
        memcmp(record.name, name, record.name_size) == 0) {
      found = true;
      break;
    }
  }
  CHECK(found);

  DXBCDocument document;
  DXBCDocumentDiagnostic document_diagnostic;
  DXBCStageContract contract;
  DXBCStageContractDiagnostic contract_diagnostic;
  DXBCContainer semantic;
  dxbc_document_init(&document);
  dxbc_stage_contract_init(&contract);
  memset(&semantic, 0, sizeof(semantic));
  CHECK(dxbc_document_parse(&document, record.dxbc, record.dxbc_size,
                            &document_diagnostic));
  CHECK(dxbc_document_decode_semantic(&document, &semantic));
  CHECK(dxbc_stage_contract_decode(&document, &semantic, &contract,
                                   &contract_diagnostic));

  USILProgram program;
  CHECK(usil_translate_with_stage_contract(&program, &semantic, &contract));
  const int result = verifier(&program);
  usil_free(&program);
  dxbc_free(&semantic);
  dxbc_stage_contract_free(&contract);
  dxbc_document_free(&document);
  return result;
}

static bool emission_is_rejected(USILProgram *program,
                                 const HLSLEmitNames *names) {
  StringBuilder hlsl;
  sb_init(&hlsl);
  const bool rejected = !hlsl_emit(program, &hlsl, NULL, NULL, names);
  sb_free(&hlsl);
  return rejected;
}

static int verify_hull_exact_inverse(USILProgram *program) {
  CHECK(program->program_type == DXBC_PROGRAM_TYPE_HULL);
  CHECK(program->tessellation.phase_count == 2u);
  CHECK(program->index_range_count == 1);
  CHECK(hlsl_exact_tessellation_lift_matches(program));

  const HLSLEmitNames names = {"hs", "unused_input", "unused_output"};
  StringBuilder hlsl;
  sb_init(&hlsl);
  CHECK(hlsl_emit(program, &hlsl, NULL, NULL, &names));
  CHECK(strstr(hlsl.buf, "[domain(\"tri\")]") != NULL);
  CHECK(strstr(hlsl.buf, "[partitioning(\"fractional_odd\")]") != NULL);
  CHECK(strstr(hlsl.buf, "[outputtopology(\"triangle_cw\")]") != NULL);
  CHECK(strstr(hlsl.buf,
               "[patchconstantfunc(\"dxbc_patch_constant\")]") != NULL);
  CHECK(strstr(hlsl.buf, "[outputcontrolpoints(3)]") != NULL);
  CHECK(strstr(hlsl.buf,
               "factors.edge[0] = factors.edge[1] = factors.edge[2] = "
               "dxbc_hs_cb0_data[3].x;") != NULL);
  CHECK(strstr(hlsl.buf, "return patch[id];") != NULL);
  sb_free(&hlsl);

  const DXBCTessellatorPartitioning partitioning =
      program->tessellation.partitioning;
  program->tessellation.partitioning = DXBC_TESSELLATOR_PARTITIONING_INTEGER;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  CHECK(emission_is_rejected(program, &names));
  program->tessellation.partitioning = partitioning;

  const uint32_t instance_count =
      program->tessellation.phases[0].instance_count;
  program->tessellation.phases[0].instance_count = 2u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->tessellation.phases[0].instance_count = instance_count;

  const uint32_t range_count = program->index_ranges[0].register_count;
  program->index_ranges[0].register_count = 2u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->index_ranges[0].register_count = range_count;

  const uint32_t system_value =
      program->signature_declarations[1].system_value_name;
  program->signature_declarations[1].system_value_name ^= 1u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->signature_declarations[1].system_value_name = system_value;

  const uint64_t cbuffer_row =
      program->instructions[1].operands[1].index_values[1];
  program->instructions[1].operands[1].index_values[1] = 2u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->instructions[1].operands[1].index_values[1] = cbuffer_row;

  const uint8_t relative_representation =
      program->instructions[1].operands[0].index_representations[0];
  program->instructions[1].operands[0].index_representations[0] = 3u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->instructions[1].operands[0].index_representations[0] =
      relative_representation;

  const int instruction_allocation = program->instruction_alloc;
  program->instruction_alloc = program->instruction_count - 1;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->instruction_alloc = instruction_allocation;

  CHECK(hlsl_exact_tessellation_lift_matches(program));
  return 0;
}

static int verify_domain_exact_inverse(USILProgram *program) {
  CHECK(program->program_type == DXBC_PROGRAM_TYPE_DOMAIN);
  CHECK(program->instruction_count == 22);
  CHECK(hlsl_exact_tessellation_lift_matches(program));

  const HLSLEmitNames names = {"ds", "unused_input", "unused_output"};
  CHECK(emission_is_rejected(program, &names));
  SerializedResourceParam texture;
  memset(&texture, 0, sizeof(texture));
  texture.name = "_DispTex";
  texture.bind_type = SERIALIZED_RESOURCE_TEXTURE;
  texture.bind_index = 0u;
  texture.sampler_index = 0u;
  SerializedProgramParameters parameters;
  memset(&parameters, 0, sizeof(parameters));
  parameters.res_count = 1;
  parameters.resources = &texture;
  StringBuilder hlsl;
  sb_init(&hlsl);
  CHECK(hlsl_emit(program, &hlsl, &parameters, NULL, &names));
  CHECK(strstr(hlsl.buf, "OutputPatch<dxbc_ds_control_point, 3>") != NULL);
  CHECK(strstr(hlsl.buf, "float3 bary : SV_DomainLocation") != NULL);
  CHECK(strstr(hlsl.buf,
               "float displacement = "
               "_DispTex.SampleLevel(sampler_DispTex, uv, 0.0f).x;") !=
        NULL);
  CHECK(strstr(hlsl.buf,
               "float4x4 dxbc_ObjectToWorld;") != NULL);
  CHECK(strstr(hlsl.buf,
               "output.pos = mul(dxbc_MatrixVP, world);") != NULL);
  sb_free(&hlsl);

  const DXBCTessellatorDomain domain = program->tessellation.domain;
  program->tessellation.domain = DXBC_TESSELLATOR_DOMAIN_QUAD;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  CHECK(emission_is_rejected(program, &names));
  program->tessellation.domain = domain;

  const USILOpcode opcode = program->instructions[2].opcode;
  program->instructions[2].opcode = USIL_OP_ADD;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->instructions[2].opcode = opcode;

  const uint8_t control_point_lane =
      program->instructions[2].operands[1].swizzle[2];
  program->instructions[2].operands[1].swizzle[2] ^= 1u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->instructions[2].operands[1].swizzle[2] = control_point_lane;

  const uint8_t resource_lane =
      program->instructions[6].operands[2].swizzle[3];
  program->instructions[6].operands[2].swizzle[3] ^= 1u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->instructions[6].operands[2].swizzle[3] = resource_lane;

  const uint32_t lod_bits =
      program->instructions[6].operands[4].immediate_words[0];
  program->instructions[6].operands[4].immediate_words[0] ^= 1u;
  program->instructions[6].operands[4].imm_values[0] ^= 1u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->instructions[6].operands[4].immediate_words[0] = lod_bits;
  program->instructions[6].operands[4].imm_values[0] = lod_bits;

  const uint64_t matrix_row =
      program->instructions[18].operands[1].index_values[1];
  program->instructions[18].operands[1].index_values[1] = 18u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->instructions[18].operands[1].index_values[1] = matrix_row;

  const uint8_t return_type = program->textures[0].return_types[0];
  program->textures[0].return_types[0] = 4u;
  CHECK(!hlsl_exact_tessellation_lift_matches(program));
  program->textures[0].return_types[0] = return_type;

  CHECK(hlsl_exact_tessellation_lift_matches(program));
  return 0;
}

static int verify_unattested_tessellation_remains_rejected(void) {
  USILProgram program;
  memset(&program, 0, sizeof(program));
  memcpy(program.shader_type_model, "hs_5_0", sizeof("hs_5_0"));
  program.program_type = DXBC_PROGRAM_TYPE_HULL;
  program.has_stage_contract = true;
  program.tessellation.valid = true;
  program.tessellation.domain = DXBC_TESSELLATOR_DOMAIN_TRIANGLE;
  CHECK(!hlsl_exact_tessellation_lift_matches(&program));
  const HLSLEmitNames names = {"hs", "unused_input", "unused_output"};
  CHECK(emission_is_rejected(&program, &names));
  return 0;
}

int main(void) {
  CommonFileBytes fixture = {0};
  CHECK(common_file_read_regular(DXBC_LIMIT_TEST2_FIXTURE,
                                 16u * 1024u * 1024u,
                                 &fixture) == COMMON_FILE_OK);
  DXBCUSBDTableView table;
  DXBCUSBDDiagnostic diagnostic;
  CHECK(dxbc_usbd_table_open(&table, fixture.data, fixture.size, &diagnostic));
  CHECK(table.record_count == 4u);
  CHECK(with_stage(&table, "hull", verify_hull_exact_inverse) == 0);
  CHECK(with_stage(&table, "domain", verify_domain_exact_inverse) == 0);
  CHECK(verify_unattested_tessellation_remains_rejected() == 0);
  common_file_bytes_dispose(&fixture);
  puts("tessellation USIL tests passed");
  return 0;
}
