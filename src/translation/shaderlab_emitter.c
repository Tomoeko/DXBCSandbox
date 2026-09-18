// SPDX-License-Identifier: GPL-3.0-only

#include "translation/shaderlab_emitter.h"
#include "dxbc/dxbc_parser.h"
#include "translation/hlsl_emitter.h"
#include "translation/usil.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "translation/shaderlab_emitter_internal.h"

static bool has_text(const char* value) {
  return value && value[0] != '\0';
}

static bool pass_has_platform_stage(const SerializedPass *pass, int stage,
                                    int platform) {
  if (!pass || stage < 0 || stage >= 6) return false;
  for (int i = 0; i < pass->subprogram_count[stage]; i++) {
    if (serialized_pass_subprogram_is_platform(pass, stage, i, platform))
      return true;
  }
  return false;
}

/*
 * Candidate emission is a projection onto one explicitly selected compiler
 * platform (D3D11, platform 4).  A serialized ordinary pass with complete
 * platform authority but no matching program is not a malformed D3D pass: it
 * is a pass for another platform and cannot participate in the D3D11 runtime
 * choice.  Keep unknown/incomplete metadata in the normal fail-closed path;
 * only exact, structurally valid metadata can prove that a pass is absent
 * from the projection.
 */
bool shaderlab_pass_is_proven_not_platform(const SerializedPass *pass,
                                           int platform) {
  if (!pass || !pass->has_serialized_platforms ||
      !serialized_shader_platform_is_known(platform) ||
      pass->platform_count < 0 ||
      (pass->platform_count > 0 && !pass->platforms)) {
    return false;
  }
  bool platform_is_listed = false;
  for (int left = 0; left < pass->platform_count; ++left) {
    if (!serialized_shader_platform_is_known(pass->platforms[left]))
      return false;
    if (pass->platforms[left] == platform) platform_is_listed = true;
    for (int right = 0; right < left; ++right) {
      if (pass->platforms[left] == pass->platforms[right]) return false;
    }
  }
  bool has_any_subprogram = false;
  uint32_t known_program_mask = 0U;
  for (int stage = 0; stage < 6; ++stage) {
    uint32_t stage_mask = 0U;
    if (!shader_stage_serialized_program_mask_bit(
            (UnitySerializedProgramStage)stage, &stage_mask)) {
      return false;
    }
    known_program_mask |= stage_mask;
    if (pass->subprogram_count[stage] < 0 ||
        (pass->subprogram_count[stage] > 0 &&
         !pass->subprograms[stage])) {
      return false;
    }
    for (int subprogram = 0; subprogram < pass->subprogram_count[stage];
         ++subprogram) {
      has_any_subprogram = true;
      bool type_has_listed_platform = false;
      for (int listed = 0; listed < pass->platform_count; ++listed) {
        if (serialized_gpu_program_type_is_platform(
                pass->subprograms[stage][subprogram].program_type,
                pass->platforms[listed])) {
          type_has_listed_platform = true;
          break;
        }
      }
      /* A GL-only platform list cannot authorize omission of a D3D or
       * unknown GPU record.  Contradictory metadata stays in the ordinary
       * fail-closed candidate path. */
      if (!type_has_listed_platform) return false;
    }
    if (pass_has_platform_stage(pass, stage, platform)) return false;
  }
  if (platform_is_listed) {
    /* Unity 2021.3 can retain an ordinary pass shell whose platform plane and
     * program mask name the compiled stages after every player subprogram for
     * that shell was stripped.  Such a pass contains no selectable program
     * for the D3D11 projection.  Accept only that exact shape: a nonempty mask
     * made solely of known stage bits and no subprogram row at all.  A zero or
     * unknown mask, or any surviving non-target row, remains contradictory
     * and follows the normal fail-closed resolver path. */
    return !has_any_subprogram && pass->program_mask != 0U &&
           (pass->program_mask & ~known_program_mask) == 0U;
  }
  return true;
}

static bool subshader_has_platform_content(const SerializedSubShader *sub,
                                           int platform) {
  if (!sub || sub->pass_count < 0 ||
      (sub->pass_count > 0 && !sub->passes)) {
    /* Let the caller's ordinary structural validation report this. */
    return true;
  }
  for (int pass_index = 0; pass_index < sub->pass_count; ++pass_index) {
    const SerializedPass *pass = &sub->passes[pass_index];
    if (pass->pass_type == 2 || pass->pass_type == 1 ||
        has_text(pass->use_name) ||
        !shaderlab_pass_is_proven_not_platform(pass, platform)) {
      return true;
    }
  }
  return false;
}

static void emit_raw_verifier_stage_source(const char *source, int stage_index,
                                           StringBuilder *output) {
  if (source && source[0]) {
    const char *cursor = source;
    while (*cursor) {
      append_indent(output, 3);
      while (*cursor && *cursor != '\n') sb_append_char(output, *cursor++);
      sb_append_char(output, '\n');
      if (*cursor == '\n') ++cursor;
    }
    return;
  }

  append_indent(output, 3);
  if (stage_index == 0) {
    sb_append(output,
              "float4 vert(float4 v : POSITION) : SV_POSITION { return v; "
              "}\n\n");
  } else {
    sb_append(output, "float4 frag() : SV_Target { return 0; }\n\n");
  }
}

static bool property_identifier_is_valid(const char *identifier) {
  if (!identifier || identifier[0] == '\0') return false;
  unsigned char first = (unsigned char)identifier[0];
  if (!((first >= 'A' && first <= 'Z') ||
        (first >= 'a' && first <= 'z') || first == '_')) {
    return false;
  }
  for (size_t i = 1; identifier[i] != '\0'; i++) {
    unsigned char c = (unsigned char)identifier[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

static bool property_attribute_is_valid(const char *attribute) {
  if (!attribute || attribute[0] == '\0') return false;
  for (const unsigned char *cursor = (const unsigned char *)attribute;
       *cursor; cursor++) {
    if (*cursor < 0x20u || *cursor == 0x7fu || *cursor == '[' ||
        *cursor == ']') {
      return false;
    }
  }
  return true;
}

static bool append_shaderlab_float(StringBuilder *sb, float value) {
  if (!sb || !isfinite(value)) return false;
  /* Nine significant decimal digits round-trip every IEEE-754 binary32. */
  sb_appendf(sb, "%.9g", value);
  return sb_ok(sb);
}

static bool emit_property(StringBuilder *sb,
                          const ParsedShaderProperty *property, int indent) {
  if (!sb || !property || property->attribute_count < 0 ||
      (property->attribute_count > 0 && !property->attributes) ||
      !property_identifier_is_valid(property->name)) {
    return false;
  }

  append_indent(sb, indent);
  for (int i = 0; i < property->attribute_count; i++) {
    if (!property_attribute_is_valid(property->attributes[i])) return false;
    sb_append_char(sb, '[');
    sb_append(sb, property->attributes[i]);
    sb_append(sb, "] ");
  }

  static const struct {
    uint32_t bit;
    const char *attribute;
  } flag_attributes[] = {
      {1u, "HideInInspector"},
      {2u, "PerRendererData"},
      {4u, "NoScaleOffset"},
      {8u, "Normal"},
      {16u, "HDR"},
      {32u, "Gamma"},
      {64u, "NonModifiableTextureData"},
      {128u, "MainTexture"},
      {256u, "MainColor"},
  };
  const uint32_t known_flags = 511u;
  if ((property->flags & ~known_flags) != 0u) return false;
  for (size_t i = 0; i < sizeof(flag_attributes) / sizeof(flag_attributes[0]);
       i++) {
    if ((property->flags & flag_attributes[i].bit) == 0u) continue;
    sb_append_char(sb, '[');
    sb_append(sb, flag_attributes[i].attribute);
    sb_append(sb, "] ");
  }

  sb_append(sb, property->name);
  sb_append(sb, " (");
  if (!append_shaderlab_quoted(sb, property->description)) return false;
  sb_append(sb, ", ");

  switch (property->type) {
  case 0:
    sb_append(sb, "Color) = (");
    break;
  case 1:
    sb_append(sb, "Vector) = (");
    break;
  case 2:
    sb_append(sb, "Float) = ");
    return append_shaderlab_float(sb, property->def_value[0]) &&
           (sb_append_char(sb, '\n'), sb_ok(sb));
  case 3:
    sb_append(sb, "Range(");
    if (!append_shaderlab_float(sb, property->def_value[1])) return false;
    sb_append(sb, ", ");
    if (!append_shaderlab_float(sb, property->def_value[2])) return false;
    sb_append(sb, ")) = ");
    return append_shaderlab_float(sb, property->def_value[0]) &&
           (sb_append_char(sb, '\n'), sb_ok(sb));
  case 4: {
    const char *dimension = NULL;
    switch (property->def_texture_dim) {
    case 1: dimension = "Any"; break;
    case 2: dimension = "2D"; break;
    case 3: dimension = "3D"; break;
    case 4: dimension = "Cube"; break;
    case 5: dimension = "2DArray"; break;
    case 6: dimension = "CubeArray"; break;
    default: return false;
    }
    sb_append(sb, dimension);
    sb_append(sb, ") = ");
    if (!append_shaderlab_quoted(sb, property->def_texture_name)) return false;
    sb_append(sb, " {}\n");
    return sb_ok(sb);
  }
  case 5: {
    float value = property->def_value[0];
    /* Compare in double: (float)INT_MAX rounds up to 2147483648 on binary32
     * and would make the following float-to-int conversion undefined. */
    if (!isfinite(value) || (double)value < (double)INT_MIN ||
        (double)value > (double)INT_MAX || (float)(int)value != value) {
      return false;
    }
    sb_appendf(sb, "Int) = %d\n", (int)value);
    return sb_ok(sb);
  }
  default:
    return false;
  }

  for (int i = 0; i < 4; i++) {
    if (i > 0) sb_append_char(sb, ',');
    if (!append_shaderlab_float(sb, property->def_value[i])) return false;
  }
  sb_append(sb, ")\n");
  return sb_ok(sb);
}

static void report_candidate_stage_failure(
    const ShaderLabStageDiagnostic *diagnostic) {
  if (!diagnostic) return;
  fprintf(stderr,
          "[shaderlab] candidate stage emission failed: stage=%d "
          "subprogram=%d conflict=%d status=%s",
          diagnostic->stage_index, diagnostic->subprogram_index,
          diagnostic->conflicting_subprogram_index,
          shaderlab_stage_status_name(diagnostic->status));
  if (diagnostic->status == SHADERLAB_STAGE_STAGE_CONTRACT_FAILED) {
    fprintf(stderr, " contract=%s tuple=%s",
            dxbc_stage_contract_status_name(
                diagnostic->stage_contract_status),
            shader_stage_tuple_status_name(
                diagnostic->stage_tuple_status));
  } else if (diagnostic->status == SHADERLAB_STAGE_VARIANT_PLAN_FAILED) {
    fprintf(stderr, " variant-plan=%s raw-keyword=%d",
            shaderlab_variant_plan_status_name(
                diagnostic->variant_plan_status),
            diagnostic->raw_keyword_index);
  } else if (diagnostic->status == SHADERLAB_STAGE_HLSL_EMISSION_FAILED) {
    fprintf(stderr, " hlsl-status=%s phase=%s reason=%s",
            hlsl_emit_status_name(diagnostic->hlsl.status),
            hlsl_emit_phase_name(diagnostic->hlsl.phase),
            hlsl_emit_reason_name(diagnostic->hlsl.reason));
    if (diagnostic->hlsl.instruction_index >= 0) {
      fprintf(stderr, " instruction=%d opcode=%s(%d)",
              diagnostic->hlsl.instruction_index,
              hlsl_emit_opcode_name(diagnostic->hlsl.opcode),
              diagnostic->hlsl.opcode);
    }
    if (diagnostic->hlsl.source_instruction_index != UINT32_MAX) {
      fprintf(stderr, " source-instruction=%" PRIu32,
              diagnostic->hlsl.source_instruction_index);
    }
    if (diagnostic->hlsl.metadata.kind != HLSL_EMIT_METADATA_NONE) {
      fprintf(stderr, " metadata=%s/%s[%d] register=%d",
              hlsl_emit_metadata_source_name(
                  diagnostic->hlsl.metadata.source),
              hlsl_emit_metadata_kind_name(
                  diagnostic->hlsl.metadata.kind),
              diagnostic->hlsl.metadata.record_index,
              diagnostic->hlsl.metadata.register_index);
    }
  }
  fputc('\n', stderr);
}

static void report_target_failure(
    const ShaderLabTargetDiagnostic *diagnostic) {
  if (!diagnostic) return;
  fprintf(stderr,
          "[shaderlab] pass target resolution failed: stage=%d "
          "subprogram=%d status=%s requirements=0x%016" PRIx64
          " sm=%u.%u document=%s contract=%s tuple=%s\n",
          diagnostic->stage_index, diagnostic->subprogram_index,
          shaderlab_target_status_name(diagnostic->status),
          diagnostic->shader_requirements, diagnostic->shader_model_major,
          diagnostic->shader_model_minor,
          dxbc_document_diagnostic_code_name(diagnostic->document_status),
          dxbc_stage_contract_status_name(
              diagnostic->stage_contract_status),
          shader_stage_tuple_status_name(diagnostic->stage_tuple_status));
}

static void set_candidate_stage_failure(
    ShaderLabCandidateDiagnostic *candidate_diagnostic, int subshader_index,
    int pass_index, const ShaderLabStageDiagnostic *stage_diagnostic) {
  if (!candidate_diagnostic || !stage_diagnostic) return;
  candidate_diagnostic->status = SHADERLAB_CANDIDATE_STAGE_FAILED;
  candidate_diagnostic->subshader_index = subshader_index;
  candidate_diagnostic->pass_index = pass_index;
  candidate_diagnostic->stage = *stage_diagnostic;
}

static void stage_diagnostic_from_variant_plan(
    ShaderLabStageDiagnostic *stage,
    const ShaderLabVariantPlanDiagnostic *plan) {
  memset(stage, 0, sizeof(*stage));
  stage->status = SHADERLAB_STAGE_VARIANT_PLAN_FAILED;
  stage->stage_index = plan ? plan->stage_index : -1;
  stage->subprogram_index = plan ? plan->subprogram_index : -1;
  stage->conflicting_subprogram_index =
      plan ? plan->conflicting_subprogram_index : -1;
  stage->stage_contract_status = DXBC_STAGE_CONTRACT_OK;
  stage->stage_tuple_status = SHADER_STAGE_TUPLE_OK;
  stage->variant_plan_status =
      plan ? plan->status : SHADERLAB_VARIANT_PLAN_INVALID_ARGUMENT;
  stage->raw_keyword_index = plan ? plan->raw_keyword_index : -1;
  hlsl_emit_diagnostic_init(&stage->hlsl);
}

static void set_candidate_failure(
    ShaderLabCandidateDiagnostic *diagnostic, ShaderLabCandidateStatus status,
    int property_index, int subshader_index, int pass_index) {
  if (!diagnostic) return;
  diagnostic->status = status;
  diagnostic->property_index = property_index;
  diagnostic->subshader_index = subshader_index;
  diagnostic->pass_index = pass_index;
}

static void set_candidate_target_failure(
    ShaderLabCandidateDiagnostic *candidate_diagnostic, int subshader_index,
    int pass_index, const ShaderLabTargetDiagnostic *target_diagnostic) {
  if (!candidate_diagnostic || !target_diagnostic) return;
  candidate_diagnostic->status = SHADERLAB_CANDIDATE_PASS_TARGET_FAILED;
  candidate_diagnostic->subshader_index = subshader_index;
  candidate_diagnostic->pass_index = pass_index;
  candidate_diagnostic->target = *target_diagnostic;
}

const char *shaderlab_candidate_status_name(ShaderLabCandidateStatus status) {
  switch (status) {
  case SHADERLAB_CANDIDATE_OK: return "ok";
  case SHADERLAB_CANDIDATE_INVALID_ARGUMENT: return "invalid-argument";
  case SHADERLAB_CANDIDATE_PROPERTY_FAILED: return "property-failed";
  case SHADERLAB_CANDIDATE_SUBSHADER_TAG_FAILED:
    return "subshader-tag-failed";
  case SHADERLAB_CANDIDATE_GRABPASS_FAILED: return "grabpass-failed";
  case SHADERLAB_CANDIDATE_USEPASS_FAILED: return "usepass-failed";
  case SHADERLAB_CANDIDATE_UNSUPPORTED_STAGE: return "unsupported-stage";
  case SHADERLAB_CANDIDATE_PASS_NAME_FAILED: return "pass-name-failed";
  case SHADERLAB_CANDIDATE_PASS_TAG_FAILED: return "pass-tag-failed";
  case SHADERLAB_CANDIDATE_RENDER_STATE_FAILED:
    return "render-state-failed";
  case SHADERLAB_CANDIDATE_PASS_TARGET_FAILED:
    return "pass-target-failed";
  case SHADERLAB_CANDIDATE_STAGE_FAILED: return "stage-failed";
  case SHADERLAB_CANDIDATE_TRAILER_FAILED: return "trailer-failed";
  case SHADERLAB_CANDIDATE_OUTPUT_FAILED: return "output-failed";
  }
  return "unknown";
}

const char *shaderlab_candidate_reason_name(
    const ShaderLabCandidateDiagnostic *diagnostic) {
  if (!diagnostic) return "unknown";
  if (diagnostic->status == SHADERLAB_CANDIDATE_STAGE_FAILED) {
    if (diagnostic->stage.status == SHADERLAB_STAGE_VARIANT_PLAN_FAILED) {
      return shaderlab_variant_plan_status_name(
          diagnostic->stage.variant_plan_status);
    }
    return shaderlab_stage_status_name(diagnostic->stage.status);
  }
  if (diagnostic->status == SHADERLAB_CANDIDATE_PASS_TARGET_FAILED) {
    return shaderlab_target_status_name(diagnostic->target.status);
  }
  return shaderlab_candidate_status_name(diagnostic->status);
}

static bool shaderlab_emit_internal(const SerializedShader *shader,
                                    const char *vertex_hlsl,
                                    const char *fragment_hlsl,
                                    const BlobEntry *blob_entries,
                                    int entry_count, uint8_t **segments,
                                    const int *segment_lengths,
                                    int segment_count,
                                    bool require_complete_stages,
                                    StringBuilder *sb,
                                    ShaderLabCandidateDiagnostic
                                        *candidate_diagnostic) {
  if (!shader || !shader->name || !sb || shader->property_count < 0 ||
      shader->subshader_count < 0 || shader->dependency_count < 0 ||
      shader->custom_editor_for_render_pipeline_count < 0 ||
      (shader->property_count > 0 && !shader->properties) ||
      (shader->subshader_count > 0 && !shader->subshaders) ||
      (shader->dependency_count > 0 && !shader->dependencies) ||
      (shader->custom_editor_for_render_pipeline_count > 0 &&
       !shader->custom_editors_for_render_pipelines)) {
    set_candidate_failure(candidate_diagnostic,
                          SHADERLAB_CANDIDATE_INVALID_ARGUMENT, -1, -1, -1);
    return false;
  }
  // 1. Shader Header
  sb_append(sb, "// Auto-generated ShaderLab from DXBCSandbox C-emitter\n");
  sb_append(sb, "Shader ");
  if (!append_shaderlab_quoted(sb, shader->name)) {
    set_candidate_failure(candidate_diagnostic,
                          SHADERLAB_CANDIDATE_OUTPUT_FAILED, -1, -1, -1);
    return false;
  }
  sb_append(sb, "\n{\n");

  // 2. Properties block
  append_indent(sb, 1);
  sb_append(sb, "Properties\n");
  append_indent(sb, 1);
  sb_append(sb, "{\n");

  for (int i = 0; i < shader->property_count; i++) {
    if (!emit_property(sb, &shader->properties[i], 2)) {
      const ParsedShaderProperty *property = &shader->properties[i];
      fprintf(stderr,
              "[shaderlab] property emission failed: shader=%s index=%d "
              "name=%s type=%d flags=0x%08x texture-dim=%d\n",
              shader->name, i, property->name ? property->name : "<null>",
              property->type,
              property->flags, property->def_texture_dim);
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_PROPERTY_FAILED, i, -1, -1);
      return false;
    }
  }

  append_indent(sb, 1);
  sb_append(sb, "}\n\n");

  // 3. SubShaders block
  for (int i = 0; i < shader->subshader_count; i++) {
    const SerializedSubShader *sub = &shader->subshaders[i];
    if (sub->pass_count < 0 || (sub->pass_count > 0 && !sub->passes)) {
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_INVALID_ARGUMENT, -1, i, -1);
      return false;
    }

    if (require_complete_stages &&
        !subshader_has_platform_content(sub, 4)) {
      continue;
    }

    append_indent(sb, 1);
    sb_append(sb, "SubShader\n");
    append_indent(sb, 1);
    sb_append(sb, "{\n");

    if (!emit_tags(sb, &sub->tags, 2)) {
      fprintf(stderr,
              "[shaderlab] subshader tag emission failed: shader=%s "
              "subshader=%d\n",
              shader->name, i);
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_SUBSHADER_TAG_FAILED, -1, i,
                            -1);
      return false;
    }
    if (sub->lod > 0) {
      append_indent(sb, 2);
      char lod_str[32];
      snprintf(lod_str, sizeof(lod_str), "LOD %d\n", sub->lod);
      sb_append(sb, lod_str);
    }

    // 4. Passes block
    for (int j = 0; j < sub->pass_count; j++) {
      const SerializedPass *pass = &sub->passes[j];

      // Handle GrabPass or UsePass
      if (pass->pass_type == 2) { // GrabPass
        append_indent(sb, 2);
        sb_append(sb, "GrabPass {\n");
        if (has_text(pass->name)) {
          append_indent(sb, 3);
          sb_append(sb, "Name ");
          if (!append_shaderlab_quoted(sb, pass->name)) {
            set_candidate_failure(candidate_diagnostic,
                                  SHADERLAB_CANDIDATE_GRABPASS_FAILED, -1, i,
                                  j);
            return false;
          }
          sb_append_char(sb, '\n');
        }
        if (has_text(pass->texture_name)) {
          append_indent(sb, 3);
          if (!append_shaderlab_quoted(sb, pass->texture_name)) {
            set_candidate_failure(candidate_diagnostic,
                                  SHADERLAB_CANDIDATE_GRABPASS_FAILED, -1, i,
                                  j);
            return false;
          }
          sb_append_char(sb, '\n');
        }
        if (!emit_tags(sb, &pass->tags, 3)) {
          fprintf(stderr,
                  "[shaderlab] GrabPass tag emission failed: shader=%s "
                  "subshader=%d pass=%d\n",
                  shader->name, i, j);
          set_candidate_failure(candidate_diagnostic,
                                SHADERLAB_CANDIDATE_GRABPASS_FAILED, -1, i,
                                j);
          return false;
        }
        append_indent(sb, 2);
        sb_append(sb, "}\n");
        continue;
      }
      if (pass->pass_type == 1 || has_text(pass->use_name)) { // UsePass
        append_indent(sb, 2);
        sb_append(sb, "UsePass ");
        if (!append_shaderlab_quoted(
                sb, has_text(pass->use_name) ? pass->use_name : pass->name)) {
          set_candidate_failure(candidate_diagnostic,
                                SHADERLAB_CANDIDATE_USEPASS_FAILED, -1, i, j);
          return false;
        }
        sb_append_char(sb, '\n');
        continue;
      }

      if (require_complete_stages &&
          shaderlab_pass_is_proven_not_platform(pass, 4)) {
        continue;
      }

      /* Ray-tracing ShaderLab reconstruction has no attested inverse. Hull
       * and domain programs are accepted below only when their byte-verified
       * tessellation lifts recognize the complete stage contract. */
      for (int stage = 5; stage < 6; stage++) {
        if (pass_has_platform_stage(pass, stage, 4)) {
          fprintf(stderr,
                  "[shaderlab] D3D11 candidate reconstruction does not yet "
                  "support serialized stage %d\n",
                  stage);
          set_candidate_failure(candidate_diagnostic,
                                SHADERLAB_CANDIDATE_UNSUPPORTED_STAGE, -1, i,
                                j);
          if (candidate_diagnostic)
            candidate_diagnostic->unsupported_stage_index = stage;
          return false;
        }
      }

      append_indent(sb, 2);
      sb_append(sb, "Pass\n");
      append_indent(sb, 2);
      sb_append(sb, "{\n");

      if (has_text(pass->state.name)) {
        append_indent(sb, 3);
        sb_append(sb, "Name ");
        if (!append_shaderlab_quoted(sb, pass->state.name)) {
          set_candidate_failure(candidate_diagnostic,
                                SHADERLAB_CANDIDATE_PASS_NAME_FAILED, -1, i,
                                j);
          return false;
        }
        sb_append_char(sb, '\n');
      }

      if (!emit_tags(sb, &pass->tags, 3)) {
        fprintf(stderr,
                "[shaderlab] pass tag emission failed: shader=%s "
                "subshader=%d pass=%d\n",
                shader->name, i, j);
        set_candidate_failure(candidate_diagnostic,
                              SHADERLAB_CANDIDATE_PASS_TAG_FAILED, -1, i, j);
        return false;
      }

      /* m_State.m_LOD is the pass-level LOD command. It is independent of
       * SerializedSubShader.m_LOD; Unity's own built-in sources use both
       * scopes. */
      if (pass->state.lod < 0) {
        set_candidate_failure(candidate_diagnostic,
                              SHADERLAB_CANDIDATE_RENDER_STATE_FAILED, -1,
                              i, j);
        return false;
      }
      if (pass->state.lod > 0) {
        append_indent(sb, 3);
        sb_appendf(sb, "LOD %d\n", pass->state.lod);
      }

      // Render states. Every modeled field is either emitted exactly or the
      // artifact fails; invalid enum values are never coerced to a default.
      if (!emit_render_state(sb, &pass->state, 3)) {
        fprintf(stderr,
                "[shaderlab] invalid serialized render state: shader=%s "
                "subshader=%d pass=%d\n",
                shader->name, i, j);
        set_candidate_failure(candidate_diagnostic,
                              SHADERLAB_CANDIDATE_RENDER_STATE_FAILED, -1, i,
                              j);
        return false;
      }

      ShaderLabPassTarget target;
      ShaderLabTargetDiagnostic target_diagnostic;
      memset(&target, 0, sizeof(target));
      memset(&target_diagnostic, 0, sizeof(target_diagnostic));
      if (require_complete_stages) {
        const ShaderLabTargetStatus target_status =
            shaderlab_pass_target_resolve(
                pass, blob_entries, entry_count, segments, segment_lengths,
                segment_count, &target, &target_diagnostic);
        if (target_status != SHADERLAB_TARGET_OK) {
          set_candidate_target_failure(candidate_diagnostic, i, j,
                                       &target_diagnostic);
          report_target_failure(&target_diagnostic);
          return false;
        }
      }

      ShaderLabVariantPlan pass_variant_plan;
      shaderlab_variant_plan_init(&pass_variant_plan);
      if (require_complete_stages) {
        ShaderLabVariantPlanDiagnostic plan_diagnostic;
        if (shaderlab_variant_plan_build(
                shader, pass, &pass_variant_plan, &plan_diagnostic) !=
            SHADERLAB_VARIANT_PLAN_OK) {
          ShaderLabStageDiagnostic stage_diagnostic;
          stage_diagnostic_from_variant_plan(&stage_diagnostic,
                                             &plan_diagnostic);
          set_candidate_stage_failure(candidate_diagnostic, i, j,
                                      &stage_diagnostic);
          report_candidate_stage_failure(&stage_diagnostic);
          return false;
        }
      }

      // Emit HLSL body inside CGPROGRAM block
      append_indent(sb, 3);
      sb_append(sb, "HLSLPROGRAM\n");
      append_indent(sb, 3);
      sb_append(
          sb,
          "#if !defined(VERTEX) && !defined(FRAGMENT) && "
          "!defined(GEOMETRY) && !defined(HULL) && !defined(DOMAIN) && "
          "!defined(SHADER_STAGE_VERTEX) && !defined(SHADER_STAGE_FRAGMENT) "
          "&& !defined(SHADER_STAGE_GEOMETRY) && "
          "!defined(SHADER_STAGE_HULL) && !defined(SHADER_STAGE_DOMAIN)\n");
      append_indent(sb, 3);
      sb_append(sb, "#define VERTEX 1\n");
      append_indent(sb, 3);
      sb_append(sb, "#define FRAGMENT 1\n");
      append_indent(sb, 3);
      sb_append(sb, "#endif\n\n");
      append_indent(sb, 3);
      sb_append(sb, "#pragma vertex vert\n");
      append_indent(sb, 3);
      sb_append(sb, "#pragma fragment frag\n");
      if (pass_has_platform_stage(pass, 2, 4)) {
        append_indent(sb, 3);
        sb_append(sb, "#pragma geometry geom\n");
      }
      if (pass_has_platform_stage(pass, 3, 4)) {
        append_indent(sb, 3);
        sb_append(sb, "#pragma hull hs\n");
      }
      if (pass_has_platform_stage(pass, 4, 4)) {
        append_indent(sb, 3);
        sb_append(sb, "#pragma domain ds\n");
      }

      if (require_complete_stages) {
        const char *target_name =
            shaderlab_target_version_name(target.version);
        if (!target_name) {
          set_candidate_target_failure(candidate_diagnostic, i, j,
                                       &target_diagnostic);
          shaderlab_variant_plan_free(&pass_variant_plan);
          return false;
        }
        append_indent(sb, 3);
        sb_append(sb, "#pragma target ");
        sb_append(sb, target_name);
        sb_append_char(sb, '\n');
        if (!shaderlab_requirements_emit_pragmas(
                sb, target.version, target.common_requirements, 3)) {
          target_diagnostic.status =
              SHADERLAB_TARGET_UNREPRESENTABLE_REQUIREMENTS;
          target_diagnostic.shader_requirements = target.common_requirements;
          set_candidate_target_failure(candidate_diagnostic, i, j,
                                       &target_diagnostic);
          shaderlab_variant_plan_free(&pass_variant_plan);
          return false;
        }
        if (!shaderlab_variant_plan_emit_pragmas(
                &pass_variant_plan, sb, 3)) {
          ShaderLabStageDiagnostic stage_diagnostic;
          ShaderLabVariantPlanDiagnostic plan_diagnostic = {
              SHADERLAB_VARIANT_PLAN_OUTPUT_FAILED, -1, -1, -1, -1};
          stage_diagnostic_from_variant_plan(&stage_diagnostic,
                                             &plan_diagnostic);
          set_candidate_stage_failure(candidate_diagnostic, i, j,
                                      &stage_diagnostic);
          shaderlab_variant_plan_free(&pass_variant_plan);
          return false;
        }
      }
      if (!require_complete_stages && pass->has_instancing_variant) {
        append_indent(sb, 3);
        sb_append(sb, "#pragma multi_compile_instancing\n");
      }
      if (pass->has_procedural_instancing_variant) {
        append_indent(sb, 3);
        sb_append(sb,
                  "#pragma instancing_options procedural:dxbc_procedural_setup\n");
      }

      sb_append(sb,
                "            #define UNIVERSAL_SHADER_VARIABLES_INCLUDED\n");
      sb_append(sb,
                "            #include \"UnityShaderVariables.cginc\"\n\n");
      if (pass->has_procedural_instancing_variant) {
        append_indent(sb, 3);
        sb_append(sb, "void dxbc_procedural_setup() {}\n\n");
      }

      // Vertex Shader
      append_indent(sb, 3);
      sb_append(sb, "#if defined(VERTEX) || defined(SHADER_STAGE_VERTEX)\n");
      if (require_complete_stages) {
        ShaderLabStageDiagnostic diagnostic;
        if (!emit_stage_hlsl_with_variant_plan(
                &pass_variant_plan, 0, blob_entries, entry_count, segments,
                segment_lengths, segment_count, sb, &diagnostic)) {
          set_candidate_stage_failure(candidate_diagnostic, i, j,
                                      &diagnostic);
          report_candidate_stage_failure(&diagnostic);
          shaderlab_variant_plan_free(&pass_variant_plan);
          return false;
        }
      } else {
        emit_raw_verifier_stage_source(vertex_hlsl, 0, sb);
      }
      append_indent(sb, 3);
      sb_append(sb, "#endif\n\n");

      // Hull Shader. Only complete source-backed inverses are emitted; every
      // other hull contract fails inside the stage emitter.
      if (pass_has_platform_stage(pass, 3, 4)) {
        append_indent(sb, 3);
        sb_append(sb, "#if defined(HULL) || defined(SHADER_STAGE_HULL)\n");
        if (require_complete_stages) {
          ShaderLabStageDiagnostic diagnostic;
          if (!emit_stage_hlsl_with_variant_plan(
                  &pass_variant_plan, 3, blob_entries, entry_count, segments,
                  segment_lengths, segment_count, sb, &diagnostic)) {
            set_candidate_stage_failure(candidate_diagnostic, i, j,
                                        &diagnostic);
            report_candidate_stage_failure(&diagnostic);
            shaderlab_variant_plan_free(&pass_variant_plan);
            return false;
          }
        }
        append_indent(sb, 3);
        sb_append(sb, "#endif\n\n");
      }

      // Domain Shader. Like hull emission, this is gated by an exact decoded
      // stage-contract inverse rather than a generic placeholder.
      if (pass_has_platform_stage(pass, 4, 4)) {
        append_indent(sb, 3);
        sb_append(sb, "#if defined(DOMAIN) || defined(SHADER_STAGE_DOMAIN)\n");
        if (require_complete_stages) {
          ShaderLabStageDiagnostic diagnostic;
          if (!emit_stage_hlsl_with_variant_plan(
                  &pass_variant_plan, 4, blob_entries, entry_count, segments,
                  segment_lengths, segment_count, sb, &diagnostic)) {
            set_candidate_stage_failure(candidate_diagnostic, i, j,
                                        &diagnostic);
            report_candidate_stage_failure(&diagnostic);
            shaderlab_variant_plan_free(&pass_variant_plan);
            return false;
          }
        }
        append_indent(sb, 3);
        sb_append(sb, "#endif\n\n");
      }

      // Geometry Shader. The stage emitter itself is fail-closed and accepts
      // only the byte-verified implicit-stream geometry subset.
      if (pass_has_platform_stage(pass, 2, 4)) {
        append_indent(sb, 3);
        sb_append(sb,
                  "#if defined(GEOMETRY) || defined(SHADER_STAGE_GEOMETRY)\n");
        if (require_complete_stages) {
          ShaderLabStageDiagnostic diagnostic;
          if (!emit_stage_hlsl_with_variant_plan(
                  &pass_variant_plan, 2, blob_entries, entry_count, segments,
                  segment_lengths, segment_count, sb, &diagnostic)) {
            set_candidate_stage_failure(candidate_diagnostic, i, j,
                                        &diagnostic);
            report_candidate_stage_failure(&diagnostic);
            shaderlab_variant_plan_free(&pass_variant_plan);
            return false;
          }
        }
        append_indent(sb, 3);
        sb_append(sb, "#endif\n\n");
      }

      // Fragment Shader
      append_indent(sb, 3);
      sb_append(sb,
                "#if defined(FRAGMENT) || defined(SHADER_STAGE_FRAGMENT)\n");
      if (require_complete_stages) {
        ShaderLabStageDiagnostic diagnostic;
        if (!emit_stage_hlsl_with_variant_plan(
                &pass_variant_plan, 1, blob_entries, entry_count, segments,
                segment_lengths, segment_count, sb, &diagnostic)) {
          set_candidate_stage_failure(candidate_diagnostic, i, j,
                                      &diagnostic);
          report_candidate_stage_failure(&diagnostic);
          shaderlab_variant_plan_free(&pass_variant_plan);
          return false;
        }
      } else {
        emit_raw_verifier_stage_source(fragment_hlsl, 1, sb);
      }
      append_indent(sb, 3);
      sb_append(sb, "#endif\n\n");

      append_indent(sb, 3);
      sb_append(sb, "ENDHLSL\n");
      append_indent(sb, 2);
      sb_append(sb, "}\n");
      shaderlab_variant_plan_free(&pass_variant_plan);
    }

    append_indent(sb, 1);
    sb_append(sb, "}\n");
  }

  // 5. Dependencies, custom editors, and fallback.
  for (int i = 0; i < shader->dependency_count; ++i) {
    const SerializedShaderDependency *dependency = &shader->dependencies[i];
    if (!has_text(dependency->from) || !has_text(dependency->to)) {
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_TRAILER_FAILED, -1, -1, -1);
      return false;
    }
    append_indent(sb, 1);
    sb_append(sb, "Dependency ");
    if (!append_shaderlab_quoted(sb, dependency->from)) return false;
    sb_append(sb, " = ");
    if (!append_shaderlab_quoted(sb, dependency->to)) return false;
    sb_append_char(sb, '\n');
  }

  if (has_text(shader->custom_editor_name)) {
    append_indent(sb, 1);
    sb_append(sb, "CustomEditor ");
    if (!append_shaderlab_quoted(sb, shader->custom_editor_name)) {
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_TRAILER_FAILED, -1, -1, -1);
      return false;
    }
    sb_append_char(sb, '\n');
  }

  for (int i = 0; i < shader->custom_editor_for_render_pipeline_count;
       ++i) {
    const SerializedCustomEditorForRenderPipeline *editor =
        &shader->custom_editors_for_render_pipelines[i];
    if (!has_text(editor->custom_editor_name) ||
        !has_text(editor->render_pipeline_type)) {
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_TRAILER_FAILED, -1, -1, -1);
      return false;
    }
    append_indent(sb, 1);
    sb_append(sb, "CustomEditorForRenderPipeline ");
    if (!append_shaderlab_quoted(sb, editor->custom_editor_name)) {
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_TRAILER_FAILED, -1, -1, -1);
      return false;
    }
    sb_append_char(sb, ' ');
    if (!append_shaderlab_quoted(sb, editor->render_pipeline_type)) {
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_TRAILER_FAILED, -1, -1, -1);
      return false;
    }
    sb_append_char(sb, '\n');
  }

  if (has_text(shader->fallback_name)) {
    append_indent(sb, 1);
    sb_append(sb, "Fallback ");
    if (!append_shaderlab_quoted(sb, shader->fallback_name)) {
      set_candidate_failure(candidate_diagnostic,
                            SHADERLAB_CANDIDATE_TRAILER_FAILED, -1, -1, -1);
      return false;
    }
    sb_append_char(sb, '\n');
  }
  /* Unity's native producer appends this independently after a named
   * fallback, so the two serialized authorities may intentionally coexist. */
  if (shader->disable_no_subshaders_message) {
    append_indent(sb, 1);
    sb_append(sb, "Fallback Off\n");
  }

  sb_append(sb, "}\n");
  if (!sb_ok(sb)) {
    set_candidate_failure(candidate_diagnostic,
                          SHADERLAB_CANDIDATE_OUTPUT_FAILED, -1, -1, -1);
    return false;
  }
  if (candidate_diagnostic)
    candidate_diagnostic->status = SHADERLAB_CANDIDATE_OK;
  return true;
}

static bool shaderlab_emit_transactional(
    const SerializedShader *shader, const char *vertex_hlsl,
    const char *fragment_hlsl, const BlobEntry *blob_entries, int entry_count,
    uint8_t **segments, const int *segment_lengths, int segment_count,
    bool require_complete_stages, StringBuilder *output,
    ShaderLabCandidateDiagnostic *candidate_diagnostic) {
  if (candidate_diagnostic) {
    memset(candidate_diagnostic, 0, sizeof(*candidate_diagnostic));
    candidate_diagnostic->status = SHADERLAB_CANDIDATE_INVALID_ARGUMENT;
    candidate_diagnostic->property_index = -1;
    candidate_diagnostic->subshader_index = -1;
    candidate_diagnostic->pass_index = -1;
    candidate_diagnostic->unsupported_stage_index = -1;
    candidate_diagnostic->target.status = SHADERLAB_TARGET_OK;
    candidate_diagnostic->target.stage_index = -1;
    candidate_diagnostic->target.subprogram_index = -1;
    candidate_diagnostic->stage.status = SHADERLAB_STAGE_OK;
    candidate_diagnostic->stage.stage_index = -1;
    candidate_diagnostic->stage.subprogram_index = -1;
    candidate_diagnostic->stage.conflicting_subprogram_index = -1;
    candidate_diagnostic->stage.variant_plan_status =
        SHADERLAB_VARIANT_PLAN_OK;
    candidate_diagnostic->stage.raw_keyword_index = -1;
    hlsl_emit_diagnostic_init(&candidate_diagnostic->stage.hlsl);
  }
  if (!output) {
    set_candidate_failure(candidate_diagnostic,
                          SHADERLAB_CANDIDATE_INVALID_ARGUMENT, -1, -1, -1);
    return false;
  }
  if (!sb_ok(output)) {
    set_candidate_failure(candidate_diagnostic,
                          SHADERLAB_CANDIDATE_OUTPUT_FAILED, -1, -1, -1);
    return false;
  }
  StringBuilder generated;
  sb_init_with_capacity(&generated, 16384);
  const bool generated_ok = shaderlab_emit_internal(
      shader, vertex_hlsl, fragment_hlsl, blob_entries, entry_count, segments,
      segment_lengths, segment_count, require_complete_stages, &generated,
      candidate_diagnostic);
  if (generated_ok) sb_append_len(output, generated.buf, generated.len);
  const bool success = generated_ok && sb_ok(output);
  if (generated_ok && !success) {
    set_candidate_failure(candidate_diagnostic,
                          SHADERLAB_CANDIDATE_OUTPUT_FAILED, -1, -1, -1);
  }
  sb_free(&generated);
  return success;
}

bool shaderlab_emit_candidate(const SerializedShader *shader,
                              const BlobEntry *blob_entries, int entry_count,
                              uint8_t **segments, const int *segment_lengths,
                              int segment_count, StringBuilder *sb) {
  return shaderlab_emit_candidate_with_diagnostic(
      shader, blob_entries, entry_count, segments, segment_lengths,
      segment_count, sb, NULL);
}

bool shaderlab_emit_candidate_with_diagnostic(
    const SerializedShader *shader, const BlobEntry *blob_entries,
    int entry_count, uint8_t **segments, const int *segment_lengths,
    int segment_count, StringBuilder *sb,
    ShaderLabCandidateDiagnostic *diagnostic) {
  return shaderlab_emit_transactional(shader, NULL, NULL, blob_entries,
                                      entry_count, segments, segment_lengths,
                                      segment_count, true, sb, diagnostic);
}

bool shaderlab_emit_raw(const SerializedShader *shader, const char *vertex_hlsl,
                        const char *fragment_hlsl, StringBuilder *sb) {
  return shaderlab_emit_transactional(shader, vertex_hlsl, fragment_hlsl, NULL,
                                      0, NULL, NULL, 0, false, sb, NULL);
}
