#include "translation/shaderlab_emitter_internal.h"
#include "dxbc/dxbc_hash.h"
#include "test_shaderlab_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,      \
              #condition);                                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static size_t count_text(const char *text, const char *needle);

static void initialize_pass(SerializedPass *pass,
                            SerializedSubProgram *subprograms,
                            SerializedSubProgramIdentity *identities,
                            int subprogram_count, int *platform) {
  memset(pass, 0, sizeof(*pass));
  *platform = 4;
  pass->has_serialized_platforms = true;
  pass->platform_count = 1;
  pass->platforms = platform;
  pass->program_mask = UINT32_C(1) <<
                       (UNITY_SERIALIZED_STAGE_VERTEX + 1u);
  pass->subprogram_count[0] = subprogram_count;
  pass->subprograms[0] = subprograms;
  pass->subprogram_identities[0] = identities;
  for (int i = 0; i < subprogram_count; ++i) {
    memset(&subprograms[i], 0, sizeof(subprograms[i]));
    memset(&identities[i], 0, sizeof(identities[i]));
    subprograms[i].program_type = 15;
    subprograms[i].blob_index = i;
    subprograms[i].shader_requirements = UINT64_C(0xe3);
    identities[i].hardware_tier_group = 3;
  }
}

static uint32_t read_u32_le(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint8_t *read_fixture_path(const char *path, size_t *out_size) {
  FILE *file = fopen(path, "rb");
  if (!file || fseek(file, 0, SEEK_END) != 0) return NULL;
  const long length = ftell(file);
  if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  uint8_t *bytes = (uint8_t *)malloc((size_t)length);
  if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
    free(bytes);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *out_size = (size_t)length;
  return bytes;
}

static uint8_t *read_fixture(size_t *out_size) {
  return read_fixture_path(SHADERLAB_STAGE_TEST_FIXTURE, out_size);
}

static const uint8_t *find_first_dxbc(const uint8_t *bytes, size_t size,
                                      size_t *out_size) {
  if (!bytes || !out_size) return NULL;
  for (size_t offset = 0; offset <= size && size - offset >= 32u; ++offset) {
    if (memcmp(bytes + offset, "DXBC", 4) != 0) continue;
    const uint32_t container_size = read_u32_le(bytes + offset + 24u);
    if (container_size >= 32u && (size_t)container_size <= size - offset) {
      *out_size = container_size;
      return bytes + offset;
    }
  }
  return NULL;
}

static uint8_t *clone_dxbc_with_shader_model(const uint8_t *dxbc,
                                             size_t dxbc_size,
                                             uint8_t major,
                                             uint8_t minor) {
  if (!dxbc || dxbc_size < 32u || major > 15u || minor > 15u ||
      read_u32_le(dxbc + 24u) != dxbc_size) {
    return NULL;
  }
  const uint32_t chunk_count = read_u32_le(dxbc + 28u);
  if (chunk_count > (dxbc_size - 32u) / 4u) return NULL;
  uint8_t *copy = (uint8_t *)malloc(dxbc_size);
  if (!copy) return NULL;
  memcpy(copy, dxbc, dxbc_size);
  bool found_executable = false;
  for (uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
    const uint32_t offset = read_u32_le(copy + 32u + chunk_index * 4u);
    if (offset > dxbc_size || dxbc_size - offset < 12u) continue;
    if (memcmp(copy + offset, "SHDR", 4u) != 0 &&
        memcmp(copy + offset, "SHEX", 4u) != 0) {
      continue;
    }
    if (found_executable) {
      free(copy);
      return NULL;
    }
    uint32_t token = read_u32_le(copy + offset + 8u);
    token = (token & ~UINT32_C(0xff)) | ((uint32_t)major << 4u) | minor;
    copy[offset + 8u] = (uint8_t)token;
    copy[offset + 9u] = (uint8_t)(token >> 8u);
    copy[offset + 10u] = (uint8_t)(token >> 16u);
    copy[offset + 11u] = (uint8_t)(token >> 24u);
    found_executable = true;
  }
  uint8_t hash[16];
  if (!found_executable || !dxbc_compute_hash(copy, dxbc_size, hash)) {
    free(copy);
    return NULL;
  }
  memcpy(copy + 4u, hash, sizeof(hash));
  return copy;
}

static int test_dynamic_keyword_universe(void) {
  enum { KEYWORD_COUNT = 513 };
  char(*storage)[24] = (char(*)[24])calloc(KEYWORD_COUNT, sizeof(*storage));
  char **keywords = (char **)calloc(KEYWORD_COUNT, sizeof(*keywords));
  CHECK(storage != NULL);
  CHECK(keywords != NULL);
  for (int i = 0; i < KEYWORD_COUNT; ++i) {
    snprintf(storage[i], sizeof(storage[i]), "EXACT_KEYWORD_%03d", i);
    keywords[i] = storage[i];
  }

  SerializedPass pass;
  SerializedSubProgram subprogram;
  SerializedSubProgramIdentity identity;
  int platform;
  initialize_pass(&pass, &subprogram, &identity, 1, &platform);
  subprogram.local_keyword_count = KEYWORD_COUNT;
  subprogram.local_keywords = keywords;

  size_t keyword_count = 0;
  size_t variant_count = 0;
  ShaderLabStageDiagnostic diagnostic;
  CHECK(!shaderlab_stage_validate_variants(&pass, 0, &keyword_count,
                                           &variant_count, &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_INCOMPLETE_PREDICATE);
  CHECK(keyword_count == 0u);
  CHECK(variant_count == 0u);

  free(keywords);
  free(storage);
  return 0;
}

static int test_duplicate_and_ambiguous_predicates(void) {
  SerializedPass pass;
  SerializedSubProgram subprograms[2];
  SerializedSubProgramIdentity identities[2];
  int platform;
  ShaderLabStageDiagnostic diagnostic;
  char *first_keywords[] = {"FEATURE_A", "FEATURE_B"};
  char *second_keywords[] = {"FEATURE_B", "FEATURE_A"};

  initialize_pass(&pass, subprograms, identities, 2, &platform);
  subprograms[0].local_keyword_count = 2;
  subprograms[0].local_keywords = first_keywords;
  subprograms[1].global_keyword_count = 2;
  subprograms[1].global_keywords = second_keywords;
  CHECK(!shaderlab_stage_validate_variants(&pass, 0, NULL, NULL,
                                           &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_AMBIGUOUS_PREDICATE);
  CHECK(diagnostic.subprogram_index >= 0);
  CHECK(diagnostic.conflicting_subprogram_index >= 0);

  char *duplicate_local[] = {"FEATURE_A"};
  char *duplicate_global[] = {"FEATURE_A"};
  initialize_pass(&pass, subprograms, identities, 1, &platform);
  subprograms[0].local_keyword_count = 1;
  subprograms[0].local_keywords = duplicate_local;
  subprograms[0].global_keyword_count = 1;
  subprograms[0].global_keywords = duplicate_global;
  CHECK(!shaderlab_stage_validate_variants(&pass, 0, NULL, NULL,
                                           &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_DUPLICATE_KEYWORD);

  char *invalid_keyword[] = {"FEATURE-A"};
  initialize_pass(&pass, subprograms, identities, 1, &platform);
  subprograms[0].local_keyword_count = 1;
  subprograms[0].local_keywords = invalid_keyword;
  CHECK(!shaderlab_stage_validate_variants(&pass, 0, NULL, NULL,
                                           &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_INVALID_KEYWORD);
  return 0;
}

static int test_hardware_tier_predicate_authority(void) {
  SerializedPass pass;
  SerializedSubProgram subprograms[3];
  SerializedSubProgramIdentity identities[3];
  int platform;
  ShaderLabStageDiagnostic diagnostic;

  initialize_pass(&pass, subprograms, identities, 3, &platform);
  identities[0].hardware_tier_group = 0;
  identities[1].hardware_tier_group = 1;
  identities[2].hardware_tier_group = 2;
  CHECK(shaderlab_stage_validate_variants(&pass, 0, NULL, NULL,
                                          &diagnostic));

  initialize_pass(&pass, subprograms, identities, 2, &platform);
  identities[0].hardware_tier_group = 0;
  identities[1].hardware_tier_group = 3;
  CHECK(!shaderlab_stage_validate_variants(&pass, 0, NULL, NULL,
                                           &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_AMBIGUOUS_PREDICATE);
  return 0;
}

static int test_incomplete_predicate_coverage_is_transactional(void) {
  SerializedPass pass;
  SerializedSubProgram subprogram;
  SerializedSubProgramIdentity identity;
  int platform;
  char *feature_keywords[] = {"FEATURE_B"};
  initialize_pass(&pass, &subprogram, &identity, 1, &platform);
  subprogram.local_keyword_count = 1;
  subprogram.local_keywords = feature_keywords;

  ShaderLabStageDiagnostic diagnostic;
  StringBuilder output;
  sb_init(&output);
  sb_append(&output, "unchanged");
  CHECK(!emit_stage_hlsl(&pass, 0, NULL, 0, NULL, NULL, 0, &output,
                         &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_INCOMPLETE_PREDICATE);
  CHECK(diagnostic.subprogram_index == -1);
  CHECK(strcmp(output.buf, "unchanged") == 0);
  CHECK(strstr(output.buf, "DXBCSandbox_exact_variant_predicate_missing") ==
        NULL);
  sb_free(&output);

  initialize_pass(&pass, &subprogram, &identity, 1, &platform);
  identity.hardware_tier_group = 0;
  sb_init(&output);
  sb_append(&output, "unchanged");
  CHECK(!emit_stage_hlsl(&pass, 0, NULL, 0, NULL, NULL, 0, &output,
                         &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_INCOMPLETE_PREDICATE);
  CHECK(diagnostic.subprogram_index == 0);
  CHECK(strcmp(output.buf, "unchanged") == 0);
  sb_free(&output);
  return 0;
}

static int test_authority_failures(void) {
  SerializedPass pass;
  SerializedSubProgram subprogram;
  SerializedSubProgramIdentity identity;
  int platform;
  ShaderLabStageDiagnostic diagnostic;

  initialize_pass(&pass, &subprogram, &identity, 1, &platform);
  pass.has_serialized_platforms = false;
  CHECK(!shaderlab_stage_validate_variants(&pass, 0, NULL, NULL,
                                           &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_MISSING_PLATFORM_AUTHORITY);

  initialize_pass(&pass, &subprogram, &identity, 1, &platform);
  subprogram.program_type = 6;
  CHECK(!shaderlab_stage_validate_variants(&pass, 0, NULL, NULL,
                                           &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_NO_D3D_VARIANTS);
  return 0;
}

static int test_failed_variant_propagates_transactionally(void) {
  SerializedPass pass;
  SerializedSubProgram vertex_subprogram;
  SerializedSubProgramIdentity vertex_identity;
  int platform;
  initialize_pass(&pass, &vertex_subprogram, &vertex_identity, 1, &platform);

  uint8_t invalid_variant[4] = {0, 0, 0, 0};
  uint8_t *segments[] = {invalid_variant};
  int segment_lengths[] = {(int)sizeof(invalid_variant)};
  BlobEntry entries[] = {{0, (int32_t)sizeof(invalid_variant), 0}};
  ShaderLabStageDiagnostic diagnostic;
  StringBuilder stage_output;
  sb_init(&stage_output);
  sb_append(&stage_output, "unchanged");
  CHECK(!emit_stage_hlsl(&pass, 0, entries, 1, segments, segment_lengths, 1,
                         &stage_output, &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_VARIANT_PARSE_FAILED);
  CHECK(strcmp(stage_output.buf, "unchanged") == 0);
  CHECK(strstr(stage_output.buf, "return v") == NULL);
  sb_free(&stage_output);

  SerializedSubProgram fragment_subprogram;
  SerializedSubProgramIdentity fragment_identity;
  memset(&fragment_subprogram, 0, sizeof(fragment_subprogram));
  memset(&fragment_identity, 0, sizeof(fragment_identity));
  fragment_subprogram.program_type = 15;
  fragment_subprogram.blob_index = 0;
  fragment_identity.hardware_tier_group = 3;
  pass.subprogram_count[1] = 1;
  pass.subprograms[1] = &fragment_subprogram;
  pass.subprogram_identities[1] = &fragment_identity;

  SerializedSubShader subshader;
  memset(&subshader, 0, sizeof(subshader));
  subshader.pass_count = 1;
  subshader.passes = &pass;
  SerializedShader shader;
  memset(&shader, 0, sizeof(shader));
  shader.name = "Stage/Failure";
  shader.subshader_count = 1;
  shader.subshaders = &subshader;

  StringBuilder candidate_output;
  ShaderLabCandidateDiagnostic candidate_diagnostic;
  sb_init(&candidate_output);
  sb_append(&candidate_output, "prefix");
  CHECK(!shaderlab_emit_candidate_with_diagnostic(
      &shader, entries, 1, segments, segment_lengths, 1, &candidate_output,
      &candidate_diagnostic));
  CHECK(strcmp(candidate_output.buf, "prefix") == 0);
  CHECK(strstr(candidate_output.buf, "return v") == NULL);
  CHECK(candidate_diagnostic.status ==
        SHADERLAB_CANDIDATE_PASS_TARGET_FAILED);
  CHECK(candidate_diagnostic.subshader_index == 0);
  CHECK(candidate_diagnostic.pass_index == 0);
  CHECK(candidate_diagnostic.target.status ==
        SHADERLAB_TARGET_VARIANT_PARSE_FAILED);
  CHECK(strcmp(shaderlab_candidate_reason_name(&candidate_diagnostic),
               "variant-parse-failed") == 0);
  sb_free(&candidate_output);

  StringBuilder readable_output;
  sb_init(&readable_output);
  CHECK(shaderlab_emit_raw(&shader, NULL, NULL, &readable_output));
  CHECK(strstr(readable_output.buf,
               "float4 vert(float4 v : POSITION) : SV_POSITION") != NULL);
  CHECK(strstr(readable_output.buf,
               "float4 frag() : SV_Target { return 0; }") != NULL);
  sb_free(&readable_output);
  return 0;
}

static int test_non_d3d_subshader_is_omitted_from_candidate(void) {
  SerializedPass passes[2];
  memset(passes, 0, sizeof(passes));
  passes[0].pass_type = 2;
  passes[0].texture_name = "_Grab";

  int glcore_platform = 15;
  SerializedSubProgram glcore_subprogram;
  memset(&glcore_subprogram, 0, sizeof(glcore_subprogram));
  glcore_subprogram.program_type = 6;
  passes[1].has_serialized_platforms = true;
  passes[1].platform_count = 1;
  passes[1].platforms = &glcore_platform;
  passes[1].subprogram_count[0] = 1;
  passes[1].subprograms[0] = &glcore_subprogram;
  CHECK(shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 15));

  /* Missing or contradictory authority must remain in the fail-closed
   * projection instead of being silently classified as another platform. */
  passes[1].has_serialized_platforms = false;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  passes[1].has_serialized_platforms = true;
  int duplicate_platforms[] = {15, 15};
  passes[1].platform_count = 2;
  passes[1].platforms = duplicate_platforms;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  passes[1].platform_count = 1;
  passes[1].platforms = &glcore_platform;

  /* Exclusion requires a complete, self-consistent platform plane.  A
   * GL-only list cannot hide D3D or unknown GPU records from the D3D
   * candidate, and unknown platform enum values carry no authority. */
  glcore_subprogram.program_type = 15;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  glcore_subprogram.program_type = 0;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  glcore_subprogram.program_type = 6;
  int unknown_platform = 999;
  passes[1].platforms = &unknown_platform;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  passes[1].platforms = &glcore_platform;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 999));

  int gl_platforms[] = {15, 9};
  SerializedSubProgram gl_subprograms[2];
  memset(gl_subprograms, 0, sizeof(gl_subprograms));
  gl_subprograms[0].program_type = 6;
  gl_subprograms[1].program_type = 2;
  passes[1].platform_count = 2;
  passes[1].platforms = gl_platforms;
  passes[1].subprogram_count[0] = 2;
  passes[1].subprograms[0] = gl_subprograms;
  CHECK(shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  gl_subprograms[1].program_type = 15;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));

  passes[1].platform_count = 1;
  passes[1].platforms = &glcore_platform;
  passes[1].subprogram_count[0] = 1;
  passes[1].subprograms[0] = &glcore_subprogram;

  SerializedSubShader subshaders[2];
  memset(subshaders, 0, sizeof(subshaders));
  subshaders[0].pass_count = 1;
  subshaders[0].passes = &passes[0];
  subshaders[1].pass_count = 1;
  subshaders[1].passes = &passes[1];

  SerializedShader shader;
  memset(&shader, 0, sizeof(shader));
  shader.name = "Stage/PlatformProjection";
  shader.subshader_count = 2;
  shader.subshaders = subshaders;

  StringBuilder candidate;
  ShaderLabCandidateDiagnostic diagnostic;
  sb_init(&candidate);
  CHECK(shaderlab_emit_candidate_with_diagnostic(
      &shader, NULL, 0, NULL, NULL, 0, &candidate, &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_CANDIDATE_OK);
  CHECK(count_text(candidate.buf, "SubShader\n") == 1u);
  CHECK(strstr(candidate.buf, "GrabPass {\n") != NULL);
  CHECK(strstr(candidate.buf, "HLSLPROGRAM") == NULL);
  sb_free(&candidate);

  /* Unity 2021.3 retains this exact shell in real PostProcessing shaders:
   * m_Platforms still names D3D11 and m_ProgramMask still names vertex plus
   * fragment, but every player-subprogram vector is empty.  It has no
   * selectable D3D program and must be projected away without dropping the
   * neighboring representable content. */
  int d3d_platform = 4;
  passes[1].platform_count = 1;
  passes[1].platforms = &d3d_platform;
  passes[1].subprogram_count[0] = 0;
  passes[1].subprograms[0] = NULL;
  passes[1].program_mask =
      (UINT32_C(1) << (UNITY_SERIALIZED_STAGE_VERTEX + 1u)) |
      (UINT32_C(1) << (UNITY_SERIALIZED_STAGE_FRAGMENT + 1u));
  CHECK(shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  sb_init(&candidate);
  CHECK(shaderlab_emit_candidate_with_diagnostic(
      &shader, NULL, 0, NULL, NULL, 0, &candidate, &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_CANDIDATE_OK);
  CHECK(count_text(candidate.buf, "SubShader\n") == 1u);
  CHECK(strstr(candidate.buf, "GrabPass {\n") != NULL);
  CHECK(strstr(candidate.buf, "HLSLPROGRAM") == NULL);
  sb_free(&candidate);

  /* A surviving target-owned row is selectable content, not a stripped
   * shell, even when every other field matches the positive case. */
  SerializedSubProgram d3d_subprogram;
  memset(&d3d_subprogram, 0, sizeof(d3d_subprogram));
  d3d_subprogram.program_type = 15;
  passes[1].subprogram_count[0] = 1;
  passes[1].subprograms[0] = &d3d_subprogram;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));

  /* Negative or pointer-inconsistent subprogram counts are malformed
   * authority and may never authorize projection. */
  passes[1].subprogram_count[0] = -1;
  passes[1].subprograms[0] = NULL;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  passes[1].subprogram_count[0] = 1;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  passes[1].subprogram_count[0] = 0;

  /* Near misses stay fail-closed: neither an empty stage mask nor an unknown
   * mask bit can establish that Unity stripped a real program shell. */
  passes[1].program_mask = 0U;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  passes[1].program_mask = UINT32_C(1) << 31;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));

  /* A target-listed plane with no target-owned row is contradictory, not a
   * provably foreign pass.  It must remain in the candidate and fail through
   * the typed target resolver instead of disappearing from the output. */
  int contradictory_platforms[] = {4, 15};
  passes[1].platform_count = 2;
  passes[1].platforms = contradictory_platforms;
  passes[1].program_mask = 0U;
  passes[1].subprogram_count[0] = 1;
  passes[1].subprograms[0] = &glcore_subprogram;
  CHECK(!shaderlab_pass_is_proven_not_platform(&passes[1], 4));
  sb_init(&candidate);
  CHECK(!shaderlab_emit_candidate_with_diagnostic(
      &shader, NULL, 0, NULL, NULL, 0, &candidate, &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_CANDIDATE_PASS_TARGET_FAILED);
  CHECK(diagnostic.subshader_index == 1);
  CHECK(diagnostic.pass_index == 0);
  CHECK(diagnostic.target.status == SHADERLAB_TARGET_NO_D3D_VARIANTS);
  CHECK(candidate.len == 0u);
  sb_free(&candidate);
  return 0;
}

static int test_canonical_exact_predicates(void) {
  size_t fixture_size = 0;
  uint8_t *fixture = read_fixture(&fixture_size);
  CHECK(fixture != NULL);
  size_t dxbc_size = 0;
  const uint8_t *dxbc = find_first_dxbc(fixture, fixture_size, &dxbc_size);
  CHECK(dxbc != NULL);

  size_t default_size = 0;
  size_t feature_size = 0;
  uint8_t *default_blob =
      test_shaderlab_variant_blob(dxbc, dxbc_size, 15, NULL, &default_size);
  uint8_t *feature_blob =
      test_shaderlab_variant_blob(dxbc, dxbc_size, 15, "FEATURE_B", &feature_size);
  CHECK(default_blob != NULL);
  CHECK(feature_blob != NULL);
  CHECK(default_size <= INT32_MAX);
  CHECK(feature_size <= INT32_MAX);

  SerializedPass pass;
  SerializedSubProgram subprograms[2];
  SerializedSubProgramIdentity identities[2];
  int platform;
  char *feature_keywords[] = {"FEATURE_B"};
  initialize_pass(&pass, subprograms, identities, 2, &platform);
  subprograms[1].local_keyword_count = 1;
  subprograms[1].local_keywords = feature_keywords;
  uint8_t *segments[] = {default_blob, feature_blob};
  int segment_lengths[] = {(int)default_size, (int)feature_size};
  BlobEntry entries[] = {
      {0, (int32_t)default_size, 0},
      {0, (int32_t)feature_size, 1},
  };

  StringBuilder first;
  StringBuilder second;
  ShaderLabStageDiagnostic diagnostic;
  sb_init(&first);
  sb_init(&second);
  CHECK(emit_stage_hlsl(&pass, 0, entries, 2, segments, segment_lengths, 2,
                        &first, &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_OK);
  CHECK(emit_stage_hlsl(&pass, 0, entries, 2, segments, segment_lengths, 2,
                        &second, &diagnostic));
  CHECK(strcmp(first.buf, second.buf) == 0);
  CHECK(strstr(first.buf, "#if !defined(FEATURE_B)") != NULL);
  CHECK(strstr(first.buf, "#elif defined(FEATURE_B)") != NULL);
  CHECK(strstr(first.buf,
               "#error DXBCSandbox_exact_variant_predicate_missing") ==
        NULL);
  CHECK(strstr(first.buf, "return v; }") == NULL);

  SerializedSubShader subshader;
  memset(&subshader, 0, sizeof(subshader));
  subshader.pass_count = 1;
  subshader.passes = &pass;
  SerializedShader shader;
  memset(&shader, 0, sizeof(shader));
  shader.name = "Stage/IncompletePredicate";
  shader.subshader_count = 1;
  shader.subshaders = &subshader;

  pass.subprogram_count[0] = 1;
  pass.subprograms[0] = &subprograms[1];
  pass.subprogram_identities[0] = &identities[1];
  ShaderLabCandidateDiagnostic candidate_diagnostic;
  StringBuilder candidate;
  sb_init(&candidate);
  sb_append(&candidate, "unchanged");
  CHECK(!shaderlab_emit_candidate_with_diagnostic(
      &shader, entries, 2, segments, segment_lengths, 2, &candidate,
      &candidate_diagnostic));
  CHECK(strcmp(candidate.buf, "unchanged") == 0);
  CHECK(candidate_diagnostic.status == SHADERLAB_CANDIDATE_STAGE_FAILED);
  CHECK(candidate_diagnostic.subshader_index == 0);
  CHECK(candidate_diagnostic.pass_index == 0);
  CHECK(candidate_diagnostic.stage.stage_index == 1);
  CHECK(candidate_diagnostic.stage.status ==
        SHADERLAB_STAGE_VARIANT_PLAN_FAILED);
  CHECK(candidate_diagnostic.stage.variant_plan_status ==
        SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT);
  CHECK(strcmp(shaderlab_candidate_status_name(candidate_diagnostic.status),
               "stage-failed") == 0);
  CHECK(strcmp(shaderlab_candidate_reason_name(&candidate_diagnostic),
               "invalid-argument") == 0);
  sb_free(&candidate);

  pass.subprogram_count[0] = 2;
  pass.subprograms[0] = subprograms;
  pass.subprogram_identities[0] = identities;

  StringBuilder rejected;
  sb_init(&rejected);
  sb_append(&rejected, "unchanged");
  pass.program_mask = 0u;
  CHECK(!emit_stage_hlsl(&pass, 0, entries, 2, segments, segment_lengths, 2,
                         &rejected, &diagnostic));
  CHECK(diagnostic.status == SHADERLAB_STAGE_STAGE_CONTRACT_FAILED);
  CHECK(diagnostic.stage_contract_status == DXBC_STAGE_CONTRACT_OK);
  CHECK(diagnostic.stage_tuple_status ==
        SHADER_STAGE_TUPLE_PROGRAM_MASK_MISMATCH);
  CHECK(strcmp(rejected.buf, "unchanged") == 0);
  sb_free(&rejected);
  sb_free(&second);
  sb_free(&first);
  free(feature_blob);
  free(default_blob);
  free(fixture);
  return 0;
}

static size_t count_text(const char *text, const char *needle) {
  size_t count = 0;
  const size_t length = strlen(needle);
  while ((text = strstr(text, needle)) != NULL) {
    ++count;
    text += length;
  }
  return count;
}

static int symbolic_axis_for_token(
    const ShaderLabVariantPlan *plan,
    const ShaderLabPassStageVariantPlan *stage,
    const char *token, size_t token_length) {
  if (!plan || !stage || !token) return -1;
  for (size_t axis_index = 0; axis_index < stage->axis_count;
       ++axis_index) {
    const ShaderLabVariantAxis *axis = &stage->axes[axis_index];
    if (axis->keyword_count != 1 || !axis->keyword_indices) return -1;
    const uint16_t raw = axis->keyword_indices[0];
    if ((int)raw >= plan->shader->keyword_names.count) return -1;
    const char *name = plan->shader->keyword_names.keywords[raw];
    if (strlen(name) == token_length &&
        memcmp(name, token, token_length) == 0) {
      return (int)axis_index;
    }
  }
  return -1;
}

static bool find_emitted_symbolic_selector_macro(
    const char *source, int stage_index, char *output,
    size_t output_capacity, const char **out_selector_begin) {
  if (!source || !output || output_capacity == 0 || stage_index < 0 ||
      stage_index >= 5) {
    return false;
  }
  char marker[80];
  const int marker_length_int = snprintf(
      marker, sizeof(marker),
      "#undef DXBCSANDBOX_SYMBOLIC_S%d_BEST_INDEX", stage_index);
  if (marker_length_int <= 0 ||
      (size_t)marker_length_int >= sizeof(marker)) {
    return false;
  }
  const char *selector_begin = strstr(source, marker);
  if (!selector_begin) return false;
  const char *name_begin = selector_begin + strlen("#undef ");
  const char *line_end = strchr(name_begin, '\n');
  if (!line_end) return false;
  const size_t name_length = (size_t)(line_end - name_begin);
  if (name_length == 0 || name_length >= output_capacity) return false;
  memcpy(output, name_begin, name_length);
  output[name_length] = '\0';
  if (out_selector_begin) *out_selector_begin = selector_begin;
  return true;
}

static bool evaluate_emitted_symbolic_selector(
    const char *source, const ShaderLabVariantPlan *plan, int stage_index,
    size_t request_mask, size_t *out_winner) {
  if (!source || !plan || !out_winner || stage_index < 0 ||
      stage_index >= 5 || plan->stages[stage_index].axis_count > 63u) {
    return false;
  }
  const ShaderLabPassStageVariantPlan *stage =
      &plan->stages[stage_index];
  char selector_macro[96];
  const char *cursor = NULL;
  if (!find_emitted_symbolic_selector_macro(
          source, stage_index, selector_macro, sizeof(selector_macro),
          &cursor)) {
    return false;
  }
  char define_prefix[112];
  const int prefix_length_int = snprintf(
      define_prefix, sizeof(define_prefix),
      "#define %s ", selector_macro);
  if (prefix_length_int <= 0 ||
      (size_t)prefix_length_int >= sizeof(define_prefix)) {
    return false;
  }
  const size_t prefix_length = (size_t)prefix_length_int;
  const char *end = strstr(
      source, "// DXBCSandbox-VariantPlan stage=");
  if (!cursor || !end || cursor >= end) return false;
  bool parent_active[64];
  bool branch_condition[64];
  size_t depth = 0;
  bool active = true;
  bool found = false;
  size_t winner = SIZE_MAX;
  while (cursor < end) {
    const char *line_end = strchr(cursor, '\n');
    if (!line_end || line_end > end) line_end = end;
    while (cursor < line_end && (*cursor == ' ' || *cursor == '\t'))
      ++cursor;
    if ((size_t)(line_end - cursor) >= 4u &&
        memcmp(cursor, "#if ", 4u) == 0) {
      const char *token = cursor + 4u;
      const char *token_end = token;
      while (token_end < line_end && *token_end != ' ' &&
             *token_end != '\t' && *token_end != '\r') {
        ++token_end;
      }
      const int axis = symbolic_axis_for_token(
          plan, stage, token, (size_t)(token_end - token));
      if (axis < 0 || depth >= 64u) return false;
      parent_active[depth] = active;
      branch_condition[depth] =
          (request_mask & (((size_t)1u) << (size_t)axis)) != 0;
      active = active && branch_condition[depth];
      ++depth;
    } else if ((size_t)(line_end - cursor) == 5u &&
               memcmp(cursor, "#else", 5u) == 0) {
      if (depth == 0) return false;
      active = parent_active[depth - 1u] &&
               !branch_condition[depth - 1u];
    } else if ((size_t)(line_end - cursor) == 6u &&
               memcmp(cursor, "#endif", 6u) == 0) {
      if (depth == 0) return false;
      active = parent_active[--depth];
    } else {
      if ((size_t)(line_end - cursor) > prefix_length &&
          memcmp(cursor, define_prefix, prefix_length) == 0 && active) {
        char *parsed_end = NULL;
        const unsigned long parsed =
            strtoul(cursor + prefix_length, &parsed_end, 10);
        if (!parsed_end || parsed_end == cursor + prefix_length ||
            parsed_end > line_end || found) {
          return false;
        }
        while (parsed_end < line_end &&
               (*parsed_end == ' ' || *parsed_end == '\t' ||
                *parsed_end == '\r')) {
          ++parsed_end;
        }
        if (parsed_end != line_end) return false;
        winner = (size_t)parsed;
        found = true;
      }
    }
    cursor = line_end < end ? line_end + 1u : end;
  }
  if (depth != 0 || !found) return false;
  *out_winner = winner;
  return true;
}

static bool expected_symbolic_winner(
    const ShaderLabVariantPlan *plan, int stage_index, size_t request_mask,
    size_t *out_winner) {
  if (!plan || !out_winner || stage_index < 0 || stage_index >= 5)
    return false;
  const ShaderLabPassStageVariantPlan *stage =
      &plan->stages[stage_index];
  ShaderLabVariantCandidate *candidates =
      (ShaderLabVariantCandidate *)calloc(
          stage->state_count, sizeof(*candidates));
  uint16_t *request_indices =
      (uint16_t *)calloc(stage->axis_count, sizeof(*request_indices));
  if (!candidates || !request_indices) {
    free(request_indices);
    free(candidates);
    return false;
  }
  for (size_t state_index = 0; state_index < stage->state_count;
       ++state_index) {
    candidates[state_index].state = stage->ordered_states[state_index];
    candidates[state_index].supported = true;
  }
  size_t request_count = 0;
  for (size_t axis_index = 0; axis_index < stage->axis_count;
       ++axis_index) {
    if ((request_mask & (((size_t)1u) << axis_index)) == 0) continue;
    request_indices[request_count++] =
        stage->axes[axis_index].keyword_indices[0];
  }
  for (size_t index = 1; index < request_count; ++index) {
    const uint16_t value = request_indices[index];
    size_t insertion = index;
    while (insertion != 0 && request_indices[insertion - 1u] > value) {
      request_indices[insertion] = request_indices[insertion - 1u];
      --insertion;
    }
    request_indices[insertion] = value;
  }
  const ShaderLabVariantState request = {
      request_indices, request_count};
  const ShaderLabVariantState pass_mask = {
      plan->pass->serialized_keyword_state_mask,
      (size_t)plan->pass->serialized_keyword_state_mask_count};
  int32_t score = INT32_MIN;
  const bool selected = shaderlab_variant_select_best(
      &request, &pass_mask, candidates, stage->state_count,
      out_winner, &score);
  free(request_indices);
  free(candidates);
  return selected;
}

static int test_sparse_shaped_planned_stage_aliases(void) {
  size_t fixture_size = 0;
  uint8_t *fixture = read_fixture(&fixture_size);
  CHECK(fixture != NULL);
  size_t dxbc_size = 0;
  const uint8_t *dxbc = find_first_dxbc(fixture, fixture_size, &dxbc_size);
  CHECK(dxbc != NULL);
  size_t blob_size = 0;
  uint8_t *blob = test_shaderlab_variant_blob(dxbc, dxbc_size, 15, NULL, &blob_size);
  CHECK(blob != NULL && blob_size <= INT32_MAX);
  uint8_t *segments[] = {blob};
  int segment_lengths[] = {(int)blob_size};
  BlobEntry entries[] = {{0, (int32_t)blob_size, 0}};

  ShaderLabBuiltinVariantDomain domain;
  CHECK(shaderlab_builtin_variant_domain_get(
      SHADERLAB_BUILTIN_VARIANT_FWDBASE,
      UNITY_SERIALIZED_STAGE_VERTEX, &domain));
  static const uint16_t sparse_masks[] = {
      0x001, 0x081, 0x00d, 0x007, 0x087, 0x00f,
      0x011, 0x091, 0x01d, 0x017, 0x097, 0x01f,
      0x101, 0x181, 0x10d, 0x111, 0x191, 0x11d};
  enum { SPARSE_ROWS = 18, SPARSE_VARIANTS = 36 };
  SerializedSubProgram *subprograms = (SerializedSubProgram *)calloc(
      SPARSE_VARIANTS, sizeof(*subprograms));
  SerializedSubProgramIdentity *identities =
      (SerializedSubProgramIdentity *)calloc(SPARSE_VARIANTS,
                                              sizeof(*identities));
  CHECK(subprograms != NULL && identities != NULL);
  for (size_t fog = 0; fog < 2; ++fog) {
    for (size_t row = 0; row < SPARSE_ROWS; ++row) {
      const size_t variant = fog * SPARSE_ROWS + row;
      subprograms[variant].program_type = 15;
      subprograms[variant].blob_index = 0;
      identities[variant].hardware_tier_group = 3;
      size_t count = 0;
      for (size_t bit = 0; bit < domain.keyword_count; ++bit) {
        if ((sparse_masks[row] & (uint16_t)(UINT16_C(1) << bit)) != 0)
          ++count;
      }
      if (fog != 0) ++count;
      int *raw = (int *)calloc(count, sizeof(*raw));
      CHECK(count == 0 || raw != NULL);
      size_t output = 0;
      for (size_t bit = 0; bit < domain.keyword_count; ++bit) {
        if ((sparse_masks[row] & (uint16_t)(UINT16_C(1) << bit)) != 0)
          raw[output++] = (int)bit;
      }
      if (fog != 0) raw[output++] = (int)domain.keyword_count;
      identities[variant].local_keyword_indices = raw;
      identities[variant].local_keyword_index_count = (int)count;
    }
  }

  int platform = 4;
  uint16_t pass_mask[10];
  char *keyword_names[10];
  uint8_t keyword_flags[10] = {0};
  for (size_t i = 0; i < domain.keyword_count; ++i) {
    pass_mask[i] = (uint16_t)i;
    keyword_names[i] = (char *)domain.keyword_names[i];
  }
  pass_mask[domain.keyword_count] = (uint16_t)domain.keyword_count;
  keyword_names[domain.keyword_count] = "FOG_LINEAR";
  SerializedPass pass;
  memset(&pass, 0, sizeof(pass));
  pass.has_serialized_platforms = true;
  pass.platform_count = 1;
  pass.platforms = &platform;
  pass.program_mask = UINT32_C(1) <<
                      (UNITY_SERIALIZED_STAGE_VERTEX + 1u);
  pass.subprogram_count[0] = SPARSE_VARIANTS;
  pass.subprograms[0] = subprograms;
  pass.subprogram_identities[0] = identities;
  pass.serialized_keyword_state_mask = pass_mask;
  pass.serialized_keyword_state_mask_count = 10;
  SerializedShader shader;
  memset(&shader, 0, sizeof(shader));
  shader.keyword_names.count = 10;
  shader.keyword_names.keywords = keyword_names;
  shader.keyword_flags = keyword_flags;

  ShaderLabVariantPlan plan;
  ShaderLabVariantPlanDiagnostic plan_diagnostic;
  shaderlab_variant_plan_init(&plan);
  CHECK(shaderlab_variant_plan_build(&shader, &pass, &plan,
                                     &plan_diagnostic) ==
        SHADERLAB_VARIANT_PLAN_OK);
  CHECK(!plan.has_builtin);
  CHECK(plan.stages[0].generated_state_count == 256);
  CHECK(plan.stages[0].generated_domain_is_symbolic_boolean);
  StringBuilder pragmas;
  sb_init(&pragmas);
  CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
  CHECK(strstr(pragmas.buf, "multi_compile_instancing") == NULL);
  CHECK(strstr(pragmas.buf, "multi_compile_fwdbase") == NULL);
  sb_free(&pragmas);

  ShaderLabStageDiagnostic stage_diagnostic;
  StringBuilder output;
  sb_init(&output);
  CHECK(emit_stage_hlsl_with_variant_plan(
      &plan, 0, entries, 1, segments, segment_lengths, 1, &output,
      &stage_diagnostic));
  CHECK(stage_diagnostic.status == SHADERLAB_STAGE_OK);
  char sparse_selector_macro[96];
  const char *sparse_selector_begin = NULL;
  CHECK(find_emitted_symbolic_selector_macro(
      output.buf, 0, sparse_selector_macro, sizeof(sparse_selector_macro),
      &sparse_selector_begin));
  char sparse_selector_guard[112];
  char sparse_selector_error[192];
  CHECK(snprintf(sparse_selector_guard, sizeof(sparse_selector_guard),
                 "#ifdef %s\n", sparse_selector_macro) > 0);
  CHECK(snprintf(sparse_selector_error, sizeof(sparse_selector_error),
                 "#error DXBCSandbox_symbolic_selector_macro_collision_%s\n",
                 sparse_selector_macro) > 0);
  CHECK(count_text(output.buf, sparse_selector_guard) == 1u);
  CHECK(count_text(output.buf, sparse_selector_error) == 1u);
  const char *sparse_guard_location =
      strstr(output.buf, sparse_selector_guard);
  const char *sparse_error_location =
      strstr(output.buf, sparse_selector_error);
  const char *sparse_guard_end = sparse_error_location
      ? strstr(sparse_error_location, "#endif\n") : NULL;
  CHECK(sparse_guard_location != NULL && sparse_error_location != NULL &&
        sparse_guard_end != NULL && sparse_selector_begin != NULL);
  CHECK(sparse_guard_location < sparse_error_location);
  CHECK(sparse_error_location < sparse_guard_end);
  CHECK(sparse_guard_end < sparse_selector_begin);
  CHECK(strstr(output.buf,
               "DXBCSandbox_exact_variant_predicate_missing") == NULL);
  CHECK(strstr(output.buf, "DXBCSANDBOX_SYMBOLIC_S0_SCORE_") == NULL);
  CHECK(strstr(output.buf, "DXBCSANDBOX_SYMBOLIC_S0_BEST_SCORE") == NULL);
  CHECK(strstr(output.buf, "DXBCSANDBOX_SYMBOLIC_S0_K0") == NULL);
  const char *manifest = strstr(
      output.buf, "// DXBCSandbox-VariantPlan stage=");
  CHECK(manifest != NULL);
  for (size_t axis_index = 0;
       axis_index < plan.stages[0].axis_count; ++axis_index) {
    const uint16_t raw =
        plan.stages[0].axes[axis_index].keyword_indices[0];
    char forbidden[128];
    CHECK(snprintf(forbidden, sizeof(forbidden), "defined(%s)",
                   shader.keyword_names.keywords[raw]) > 0);
    const char *found = strstr(output.buf, forbidden);
    CHECK(found == NULL || found >= manifest);
  }
  CHECK(strstr(output.buf, "#if FOG_LINEAR\n") != NULL);
  CHECK(strstr(output.buf,
               "#if DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX == 0") != NULL);
  CHECK(strstr(output.buf,
               "#undef DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX") != NULL);
  CHECK(count_text(output.buf,
                   "#define DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX ") <
        plan.stages[0].generated_state_count);
  for (size_t request_mask = 0;
       request_mask < plan.stages[0].generated_state_count;
       ++request_mask) {
    size_t emitted_winner = SIZE_MAX;
    size_t expected_winner = SIZE_MAX;
    CHECK(evaluate_emitted_symbolic_selector(
        output.buf, &plan, 0, request_mask, &emitted_winner));
    CHECK(expected_symbolic_winner(
        &plan, 0, request_mask, &expected_winner));
    CHECK(emitted_winner == expected_winner);
  }
  CHECK(count_text(output.buf,
                   "DXBCSandbox-VariantPlan stage=vertex") ==
        SPARSE_VARIANTS);
  CHECK(plan.stages[0].generated_states == NULL);
  CHECK(plan.stages[0].generated_aliases == NULL);

  CHECK(shaderlab_stage_symbolic_score_budget_allows(100000u, 1000u));
  CHECK(!shaderlab_stage_symbolic_score_budget_allows(100001u, 1000u));
  CHECK(!shaderlab_stage_symbolic_score_budget_allows(SIZE_MAX, 2u));
  CHECK(!shaderlab_stage_symbolic_score_budget_allows(1u, 0u));
  CHECK(!shaderlab_stage_symbolic_score_budget_allows(0u, 1u));

  const size_t saved_state_count = plan.stages[0].state_count;
  plan.stages[0].state_count = 390626u;
  StringBuilder work_limited;
  sb_init(&work_limited);
  sb_append(&work_limited, "unchanged");
  CHECK(!emit_stage_hlsl_with_variant_plan(
      &plan, 0, entries, 1, segments, segment_lengths, 1, &work_limited,
      &stage_diagnostic));
  CHECK(stage_diagnostic.status == SHADERLAB_STAGE_VARIANT_PLAN_FAILED);
  CHECK(stage_diagnostic.variant_plan_status ==
        SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED);
  CHECK(strcmp(work_limited.buf, "unchanged") == 0);
  sb_free(&work_limited);
  plan.stages[0].state_count = saved_state_count;

  ShaderLabVariantAxis limit_axes[19];
  for (size_t axis_index = 0; axis_index < 19u; ++axis_index) {
    limit_axes[axis_index] =
        plan.stages[0].axes[axis_index % plan.stages[0].axis_count];
  }
  ShaderLabVariantAxis *saved_axes = plan.stages[0].axes;
  const size_t saved_axis_count = plan.stages[0].axis_count;
  const size_t saved_generated_state_count =
      plan.stages[0].generated_state_count;
  plan.stages[0].axes = limit_axes;
  plan.stages[0].axis_count = 19u;
  plan.stages[0].generated_state_count = ((size_t)1u) << 19u;
  StringBuilder limited;
  sb_init(&limited);
  sb_append(&limited, "unchanged");
  CHECK(!emit_stage_hlsl_with_variant_plan(
      &plan, 0, entries, 1, segments, segment_lengths, 1, &limited,
      &stage_diagnostic));
  CHECK(stage_diagnostic.status == SHADERLAB_STAGE_VARIANT_PLAN_FAILED);
  CHECK(stage_diagnostic.variant_plan_status ==
        SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED);
  CHECK(strcmp(limited.buf, "unchanged") == 0);
  sb_free(&limited);
  plan.stages[0].axes = saved_axes;
  plan.stages[0].axis_count = saved_axis_count;
  plan.stages[0].generated_state_count = saved_generated_state_count;
  sb_free(&output);
  shaderlab_variant_plan_free(&plan);
  for (size_t i = 0; i < SPARSE_VARIANTS; ++i)
    free(identities[i].local_keyword_indices);
  free(identities);
  free(subprograms);
  free(blob);
  free(fixture);
  return 0;
}

static int test_two_stage_symbolic_selectors_are_exhaustive(void) {
  size_t fixture_size = 0;
  uint8_t *fixture = read_fixture(&fixture_size);
  CHECK(fixture != NULL);
  size_t vertex_dxbc_size = 0;
  const uint8_t *vertex_dxbc =
      find_first_dxbc(fixture, fixture_size, &vertex_dxbc_size);
  CHECK(vertex_dxbc != NULL);
  const size_t after_vertex =
      (size_t)(vertex_dxbc - fixture) + vertex_dxbc_size;
  CHECK(after_vertex <= fixture_size);
  size_t fragment_dxbc_size = 0;
  const uint8_t *fragment_dxbc = find_first_dxbc(
      fixture + after_vertex, fixture_size - after_vertex,
      &fragment_dxbc_size);
  CHECK(fragment_dxbc != NULL);

  size_t vertex_blob_size = 0;
  size_t fragment_blob_size = 0;
  uint8_t *vertex_blob = test_shaderlab_variant_blob(
      vertex_dxbc, vertex_dxbc_size, 15, NULL, &vertex_blob_size);
  uint8_t *fragment_blob = test_shaderlab_variant_blob(
      fragment_dxbc, fragment_dxbc_size, 17, NULL,
      &fragment_blob_size);
  CHECK(vertex_blob != NULL && fragment_blob != NULL);
  CHECK(vertex_blob_size <= INT32_MAX && fragment_blob_size <= INT32_MAX);
  uint8_t *segments[] = {vertex_blob, fragment_blob};
  int segment_lengths[] = {
      (int)vertex_blob_size, (int)fragment_blob_size};
  BlobEntry entries[] = {
      {0, (int32_t)vertex_blob_size, 0},
      {0, (int32_t)fragment_blob_size, 1},
  };

  enum { STATE_COUNT = 5 };
  SerializedSubProgram vertex_subprograms[STATE_COUNT];
  SerializedSubProgram fragment_subprograms[STATE_COUNT];
  SerializedSubProgramIdentity vertex_identities[STATE_COUNT];
  SerializedSubProgramIdentity fragment_identities[STATE_COUNT];
  memset(vertex_subprograms, 0, sizeof(vertex_subprograms));
  memset(fragment_subprograms, 0, sizeof(fragment_subprograms));
  memset(vertex_identities, 0, sizeof(vertex_identities));
  memset(fragment_identities, 0, sizeof(fragment_identities));

  int vertex_a[] = {0};
  int vertex_b[] = {1};
  int vertex_only[] = {2};
  int vertex_ab[] = {0, 1};
  int fragment_b[] = {3};
  int fragment_a[] = {4};
  int fragment_only[] = {5};
  int fragment_ba[] = {3, 4};
  int *vertex_states[STATE_COUNT] = {
      NULL, vertex_a, vertex_b, vertex_only, vertex_ab};
  int *fragment_states[STATE_COUNT] = {
      NULL, fragment_b, fragment_a, fragment_only, fragment_ba};
  const int state_counts[STATE_COUNT] = {0, 1, 1, 1, 2};
  for (size_t state = 0; state < STATE_COUNT; ++state) {
    vertex_subprograms[state].program_type = 15;
    vertex_subprograms[state].blob_index = 0;
    vertex_subprograms[state].shader_requirements = UINT64_C(0xe3);
    vertex_identities[state].hardware_tier_group = 3;
    vertex_identities[state].local_keyword_indices = vertex_states[state];
    vertex_identities[state].local_keyword_index_count = state_counts[state];

    fragment_subprograms[state].program_type = 17;
    fragment_subprograms[state].blob_index = 1;
    fragment_subprograms[state].shader_requirements = UINT64_C(0xe3);
    fragment_identities[state].hardware_tier_group = 3;
    fragment_identities[state].local_keyword_indices = fragment_states[state];
    fragment_identities[state].local_keyword_index_count = state_counts[state];
  }

  int platform = 4;
  uint16_t pass_mask[] = {0, 1, 2, 3, 4, 5};
  SerializedPass pass;
  memset(&pass, 0, sizeof(pass));
  pass.has_serialized_platforms = true;
  pass.platform_count = 1;
  pass.platforms = &platform;
  pass.program_mask =
      (UINT32_C(1) << (UNITY_SERIALIZED_STAGE_VERTEX + 1u)) |
      (UINT32_C(1) << (UNITY_SERIALIZED_STAGE_FRAGMENT + 1u));
  pass.subprogram_count[0] = STATE_COUNT;
  pass.subprograms[0] = vertex_subprograms;
  pass.subprogram_identities[0] = vertex_identities;
  pass.subprogram_count[1] = STATE_COUNT;
  pass.subprograms[1] = fragment_subprograms;
  pass.subprogram_identities[1] = fragment_identities;
  pass.serialized_keyword_state_mask = pass_mask;
  pass.serialized_keyword_state_mask_count = 6;

  char collision[] = "DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX";
  char *keyword_names[] = {
      collision, "B", "V_ONLY", "B", collision,
      "F_ONLY", "DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX_1"};
  uint8_t keyword_flags[7] = {0};
  SerializedShader shader;
  memset(&shader, 0, sizeof(shader));
  shader.keyword_names.count = 7;
  shader.keyword_names.keywords = keyword_names;
  shader.keyword_flags = keyword_flags;

  ShaderLabVariantPlan plan;
  ShaderLabVariantPlanDiagnostic plan_diagnostic;
  shaderlab_variant_plan_init(&plan);
  CHECK(shaderlab_variant_plan_build(
            &shader, &pass, &plan, &plan_diagnostic) ==
        SHADERLAB_VARIANT_PLAN_OK);
  CHECK(!plan.has_builtin);
  CHECK(!plan.external_axes_are_shared);
  for (int stage_index = 0; stage_index < 2; ++stage_index) {
    CHECK(plan.stages[stage_index].generated_domain_is_symbolic_boolean);
    CHECK(plan.stages[stage_index].axis_count == 3u);
    CHECK(plan.stages[stage_index].generated_state_count == 8u);
  }

  StringBuilder pragmas;
  sb_init(&pragmas);
  CHECK(shaderlab_variant_plan_emit_pragmas(&plan, &pragmas, 0));
  char collision_pragma[128];
  CHECK(snprintf(collision_pragma, sizeof(collision_pragma),
                 "#pragma multi_compile __ %s\n", collision) > 0);
  CHECK(count_text(pragmas.buf, collision_pragma) == 1u);
  CHECK(count_text(pragmas.buf, "#pragma multi_compile __ B\n") == 1u);
  CHECK(strstr(pragmas.buf,
               "#pragma multi_compile_vertex __ V_ONLY\n") != NULL);
  CHECK(strstr(pragmas.buf,
               "#pragma multi_compile_fragment __ F_ONLY\n") != NULL);
  sb_free(&pragmas);

  for (int stage_index = 0; stage_index < 2; ++stage_index) {
    ShaderLabStageDiagnostic stage_diagnostic;
    StringBuilder output;
    sb_init(&output);
    const bool emitted = emit_stage_hlsl_with_variant_plan(
        &plan, stage_index, entries, 2, segments, segment_lengths, 2,
        &output, &stage_diagnostic);
    if (!emitted) {
      fprintf(stderr,
              "two-stage symbolic emission failed: stage=%d status=%d "
              "plan=%d subprogram=%d contract=%d tuple=%d\n",
              stage_index, (int)stage_diagnostic.status,
              (int)stage_diagnostic.variant_plan_status,
              stage_diagnostic.subprogram_index,
              (int)stage_diagnostic.stage_contract_status,
              (int)stage_diagnostic.stage_tuple_status);
    }
    CHECK(emitted);
    CHECK(stage_diagnostic.status == SHADERLAB_STAGE_OK);
    char selector_macro[96];
    const char *selector_begin = NULL;
    CHECK(find_emitted_symbolic_selector_macro(
        output.buf, stage_index, selector_macro, sizeof(selector_macro),
        &selector_begin));
    if (stage_index == 0) {
      CHECK(strcmp(selector_macro,
                   "DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX_2") == 0);
      CHECK(strstr(output.buf,
                   "#if DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX\n") != NULL);
      CHECK(strstr(output.buf,
                   "#undef DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX\n") == NULL);
    } else {
      CHECK(strcmp(selector_macro,
                   "DXBCSANDBOX_SYMBOLIC_S1_BEST_INDEX") == 0);
    }
    char selector_undef[112];
    char selector_define[112];
    char selector_predicate[112];
    CHECK(snprintf(selector_undef, sizeof(selector_undef), "#undef %s\n",
                   selector_macro) > 0);
    CHECK(snprintf(selector_define, sizeof(selector_define), "#define %s ",
                   selector_macro) > 0);
    CHECK(snprintf(selector_predicate, sizeof(selector_predicate), "%s == ",
                   selector_macro) > 0);
    char selector_guard[112];
    CHECK(snprintf(selector_guard, sizeof(selector_guard), "#ifdef %s\n",
                   selector_macro) > 0);
    CHECK(count_text(output.buf, selector_undef) == 2u);
    CHECK(count_text(output.buf, selector_guard) == 1u);
    const char *selector_guard_location = strstr(output.buf, selector_guard);
    const char *selector_guard_end = selector_guard_location
        ? strstr(selector_guard_location, "#endif\n")
        : NULL;
    CHECK(selector_guard_location != NULL && selector_guard_end != NULL &&
          selector_begin != NULL);
    CHECK(selector_guard_location < selector_guard_end);
    CHECK(selector_guard_end < selector_begin);
    CHECK(count_text(output.buf, selector_define) > 0u);
    CHECK(count_text(output.buf, selector_predicate) == STATE_COUNT);
    if (stage_index == 0) {
      CHECK(strstr(output.buf,
                   "#ifdef DXBCSANDBOX_SYMBOLIC_S0_BEST_INDEX\n") == NULL);
    }
    for (size_t request_mask = 0;
         request_mask < plan.stages[stage_index].generated_state_count;
         ++request_mask) {
      size_t emitted_winner = SIZE_MAX;
      size_t expected_winner = SIZE_MAX;
      CHECK(evaluate_emitted_symbolic_selector(
          output.buf, &plan, stage_index, request_mask, &emitted_winner));
      CHECK(expected_symbolic_winner(
          &plan, stage_index, request_mask, &expected_winner));
      CHECK(emitted_winner == expected_winner);
    }
    sb_free(&output);
  }

  shaderlab_variant_plan_free(&plan);
  free(fragment_blob);
  free(vertex_blob);
  free(fixture);
  return 0;
}

/* The checked-in target comes from the adjacent authored float4 source.
 * Full-container equality and finite rendering are separate live checks; this
 * test pins candidate selection, metadata reuse, and atomic rejection. */
static int test_high_level_shaderlab_candidate(bool conditional) {
  size_t fixture_size = 0;
  uint8_t *fixture = read_fixture_path(conditional ? SHADERLAB_CONDITIONAL_TEST_FIXTURE :
                                                    SHADERLAB_EXPRESSION_TEST_FIXTURE,
                                      &fixture_size);
  CHECK(fixture != NULL);
  uint8_t *segments[3] = {NULL, NULL, NULL};
  int segment_lengths[3] = {0, 0, 0};
  BlobEntry entries[3];
  uint8_t target_digests[2][COMMON_SHA256_DIGEST_SIZE];
  size_t cursor = 0;
  for (int stage = 0; stage < 2; ++stage) {
    size_t dxbc_size = 0;
    const uint8_t *dxbc = find_first_dxbc(fixture + cursor,
                                         fixture_size - cursor, &dxbc_size);
    CHECK(dxbc != NULL);
    common_sha256(dxbc, dxbc_size, target_digests[stage]);
    cursor = (size_t)(dxbc - fixture) + dxbc_size;
    size_t blob_size = 0;
    segments[stage] = test_shaderlab_variant_blob(dxbc, dxbc_size, stage ? 17 : 15,
                                         NULL, &blob_size);
    CHECK(segments[stage] != NULL && blob_size <= INT32_MAX);
    segment_lengths[stage] = (int)blob_size;
    entries[stage] = (BlobEntry){0, (int32_t)blob_size, stage};
  }

  SerializedPass passes[2];
  SerializedSubProgram programs[2];
  SerializedSubProgramIdentity identities[2];
  int platform;
  initialize_pass(&passes[0], programs, identities, 2, &platform);
  programs[1].program_type = 17;
  passes[0].subprogram_count[0] = 1;
  passes[0].program_mask |= UINT32_C(1) <<
                            (UNITY_SERIALIZED_STAGE_FRAGMENT + 1u);
  passes[0].subprogram_count[1] = 1;
  passes[0].subprograms[1] = &programs[1];
  passes[0].subprogram_identities[1] = &identities[1];
  passes[1] = passes[0];
  SerializedSubShader subshader = {0};
  subshader.pass_count = 1;
  subshader.passes = passes;
  SerializedShader shader = {0};
  shader.name = conditional ? "Experiment/ConditionalFixture" : "Experiment/ExpressionFixture";
  shader.subshader_count = 1;
  shader.subshaders = &subshader;

  StringBuilder high;
  StringBuilder low;
  StringBuilder repeated;
  sb_init(&high);
  sb_init(&low);
  sb_init(&repeated);
  ShaderLabCandidateDiagnostic diagnostic;
  ShaderLabExpressionSourceMap map = {0};
  const char *prefix = "// caller prefix\n";
  sb_append(&high, prefix);
  CHECK(shaderlab_emit_high_level_candidate_with_source_map(
      &shader, entries, 2, segments, segment_lengths, 2, &high, &map, &diagnostic));
  CHECK(shaderlab_expression_source_map_matches_source(&map, &high));
  CHECK(map.count == 2u);
  for (size_t i = 0; i < map.count; ++i) {
    const ShaderLabExpressionSourceRecord *record = &map.records[i];
    CHECK(record->subshader_index == 0 && record->pass_index == 0);
    CHECK(record->stage_index == (int)i && record->subprogram_index == 0);
    CHECK(record->blob_index == (int)i && record->hardware_tier_group == 3);
    CHECK(record->serialized_state == 0 &&
          record->instructions.count == (conditional ? (i ? 11u : 4u) : 3u));
    CHECK(memcmp(record->target_digest, target_digests[i], sizeof(record->target_digest)) == 0);
    for (size_t j = 0; j < record->instructions.count; ++j) {
      const HLSLExpressionOrigin *origin = &record->instructions.origins[j];
      CHECK(origin->source_begin >= strlen(prefix));
      CHECK(origin->source_end <= high.len);
      CHECK(high.buf[origin->source_begin] == '(' ||
            high.buf[origin->source_begin] == ' ');
      if (origin->kind == HLSL_EXPRESSION_ORIGIN_RETURN)
        CHECK(high.buf[origin->source_end - 1] == '\n');
    }
  }
  const HLSLExpressionSourceMap *fragment_map = &map.records[1].instructions;
  const HLSLExpressionOrigin *nested = &fragment_map->origins[0];
  const HLSLExpressionOrigin *outer = &fragment_map->origins[1];
  if (conditional) {
    CHECK(nested->kind == HLSL_EXPRESSION_ORIGIN_CONTROL);
    CHECK(count_text(high.buf, "[branch] if (") == 2u);
    CHECK(strstr(high.buf, "dxbc_merge_i9_r0 = dxbc_merge_i6_r0;") != NULL);
    for (size_t index = 0; index < fragment_map->count; ++index) {
      const HLSLExpressionOrigin *origin = &fragment_map->origins[index];
      if (origin->kind == HLSL_EXPRESSION_ORIGIN_CONTROL) {
        CHECK(origin->destination_lanes == 0);
        CHECK(high.buf[origin->source_end - 1] == '\n');
        const char *token = index == 0u || index == 1u ? "[branch] if (" :
                            index == 3u || index == 6u ? "} else {" : "}";
        const char *found = strstr(high.buf + origin->source_begin, token);
        CHECK(found && (size_t)(found - high.buf) + strlen(token) <= origin->source_end);
      }
    }
  } else {
    CHECK(nested->source_begin > outer->source_begin);
    CHECK(nested->source_end < outer->source_end);
    CHECK(outer->source_end - outer->source_begin == strlen("(((v1) * (v1.yzwx)) * (v1.zwxy))"));
    CHECK(memcmp(high.buf + outer->source_begin,
                 "(((v1) * (v1.yzwx)) * (v1.zwxy))",
                 outer->source_end - outer->source_begin) == 0);
  }
  for (int mutation = 0; mutation < 8; ++mutation) {
    ShaderLabExpressionSourceRecord saved = map.records[0];
    const size_t saved_size = map.source_size;
    switch (mutation) {
      case 0: ++map.source_size; break;
      case 1: map.source_digest[0] ^= 1u; break;
      case 2: map.records[0].instructions.origins[0].source_end = high.len + 1; break;
      case 3: map.records[0].instructions.origins[0].instruction_index = -1; break;
      case 4: map.records[0].instructions.origins[0].kind = HLSL_EXPRESSION_ORIGIN_UNMAPPED; break;
      case 5: map.records[0].instructions.origins[0].kind = HLSL_EXPRESSION_ORIGIN_DEAD; break;
      case 6: map.records[0].hardware_tier_group = 4; break;
      case 7: high.buf[0] = '#'; break;
    }
    CHECK(!shaderlab_expression_source_map_matches_source(&map, &high));
    map.records[0] = saved;
    map.source_size = saved_size;
    if (mutation == 1) map.source_digest[0] ^= 1u;
    high.buf[0] = '/';
    CHECK(shaderlab_expression_source_map_matches_source(&map, &high));
  }
  CHECK(diagnostic.status == SHADERLAB_CANDIDATE_OK);
  if (!conditional)
    CHECK(strstr(high.buf, "o0 = (((v1) * (v1.yzwx)) * (v1.zwxy));") != NULL);
  CHECK(strstr(high.buf, "float4 r0") == NULL);
  CHECK(count_text(high.buf, "Single exact planned variant") == 2u);
  CHECK(shaderlab_emit_candidate_with_diagnostic(
      &shader, entries, 2, segments, segment_lengths, 2, &low, &diagnostic));
  CHECK(strstr(low.buf, "float4 r0") != NULL);
  CHECK(strcmp(low.buf, high.buf) != 0);
  sb_append(&repeated, prefix);
  CHECK(shaderlab_emit_high_level_candidate(
      &shader, entries, 2, segments, segment_lengths, 2, &repeated, &diagnostic));
  CHECK(strcmp(high.buf, repeated.buf) == 0);
  sb_free(&repeated);

  /* Each concrete tier has its own source body even when target bytes agree.
   * The map must retain those identities rather than deduplicate by hash. */
  SerializedSubProgram tier_programs[2][3];
  SerializedSubProgramIdentity tier_identities[2][3];
  for (int stage = 0; stage < 2; ++stage) {
    for (int tier = 0; tier < 3; ++tier) {
      tier_programs[stage][tier] = programs[stage];
      tier_identities[stage][tier] = identities[stage];
      tier_identities[stage][tier].hardware_tier_group = tier;
    }
    passes[0].subprogram_count[stage] = 3;
    passes[0].subprograms[stage] = tier_programs[stage];
    passes[0].subprogram_identities[stage] = tier_identities[stage];
  }
  sb_init(&repeated);
  CHECK(shaderlab_emit_high_level_candidate_with_source_map(
      &shader, entries, 2, segments, segment_lengths, 2, &repeated, &map, &diagnostic));
  CHECK(shaderlab_expression_source_map_matches_source(&map, &repeated));
  CHECK(map.count == 6u);
  for (size_t i = 0; i < map.count; ++i) {
    CHECK(map.records[i].stage_index == (int)(i / 3u));
    CHECK(map.records[i].hardware_tier_group == (int)(i % 3u));
    CHECK(map.records[i].subprogram_index == (int)(i % 3u));
    CHECK(map.records[i].serialized_state == 0u);
    if (i) CHECK(map.records[i].instructions.origins[0].source_begin >
                 map.records[i - 1].instructions.origins[2].source_end);
  }
  sb_free(&repeated);
  for (int stage = 0; stage < 2; ++stage) {
    passes[0].subprogram_count[stage] = 1;
    passes[0].subprograms[stage] = &programs[stage];
    passes[0].subprogram_identities[stage] = &identities[stage];
  }

  /* Reject a valid low-level vertex with cbuffer operations in a later pass.
   * The fully emitted first pass must not leak into the caller's fallback. */
  size_t unsupported_size = 0;
  uint8_t *unsupported = read_fixture(&unsupported_size);
  CHECK(unsupported != NULL);
  size_t dxbc_size = 0;
  const uint8_t *dxbc = find_first_dxbc(unsupported, unsupported_size, &dxbc_size);
  CHECK(dxbc != NULL);
  size_t blob_size = 0;
  segments[2] = test_shaderlab_variant_blob(dxbc, dxbc_size, 15, NULL, &blob_size);
  CHECK(segments[2] != NULL && blob_size <= INT32_MAX);
  segment_lengths[2] = (int)blob_size;
  entries[2] = (BlobEntry){0, (int32_t)blob_size, 2};
  SerializedSubProgram unsupported_program = programs[0];
  unsupported_program.blob_index = 2;
  passes[1].subprograms[0] = &unsupported_program;
  subshader.pass_count = 2;
  sb_init(&repeated);
  sb_append(&repeated, low.buf);
  CHECK(!shaderlab_emit_high_level_candidate_with_source_map(
      &shader, entries, 3, segments, segment_lengths, 3, &repeated, &map, &diagnostic));
  CHECK(!map.complete && map.count == 0u && map.records == NULL);
  CHECK(strcmp(repeated.buf, low.buf) == 0);
  CHECK(diagnostic.status == SHADERLAB_CANDIDATE_STAGE_FAILED);
  CHECK(diagnostic.subshader_index == 0 && diagnostic.pass_index == 1);
  CHECK(diagnostic.stage.stage_index == 0);
  CHECK(diagnostic.stage.status == SHADERLAB_STAGE_HLSL_EMISSION_FAILED);
  sb_free(&repeated);

  /* Reject a later variant as well as a later pass. Every generated branch
   * must meet the high-level contract, including a feature-disabled default. */
  subshader.pass_count = 1;
  SerializedSubProgram variants[] = {programs[0], unsupported_program};
  SerializedSubProgramIdentity variant_identities[] = {identities[0], identities[0]};
  char *feature_names[] = {"FEATURE"};
  uint8_t feature_flags[] = {0};
  int feature_index = 0;
  uint16_t keyword_mask = 0;
  variants[1].local_keyword_count = 1;
  variants[1].local_keywords = feature_names;
  variant_identities[1].local_keyword_index_count = 1;
  variant_identities[1].local_keyword_indices = &feature_index;
  passes[0].subprogram_count[0] = 2;
  passes[0].subprograms[0] = variants;
  passes[0].subprogram_identities[0] = variant_identities;
  passes[0].serialized_keyword_state_mask_count = 1;
  passes[0].serialized_keyword_state_mask = &keyword_mask;
  shader.keyword_names.count = 1;
  shader.keyword_names.keywords = feature_names;
  shader.keyword_flags = feature_flags;
  sb_init(&repeated);
  sb_append(&repeated, low.buf);
  CHECK(!shaderlab_emit_high_level_candidate_with_source_map(
      &shader, entries, 3, segments, segment_lengths, 3, &repeated, &map, &diagnostic));
  CHECK(!map.complete && map.count == 0u && map.records == NULL);
  CHECK(strcmp(repeated.buf, low.buf) == 0);
  CHECK(diagnostic.status == SHADERLAB_CANDIDATE_STAGE_FAILED);
  CHECK(diagnostic.stage.subprogram_index == 1);
  CHECK(diagnostic.stage.status == SHADERLAB_STAGE_HLSL_EMISSION_FAILED);
  sb_free(&repeated);
  variants[1].blob_index = 0;
  sb_init(&repeated);
  CHECK(shaderlab_emit_high_level_candidate_with_source_map(
      &shader, entries, 2, segments, segment_lengths, 2, &repeated, &map, &diagnostic));
  CHECK(shaderlab_expression_source_map_matches_source(&map, &repeated));
  CHECK(map.count == 3u);
  CHECK(map.records[1].stage_index == 0 && map.records[1].subprogram_index == 1);
  CHECK(map.records[1].serialized_state == 1u);
  CHECK(map.records[1].instructions.origins[0].source_begin >
        map.records[0].instructions.origins[2].source_end);
  CHECK(map.records[2].instructions.origins[0].source_begin >
        map.records[1].instructions.origins[2].source_end);
  CHECK(count_text(repeated.buf, "// DXBCSandbox-VariantPlan stage=vertex") == 2u);
  CHECK(strstr(repeated.buf, "#pragma multi_compile_vertex __ FEATURE") != NULL);
  sb_free(&repeated);
  passes[0].subprogram_count[0] = 1;
  passes[0].subprograms[0] = programs;
  passes[0].subprogram_identities[0] = identities;
  passes[0].serialized_keyword_state_mask_count = 0;
  passes[0].serialized_keyword_state_mask = NULL;

  /* A macro collision in the complete shader keyword universe invalidates a
   * candidate even when that keyword is absent in the currently selected row. */
  char *keywords[] = {"v1"};
  uint8_t flags[] = {0};
  shader.keyword_names.count = 1;
  shader.keyword_names.keywords = keywords;
  shader.keyword_flags = flags;
  sb_init(&repeated);
  sb_append(&repeated, low.buf);
  CHECK(!shaderlab_emit_high_level_candidate_with_source_map(
      &shader, entries, 2, segments, segment_lengths, 2, &repeated, &map, &diagnostic));
  CHECK(!map.complete && map.count == 0u && map.records == NULL);
  CHECK(strcmp(repeated.buf, low.buf) == 0);
  CHECK(diagnostic.status == SHADERLAB_CANDIDATE_STAGE_FAILED);
  CHECK(diagnostic.stage.status == SHADERLAB_STAGE_HLSL_EMISSION_FAILED);
  sb_free(&repeated);
  sb_free(&low);
  sb_free(&high);
  shaderlab_expression_source_map_free(&map);
  for (size_t i = 0; i < 3; ++i) free(segments[i]);
  free(unsupported);
  free(fixture);
  return 0;
}

static int test_requirements_target_projection(void) {
  static const struct {
    uint64_t requirements;
    ShaderLabTargetVersion target;
  } cases[] = {
      {UINT64_C(0x1), SHADERLAB_TARGET_2_0},
      {UINT64_C(0x21), SHADERLAB_TARGET_2_5},
      {UINT64_C(0xe3), SHADERLAB_TARGET_3_0},
      {UINT64_C(0xfeb), SHADERLAB_TARGET_3_5},
      {UINT64_C(0x1feb), SHADERLAB_TARGET_4_0},
      {UINT64_C(0x10cfeb), SHADERLAB_TARGET_4_5},
      {UINT64_C(0x131feb), SHADERLAB_TARGET_4_6},
      {UINT64_C(0x13dfeb), SHADERLAB_TARGET_5_0},
      {UINT64_C(0), SHADERLAB_TARGET_3_0},
      {UINT64_C(0x800), SHADERLAB_TARGET_3_5},
      {UINT64_C(0x8), SHADERLAB_TARGET_3_0},
      {UINT64_C(0x4000), SHADERLAB_TARGET_4_5},
      {UINT64_C(0x1000), SHADERLAB_TARGET_4_0},
      {UINT64_C(0x20000), SHADERLAB_TARGET_4_6},
      {UINT64_C(0x1008), SHADERLAB_TARGET_4_0},
      {UINT64_C(0x4800), SHADERLAB_TARGET_4_5},
      {UINT64_C(0x24000), SHADERLAB_TARGET_5_0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    CHECK(shaderlab_target_from_requirements(cases[i].requirements) ==
          cases[i].target);
  }
  CHECK(shaderlab_target_from_requirements(
            UINT64_C(0x8000000000000000) | UINT64_C(0xe3)) ==
        SHADERLAB_TARGET_3_0);
  CHECK(strcmp(shaderlab_target_version_name(SHADERLAB_TARGET_2_0),
               "2.0") == 0);
  CHECK(strcmp(shaderlab_target_version_name(SHADERLAB_TARGET_5_0),
               "5.0") == 0);
  CHECK(shaderlab_target_version_name((ShaderLabTargetVersion)99) == NULL);

  StringBuilder pragmas;
  sb_init(&pragmas);
  CHECK(shaderlab_requirements_emit_pragmas(
      &pragmas, SHADERLAB_TARGET_2_5, UINT64_C(0x2061), 3));
  CHECK(strcmp(pragmas.buf,
               "            #pragma require samplelod cubearray\n") == 0);
  sb_free(&pragmas);

  sb_init(&pragmas);
  CHECK(shaderlab_requirements_emit_pragmas(
      &pragmas, SHADERLAB_TARGET_3_0, UINT64_C(0xe3), 0));
  CHECK(pragmas.len == 0u);
  sb_free(&pragmas);

  sb_init(&pragmas);
  sb_append(&pragmas, "unchanged");
  CHECK(!shaderlab_requirements_emit_pragmas(
      &pragmas, SHADERLAB_TARGET_4_5, UINT64_C(0x100feb), 0));
  CHECK(strcmp(pragmas.buf, "unchanged") == 0);
  CHECK(!shaderlab_requirements_emit_pragmas(
      &pragmas, SHADERLAB_TARGET_2_0, UINT64_C(0x101), 0));
  CHECK(strcmp(pragmas.buf, "unchanged") == 0);
  CHECK(!shaderlab_requirements_emit_pragmas(
      &pragmas, SHADERLAB_TARGET_2_0,
      UINT64_C(0x8000000000000001), 0));
  CHECK(strcmp(pragmas.buf, "unchanged") == 0);
  sb_free(&pragmas);
  return 0;
}

static int test_target_authority_is_strict_and_typed(void) {
  size_t fixture_size = 0;
  uint8_t *fixture = read_fixture(&fixture_size);
  CHECK(fixture != NULL);
  size_t sm4_size = 0;
  const uint8_t *sm4 = find_first_dxbc(fixture, fixture_size, &sm4_size);
  CHECK(sm4 != NULL);
  uint8_t *sm5 = clone_dxbc_with_shader_model(sm4, sm4_size, 5u, 0u);
  CHECK(sm5 != NULL);

  size_t sm4_blob_size = 0;
  size_t sm5_blob_size = 0;
  uint8_t *sm4_blob =
      test_shaderlab_variant_blob(sm4, sm4_size, 15, NULL, &sm4_blob_size);
  uint8_t *sm5_blob =
      test_shaderlab_variant_blob(sm5, sm4_size, 16, NULL, &sm5_blob_size);
  CHECK(sm4_blob != NULL && sm5_blob != NULL);
  CHECK(sm4_blob_size <= INT32_MAX && sm5_blob_size <= INT32_MAX);
  uint8_t *segments[] = {sm4_blob, sm5_blob};
  int segment_lengths[] = {(int)sm4_blob_size, (int)sm5_blob_size};
  BlobEntry entries[] = {
      {0, (int32_t)sm4_blob_size, 0},
      {0, (int32_t)sm5_blob_size, 1},
  };

  SerializedPass pass;
  SerializedSubProgram subprograms[2];
  SerializedSubProgramIdentity identities[2];
  int platform;
  initialize_pass(&pass, subprograms, identities, 1, &platform);
  subprograms[0].shader_requirements = UINT64_C(0xe3);
  ShaderLabPassTarget target;
  ShaderLabTargetDiagnostic diagnostic;
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_OK);
  CHECK(diagnostic.status == SHADERLAB_TARGET_OK);
  CHECK(target.version == SHADERLAB_TARGET_3_0);
  CHECK(target.common_requirements == UINT64_C(0xe3));
  CHECK(target.union_requirements == UINT64_C(0xe3));
  CHECK(target.d3d_variant_count == 1u);

  /* Backend model is a capability ceiling, not a duplicate target field:
   * Unity legitimately emits SM5 DXBC for variants with lower requirements. */
  subprograms[0].program_type = 16;
  subprograms[0].blob_index = 1;
  subprograms[0].shader_requirements = UINT64_C(0xe3);
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_OK);
  CHECK(target.version == SHADERLAB_TARGET_3_0);
  CHECK(target.d3d_variant_count == 1u);

  /* A single high-end feature elevates Unity's approximate runtime target,
   * but source inversion must not add target-4.5's compute/random-write bits.
   * The exact spelling is target 3.5 plus `require msaatex`. */
  subprograms[0].shader_requirements = UINT64_C(0x100feb);
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_OK);
  CHECK(target.version == SHADERLAB_TARGET_3_5);
  CHECK(target.common_requirements == UINT64_C(0x100feb));
  StringBuilder exact_pragmas;
  sb_init(&exact_pragmas);
  CHECK(shaderlab_requirements_emit_pragmas(
      &exact_pragmas, target.version, target.common_requirements, 0));
  CHECK(strcmp(exact_pragmas.buf, "#pragma require msaatex\n") == 0);
  sb_free(&exact_pragmas);

  subprograms[0].program_type = 15;
  subprograms[0].blob_index = 0;
  subprograms[0].shader_requirements = UINT64_C(0x10cfeb);
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_REQUIREMENT_MODEL_CONFLICT);
  CHECK(diagnostic.stage_index == 0 && diagnostic.subprogram_index == 0);
  CHECK(diagnostic.shader_model_major == 4u);

  subprograms[0].shader_requirements = UINT64_C(0xe3);
  pass.program_mask = 0u;
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_STAGE_TUPLE_CONFLICT);
  CHECK(diagnostic.stage_tuple_status ==
        SHADER_STAGE_TUPLE_PROGRAM_MASK_MISMATCH);
  pass.program_mask = UINT32_C(1) <<
                      (UNITY_SERIALIZED_STAGE_VERTEX + 1u);

  subprograms[0].program_type = 16;
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_VARIANT_METADATA_MISMATCH);
  subprograms[0].program_type = 15;

  const int saved_sm4_length = segment_lengths[0];
  --segment_lengths[0];
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_INVALID_BLOB);
  segment_lengths[0] = saved_sm4_length;

  pass.has_serialized_platforms = false;
  memset(&target, 0xa5, sizeof(target));
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_MISSING_PLATFORM_AUTHORITY);
  CHECK(target.d3d_variant_count == 0u);
  pass.has_serialized_platforms = true;

  int duplicate_platforms[] = {4, 4};
  pass.platform_count = 2;
  pass.platforms = duplicate_platforms;
  memset(&target, 0xa5, sizeof(target));
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_PLATFORM_CONFLICT);
  CHECK(target.d3d_variant_count == 0u);
  pass.platform_count = 1;
  pass.platforms = &platform;

  subprograms[0].program_type = 6;
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_NO_D3D_VARIANTS);
  CHECK(shaderlab_pass_target_resolve(
            &pass, NULL, 0, NULL, NULL, 0, &target,
            &diagnostic) == SHADERLAB_TARGET_NO_D3D_VARIANTS);
  subprograms[0].program_type = 15;

  CHECK(shaderlab_pass_target_resolve(
            &pass, NULL, 0, NULL, NULL, 0, &target,
            &diagnostic) == SHADERLAB_TARGET_INVALID_BLOB);

  initialize_pass(&pass, subprograms, identities, 2, &platform);
  subprograms[0].shader_requirements = UINT64_C(0xe3);
  subprograms[1].program_type = 16;
  subprograms[1].shader_requirements = UINT64_C(0x40e3);
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_OK);
  CHECK(target.version == SHADERLAB_TARGET_3_0);
  CHECK(target.common_requirements == UINT64_C(0xe3));
  CHECK(target.union_requirements == UINT64_C(0x40e3));

  /* Unity can infer higher requirements from only one variant's emitted body.
   * Promoting the union to a global pragma changes every other variant. */
  subprograms[0].shader_requirements = UINT64_C(0x1);
  subprograms[1].shader_requirements = UINT64_C(0xfeb);
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) == SHADERLAB_TARGET_OK);
  CHECK(target.version == SHADERLAB_TARGET_2_0);
  CHECK(target.common_requirements == UINT64_C(0x1));
  CHECK(target.union_requirements == UINT64_C(0xfeb));
  CHECK(target.d3d_variant_count == 2u);

  initialize_pass(&pass, subprograms, identities, 1, &platform);
  subprograms[0].shader_requirements = UINT64_C(0x101);
  CHECK(shaderlab_pass_target_resolve(
            &pass, entries, 2, segments, segment_lengths, 2, &target,
            &diagnostic) ==
        SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS);
  CHECK(diagnostic.status ==
        SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS);
  CHECK(diagnostic.shader_requirements == UINT64_C(0x101));
  CHECK(diagnostic.stage_index == -1);
  CHECK(diagnostic.subprogram_index == -1);

  SerializedSubShader target_subshader;
  SerializedShader target_shader;
  memset(&target_subshader, 0, sizeof(target_subshader));
  memset(&target_shader, 0, sizeof(target_shader));
  target_subshader.pass_count = 1;
  target_subshader.passes = &pass;
  target_shader.name = "Target/Unrepresentable";
  target_shader.subshader_count = 1;
  target_shader.subshaders = &target_subshader;
  StringBuilder candidate;
  ShaderLabCandidateDiagnostic candidate_diagnostic;
  sb_init(&candidate);
  sb_append(&candidate, "unchanged");
  CHECK(!shaderlab_emit_candidate_with_diagnostic(
      &target_shader, entries, 2, segments, segment_lengths, 2,
      &candidate, &candidate_diagnostic));
  CHECK(strcmp(candidate.buf, "unchanged") == 0);
  CHECK(candidate_diagnostic.status ==
        SHADERLAB_CANDIDATE_PASS_TARGET_FAILED);
  CHECK(candidate_diagnostic.target.status ==
        SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS);
  CHECK(candidate_diagnostic.target.shader_requirements == UINT64_C(0x101));
  sb_free(&candidate);

  uint8_t *bad_dxbc = (uint8_t *)malloc(sm4_size);
  CHECK(bad_dxbc != NULL);
  memcpy(bad_dxbc, sm4, sm4_size);
  bad_dxbc[4] ^= 1u;
  size_t bad_blob_size = 0;
  uint8_t *bad_blob =
      test_shaderlab_variant_blob(bad_dxbc, sm4_size, 15, NULL, &bad_blob_size);
  CHECK(bad_blob != NULL && bad_blob_size <= INT32_MAX);
  uint8_t *bad_segments[] = {bad_blob};
  int bad_segment_lengths[] = {(int)bad_blob_size};
  BlobEntry bad_entries[] = {{0, (int32_t)bad_blob_size, 0}};
  initialize_pass(&pass, subprograms, identities, 1, &platform);
  subprograms[0].shader_requirements = UINT64_C(0xe3);
  CHECK(shaderlab_pass_target_resolve(
            &pass, bad_entries, 1, bad_segments, bad_segment_lengths, 1,
            &target, &diagnostic) ==
        SHADERLAB_TARGET_DXBC_DOCUMENT_FAILED);
  CHECK(diagnostic.document_status == DXBC_DOCUMENT_HASH_MISMATCH);

  uint8_t *prefixed_dxbc = (uint8_t *)malloc(sm4_size + 1u);
  CHECK(prefixed_dxbc != NULL);
  prefixed_dxbc[0] = 0;
  memcpy(prefixed_dxbc + 1u, sm4, sm4_size);
  size_t prefixed_blob_size = 0;
  uint8_t *prefixed_blob = test_shaderlab_variant_blob(
      prefixed_dxbc, sm4_size + 1u, 15, NULL, &prefixed_blob_size);
  CHECK(prefixed_blob != NULL && prefixed_blob_size <= INT32_MAX);
  uint8_t *prefixed_segments[] = {prefixed_blob};
  int prefixed_segment_lengths[] = {(int)prefixed_blob_size};
  BlobEntry prefixed_entries[] = {
      {0, (int32_t)prefixed_blob_size, 0},
  };
  CHECK(shaderlab_pass_target_resolve(
            &pass, prefixed_entries, 1, prefixed_segments,
            prefixed_segment_lengths, 1, &target, &diagnostic) ==
        SHADERLAB_TARGET_DXBC_DOCUMENT_FAILED);
  CHECK(diagnostic.document_status == DXBC_DOCUMENT_NOT_DXBC);

  free(prefixed_blob);
  free(prefixed_dxbc);
  free(bad_blob);
  free(bad_dxbc);
  free(sm5_blob);
  free(sm4_blob);
  free(sm5);
  free(fixture);
  return 0;
}

int main(void) {
  CHECK(test_dynamic_keyword_universe() == 0);
  CHECK(test_duplicate_and_ambiguous_predicates() == 0);
  CHECK(test_hardware_tier_predicate_authority() == 0);
  CHECK(test_incomplete_predicate_coverage_is_transactional() == 0);
  CHECK(test_authority_failures() == 0);
  CHECK(test_failed_variant_propagates_transactionally() == 0);
  CHECK(test_non_d3d_subshader_is_omitted_from_candidate() == 0);
  CHECK(test_canonical_exact_predicates() == 0);
  CHECK(test_sparse_shaped_planned_stage_aliases() == 0);
  CHECK(test_two_stage_symbolic_selectors_are_exhaustive() == 0);
  CHECK(test_high_level_shaderlab_candidate(false) == 0);
  CHECK(test_high_level_shaderlab_candidate(true) == 0);
  CHECK(test_requirements_target_projection() == 0);
  CHECK(test_target_authority_is_strict_and_typed() == 0);
  CHECK(strcmp(shaderlab_stage_status_name(SHADERLAB_STAGE_OUTPUT_FAILED),
               "output-failed") == 0);
  CHECK(strcmp(
            shaderlab_stage_status_name(SHADERLAB_STAGE_INCOMPLETE_PREDICATE),
            "incomplete-predicate") == 0);
  CHECK(g_allocations_count == 0u);
  CHECK(g_allocated_bytes == 0u);
  printf("ShaderLab stage unit tests passed.\n");
  return 0;
}
