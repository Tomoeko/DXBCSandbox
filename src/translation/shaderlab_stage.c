// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_emitter.h"

#include "common/shader_stage.h"
#include "dxbc/dxbc_document.h"
#include "dxbc/dxbc_parser.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/shaderlab_emitter_internal.h"
#include "translation/usil.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int subprogram_index;
  int hardware_tier_group;
  size_t enabled_keyword_count;
  size_t *enabled_keyword_indices;
} ShaderLabStageVariantPlan;

typedef struct {
  size_t keyword_count;
  size_t keyword_capacity;
  char **keywords;
  size_t variant_count;
  size_t variant_capacity;
  ShaderLabStageVariantPlan *variants;
} ShaderLabStagePlan;

static void set_diagnostic(ShaderLabStageDiagnostic *diagnostic,
                           ShaderLabStageStatus status, int stage_index,
                           int subprogram_index,
                           int conflicting_subprogram_index) {
  if (!diagnostic) return;
  diagnostic->status = status;
  diagnostic->stage_index = stage_index;
  diagnostic->subprogram_index = subprogram_index;
  diagnostic->conflicting_subprogram_index = conflicting_subprogram_index;
  diagnostic->stage_contract_status = DXBC_STAGE_CONTRACT_OK;
  diagnostic->stage_tuple_status = SHADER_STAGE_TUPLE_OK;
  diagnostic->variant_plan_status = SHADERLAB_VARIANT_PLAN_OK;
  diagnostic->raw_keyword_index = -1;
  hlsl_emit_diagnostic_init(&diagnostic->hlsl);
}

const char *shaderlab_stage_status_name(ShaderLabStageStatus status) {
  switch (status) {
  case SHADERLAB_STAGE_OK:
    return "ok";
  case SHADERLAB_STAGE_INVALID_ARGUMENT:
    return "invalid-argument";
  case SHADERLAB_STAGE_MISSING_PLATFORM_AUTHORITY:
    return "missing-platform-authority";
  case SHADERLAB_STAGE_NO_D3D_VARIANTS:
    return "no-d3d-variants";
  case SHADERLAB_STAGE_INVALID_KEYWORD:
    return "invalid-keyword";
  case SHADERLAB_STAGE_DUPLICATE_KEYWORD:
    return "duplicate-keyword";
  case SHADERLAB_STAGE_AMBIGUOUS_PREDICATE:
    return "ambiguous-predicate";
  case SHADERLAB_STAGE_INCOMPLETE_PREDICATE:
    return "incomplete-predicate";
  case SHADERLAB_STAGE_VARIANT_PLAN_FAILED:
    return "variant-plan-failed";
  case SHADERLAB_STAGE_ALLOCATION_FAILED:
    return "allocation-failed";
  case SHADERLAB_STAGE_INVALID_BLOB:
    return "invalid-blob";
  case SHADERLAB_STAGE_INVALID_PARAMETER_BLOB:
    return "invalid-parameter-blob";
  case SHADERLAB_STAGE_VARIANT_PARSE_FAILED:
    return "variant-parse-failed";
  case SHADERLAB_STAGE_VARIANT_METADATA_MISMATCH:
    return "variant-metadata-mismatch";
  case SHADERLAB_STAGE_DXBC_DOCUMENT_FAILED:
    return "dxbc-document-failed";
  case SHADERLAB_STAGE_STAGE_CONTRACT_FAILED:
    return "stage-contract-failed";
  case SHADERLAB_STAGE_SEMANTIC_DECODE_FAILED:
    return "semantic-decode-failed";
  case SHADERLAB_STAGE_USIL_TRANSLATION_FAILED:
    return "usil-translation-failed";
  case SHADERLAB_STAGE_HLSL_EMISSION_FAILED:
    return "hlsl-emission-failed";
  case SHADERLAB_STAGE_OUTPUT_FAILED:
    return "output-failed";
  }
  return "unknown";
}

static bool grow_array(void **array, size_t *capacity, size_t minimum_count,
                       size_t item_size) {
  if (!array || !capacity || item_size == 0 ||
      minimum_count > SIZE_MAX / item_size) {
    return false;
  }
  if (*capacity >= minimum_count) return true;

  size_t new_capacity = *capacity == 0 ? 8u : *capacity;
  while (new_capacity < minimum_count) {
    if (new_capacity > SIZE_MAX / 2u) {
      new_capacity = minimum_count;
      break;
    }
    new_capacity *= 2u;
  }
  if (new_capacity > SIZE_MAX / item_size ||
      *capacity > SIZE_MAX / item_size) {
    return false;
  }

  const size_t old_size = *capacity * item_size;
  const size_t new_size = new_capacity * item_size;
  void *resized = mem_realloc(*array, old_size, new_size);
  if (!resized) return false;
  if (new_size > old_size) {
    memset((uint8_t *)resized + old_size, 0, new_size - old_size);
  }
  *array = resized;
  *capacity = new_capacity;
  return true;
}

static void shaderlab_stage_plan_free(ShaderLabStagePlan *plan) {
  if (!plan) return;
  if (plan->keywords) {
    for (size_t i = 0; i < plan->keyword_count; ++i) {
      if (plan->keywords[i]) {
        const size_t size = strlen(plan->keywords[i]) + 1u;
        mem_free(plan->keywords[i], size);
      }
    }
    mem_free(plan->keywords,
             plan->keyword_capacity * sizeof(*plan->keywords));
  }
  if (plan->variants) {
    for (size_t i = 0; i < plan->variant_count; ++i) {
      ShaderLabStageVariantPlan *variant = &plan->variants[i];
      if (variant->enabled_keyword_indices) {
        mem_free(variant->enabled_keyword_indices,
                 variant->enabled_keyword_count *
                     sizeof(*variant->enabled_keyword_indices));
      }
    }
    mem_free(plan->variants,
             plan->variant_capacity * sizeof(*plan->variants));
  }
  memset(plan, 0, sizeof(*plan));
}

static bool is_generated_control_keyword(const char *keyword) {
  static const char *const generated[] = {
      "VERTEX",          "FRAGMENT",          "GEOMETRY",
      "HULL",            "DOMAIN",            "SHADER_STAGE_VERTEX",
      "SHADER_STAGE_FRAGMENT",                 "SHADER_STAGE_GEOMETRY",
      "SHADER_STAGE_HULL",                     "SHADER_STAGE_DOMAIN",
      "UNITY_HARDWARE_TIER1",                  "UNITY_HARDWARE_TIER2",
      "UNITY_HARDWARE_TIER3"};
  for (size_t i = 0; i < sizeof(generated) / sizeof(generated[0]); ++i) {
    if (strcmp(keyword, generated[i]) == 0) return true;
  }
  return false;
}

static bool keyword_is_exact_preprocessor_identifier(const char *keyword) {
  if (!keyword || keyword[0] == '\0' || strcmp(keyword, "defined") == 0 ||
      is_generated_control_keyword(keyword)) {
    return false;
  }
  const unsigned char first = (unsigned char)keyword[0];
  if (!((first >= 'A' && first <= 'Z') ||
        (first >= 'a' && first <= 'z') || first == '_')) {
    return false;
  }
  for (size_t i = 1; keyword[i] != '\0'; ++i) {
    const unsigned char value = (unsigned char)keyword[i];
    if (!((value >= 'A' && value <= 'Z') ||
          (value >= 'a' && value <= 'z') ||
          (value >= '0' && value <= '9') || value == '_')) {
      return false;
    }
  }
  return true;
}

static const char *subprogram_keyword_at(const SerializedSubProgram *sub,
                                         size_t index) {
  const size_t local_count = (size_t)sub->local_keyword_count;
  if (index < local_count) return sub->local_keywords[index];
  return sub->global_keywords[index - local_count];
}

static bool validate_subprogram_keywords(const SerializedSubProgram *sub) {
  if (!sub || sub->local_keyword_count < 0 || sub->global_keyword_count < 0 ||
      (sub->local_keyword_count > 0 && !sub->local_keywords) ||
      (sub->global_keyword_count > 0 && !sub->global_keywords) ||
      (size_t)sub->local_keyword_count >
          SIZE_MAX - (size_t)sub->global_keyword_count) {
    return false;
  }
  const size_t count = (size_t)sub->local_keyword_count +
                       (size_t)sub->global_keyword_count;
  for (size_t i = 0; i < count; ++i) {
    const char *keyword = subprogram_keyword_at(sub, i);
    if (!keyword_is_exact_preprocessor_identifier(keyword)) return false;
    for (size_t previous = 0; previous < i; ++previous) {
      if (strcmp(keyword, subprogram_keyword_at(sub, previous)) == 0) {
        return false;
      }
    }
  }
  return true;
}

static bool stage_plan_add_keyword(ShaderLabStagePlan *plan,
                                   const char *keyword) {
  for (size_t i = 0; i < plan->keyword_count; ++i) {
    if (strcmp(plan->keywords[i], keyword) == 0) return true;
  }
  if (!grow_array((void **)&plan->keywords, &plan->keyword_capacity,
                  plan->keyword_count + 1u, sizeof(*plan->keywords))) {
    return false;
  }
  const size_t length = strlen(keyword);
  char *copy = (char *)mem_alloc(length + 1u);
  if (!copy) return false;
  memcpy(copy, keyword, length + 1u);
  plan->keywords[plan->keyword_count++] = copy;
  return true;
}

static int compare_keyword_pointers(const void *left, const void *right) {
  const char *const *left_keyword = (const char *const *)left;
  const char *const *right_keyword = (const char *const *)right;
  return strcmp(*left_keyword, *right_keyword);
}

static size_t find_sorted_keyword(const ShaderLabStagePlan *plan,
                                  const char *keyword) {
  size_t low = 0;
  size_t high = plan->keyword_count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2u;
    const int comparison = strcmp(keyword, plan->keywords[middle]);
    if (comparison == 0) return middle;
    if (comparison < 0)
      high = middle;
    else
      low = middle + 1u;
  }
  return SIZE_MAX;
}

static int compare_size_values(const void *left, const void *right) {
  const size_t left_value = *(const size_t *)left;
  const size_t right_value = *(const size_t *)right;
  return left_value < right_value ? -1 : left_value > right_value ? 1 : 0;
}

static int compare_variant_plans(const void *left, const void *right) {
  const ShaderLabStageVariantPlan *left_variant =
      (const ShaderLabStageVariantPlan *)left;
  const ShaderLabStageVariantPlan *right_variant =
      (const ShaderLabStageVariantPlan *)right;
  const size_t common_count =
      left_variant->enabled_keyword_count < right_variant->enabled_keyword_count
          ? left_variant->enabled_keyword_count
          : right_variant->enabled_keyword_count;
  for (size_t i = 0; i < common_count; ++i) {
    if (left_variant->enabled_keyword_indices[i] <
        right_variant->enabled_keyword_indices[i])
      return -1;
    if (left_variant->enabled_keyword_indices[i] >
        right_variant->enabled_keyword_indices[i])
      return 1;
  }
  if (left_variant->enabled_keyword_count <
      right_variant->enabled_keyword_count)
    return -1;
  if (left_variant->enabled_keyword_count >
      right_variant->enabled_keyword_count)
    return 1;
  if (left_variant->hardware_tier_group <
      right_variant->hardware_tier_group)
    return -1;
  if (left_variant->hardware_tier_group >
      right_variant->hardware_tier_group)
    return 1;
  return left_variant->subprogram_index < right_variant->subprogram_index
             ? -1
         : left_variant->subprogram_index > right_variant->subprogram_index
             ? 1
             : 0;
}

static bool enabled_sets_equal(const ShaderLabStageVariantPlan *left,
                               const ShaderLabStageVariantPlan *right) {
  return left->enabled_keyword_count == right->enabled_keyword_count &&
         (left->enabled_keyword_count == 0 ||
          memcmp(left->enabled_keyword_indices, right->enabled_keyword_indices,
                 left->enabled_keyword_count *
                     sizeof(*left->enabled_keyword_indices)) == 0);
}

/* Every emitted keyword predicate fixes every keyword in the observed
 * universe, so distinct enabled sets denote distinct Boolean assignments.
 * The sorted plan covers that domain exactly iff it contains all 2^K
 * assignments.  A generic tier entry covers Unity's complete tier domain;
 * otherwise each assignment must have one entry for each of tiers 1..3.
 * stage_plan_build() already rejects duplicate and generic/specific overlap,
 * making this a linear, non-heuristic completeness proof. */
static bool stage_plan_has_complete_predicate_coverage(
    const ShaderLabStagePlan *plan, int *out_incomplete_subprogram_index) {
  if (out_incomplete_subprogram_index)
    *out_incomplete_subprogram_index = -1;
  if (!plan || plan->variant_count == 0) return false;

  size_t keyword_assignment_count = 0;
  for (size_t first = 0; first < plan->variant_count;) {
    size_t end = first + 1u;
    while (end < plan->variant_count &&
           enabled_sets_equal(&plan->variants[first], &plan->variants[end])) {
      ++end;
    }

    bool has_generic_tier = false;
    unsigned int specific_tier_mask = 0u;
    for (size_t index = first; index < end; ++index) {
      const int tier = plan->variants[index].hardware_tier_group;
      if (tier == 3)
        has_generic_tier = true;
      else
        specific_tier_mask |= 1u << (unsigned int)tier;
    }
    if (!has_generic_tier && specific_tier_mask != 0x7u) {
      if (out_incomplete_subprogram_index) {
        *out_incomplete_subprogram_index =
            plan->variants[first].subprogram_index;
      }
      return false;
    }

    ++keyword_assignment_count;
    first = end;
  }

  if (plan->keyword_count >= sizeof(size_t) * CHAR_BIT) return false;
  const size_t required_keyword_assignment_count =
      ((size_t)1u) << plan->keyword_count;
  return keyword_assignment_count == required_keyword_assignment_count;
}

static bool keyword_failure_is_duplicate(const SerializedSubProgram *sub) {
  if (!sub || sub->local_keyword_count < 0 || sub->global_keyword_count < 0 ||
      (sub->local_keyword_count > 0 && !sub->local_keywords) ||
      (sub->global_keyword_count > 0 && !sub->global_keywords)) {
    return false;
  }
  const size_t count = (size_t)sub->local_keyword_count +
                       (size_t)sub->global_keyword_count;
  for (size_t i = 0; i < count; ++i) {
    const char *keyword = subprogram_keyword_at(sub, i);
    if (!keyword_is_exact_preprocessor_identifier(keyword)) return false;
    for (size_t previous = 0; previous < i; ++previous) {
      if (strcmp(keyword, subprogram_keyword_at(sub, previous)) == 0)
        return true;
    }
  }
  return false;
}

static bool stage_plan_build(const SerializedPass *pass, int stage_index,
                             ShaderLabStagePlan *plan,
                             ShaderLabStageDiagnostic *diagnostic) {
  if (plan) memset(plan, 0, sizeof(*plan));
  set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_ARGUMENT, stage_index, -1,
                 -1);
  if (!pass || !plan || stage_index < 0 || stage_index >= 6 ||
      pass->subprogram_count[stage_index] < 0 ||
      (pass->subprogram_count[stage_index] > 0 &&
       (!pass->subprograms[stage_index] ||
        !pass->subprogram_identities[stage_index]))) {
    return false;
  }
  if (!pass->has_serialized_platforms || pass->platform_count <= 0 ||
      !pass->platforms) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_MISSING_PLATFORM_AUTHORITY,
                   stage_index, -1, -1);
    return false;
  }

  const int subprogram_count = pass->subprogram_count[stage_index];
  for (int index = 0; index < subprogram_count; ++index) {
    if (!serialized_pass_subprogram_is_platform(pass, stage_index, index, 4))
      continue;
    const SerializedSubProgram *sub = &pass->subprograms[stage_index][index];
    const SerializedSubProgramIdentity *identity =
        &pass->subprogram_identities[stage_index][index];
    if (identity->hardware_tier_group < 0 ||
        identity->hardware_tier_group > 3) {
      set_diagnostic(diagnostic, SHADERLAB_STAGE_MISSING_PLATFORM_AUTHORITY,
                     stage_index, index, -1);
      goto fail;
    }
    if (!validate_subprogram_keywords(sub)) {
      set_diagnostic(diagnostic,
                     keyword_failure_is_duplicate(sub)
                         ? SHADERLAB_STAGE_DUPLICATE_KEYWORD
                         : SHADERLAB_STAGE_INVALID_KEYWORD,
                     stage_index, index, -1);
      goto fail;
    }
    const size_t keyword_count = (size_t)sub->local_keyword_count +
                                 (size_t)sub->global_keyword_count;
    for (size_t keyword_index = 0; keyword_index < keyword_count;
         ++keyword_index) {
      if (!stage_plan_add_keyword(
              plan, subprogram_keyword_at(sub, keyword_index))) {
        set_diagnostic(diagnostic, SHADERLAB_STAGE_ALLOCATION_FAILED,
                       stage_index, index, -1);
        goto fail;
      }
    }
    if (!grow_array((void **)&plan->variants, &plan->variant_capacity,
                    plan->variant_count + 1u, sizeof(*plan->variants))) {
      set_diagnostic(diagnostic, SHADERLAB_STAGE_ALLOCATION_FAILED,
                     stage_index, index, -1);
      goto fail;
    }
    ShaderLabStageVariantPlan *variant =
        &plan->variants[plan->variant_count++];
    variant->subprogram_index = index;
    variant->hardware_tier_group = identity->hardware_tier_group;
    variant->enabled_keyword_count = keyword_count;
  }
  if (plan->variant_count == 0) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_NO_D3D_VARIANTS, stage_index,
                   -1, -1);
    goto fail;
  }

  if (plan->keyword_count > 1) {
    qsort(plan->keywords, plan->keyword_count, sizeof(*plan->keywords),
          compare_keyword_pointers);
  }
  for (size_t variant_index = 0; variant_index < plan->variant_count;
       ++variant_index) {
    ShaderLabStageVariantPlan *variant = &plan->variants[variant_index];
    const SerializedSubProgram *sub =
        &pass->subprograms[stage_index][variant->subprogram_index];
    if (variant->enabled_keyword_count > 0) {
      if (variant->enabled_keyword_count >
          SIZE_MAX / sizeof(*variant->enabled_keyword_indices)) {
        set_diagnostic(diagnostic, SHADERLAB_STAGE_ALLOCATION_FAILED,
                       stage_index, variant->subprogram_index, -1);
        goto fail;
      }
      variant->enabled_keyword_indices = (size_t *)mem_alloc(
          variant->enabled_keyword_count *
          sizeof(*variant->enabled_keyword_indices));
      if (!variant->enabled_keyword_indices) {
        set_diagnostic(diagnostic, SHADERLAB_STAGE_ALLOCATION_FAILED,
                       stage_index, variant->subprogram_index, -1);
        goto fail;
      }
      for (size_t keyword_index = 0;
           keyword_index < variant->enabled_keyword_count; ++keyword_index) {
        const size_t universe_index = find_sorted_keyword(
            plan, subprogram_keyword_at(sub, keyword_index));
        if (universe_index == SIZE_MAX) {
          set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_ARGUMENT,
                         stage_index, variant->subprogram_index, -1);
          goto fail;
        }
        variant->enabled_keyword_indices[keyword_index] = universe_index;
      }
      if (variant->enabled_keyword_count > 1) {
        qsort(variant->enabled_keyword_indices,
              variant->enabled_keyword_count,
              sizeof(*variant->enabled_keyword_indices), compare_size_values);
      }
    }
  }
  if (plan->variant_count > 1) {
    qsort(plan->variants, plan->variant_count, sizeof(*plan->variants),
          compare_variant_plans);
  }
  for (size_t i = 1; i < plan->variant_count; ++i) {
    const ShaderLabStageVariantPlan *previous = &plan->variants[i - 1u];
    const ShaderLabStageVariantPlan *current = &plan->variants[i];
    if (!enabled_sets_equal(previous, current)) continue;
    if (previous->hardware_tier_group == current->hardware_tier_group ||
        previous->hardware_tier_group == 3 ||
        current->hardware_tier_group == 3) {
      set_diagnostic(diagnostic, SHADERLAB_STAGE_AMBIGUOUS_PREDICATE,
                     stage_index, current->subprogram_index,
                     previous->subprogram_index);
      goto fail;
    }
  }

  int incomplete_subprogram_index = -1;
  if (!stage_plan_has_complete_predicate_coverage(
          plan, &incomplete_subprogram_index)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_INCOMPLETE_PREDICATE,
                   stage_index, incomplete_subprogram_index, -1);
    goto fail;
  }

  set_diagnostic(diagnostic, SHADERLAB_STAGE_OK, stage_index, -1, -1);
  return true;

fail:
  shaderlab_stage_plan_free(plan);
  return false;
}

bool shaderlab_stage_validate_variants(
    const SerializedPass *pass, int stage_index, size_t *out_keyword_count,
    size_t *out_variant_count, ShaderLabStageDiagnostic *diagnostic) {
  if (out_keyword_count) *out_keyword_count = 0;
  if (out_variant_count) *out_variant_count = 0;
  ShaderLabStagePlan plan;
  if (!stage_plan_build(pass, stage_index, &plan, diagnostic)) return false;
  if (out_keyword_count) *out_keyword_count = plan.keyword_count;
  if (out_variant_count) *out_variant_count = plan.variant_count;
  shaderlab_stage_plan_free(&plan);
  return true;
}

static bool blob_entry_is_available(const BlobEntry *entries, int entry_count,
                                    uint8_t **segments,
                                    const int *segment_lengths,
                                    int segment_count, int index) {
  if (!entries || !segments || !segment_lengths || entry_count <= 0 ||
      segment_count <= 0 || index < 0 || index >= entry_count) {
    return false;
  }
  const BlobEntry *entry = &entries[index];
  if (entry->offset < 0 || entry->length <= 0 || entry->segment < 0 ||
      entry->segment >= segment_count || !segments[entry->segment] ||
      segment_lengths[entry->segment] < 0) {
    return false;
  }
  const size_t segment_length = (size_t)segment_lengths[entry->segment];
  const size_t offset = (size_t)entry->offset;
  const size_t length = (size_t)entry->length;
  return offset <= segment_length && length <= segment_length - offset;
}

static bool translate_stage_to_hlsl(
    const SerializedPass *pass, int stage_index, int subprogram_index,
    const BlobEntry *blob_entries, int entry_count, uint8_t **segments,
    const int *segment_lengths, int segment_count, StringBuilder *out_hlsl,
    const HLSLEmitNames *names,
    const char *const *reserved_preprocessor_identifiers,
    size_t reserved_preprocessor_identifier_count, bool high_level,
    ShaderLabStageDiagnostic *diagnostic) {
  if (!pass || !out_hlsl || !names || stage_index < 0 || stage_index >= 6 ||
      subprogram_index < 0 ||
      subprogram_index >= pass->subprogram_count[stage_index] ||
      !serialized_pass_subprogram_is_platform(pass, stage_index,
                                              subprogram_index, 4)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_ARGUMENT, stage_index,
                   subprogram_index, -1);
    return false;
  }

  const SerializedSubProgram *sub =
      &pass->subprograms[stage_index][subprogram_index];
  const int32_t blob_index = sub->blob_index;
  if (!blob_entry_is_available(blob_entries, entry_count, segments,
                               segment_lengths, segment_count, blob_index)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_BLOB, stage_index,
                   subprogram_index, -1);
    return false;
  }

  const BlobEntry entry = blob_entries[blob_index];
  const uint8_t *payload = segments[entry.segment] + (size_t)entry.offset;
  const size_t payload_length = (size_t)entry.length;

  SerializedProgramParameters parameters;
  serialized_program_parameters_init(&parameters);
  const SerializedProgramParameters *selected_parameters =
      &pass->common_parameters[stage_index];
  const int parameter_blob_index =
      pass->subprogram_param_blob_indices[stage_index]
          ? pass->subprogram_param_blob_indices[stage_index][subprogram_index]
          : -1;
  if (parameter_blob_index < -1) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_PARAMETER_BLOB,
                   stage_index, subprogram_index, -1);
    return false;
  }
  if (parameter_blob_index >= 0) {
    if (!blob_entry_is_available(blob_entries, entry_count, segments,
                                 segment_lengths, segment_count,
                                 parameter_blob_index)) {
      set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_PARAMETER_BLOB,
                     stage_index, subprogram_index, -1);
      return false;
    }
    const BlobEntry parameter_entry = blob_entries[parameter_blob_index];
    ByteStream parameter_stream;
    stream_init(&parameter_stream,
                segments[parameter_entry.segment] +
                    (size_t)parameter_entry.offset,
                (size_t)parameter_entry.length);
    stream_set_endian(&parameter_stream, false);
    if (!subprogram_metadata_parse_parameters(&parameter_stream, &parameters)) {
      serialized_program_parameters_free(&parameters);
      set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_PARAMETER_BLOB,
                     stage_index, subprogram_index, -1);
      return false;
    }
    selected_parameters = &parameters;
  }

  ByteStream subprogram_stream;
  stream_init(&subprogram_stream, payload, payload_length);
  stream_set_endian(&subprogram_stream, false);
  PlayerSubProgramMetadata binary_subprogram;
  if (!subprogram_metadata_parse_variant(&subprogram_stream,
                                         &binary_subprogram)) {
    serialized_program_parameters_free(&parameters);
    set_diagnostic(diagnostic, SHADERLAB_STAGE_VARIANT_PARSE_FAILED,
                   stage_index, subprogram_index, -1);
    return false;
  }
  if (binary_subprogram.program_type != sub->program_type) {
    subprogram_metadata_free_variant(&binary_subprogram);
    serialized_program_parameters_free(&parameters);
    set_diagnostic(diagnostic, SHADERLAB_STAGE_VARIANT_METADATA_MISMATCH,
                   stage_index, subprogram_index, -1);
    return false;
  }
  if (!binary_subprogram.bytecode || binary_subprogram.bytecode_length == 0) {
    subprogram_metadata_free_variant(&binary_subprogram);
    serialized_program_parameters_free(&parameters);
    set_diagnostic(diagnostic, SHADERLAB_STAGE_DXBC_DOCUMENT_FAILED,
                   stage_index, subprogram_index, -1);
    return false;
  }

  DXBCContainerView raw_view;
  DXBCDocument document;
  DXBCDocumentDiagnostic document_diagnostic;
  DXBCStageContract contract;
  DXBCStageContractDiagnostic contract_diagnostic;
  DXBCContainer semantic;
  bool semantic_decoded = false;
  bool success = false;
  dxbc_document_init(&document);
  dxbc_stage_contract_init(&contract);
  memset(&document_diagnostic, 0, sizeof(document_diagnostic));
  memset(&contract_diagnostic, 0, sizeof(contract_diagnostic));
  memset(&semantic, 0, sizeof(semantic));

  if (!dxbc_container_view_first(binary_subprogram.bytecode,
                                 binary_subprogram.bytecode_length,
                                 &raw_view) ||
      !dxbc_document_parse(&document, raw_view.data, raw_view.size,
                           &document_diagnostic)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_DXBC_DOCUMENT_FAILED,
                   stage_index, subprogram_index, -1);
    goto cleanup;
  }
  if (!dxbc_document_decode_semantic(&document, &semantic)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_SEMANTIC_DECODE_FAILED,
                   stage_index, subprogram_index, -1);
    goto cleanup;
  }
  semantic_decoded = true;

  if (!dxbc_stage_contract_decode(&document, &semantic, &contract,
                                  &contract_diagnostic)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_STAGE_CONTRACT_FAILED,
                   stage_index, subprogram_index, -1);
    if (diagnostic) {
      diagnostic->stage_contract_status = contract_diagnostic.status;
    }
    goto cleanup;
  }
  UnityCompilerProgramStage compiler_stage;
  ShaderStageTuple tuple;
  memset(&tuple, 0, sizeof(tuple));
  tuple.serialized_stage = (UnitySerializedProgramStage)stage_index;
  if (!shader_stage_serialized_to_compiler(tuple.serialized_stage,
                                           &compiler_stage)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_STAGE_CONTRACT_FAILED,
                   stage_index, subprogram_index, -1);
    if (diagnostic) {
      diagnostic->stage_tuple_status =
          SHADER_STAGE_TUPLE_INVALID_SERIALIZED_STAGE;
    }
    goto cleanup;
  }
  tuple.compiler_program = compiler_stage;
  tuple.serialized_program_mask = pass->program_mask;
  tuple.gpu_program_type =
      (UnityGPUProgramType)binary_subprogram.program_type;
  tuple.dxbc_program_type = contract.program_type;
  tuple.shader_model_major = contract.shader_model_major;
  tuple.shader_model_minor = contract.shader_model_minor;
  ShaderStageTupleStatus tuple_status =
      shader_stage_validate_d3d11_tuple(&tuple);
  if (tuple_status != SHADER_STAGE_TUPLE_OK) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_STAGE_CONTRACT_FAILED,
                   stage_index, subprogram_index, -1);
    if (diagnostic) diagnostic->stage_tuple_status = tuple_status;
    goto cleanup;
  }

  USILProgram usil;
  if (!usil_translate_with_stage_contract(&usil, &semantic, &contract)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_USIL_TRANSLATION_FAILED,
                   stage_index, subprogram_index, -1);
    goto cleanup;
  }
  HLSLEmitOptions emit_options = HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
  if (high_level) emit_options.mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE;
  emit_options.omit_unity_builtin_declarations = true;
  emit_options.reserved_preprocessor_identifiers =
      reserved_preprocessor_identifiers;
  emit_options.reserved_preprocessor_identifier_count =
      reserved_preprocessor_identifier_count;
  HLSLEmitDiagnostic hlsl_diagnostic;
  if (!hlsl_emit_with_options_diagnostic(
          &usil, out_hlsl, selected_parameters,
          &pass->common_parameters[stage_index], names, &emit_options,
          &hlsl_diagnostic)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_HLSL_EMISSION_FAILED,
                   stage_index, subprogram_index, -1);
    if (diagnostic) diagnostic->hlsl = hlsl_diagnostic;
  } else if (!sb_ok(out_hlsl)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_OUTPUT_FAILED, stage_index,
                   subprogram_index, -1);
  } else {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_OK, stage_index,
                   subprogram_index, -1);
    success = true;
  }
  usil_free(&usil);

cleanup:
  if (semantic_decoded) dxbc_free(&semantic);
  dxbc_stage_contract_free(&contract);
  dxbc_document_free(&document);
  subprogram_metadata_free_variant(&binary_subprogram);
  serialized_program_parameters_free(&parameters);
  return success;
}

static void emit_variant_manifest(const SerializedSubProgram *sub,
                                  const ShaderLabStagePlan *plan,
                                  const ShaderLabStageVariantPlan *variant,
                                  int stage_index, StringBuilder *output) {
  append_indent(output, 3);
  sb_append(output, "// DXBCSandbox-Variant stage=");
  static const char *const stage_names[] = {
      "vertex", "fragment", "geometry", "hull", "domain", "ray-tracing"};
  if (stage_index < 0 || stage_index >= 6) {
    output->failed = true;
    return;
  }
  sb_append(output, stage_names[stage_index]);
  sb_appendf(output, " blob=%d requirements=%" PRIu64 " tier=", sub->blob_index,
             sub->shader_requirements);
  if (variant->hardware_tier_group == 3)
    sb_append(output, "generic");
  else
    sb_appendf(output, "%d", variant->hardware_tier_group + 1);
  sb_append(output, " keywords=");
  for (size_t i = 0; i < variant->enabled_keyword_count; ++i) {
    if (i > 0) sb_append_char(output, ',');
    sb_append(output, plan->keywords[variant->enabled_keyword_indices[i]]);
  }
  sb_append_char(output, '\n');
}

static void emit_variant_predicate(const ShaderLabStagePlan *plan,
                                   const ShaderLabStageVariantPlan *variant,
                                   StringBuilder *output) {
  size_t enabled_cursor = 0;
  bool emitted_term = false;
  for (size_t keyword_index = 0; keyword_index < plan->keyword_count;
       ++keyword_index) {
    if (emitted_term) sb_append(output, " && ");
    const bool enabled =
        enabled_cursor < variant->enabled_keyword_count &&
        variant->enabled_keyword_indices[enabled_cursor] == keyword_index;
    if (!enabled) sb_append_char(output, '!');
    sb_append(output, "defined(");
    sb_append(output, plan->keywords[keyword_index]);
    sb_append_char(output, ')');
    if (enabled) ++enabled_cursor;
    emitted_term = true;
  }
  if (variant->hardware_tier_group >= 0 &&
      variant->hardware_tier_group < 3) {
    for (int tier = 0; tier < 3; ++tier) {
      if (emitted_term) sb_append(output, " && ");
      if (tier != variant->hardware_tier_group) sb_append_char(output, '!');
      sb_appendf(output, "defined(UNITY_HARDWARE_TIER%d)", tier + 1);
      emitted_term = true;
    }
  }
}

static void append_indented_source(StringBuilder *output,
                                   const StringBuilder *source) {
  const char *cursor = source->buf;
  while (cursor && *cursor) {
    append_indent(output, 3);
    while (*cursor && *cursor != '\n') sb_append_char(output, *cursor++);
    sb_append_char(output, '\n');
    if (*cursor == '\n') ++cursor;
  }
}

static bool stage_emit_names(int stage_index, HLSLEmitNames *names) {
  if (!names) return false;
  switch (stage_index) {
  case 0:
    *names = (HLSLEmitNames){"vert", "appdata", "v2f"};
    return true;
  case 1:
    *names = (HLSLEmitNames){"frag", "ps_input", "fout"};
    return true;
  case 2:
    *names = (HLSLEmitNames){"geom", "gs_input", "gs_output"};
    return true;
  case 3:
    *names = (HLSLEmitNames){"hs", "unused_input", "unused_output"};
    return true;
  case 4:
    *names = (HLSLEmitNames){"ds", "unused_input", "unused_output"};
    return true;
  default:
    return false;
  }
}

bool emit_stage_hlsl(const SerializedPass *pass, int stage_index,
                     const BlobEntry *blob_entries, int entry_count,
                     uint8_t **segments, const int *segment_lengths,
                     int segment_count,
                     StringBuilder *output,
                     ShaderLabStageDiagnostic *diagnostic) {
  set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_ARGUMENT, stage_index, -1,
                 -1);
  if (!output || stage_index < 0 || stage_index > 4) return false;

  ShaderLabStagePlan plan;
  if (!stage_plan_build(pass, stage_index, &plan, diagnostic)) return false;

  HLSLEmitNames names;
  if (!stage_emit_names(stage_index, &names)) {
    shaderlab_stage_plan_free(&plan);
    return false;
  }

  bool has_conditional_predicates = plan.keyword_count > 0;
  for (size_t i = 0; i < plan.variant_count; ++i) {
    if (plan.variants[i].hardware_tier_group < 3) {
      has_conditional_predicates = true;
      break;
    }
  }

  StringBuilder stage_output;
  sb_init_with_capacity(&stage_output, 4096);
  for (size_t variant_index = 0; variant_index < plan.variant_count;
       ++variant_index) {
    const ShaderLabStageVariantPlan *variant = &plan.variants[variant_index];
    const SerializedSubProgram *sub =
        &pass->subprograms[stage_index][variant->subprogram_index];
    StringBuilder variant_hlsl;
    sb_init_with_capacity(&variant_hlsl, 4096);
    if (!translate_stage_to_hlsl(
            pass, stage_index, variant->subprogram_index, blob_entries,
            entry_count, segments, segment_lengths, segment_count,
            &variant_hlsl, &names,
            (const char *const *)plan.keywords, plan.keyword_count, false,
            diagnostic)) {
      sb_free(&variant_hlsl);
      sb_free(&stage_output);
      shaderlab_stage_plan_free(&plan);
      return false;
    }

    emit_variant_manifest(sub, &plan, variant, stage_index, &stage_output);
    if (has_conditional_predicates) {
      append_indent(&stage_output, 3);
      sb_append(&stage_output, variant_index == 0 ? "#if " : "#elif ");
      emit_variant_predicate(&plan, variant, &stage_output);
      sb_append_char(&stage_output, '\n');
    } else {
      append_indent(&stage_output, 3);
      sb_append(&stage_output, "// Single exact variant\n");
    }
    append_indented_source(&stage_output, &variant_hlsl);
    sb_free(&variant_hlsl);
    if (!sb_ok(&stage_output)) {
      set_diagnostic(diagnostic, SHADERLAB_STAGE_OUTPUT_FAILED, stage_index,
                     variant->subprogram_index, -1);
      sb_free(&stage_output);
      shaderlab_stage_plan_free(&plan);
      return false;
    }
  }
  if (has_conditional_predicates) {
    append_indent(&stage_output, 3);
    sb_append(&stage_output, "#endif\n");
  }
  sb_append_char(&stage_output, '\n');
  if (!sb_ok(&stage_output)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_OUTPUT_FAILED, stage_index, -1,
                   -1);
    sb_free(&stage_output);
    shaderlab_stage_plan_free(&plan);
    return false;
  }

  sb_append_len(output, stage_output.buf, stage_output.len);
  const bool success = sb_ok(output);
  if (!success) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_OUTPUT_FAILED, stage_index, -1,
                   -1);
  } else {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_OK, stage_index, -1, -1);
  }
  sb_free(&stage_output);
  shaderlab_stage_plan_free(&plan);
  return success;
}

static bool planned_state_has_keyword(const ShaderLabVariantState *state,
                                      uint16_t raw_keyword) {
  size_t low = 0;
  size_t high = state->keyword_count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2u;
    if (state->keyword_indices[middle] == raw_keyword) return true;
    if (state->keyword_indices[middle] < raw_keyword)
      low = middle + 1u;
    else
      high = middle;
  }
  return false;
}

static void emit_planned_exact_state_predicate(
    const ShaderLabVariantPlan *plan,
    const ShaderLabVariantState *state, const uint8_t *generated_used,
    StringBuilder *output) {
  bool emitted = false;
  for (int raw = 0; raw < plan->shader->keyword_names.count; ++raw) {
    if (!generated_used[raw]) continue;
    if (emitted) sb_append(output, " && ");
    if (!planned_state_has_keyword(state, (uint16_t)raw))
      sb_append_char(output, '!');
    sb_append(output, "defined(");
    sb_append(output, plan->shader->keyword_names.keywords[raw]);
    sb_append_char(output, ')');
    emitted = true;
  }
  if (!emitted) sb_append_char(output, '1');
}

static void emit_planned_alias_predicate(
    const ShaderLabVariantPlan *plan,
    const ShaderLabPassStageVariantPlan *stage, size_t original_state,
    size_t alias_count, const uint8_t *generated_used, int tier,
    StringBuilder *output) {
  if (alias_count > 1) sb_append_char(output, '(');
  size_t emitted_aliases = 0;
  for (size_t i = 0; i < stage->generated_state_count; ++i) {
    if (stage->generated_aliases[i] != original_state) continue;
    if (emitted_aliases != 0) sb_append(output, " || ");
    sb_append_char(output, '(');
    emit_planned_exact_state_predicate(plan,
                                       &stage->generated_states[i],
                                       generated_used, output);
    sb_append_char(output, ')');
    ++emitted_aliases;
  }
  if (alias_count > 1) sb_append_char(output, ')');
  if (tier >= 0 && tier < 3) {
    if (emitted_aliases != 0) sb_append(output, " && ");
    sb_appendf(output, "defined(UNITY_HARDWARE_TIER%d)", tier + 1);
  }
}

#define SHADERLAB_STAGE_SYMBOLIC_MACRO_NAME_CAPACITY ((size_t)96)
#define SHADERLAB_STAGE_SYMBOLIC_SUFFIX_BITMAP_BYTES \
  ((size_t)((UINT16_MAX + UINT32_C(2) + UINT32_C(7)) / UINT32_C(8)))

static bool parse_symbolic_macro_suffix(
    const char *keyword, const char *prefix, size_t maximum,
    size_t *out_suffix) {
  if (!keyword || !prefix || !out_suffix) return false;
  const size_t prefix_length = strlen(prefix);
  if (strncmp(keyword, prefix, prefix_length) != 0) return false;
  const char *digits = keyword + prefix_length;
  if (*digits < '1' || *digits > '9') return false;
  size_t value = 0;
  for (; *digits != '\0'; ++digits) {
    if (*digits < '0' || *digits > '9') return false;
    const size_t digit = (size_t)(*digits - '0');
    if (digit > maximum || value > (maximum - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  if (value == 0 || value > maximum) return false;
  *out_suffix = value;
  return true;
}

/* Internal preprocessor state must never shadow a recovered keyword.  Try the
 * stable unsuffixed spelling first, then at most keyword_count suffixed names;
 * by pigeonhole, one of those keyword_count + 1 distinct names is free. */
static bool choose_symbolic_best_index_macro_name(
    const ShaderLabVariantPlan *plan, int stage_index, char *output,
    size_t output_capacity) {
  if (!plan || !plan->shader || !output || output_capacity == 0 ||
      stage_index < 0 || stage_index >= 5 ||
      plan->shader->keyword_names.count < 0) {
    return false;
  }
  const size_t keyword_count =
      (size_t)plan->shader->keyword_names.count;
  char base[SHADERLAB_STAGE_SYMBOLIC_MACRO_NAME_CAPACITY];
  char prefix[SHADERLAB_STAGE_SYMBOLIC_MACRO_NAME_CAPACITY];
  const int base_length = snprintf(
      base, sizeof(base), "DXBCSANDBOX_SYMBOLIC_S%d_BEST_INDEX", stage_index);
  const int prefix_length = snprintf(
      prefix, sizeof(prefix), "DXBCSANDBOX_SYMBOLIC_S%d_BEST_INDEX_",
      stage_index);
  if (base_length <= 0 || (size_t)base_length >= sizeof(base) ||
      prefix_length <= 0 || (size_t)prefix_length >= sizeof(prefix) ||
      (keyword_count != 0 &&
       !plan->shader->keyword_names.keywords)) {
    return false;
  }
  uint8_t suffix_used[SHADERLAB_STAGE_SYMBOLIC_SUFFIX_BITMAP_BYTES];
  memset(suffix_used, 0, sizeof(suffix_used));
  bool base_used = false;
  for (size_t raw = 0; raw < keyword_count; ++raw) {
    const char *keyword = plan->shader->keyword_names.keywords[raw];
    if (!keyword) return false;
    if (strcmp(keyword, base) == 0) {
      base_used = true;
      continue;
    }
    size_t suffix = 0;
    if (parse_symbolic_macro_suffix(
            keyword, prefix, keyword_count, &suffix)) {
      suffix_used[suffix / 8u] |= (uint8_t)(1u << (suffix % 8u));
    }
  }
  if (!base_used) {
    if ((size_t)base_length >= output_capacity) return false;
    memcpy(output, base, (size_t)base_length + 1u);
    return true;
  }
  for (size_t suffix = 1; suffix <= keyword_count; ++suffix) {
    if ((suffix_used[suffix / 8u] &
         (uint8_t)(1u << (suffix % 8u))) != 0) {
      continue;
    }
    const int length = snprintf(
        output, output_capacity,
        "DXBCSANDBOX_SYMBOLIC_S%d_BEST_INDEX_%zu", stage_index, suffix);
    return length > 0 && (size_t)length < output_capacity;
  }
  return false;
}

/* The full symbolic truth table is finite but may be substantially larger
 * than its serialized sparse row set.  Keep both the evaluation table and
 * emitted decision tree under explicit deterministic bounds. */
#define SHADERLAB_STAGE_MAX_SYMBOLIC_DECISION_LEAVES \
  ((size_t)UINT32_C(262144))
#define SHADERLAB_STAGE_MAX_SYMBOLIC_DECISION_NODES \
  ((size_t)UINT32_C(524287))
#define SHADERLAB_STAGE_MAX_SYMBOLIC_SCORE_EVALUATIONS \
  ((size_t)UINT32_C(100000000))

typedef struct {
  const ShaderLabVariantPlan *plan;
  const ShaderLabPassStageVariantPlan *stage;
  const size_t *selected_states;
  const char *selector_macro_name;
  size_t emitted_node_count;
  StringBuilder *output;
} ShaderLabSymbolicDecisionContext;

static bool symbolic_range_has_one_winner(
    const size_t *selected_states, size_t begin, size_t count,
    size_t *out_winner) {
  if (!selected_states || count == 0 || !out_winner) return false;
  const size_t winner = selected_states[begin];
  for (size_t offset = 1; offset < count; ++offset) {
    if (selected_states[begin + offset] != winner) return false;
  }
  *out_winner = winner;
  return true;
}

static bool emit_symbolic_literal_leaf(
    ShaderLabSymbolicDecisionContext *context, size_t winner) {
  if (!context || !context->output ||
      context->emitted_node_count >=
          SHADERLAB_STAGE_MAX_SYMBOLIC_DECISION_NODES) {
    return false;
  }
  ++context->emitted_node_count;
  append_indent(context->output, 3);
  sb_append(context->output, "#define ");
  sb_append(context->output, context->selector_macro_name);
  sb_appendf(context->output, " %zu\n", winner);
  return sb_ok(context->output);
}

static bool emit_symbolic_selector_decision_node(
    ShaderLabSymbolicDecisionContext *context, size_t remaining_axes,
    size_t begin, size_t count) {
  size_t winner = SIZE_MAX;
  if (symbolic_range_has_one_winner(
          context->selected_states, begin, count, &winner)) {
    return emit_symbolic_literal_leaf(context, winner);
  }
  if (remaining_axes == 0 || count < 2 || (count & 1u) != 0 ||
      context->emitted_node_count >=
          SHADERLAB_STAGE_MAX_SYMBOLIC_DECISION_NODES) {
    return false;
  }
  const size_t axis_index = remaining_axes - 1u;
  const ShaderLabVariantAxis *axis = &context->stage->axes[axis_index];
  if (!axis->has_default || axis->keyword_count != 1 ||
      !axis->keyword_indices) {
    return false;
  }
  const uint16_t raw_keyword = axis->keyword_indices[0];
  if ((int)raw_keyword >= context->plan->shader->keyword_names.count) {
    return false;
  }
  ++context->emitted_node_count;
  append_indent(context->output, 3);
  sb_append(context->output, "#if ");
  sb_append(context->output,
            context->plan->shader->keyword_names.keywords[raw_keyword]);
  sb_append_char(context->output, '\n');
  const size_t half = count / 2u;
  if (!emit_symbolic_selector_decision_node(
          context, axis_index, begin + half, half)) {
    return false;
  }
  append_indent(context->output, 3);
  sb_append(context->output, "#else\n");
  if (!emit_symbolic_selector_decision_node(
          context, axis_index, begin, half)) {
    return false;
  }
  append_indent(context->output, 3);
  sb_append(context->output, "#endif\n");
  return sb_ok(context->output);
}

static unsigned int symbolic_popcount(size_t value) {
  unsigned int count = 0;
  while (value != 0) {
    value &= value - 1u;
    ++count;
  }
  return count;
}

bool shaderlab_stage_symbolic_score_budget_allows(
    size_t request_count, size_t candidate_count) {
  return request_count != 0 && candidate_count != 0 &&
         request_count <=
             SHADERLAB_STAGE_MAX_SYMBOLIC_SCORE_EVALUATIONS /
                 candidate_count;
}

static ShaderLabVariantPlanStatus build_symbolic_selection_table(
    const ShaderLabVariantPlan *plan,
    const ShaderLabPassStageVariantPlan *stage, size_t *selected_states) {
  if (!plan || !plan->shader || !stage || !selected_states ||
      stage->axis_count == 0 ||
      stage->axis_count >= sizeof(size_t) * CHAR_BIT ||
      stage->generated_state_count !=
          (((size_t)1u) << stage->axis_count)) {
    return SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED;
  }
  /* Check by division before allocating or writing the caller's table.  The
   * leaf/node bounds alone constrain memory and source size, but do not make
   * the request-by-candidate scoring product practical. */
  if (!shaderlab_stage_symbolic_score_budget_allows(
          stage->generated_state_count, stage->state_count)) {
    return SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED;
  }
  size_t *candidate_masks = (size_t *)mem_alloc(
      stage->state_count * sizeof(*candidate_masks));
  if (!candidate_masks) return SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED;
  for (size_t state_index = 0; state_index < stage->state_count;
       ++state_index) {
    size_t mask = 0;
    const ShaderLabVariantState *state =
        &stage->ordered_states[state_index];
    for (size_t keyword_index = 0;
         keyword_index < state->keyword_count; ++keyword_index) {
      bool found = false;
      for (size_t axis_index = 0; axis_index < stage->axis_count;
           ++axis_index) {
        const ShaderLabVariantAxis *axis = &stage->axes[axis_index];
        if (axis->has_default && axis->keyword_count == 1 &&
            axis->keyword_indices &&
            axis->keyword_indices[0] ==
                state->keyword_indices[keyword_index]) {
          mask |= ((size_t)1u) << axis_index;
          found = true;
          break;
        }
      }
      if (!found) {
        mem_free(candidate_masks,
                 stage->state_count * sizeof(*candidate_masks));
        return SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED;
      }
    }
    candidate_masks[state_index] = mask;
  }
  for (size_t request_mask = 0;
       request_mask < stage->generated_state_count; ++request_mask) {
    size_t selected = SIZE_MAX;
    int32_t best_score = INT32_MIN;
    for (size_t state_index = 0; state_index < stage->state_count;
         ++state_index) {
      const size_t candidate_mask = candidate_masks[state_index];
      const unsigned int common =
          symbolic_popcount(request_mask & candidate_mask);
      const unsigned int candidate_only =
          symbolic_popcount(candidate_mask & ~request_mask);
      const int32_t score =
          (int32_t)common - 16 * (int32_t)candidate_only;
      if (selected == SIZE_MAX || score > best_score) {
        selected = state_index;
        best_score = score;
      }
    }
    if (selected == SIZE_MAX) {
      mem_free(candidate_masks,
               stage->state_count * sizeof(*candidate_masks));
      return SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED;
    }
    selected_states[request_mask] = selected;
  }
  mem_free(candidate_masks,
           stage->state_count * sizeof(*candidate_masks));
  return SHADERLAB_VARIANT_PLAN_OK;
}

/* `defined(KEYWORD)` alone cannot distinguish an enabled keyword from a
 * disabled keyword represented by a zero-valued macro, while the old mutable
 * normalizer/score-alias circuit also dispatched the wrong body under Unity's
 * legacy preprocessor.  Numeric #if truthiness handles zero-valued and
 * undefined disabled keywords; the pruned tree assigns one literal winner at
 * each reachable leaf without mutable aliases.  Winner computation preserves
 * Unity's exact -16 score and first-candidate tie rule over the planner's
 * serialized candidate set.  Runtime/device unsupported-row filtering is a
 * separate eligibility authority and is not inferred here. */
static ShaderLabVariantPlanStatus emit_symbolic_selector_decision_tree(
    const ShaderLabVariantPlan *plan,
    const ShaderLabPassStageVariantPlan *stage,
    const char *selector_macro_name, StringBuilder *output) {
  if (!plan || !stage || !selector_macro_name || !output ||
      stage->generated_state_count == 0 ||
      stage->generated_state_count >
          SHADERLAB_STAGE_MAX_SYMBOLIC_DECISION_LEAVES) {
    return SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED;
  }
  size_t *selected_states = (size_t *)mem_alloc(
      stage->generated_state_count * sizeof(*selected_states));
  if (!selected_states) return SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED;
  ShaderLabVariantPlanStatus status = build_symbolic_selection_table(
      plan, stage, selected_states);
  if (status != SHADERLAB_VARIANT_PLAN_OK) {
    mem_free(selected_states,
             stage->generated_state_count * sizeof(*selected_states));
    return status;
  }
  /* The recovered keyword table reserves every shader-controlled spelling,
   * but Unity's include stack may introduce additional macros before this
   * program body.  Fail compilation on that otherwise unknowable collision
   * instead of silently erasing or substituting the private selector. */
  append_indent(output, 3);
  sb_append(output, "#ifdef ");
  sb_append(output, selector_macro_name);
  sb_append_char(output, '\n');
  append_indent(output, 3);
  sb_append(output,
            "#error DXBCSandbox_symbolic_selector_macro_collision_");
  sb_append(output, selector_macro_name);
  sb_append_char(output, '\n');
  append_indent(output, 3);
  sb_append(output, "#endif\n");
  append_indent(output, 3);
  sb_append(output, "#undef ");
  sb_append(output, selector_macro_name);
  sb_append_char(output, '\n');
  ShaderLabSymbolicDecisionContext context = {
      plan, stage, selected_states, selector_macro_name, 0, output};
  if (!emit_symbolic_selector_decision_node(
          &context, stage->axis_count, 0,
          stage->generated_state_count)) {
    status = sb_ok(output) ? SHADERLAB_VARIANT_PLAN_PROOF_LIMIT_EXCEEDED
                           : SHADERLAB_VARIANT_PLAN_OUTPUT_FAILED;
  }
  mem_free(selected_states,
           stage->generated_state_count * sizeof(*selected_states));
  return status;
}

static void undefine_symbolic_selector(
    const char *selector_macro_name, StringBuilder *output) {
  append_indent(output, 3);
  sb_append(output, "#undef ");
  sb_append(output, selector_macro_name);
  sb_append_char(output, '\n');
}

static void emit_planned_symbolic_selector_predicate(
    size_t original_state, const char *selector_macro_name, int tier,
    StringBuilder *output) {
  sb_append(output, selector_macro_name);
  sb_appendf(output, " == %zu", original_state);
  if (tier >= 0 && tier < 3) {
    sb_append(output, " && ");
    sb_appendf(output, "defined(UNITY_HARDWARE_TIER%d)", tier + 1);
  }
}

static int planned_subprogram_for_state_and_tier(
    const ShaderLabPassStageVariantPlan *stage, size_t original_state,
    int tier) {
  if (!stage || original_state >= stage->state_count) return -1;
  size_t ordinal = 0;
  for (size_t i = 0; i < stage->variant_count; ++i) {
    const ShaderLabPlannedVariant *variant = &stage->variants[i];
    if (variant->hardware_tier_group != tier) continue;
    if (ordinal++ == original_state) return variant->subprogram_index;
  }
  return -1;
}

int shaderlab_stage_planned_subprogram_index(
    const ShaderLabVariantPlan *plan, int stage_index,
    size_t original_state, int hardware_tier_group) {
  if (!plan || stage_index < 0 || stage_index >= 5) return -1;
  return planned_subprogram_for_state_and_tier(
      &plan->stages[stage_index], original_state, hardware_tier_group);
}

static void emit_planned_manifest(
    const SerializedSubProgram *sub,
    size_t original_state, size_t alias_count, bool symbolic_selector,
    int stage_index, int tier,
    StringBuilder *output) {
  static const char *const stage_names[] = {
      "vertex", "fragment", "geometry", "hull", "domain"};
  append_indent(output, 3);
  sb_append(output, "// DXBCSandbox-VariantPlan stage=");
  if (stage_index < 0 || stage_index >= 5) {
    output->failed = true;
    return;
  }
  sb_append(output, stage_names[stage_index]);
  sb_appendf(output, " blob=%d requirements=%" PRIu64 " tier=",
             sub->blob_index, sub->shader_requirements);
  if (tier == 3)
    sb_append(output, "generic");
  else
    sb_appendf(output, "%d", tier + 1);
  sb_appendf(output, " serialized-state=%zu", original_state);
  if (symbolic_selector)
    sb_append(output, " selector=symbolic-first-best\n");
  else
    sb_appendf(output, " generated-aliases=%zu\n", alias_count);
}

bool emit_stage_hlsl_with_variant_plan_mode(
    const ShaderLabVariantPlan *variant_plan, int stage_index,
    const BlobEntry *blob_entries, int entry_count, uint8_t **segments,
    const int *segment_lengths, int segment_count, bool high_level,
    StringBuilder *output, ShaderLabStageDiagnostic *diagnostic) {
  set_diagnostic(diagnostic, SHADERLAB_STAGE_INVALID_ARGUMENT, stage_index, -1,
                 -1);
  if (!variant_plan || !variant_plan->pass || !variant_plan->shader ||
      !output || stage_index < 0 || stage_index > 4) return false;
  const ShaderLabPassStageVariantPlan *stage =
      &variant_plan->stages[stage_index];
  const bool symbolic_selector =
      stage->generated_domain_is_symbolic_boolean;
  if (!stage->active || stage->state_count == 0 ||
      !stage->ordered_states || stage->generated_state_count == 0 ||
      (symbolic_selector
           ? (stage->axis_count == 0 || stage->generated_states ||
              stage->generated_aliases)
           : (!stage->generated_states || !stage->generated_aliases))) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_VARIANT_PLAN_FAILED,
                   stage_index, -1, -1);
    if (diagnostic)
      diagnostic->variant_plan_status =
          SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT;
    return false;
  }

  uint8_t *generated_used = NULL;
  size_t *alias_counts = NULL;
  if (variant_plan->shader->keyword_names.count > 0) {
    generated_used = (uint8_t *)mem_alloc(
        (size_t)variant_plan->shader->keyword_names.count *
        sizeof(*generated_used));
  }
  alias_counts =
      (size_t *)mem_alloc(stage->state_count * sizeof(*alias_counts));
  if ((variant_plan->shader->keyword_names.count > 0 && !generated_used) ||
      !alias_counts) {
    if (generated_used)
      mem_free(generated_used,
               (size_t)variant_plan->shader->keyword_names.count *
                   sizeof(*generated_used));
    if (alias_counts)
      mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
    set_diagnostic(diagnostic, SHADERLAB_STAGE_ALLOCATION_FAILED,
                   stage_index, -1, -1);
    return false;
  }
  if (generated_used)
    memset(generated_used, 0,
           (size_t)variant_plan->shader->keyword_names.count *
               sizeof(*generated_used));
  memset(alias_counts, 0, stage->state_count * sizeof(*alias_counts));
  if (symbolic_selector) {
    for (size_t original_state = 0;
         original_state < stage->state_count; ++original_state)
      alias_counts[original_state] = 1;
    for (size_t axis_index = 0; axis_index < stage->axis_count;
         ++axis_index) {
      const ShaderLabVariantAxis *axis = &stage->axes[axis_index];
      for (size_t keyword_index = 0;
           keyword_index < axis->keyword_count; ++keyword_index)
        generated_used[axis->keyword_indices[keyword_index]] = 1;
    }
  } else {
    for (size_t generated_index = 0;
         generated_index < stage->generated_state_count; ++generated_index) {
      const size_t alias = stage->generated_aliases[generated_index];
      if (alias >= stage->state_count) {
        mem_free(generated_used,
                 (size_t)variant_plan->shader->keyword_names.count *
                     sizeof(*generated_used));
        mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
        set_diagnostic(diagnostic, SHADERLAB_STAGE_VARIANT_PLAN_FAILED,
                       stage_index, -1, -1);
        if (diagnostic)
          diagnostic->variant_plan_status =
              SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED;
        return false;
      }
      ++alias_counts[alias];
      const ShaderLabVariantState *state =
          &stage->generated_states[generated_index];
      for (size_t keyword_index = 0;
           keyword_index < state->keyword_count; ++keyword_index) {
        generated_used[state->keyword_indices[keyword_index]] = 1;
      }
    }
  }

  HLSLEmitNames names;
  if (!stage_emit_names(stage_index, &names)) {
    mem_free(generated_used,
             (size_t)variant_plan->shader->keyword_names.count *
                 sizeof(*generated_used));
    mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
    return false;
  }

  const bool conditional = stage->state_count > 1 ||
                           stage->uses_specific_hardware_tiers;
  StringBuilder stage_output;
  sb_init_with_capacity(&stage_output, 4096);
  char symbolic_selector_macro[
      SHADERLAB_STAGE_SYMBOLIC_MACRO_NAME_CAPACITY];
  if (symbolic_selector) {
    if (!choose_symbolic_best_index_macro_name(
            variant_plan, stage_index, symbolic_selector_macro,
            sizeof(symbolic_selector_macro))) {
      set_diagnostic(diagnostic, SHADERLAB_STAGE_VARIANT_PLAN_FAILED,
                     stage_index, -1, -1);
      if (diagnostic)
        diagnostic->variant_plan_status =
            SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED;
      sb_free(&stage_output);
      mem_free(generated_used,
               (size_t)variant_plan->shader->keyword_names.count *
                   sizeof(*generated_used));
      mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
      return false;
    }
    const ShaderLabVariantPlanStatus selector_status =
        emit_symbolic_selector_decision_tree(
            variant_plan, stage, symbolic_selector_macro, &stage_output);
    if (selector_status != SHADERLAB_VARIANT_PLAN_OK) {
      set_diagnostic(
          diagnostic,
          selector_status == SHADERLAB_VARIANT_PLAN_ALLOCATION_FAILED
              ? SHADERLAB_STAGE_ALLOCATION_FAILED
              : (selector_status == SHADERLAB_VARIANT_PLAN_OUTPUT_FAILED
                     ? SHADERLAB_STAGE_OUTPUT_FAILED
                     : SHADERLAB_STAGE_VARIANT_PLAN_FAILED),
          stage_index, -1, -1);
      if (diagnostic) diagnostic->variant_plan_status = selector_status;
      sb_free(&stage_output);
      mem_free(generated_used,
               (size_t)variant_plan->shader->keyword_names.count *
                   sizeof(*generated_used));
      mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
      return false;
    }
  }
  size_t emitted_body_count = 0;
  const int first_tier = stage->uses_specific_hardware_tiers ? 0 : 3;
  const int tier_end = stage->uses_specific_hardware_tiers ? 3 : 4;
  for (int tier = first_tier; tier < tier_end; ++tier) {
    for (size_t original_state = 0; original_state < stage->state_count;
         ++original_state) {
      if (alias_counts[original_state] == 0) {
        set_diagnostic(diagnostic, SHADERLAB_STAGE_VARIANT_PLAN_FAILED,
                       stage_index, -1, -1);
        if (diagnostic)
          diagnostic->variant_plan_status =
              SHADERLAB_VARIANT_PLAN_ALIAS_PROOF_FAILED;
        sb_free(&stage_output);
        mem_free(generated_used,
                 (size_t)variant_plan->shader->keyword_names.count *
                     sizeof(*generated_used));
        mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
        return false;
      }
      const int subprogram_index = planned_subprogram_for_state_and_tier(
          stage, original_state, tier);
      if (subprogram_index < 0) {
        set_diagnostic(diagnostic, SHADERLAB_STAGE_VARIANT_PLAN_FAILED,
                       stage_index, -1, -1);
        if (diagnostic)
          diagnostic->variant_plan_status =
              SHADERLAB_VARIANT_PLAN_TIER_DOMAIN_MISMATCH;
        sb_free(&stage_output);
        mem_free(generated_used,
                 (size_t)variant_plan->shader->keyword_names.count *
                     sizeof(*generated_used));
        mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
        return false;
      }
      const SerializedSubProgram *sub =
          &variant_plan->pass->subprograms[stage_index][subprogram_index];
      StringBuilder variant_hlsl;
      sb_init_with_capacity(&variant_hlsl, 4096);
      if (!translate_stage_to_hlsl(
              variant_plan->pass, stage_index, subprogram_index, blob_entries,
              entry_count, segments, segment_lengths, segment_count,
              &variant_hlsl, &names,
              (const char *const *)
                  variant_plan->shader->keyword_names.keywords,
              (size_t)variant_plan->shader->keyword_names.count, high_level,
              diagnostic)) {
        sb_free(&variant_hlsl);
        sb_free(&stage_output);
        mem_free(generated_used,
                 (size_t)variant_plan->shader->keyword_names.count *
                     sizeof(*generated_used));
        mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
        return false;
      }
      emit_planned_manifest(sub, original_state,
                            alias_counts[original_state], symbolic_selector,
                            stage_index, tier, &stage_output);
      if (conditional) {
        append_indent(&stage_output, 3);
        sb_append(&stage_output,
                  emitted_body_count == 0 ? "#if " : "#elif ");
        if (symbolic_selector) {
          emit_planned_symbolic_selector_predicate(
              original_state, symbolic_selector_macro, tier, &stage_output);
        } else {
          emit_planned_alias_predicate(
              variant_plan, stage, original_state,
              alias_counts[original_state], generated_used, tier,
              &stage_output);
        }
        sb_append_char(&stage_output, '\n');
      } else {
        append_indent(&stage_output, 3);
        sb_append(&stage_output, "// Single exact planned variant\n");
      }
      append_indented_source(&stage_output, &variant_hlsl);
      sb_free(&variant_hlsl);
      ++emitted_body_count;
      if (!sb_ok(&stage_output)) {
        set_diagnostic(diagnostic, SHADERLAB_STAGE_OUTPUT_FAILED, stage_index,
                       subprogram_index, -1);
        sb_free(&stage_output);
        mem_free(generated_used,
                 (size_t)variant_plan->shader->keyword_names.count *
                     sizeof(*generated_used));
        mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
        return false;
      }
    }
  }
  if (conditional) {
    append_indent(&stage_output, 3);
    sb_append(&stage_output, "#endif\n");
  }
  if (symbolic_selector) {
    undefine_symbolic_selector(symbolic_selector_macro, &stage_output);
  }
  sb_append_char(&stage_output, '\n');
  if (!sb_ok(&stage_output)) {
    set_diagnostic(diagnostic, SHADERLAB_STAGE_OUTPUT_FAILED, stage_index, -1,
                   -1);
    sb_free(&stage_output);
    mem_free(generated_used,
             (size_t)variant_plan->shader->keyword_names.count *
                 sizeof(*generated_used));
    mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
    return false;
  }
  sb_append_len(output, stage_output.buf, stage_output.len);
  const bool success = sb_ok(output);
  sb_free(&stage_output);
  mem_free(generated_used,
           (size_t)variant_plan->shader->keyword_names.count *
               sizeof(*generated_used));
  mem_free(alias_counts, stage->state_count * sizeof(*alias_counts));
  set_diagnostic(diagnostic,
                 success ? SHADERLAB_STAGE_OK
                         : SHADERLAB_STAGE_OUTPUT_FAILED,
                 stage_index, -1, -1);
  return success;
}

bool emit_stage_hlsl_with_variant_plan(
    const ShaderLabVariantPlan *variant_plan, int stage_index,
    const BlobEntry *blob_entries, int entry_count, uint8_t **segments,
    const int *segment_lengths, int segment_count, StringBuilder *output,
    ShaderLabStageDiagnostic *diagnostic) {
  return emit_stage_hlsl_with_variant_plan_mode(
      variant_plan, stage_index, blob_entries, entry_count, segments,
      segment_lengths, segment_count, false, output, diagnostic);
}
