// SPDX-License-Identifier: GPL-3.0-only

#include "translation/compute_source_candidate.h"
#include "hlsl_source_identifier.h"
#include "hlsl_compute_source_internal.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_emitter.h"
#include "translation/usil.h"
#include "translation/usil_validation.h"
#include "translation/shaderlab_emitter_internal.h"

#include <stdlib.h>
#include <string.h>

/* Unity 2021.3's existing requirement inversion names 0x4000 "compute";
 * 0x1 is BaseShaders. A .compute entry has the compute feature implicitly.
 * The selected Editor omission control retained requirements 0x4001 and all
 * kernel containers while avoiding the redundant require-compute warnings.
 * Other missing or additional bits remain unsupported. */
static const uint64_t compute_stage_feature = UINT64_C(0x4000);
static const uint64_t required_features = UINT64_C(1) | UINT64_C(0x4000);
enum { MAX_NAME_BYTES = 255, MAX_KEY_BYTES = 2048, MAX_CODE_BYTES = 1048576,
       MAX_OBJECT_BYTES = 16 * 1048576, MAX_TOTAL_CODE_BYTES = 64 * 1048576 };

typedef struct {
    HLSLSourceQualityFacts facts[COMPUTE_SOURCE_MAX_INSTRUCTIONS + 8];
    size_t count;
    ComputeSourceVariant *evidence;
} EntryEvents;


static bool borrowed_span(const ComputeShaderObject *object, const void *bytes, size_t size) {
    const uintptr_t start = (uintptr_t)object->serialized_object_bytes;
    const uintptr_t pointer = (uintptr_t)bytes;
    if (!bytes) return size == 0;
    return pointer >= start && pointer - start <= object->serialized_object_size &&
           size <= object->serialized_object_size - (pointer - start);
}

static ComputeSourceStatus copy_view(const ComputeShaderObject *object,
    ComputeShaderStringView view, size_t limit, char **destination,
    ComputeSourceStatus invalid_status) {
    if (view.size > limit || !borrowed_span(object, view.bytes, view.size) ||
        (view.size && memchr(view.bytes, 0, view.size))) return invalid_status;
    char *copy = malloc(view.size + 1);
    if (!copy) return COMPUTE_SOURCE_ALLOCATION_FAILED;
    if (view.size) memcpy(copy, view.bytes, view.size);
    copy[view.size] = '\0';
    *destination = copy;
    return COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
}

static void hash_number(CommonSha256Context *hash, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned byte = 0; byte < 8; ++byte) bytes[byte] = (uint8_t)(value >> (byte * 8u));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

static void hash_view(CommonSha256Context *hash, ComputeShaderStringView view) {
    hash_number(hash, view.size);
    common_sha256_update(hash, view.bytes, view.size);
}

static bool equal_view(ComputeShaderStringView view, const char *string) {
    return view.size == strlen(string) && (!view.size || memcmp(view.bytes, string, view.size) == 0);
}

static bool keyword_mask(const ComputeSourceCandidate *candidate, const char *key, uint64_t *mask) {
    *mask = 0;
    if (!*key) return true;
    const size_t count = candidate->global_keyword_count + candidate->local_keyword_count;
    const char *start = key;
    while (*start) {
        const char *end = strchr(start, ' ');
        const size_t length = end ? (size_t)(end - start) : strlen(start);
        if (!length) return false;
        size_t match = count;
        for (size_t index = 0; index < count; ++index) {
            if (strlen(candidate->keywords[index]) == length &&
                memcmp(candidate->keywords[index], start, length) == 0) { match = index; break; }
        }
        if (match == count || (*mask & (UINT64_C(1) << match))) return false;
        *mask |= UINT64_C(1) << match;
        if (!end) return true;
        start = end + 1;
        if (!*start) return false;
    }
    return true;
}

void compute_source_candidate_init(ComputeSourceCandidate *candidate) {
    if (!candidate) return;
    memset(candidate, 0, sizeof(*candidate));
    sb_init(&candidate->source);
    candidate->status = COMPUTE_SOURCE_LAYOUT_UNAVAILABLE;
    candidate->source_quality.stage = DXBC_PROGRAM_TYPE_COMPUTE;
    candidate->source_quality.emission_status = HLSL_EMIT_STATUS_UNSUPPORTED;
    candidate->source_quality.classification = HLSL_SOURCE_QUALITY_UNSUPPORTED;
    candidate->source_quality.reasons = HLSL_SOURCE_QUALITY_REASON_EMISSION_UNSUPPORTED;
}

void compute_source_candidate_dispose(ComputeSourceCandidate *candidate) {
    if (!candidate) return;
    for (size_t index = 0; candidate->variants && index < candidate->variant_count; ++index) {
        free(candidate->variants[index].kernel_name);
        free(candidate->variants[index].keyword_key);
        free(candidate->variants[index].emission_facts);
        for (size_t expression = 0; expression < candidate->variants[index].expression_count; ++expression)
            ast_free_expr(candidate->variants[index].expressions[expression]);
        free(candidate->variants[index].expressions);
    }
    free(candidate->variants);
    for (size_t index = 0; candidate->keywords &&
         index < candidate->global_keyword_count + candidate->local_keyword_count; ++index)
        free(candidate->keywords[index]);
    free(candidate->keywords);
    for (size_t resource = 0; resource < candidate->resource_count; ++resource) {
        free(candidate->resources[resource].name);
        free(candidate->resources[resource].variant_witnesses);
    }
    sb_free(&candidate->source);
    compute_source_candidate_init(candidate);
}

static bool collect_entry_events(void *context, const HLSLSourceQualityObservation *observation) {
    EntryEvents *events = context;
    if (observation->reasons) return false;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EXPRESSION) return true;
    if (observation->kind != HLSL_SOURCE_OBSERVATION_EMISSION ||
        events->count == sizeof(events->facts) / sizeof(events->facts[0])) return false;
    events->facts[events->count++] = observation->facts;
    return true;
}

static bool retain_entry_expression(void *context, ASTExpr *expression) {
    EntryEvents *events = context;
    ComputeSourceVariant *evidence = events->evidence;
    if (!evidence || evidence->expression_count >= 3u) return false;
    if (!evidence->expressions) {
        evidence->expressions = calloc(3u, sizeof(*evidence->expressions));
        if (!evidence->expressions) return false;
    }
    evidence->expressions[evidence->expression_count++] = expression;
    return true;
}

static void hash_resource(CommonSha256Context *hash, const ComputeShaderResource *resource) {
    hash_view(hash, resource->name);
    hash_view(hash, resource->generated_name);
    hash_number(hash, (uint64_t)resource->bind_point);
    hash_number(hash, (uint64_t)resource->sampler_bind_point);
    hash_number(hash, (uint64_t)resource->texture_dimension);
}

static ComputeSourceStatus resource_binding(const ComputeShaderObject *object,
    const ComputeShaderResource *resource, bool writable, bool structured, ComputeSourceCandidate *candidate,
    HLSLComputeTypedResource *binding, uint32_t variant_row) {
    if (resource->generated_name.size || !borrowed_span(object, resource->generated_name.bytes, resource->generated_name.size) ||
        resource->bind_point < 0 || resource->bind_point >= (writable ? 8 : 128) ||
        resource->sampler_bind_point != -1 || resource->texture_dimension != (structured ? -1 : 2))
        return COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE;
    char *name = NULL;
    ComputeSourceStatus status = copy_view(object, resource->name, MAX_NAME_BYTES,
                                           &name, COMPUTE_SOURCE_NAME_UNREPRESENTABLE);
    if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) return status;
    if (!hlsl_source_identifier_valid(name) || !strcmp(name, "dispatchThreadId")) {
        free(name); return COMPUTE_SOURCE_NAME_UNREPRESENTABLE;
    }
    for (size_t keyword = 0; keyword < candidate->global_keyword_count + candidate->local_keyword_count; ++keyword)
        if (!strcmp(name, candidate->keywords[keyword])) { free(name); return COMPUTE_SOURCE_NAME_UNREPRESENTABLE; }
    for (size_t index = 0; index < candidate->resource_count; ++index) {
        ComputeSourceTypedResource *existing = &candidate->resources[index];
        const bool same_name = !strcmp(existing->name, name);
        const bool same_binding = existing->writable == writable && existing->binding_register == (uint32_t)resource->bind_point;
        if (same_name || same_binding) {
            free(name);
            if (!same_name || !same_binding ||
                existing->kind != (structured ? COMPUTE_SOURCE_STRUCTURED_UINT4_BITS : COMPUTE_SOURCE_TEXTURE2D_UINT4))
                return COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE;
            if (existing->witness_count >= candidate->variant_count ||
                (existing->witness_count && existing->variant_witnesses[existing->witness_count - 1] >= variant_row))
                return COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE;
            existing->variant_witnesses[existing->witness_count++] = variant_row;
            *binding = (HLSLComputeTypedResource){existing->name, existing->binding_register, existing->writable, structured};
            return COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
        }
    }
    if (candidate->resource_count == COMPUTE_SOURCE_MAX_TYPED_RESOURCES) {
        free(name); return COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE;
    }
    ComputeSourceTypedResource *owned = &candidate->resources[candidate->resource_count++];
    *owned = (ComputeSourceTypedResource){.name = name, .binding_register = (uint32_t)resource->bind_point,
        .writable = writable, .kind = structured ? COMPUTE_SOURCE_STRUCTURED_UINT4_BITS : COMPUTE_SOURCE_TEXTURE2D_UINT4,
        .byte_stride = structured ? 16u : 0u, .original_element_type_known = !structured};
    owned->variant_witnesses = calloc(candidate->variant_count, sizeof(*owned->variant_witnesses));
    if (!owned->variant_witnesses) return COMPUTE_SOURCE_ALLOCATION_FAILED;
    owned->variant_witnesses[owned->witness_count++] = variant_row;
    *binding = (HLSLComputeTypedResource){owned->name, owned->binding_register, owned->writable, structured};
    return COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
}

static ComputeSourceStatus resource_inventory(const ComputeShaderObject *object,
    const ComputeShaderKernelVariant *variant, ComputeSourceCandidate *candidate,
    HLSLComputeTypedResource bindings[COMPUTE_SOURCE_MAX_TYPED_RESOURCES], size_t *binding_count, uint32_t variant_row) {
    *binding_count = 0;
    if (variant->constant_buffer_variant_index_count || variant->constant_buffer_count ||
        variant->builtin_sampler_count || variant->input_buffer_count > 1 || variant->texture_count > 1 ||
        (variant->input_buffer_count && (!variant->input_buffers || variant->texture_count || !variant->output_buffer_count)) ||
        variant->output_buffer_count > 1 || (variant->texture_count && !variant->textures) ||
        (variant->output_buffer_count && !variant->output_buffers) ||
        (variant->texture_count && !variant->output_buffer_count))
        return COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE;
    if (variant->texture_count) {
        ComputeSourceStatus status = resource_binding(object, &variant->textures[0], false, false, candidate,
                                                     &bindings[(*binding_count)++], variant_row);
        if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) return status;
    }
    if (variant->input_buffer_count) {
        ComputeSourceStatus status = resource_binding(object, &variant->input_buffers[0], false, true, candidate,
                                                     &bindings[(*binding_count)++], variant_row);
        if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) return status;
    }
    if (variant->output_buffer_count) {
        ComputeSourceStatus status = resource_binding(object, &variant->output_buffers[0], true, variant->input_buffer_count != 0, candidate,
                                                     &bindings[(*binding_count)++], variant_row);
        if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) return status;
    }
    return COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
}

static bool configuration_event(HLSLSourceQualityAnalysis *analysis) {
    HLSLSourceQualityFacts fact;
    hlsl_source_quality_facts_init(&fact);
    fact.known = true;
    return hlsl_source_quality_analysis_emission(analysis, &fact);
}

static bool append_condition(StringBuilder *source, const ComputeSourceCandidate *candidate,
                              uint64_t mask) {
    const size_t count = candidate->global_keyword_count + candidate->local_keyword_count;
    if (!count) return true;
    sb_append(source, "#if ");
    for (size_t index = 0; index < count; ++index)
        sb_appendf(source, "%s%sdefined(%s)", index ? " && " : "",
                   (mask & (UINT64_C(1) << index)) ? "" : "!", candidate->keywords[index]);
    sb_append(source, "\n");
    return sb_ok(source);
}

static ComputeSourceStatus emit_variant(const ComputeShaderObject *object,
    const ComputeShaderKernelVariant *variant, ComputeSourceCandidate *candidate,
    ComputeSourceVariant *evidence, StringBuilder *entry, ComputeSourceDiagnostic *diagnostic) {
    HLSLComputeTypedResource bindings[COMPUTE_SOURCE_MAX_TYPED_RESOURCES];
    size_t binding_count;
    ComputeSourceStatus inventory_status = resource_inventory(object, variant, candidate, bindings, &binding_count, evidence->source_unit_id - 1u);
    if (inventory_status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) return inventory_status;
    if (variant->requirements < 0 || (uint64_t)variant->requirements != required_features)
        return COMPUTE_SOURCE_REQUIREMENTS_UNSUPPORTED;
    if (!variant->code_size || variant->code_size > MAX_CODE_BYTES ||
        !borrowed_span(object, variant->code, variant->code_size)) return COMPUTE_SOURCE_DXBC_UNAVAILABLE;
    if (variant->thread_group_size_count != 3 || !variant->thread_group_size)
        return COMPUTE_SOURCE_THREAD_GROUP_MISMATCH;
    DXBCContainerView view;
    if (!dxbc_container_view_first(variant->code, variant->code_size, &view))
        return COMPUTE_SOURCE_DXBC_UNAVAILABLE;
    DXBCDocument document;
    DXBCStageContract contract;
    DXBCContainer semantic = {0};
    USILProgram program = {0};
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    ComputeSourceStatus status = COMPUTE_SOURCE_DXBC_UNAVAILABLE;
    if (!dxbc_document_parse(&document, view.data, view.size, &diagnostic->document) ||
        !dxbc_document_decode_semantic(&document, &semantic)) goto cleanup;
    status = COMPUTE_SOURCE_STAGE_CONTRACT_FAILED;
    if (!dxbc_stage_contract_decode(&document, &semantic, &contract, &diagnostic->stage_contract) ||
        contract.program_type != DXBC_PROGRAM_TYPE_COMPUTE || contract.shader_model_major != 5 ||
        contract.shader_model_minor != 0 || !usil_translate_with_stage_contract(&program, &semantic, &contract))
        goto cleanup;
    status = COMPUTE_SOURCE_THREAD_GROUP_MISMATCH;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (variant->thread_group_size[axis] != contract.thread_group_size[axis]) goto cleanup;
    status = COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE;
    if (program.compute.shared_memory_count || program.cbuffer_count || program.sampler_count ||
        (size_t)program.texture_count != variant->texture_count + variant->input_buffer_count ||
        (size_t)program.uav_count != variant->output_buffer_count || program.icb_value_count ||
        program.indexable_temp_count || program.index_range_count) goto cleanup;
    status = COMPUTE_SOURCE_BODY_UNSUPPORTED;
    if (program.instruction_count <= 0 || program.instruction_count > COMPUTE_SOURCE_MAX_INSTRUCTIONS)
        goto cleanup;
    for (int index = 0; index < program.instruction_count; ++index)
        if (!binding_count && program.instructions[index].opcode != USIL_OP_RET &&
            program.instructions[index].opcode != USIL_OP_SYNC) goto cleanup;
    EntryEvents events = {.evidence = evidence};
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.reserved_preprocessor_identifiers = (const char *const *)candidate->keywords;
    options.reserved_preprocessor_identifier_count = candidate->global_keyword_count + candidate->local_keyword_count;
    options.source_quality = &evidence->entry_quality;
    options.source_quality_pass_index = (uint32_t)evidence->kernel_index;
    options.source_quality_entry_point_index = (uint32_t)evidence->variant_index;
    options.source_quality_observer = collect_entry_events;
    options.source_quality_observer_context = &events;
    const HLSLEmitNames names = {.entry_point = evidence->kernel_name};
    status = COMPUTE_SOURCE_EMISSION_FAILED;
    const HLSLComputeTypedSource typed = {.resources = bindings, .resource_count = binding_count,
        .retain_expression = retain_entry_expression, .expression_context = &events};
    const bool emitted = binding_count
        ? hlsl_emit_compute_typed_stage(&program, entry, &names, &options, &typed, &diagnostic->emission)
        : hlsl_emit_with_options_diagnostic(&program, entry, NULL, NULL, &names, &options, &diagnostic->emission);
    if (!emitted) goto cleanup;
    if (binding_count) {
        for (int index = 0; index < program.instruction_count; ++index) {
            const USILInstruction *instruction = &program.instructions[index];
            if (instruction->opcode != USIL_OP_LD && instruction->opcode != USIL_OP_LD_UAV_TYPED &&
                instruction->opcode != USIL_OP_STORE_UAV_TYPED &&
                instruction->opcode != USIL_OP_LD_STRUCTURED && instruction->opcode != USIL_OP_STORE_STRUCTURED) continue;
            USILEffectFlags effects;
            if (evidence->memory_effect_count >= 2u || !usil_instruction_effects(&program, instruction, &effects)) goto cleanup;
            int binding_operand = instruction->opcode == USIL_OP_LD ? 2 : 0;
            if (instruction->opcode == USIL_OP_LD_UAV_TYPED || instruction->opcode == USIL_OP_LD_STRUCTURED ||
                instruction->opcode == USIL_OP_STORE_STRUCTURED) {
                USILMemoryAccess memory;
                if (!usil_instruction_memory_access(&program, instruction, &memory)) goto cleanup;
                binding_operand = memory.binding_operand;
            }
            evidence->memory_effects[evidence->memory_effect_count++] = (ComputeSourceMemoryEffect){
                .opcode = instruction->opcode, .effect_flags = (uint32_t)effects, .instruction_index = index,
                .source_instruction_index = instruction->source_instruction_index,
                .binding_register = (uint32_t)instruction->operands[binding_operand].register_index};
        }
    }
    status = COMPUTE_SOURCE_QUALITY_FAILED;
    if (evidence->entry_quality.classification != HLSL_SOURCE_QUALITY_CLEAN || !events.count)
        goto cleanup;
    status = COMPUTE_SOURCE_ALLOCATION_FAILED;
    evidence->emission_facts = malloc(events.count * sizeof(*evidence->emission_facts));
    if (!evidence->emission_facts) goto cleanup;
    memcpy(evidence->emission_facts, events.facts, events.count * sizeof(*events.facts));
    evidence->emission_fact_count = events.count;
    evidence->requirements = (uint64_t)variant->requirements;
    memcpy(evidence->thread_group_size, variant->thread_group_size, sizeof(evidence->thread_group_size));
    common_sha256(variant->code, variant->code_size, evidence->serialized_program_sha256);
    common_sha256(view.data, view.size, evidence->dxbc_sha256);
    common_sha256(entry->buf, entry->len, evidence->source_sha256);
    status = COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
cleanup:
    usil_free(&program);
    dxbc_free(&semantic);
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    return status;
}

static ComputeSourceStatus keyword_universe(const ComputeShaderObject *object,
    const ComputeShaderKernelParent *kernel, ComputeSourceCandidate *candidate) {
    if (kernel->global_keyword_count > COMPUTE_SOURCE_MAX_KEYWORDS ||
        kernel->local_keyword_count > COMPUTE_SOURCE_MAX_KEYWORDS - kernel->global_keyword_count)
        return COMPUTE_SOURCE_LIMIT_EXCEEDED;
    if ((kernel->global_keyword_count && !kernel->global_keywords) ||
        (kernel->local_keyword_count && !kernel->local_keywords)) return COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE;
    const size_t count = kernel->global_keyword_count + kernel->local_keyword_count;
    candidate->keywords = count ? calloc(count, sizeof(*candidate->keywords)) : NULL;
    if (count && !candidate->keywords) return COMPUTE_SOURCE_ALLOCATION_FAILED;
    candidate->global_keyword_count = kernel->global_keyword_count;
    candidate->local_keyword_count = kernel->local_keyword_count;
    for (size_t index = 0; index < count; ++index) {
        const ComputeShaderStringView view = index < kernel->global_keyword_count
            ? kernel->global_keywords[index] : kernel->local_keywords[index - kernel->global_keyword_count];
        ComputeSourceStatus status = copy_view(object, view, MAX_NAME_BYTES,
            &candidate->keywords[index], COMPUTE_SOURCE_NAME_UNREPRESENTABLE);
        if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) return status;
        if (!hlsl_source_identifier_valid(candidate->keywords[index])) return COMPUTE_SOURCE_NAME_UNREPRESENTABLE;
        for (size_t prior = 0; prior < index; ++prior)
            if (strcmp(candidate->keywords[prior], candidate->keywords[index]) == 0)
                return COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE;
    }
    return COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
}

static bool kernel_universe_equal(const ComputeShaderObject *object,
    const ComputeShaderKernelParent *kernel, const ComputeSourceCandidate *candidate) {
    if (kernel->global_keyword_count != candidate->global_keyword_count ||
        kernel->local_keyword_count != candidate->local_keyword_count ||
        (kernel->global_keyword_count && !kernel->global_keywords) ||
        (kernel->local_keyword_count && !kernel->local_keywords)) return false;
    for (size_t index = 0; index < kernel->global_keyword_count + kernel->local_keyword_count; ++index) {
        const ComputeShaderStringView view = index < kernel->global_keyword_count
            ? kernel->global_keywords[index] : kernel->local_keywords[index - kernel->global_keyword_count];
        if (!borrowed_span(object, view.bytes, view.size) || !equal_view(view, candidate->keywords[index])) return false;
    }
    return true;
}

static ComputeSourceStatus requested_inventory(const ComputeShaderObject *object,
    ComputeSourceDiagnostic *diagnostic) {
    enum { MAX_INVENTORY_PLATFORMS = 16 };
    if (object->platform_count > MAX_INVENTORY_PLATFORMS) return COMPUTE_SOURCE_LIMIT_EXCEEDED;
    if (object->platform_count && !object->platforms) return COMPUTE_SOURCE_INVALID_ARGUMENT;
    for (size_t platform_index = 0; platform_index < object->platform_count; ++platform_index) {
        const ComputeShaderPlatformVariant *platform = &object->platforms[platform_index];
        diagnostic->platform_index = platform_index;
        if (platform->kernel_count > COMPUTE_SOURCE_MAX_KERNELS) return COMPUTE_SOURCE_LIMIT_EXCEEDED;
        if (platform->kernel_count && !platform->kernels) return COMPUTE_SOURCE_INVALID_ARGUMENT;
        diagnostic->requested_kernels += platform->kernel_count;
        for (size_t kernel_index = 0; kernel_index < platform->kernel_count; ++kernel_index) {
            const size_t count = platform->kernels[kernel_index].variant_count;
            if (count > SIZE_MAX - diagnostic->requested_variants) return COMPUTE_SOURCE_LIMIT_EXCEEDED;
            diagnostic->requested_variants += count;
        }
    }
    diagnostic->requested_counts_known = true;
    if (diagnostic->requested_kernels > COMPUTE_SOURCE_MAX_KERNELS ||
        diagnostic->requested_variants > COMPUTE_SOURCE_MAX_VARIANTS) return COMPUTE_SOURCE_LIMIT_EXCEEDED;
    return COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
}

ComputeSourceStatus compute_source_candidate_build(const ComputeShaderObject *object,
    ComputeSourceCandidate *destination, ComputeSourceDiagnostic *optional_diagnostic) {
    ComputeSourceDiagnostic local;
    ComputeSourceDiagnostic *diagnostic = optional_diagnostic ? optional_diagnostic : &local;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = COMPUTE_SOURCE_INVALID_ARGUMENT;
    diagnostic->platform_index = diagnostic->kernel_index = diagnostic->variant_index = SIZE_MAX;
    if (!object || !destination) return diagnostic->status;
    ComputeSourceCandidate candidate;
    compute_source_candidate_init(&candidate);
    HLSLSourceQualityAnalysis *quality = NULL;
    StringBuilder *entries = NULL;
    CommonSha256Context modeled;
    common_sha256_init(&modeled);
    static const char identity[] = "DXBCSandbox.ComputeSourceCandidate.ModeledInput.v2";
    common_sha256_update(&modeled, identity, sizeof(identity));
    ComputeSourceStatus status = COMPUTE_SOURCE_LAYOUT_UNAVAILABLE;
    if ((object->unity_version.size && !object->unity_version.bytes) ||
        object->unity_version.size > MAX_NAME_BYTES ||
        !compute_shader_object_has_layout_authority(object) || object->serialized_object_size > MAX_OBJECT_BYTES)
        goto cleanup;
    status = requested_inventory(object, diagnostic);
    if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) goto cleanup;
    status = COMPUTE_SOURCE_PLATFORM_UNSUPPORTED;
    if (object->target_platform != 19 || object->platform_count != 1) goto cleanup;
    const ComputeShaderPlatformVariant *platform = &object->platforms[0];
    diagnostic->platform_index = 0;
    status = COMPUTE_SOURCE_LIMIT_EXCEEDED;
    if (!platform->kernel_count || !diagnostic->requested_variants) goto cleanup;
    status = COMPUTE_SOURCE_PLATFORM_UNSUPPORTED;
    /* This is the Class72 renderer discriminator, not compiler-platform4. */
    if (platform->target_renderer != 2 || platform->target_level != 0) goto cleanup;
    status = COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE;
    if (!platform->resources_resolved || platform->constant_buffer_count) goto cleanup;
    candidate.kernel_count = platform->kernel_count;
    candidate.variant_count = diagnostic->requested_variants;
    candidate.variants = calloc(candidate.variant_count, sizeof(*candidate.variants));
    status = COMPUTE_SOURCE_ALLOCATION_FAILED;
    if (!candidate.variants) goto cleanup;
    entries = calloc(candidate.variant_count, sizeof(*entries));
    if (!entries) goto cleanup;
    for (size_t index = 0; index < candidate.variant_count; ++index) sb_init(&entries[index]);
    status = keyword_universe(object, &platform->kernels[0], &candidate);
    if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) goto cleanup;
    const size_t keyword_count = candidate.global_keyword_count + candidate.local_keyword_count;
    const size_t states = (size_t)1 << keyword_count;
    common_sha256(object->serialized_object_bytes, object->serialized_object_size, candidate.serialized_object_sha256);
    common_sha256_update(&modeled, candidate.serialized_object_sha256, sizeof(candidate.serialized_object_sha256));
    hash_number(&modeled, object->serialized_object_size);
    hash_number(&modeled, object->serialized_file_version);
    hash_number(&modeled, object->target_platform);
    hash_number(&modeled, (uint64_t)object->path_id);
    hash_view(&modeled, object->unity_version);
    if (!borrowed_span(object, object->name.bytes, object->name.size)) { status = COMPUTE_SOURCE_LAYOUT_UNAVAILABLE; goto cleanup; }
    hash_view(&modeled, object->name);
    common_sha256_update(&modeled, object->serialized_type_hash, sizeof(object->serialized_type_hash));
    hash_number(&modeled, object->platform_count);
    hash_number(&modeled, (uint64_t)platform->target_renderer);
    hash_number(&modeled, (uint64_t)platform->target_level);
    hash_number(&modeled, platform->resources_resolved);
    hash_number(&modeled, platform->constant_buffer_count);
    hash_number(&modeled, platform->kernel_count);
    const HLSLSourceQualityRequest request = {
        .stage = DXBC_PROGRAM_TYPE_COMPUTE, .entry_point_index = UINT32_MAX,
        .emission_status = HLSL_EMIT_STATUS_OK, .expected_entry_point_count = candidate.variant_count,
        .expression_facts = hlsl_source_quality_owned_expression_facts};
    quality = hlsl_source_quality_analysis_create(&request, &candidate.source_quality);
    status = COMPUTE_SOURCE_QUALITY_FAILED;
    if (!quality || !hlsl_source_quality_analysis_begin_unit(quality, 0, HLSL_SOURCE_UNIT_CONFIGURATION, true)) goto cleanup;
    char *kernel_names[COMPUTE_SOURCE_MAX_KERNELS] = {0}; /* Borrow owned evidence names. */
    size_t row = 0, total_code_bytes = 0, total_entry_bytes = 0;
    for (size_t kernel_index = 0; kernel_index < platform->kernel_count; ++kernel_index) {
        const ComputeShaderKernelParent *kernel = &platform->kernels[kernel_index];
        diagnostic->kernel_index = kernel_index;
        diagnostic->variant_index = SIZE_MAX;
        status = COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE;
        if (!kernel_universe_equal(object, kernel, &candidate) || kernel->variant_count != states || !kernel->variants) goto cleanup;
        status = COMPUTE_SOURCE_NAME_UNREPRESENTABLE;
        if (kernel->name.size > MAX_NAME_BYTES ||
            !borrowed_span(object, kernel->name.bytes, kernel->name.size) ||
            (kernel->name.size && memchr(kernel->name.bytes, 0, kernel->name.size))) goto cleanup;
        hash_view(&modeled, kernel->name);
        hash_number(&modeled, kernel->global_keyword_count);
        for (size_t index = 0; index < kernel->global_keyword_count; ++index) hash_view(&modeled, kernel->global_keywords[index]);
        hash_number(&modeled, kernel->local_keyword_count);
        for (size_t index = 0; index < kernel->local_keyword_count; ++index) hash_view(&modeled, kernel->local_keywords[index]);
        hash_number(&modeled, kernel->variant_count);
        bool seen[(size_t)1 << COMPUTE_SOURCE_MAX_KEYWORDS] = {false};
        for (size_t variant_index = 0; variant_index < kernel->variant_count; ++variant_index, ++row) {
            const ComputeShaderKernelVariant *variant = &kernel->variants[variant_index];
            ComputeSourceVariant *evidence = &candidate.variants[row];
            diagnostic->variant_index = variant_index;
            ++diagnostic->examined_variants;
            evidence->platform_index = 0;
            evidence->kernel_index = kernel_index;
            evidence->variant_index = variant_index;
            evidence->source_unit_id = (uint32_t)row + 1;
            status = copy_view(object, kernel->name, MAX_NAME_BYTES, &evidence->kernel_name,
                               COMPUTE_SOURCE_NAME_UNREPRESENTABLE);
            if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) goto cleanup;
            status = COMPUTE_SOURCE_NAME_UNREPRESENTABLE;
            if (!hlsl_source_identifier_valid(evidence->kernel_name)) goto cleanup;
            status = copy_view(object, variant->keyword_key, MAX_KEY_BYTES, &evidence->keyword_key,
                               COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE);
            if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) goto cleanup;
            if (!variant_index) {
                status = COMPUTE_SOURCE_NAME_UNREPRESENTABLE;
                for (size_t keyword = 0; keyword < keyword_count; ++keyword)
                    if (strcmp(candidate.keywords[keyword], evidence->kernel_name) == 0) goto cleanup;
                for (size_t prior = 0; prior < kernel_index; ++prior)
                    if (strcmp(kernel_names[prior], evidence->kernel_name) == 0) goto cleanup;
                kernel_names[kernel_index] = evidence->kernel_name;
                sb_appendf(&candidate.source, "#pragma kernel %s\n", evidence->kernel_name);
                if (!configuration_event(quality)) { status = COMPUTE_SOURCE_QUALITY_FAILED; goto cleanup; }
            }
            status = COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE;
            if (!evidence->keyword_key || !keyword_mask(&candidate, evidence->keyword_key, &evidence->keyword_mask) ||
                seen[evidence->keyword_mask]) goto cleanup;
            seen[evidence->keyword_mask] = true;
            status = COMPUTE_SOURCE_LIMIT_EXCEEDED;
            if (variant->code_size > MAX_TOTAL_CODE_BYTES - total_code_bytes) goto cleanup;
            total_code_bytes += variant->code_size;
            status = emit_variant(object, variant, &candidate, evidence, &entries[row], diagnostic);
            if (status != COMPUTE_SOURCE_CANDIDATE_UNVERIFIED) goto cleanup;
            status = COMPUTE_SOURCE_LIMIT_EXCEEDED;
            if (entries[row].len > COMPUTE_SOURCE_MAX_BYTES - total_entry_bytes) goto cleanup;
            total_entry_bytes += entries[row].len;
            hash_view(&modeled, variant->keyword_key);
            hash_number(&modeled, evidence->keyword_mask);
            hash_number(&modeled, (uint64_t)variant->requirements);
            hash_number(&modeled, variant->thread_group_size_count);
            for (unsigned axis = 0; axis < 3; ++axis) hash_number(&modeled, variant->thread_group_size[axis]);
            hash_number(&modeled, variant->constant_buffer_variant_index_count);
            hash_number(&modeled, variant->constant_buffer_count);
            hash_number(&modeled, variant->texture_count);
            for (size_t resource = 0; resource < variant->texture_count; ++resource) hash_resource(&modeled, &variant->textures[resource]);
            hash_number(&modeled, variant->builtin_sampler_count);
            hash_number(&modeled, variant->input_buffer_count);
            for (size_t resource = 0; resource < variant->input_buffer_count; ++resource) hash_resource(&modeled, &variant->input_buffers[resource]);
            hash_number(&modeled, variant->output_buffer_count);
            for (size_t resource = 0; resource < variant->output_buffer_count; ++resource) hash_resource(&modeled, &variant->output_buffers[resource]);
            hash_number(&modeled, variant->code_size);
            common_sha256_update(&modeled, evidence->serialized_program_sha256, sizeof(evidence->serialized_program_sha256));
            ++diagnostic->represented_variants;
        }
    }
    for (size_t index = 0; index < keyword_count; ++index) {
        sb_appendf(&candidate.source, "#pragma multi_compile%s _ %s\n",
                   index < candidate.global_keyword_count ? "" : "_local", candidate.keywords[index]);
        if (!configuration_event(quality)) { status = COMPUTE_SOURCE_QUALITY_FAILED; goto cleanup; }
    }
    status = COMPUTE_SOURCE_REQUIREMENTS_UNSUPPORTED;
    if (!shaderlab_requirements_emit_pragmas(&candidate.source, SHADERLAB_TARGET_2_0,
                                            required_features & ~compute_stage_feature, 0)) goto cleanup;
    if (!configuration_event(quality)) { status = COMPUTE_SOURCE_QUALITY_FAILED; goto cleanup; }
    for (size_t resource = 0; resource < candidate.resource_count; ++resource) {
        const ComputeSourceTypedResource *declaration = &candidate.resources[resource];
        for (size_t kernel = 0; kernel < candidate.kernel_count; ++kernel)
            if (!strcmp(declaration->name, kernel_names[kernel])) { status = COMPUTE_SOURCE_NAME_UNREPRESENTABLE; goto cleanup; }
        const bool structured = declaration->kind == COMPUTE_SOURCE_STRUCTURED_UINT4_BITS;
        const char *type = structured ? (declaration->writable ? "RWStructuredBuffer" : "StructuredBuffer")
                                      : (declaration->writable ? "RWTexture2D" : "Texture2D");
        sb_appendf(&candidate.source, "%s<uint4> %s;\n", type, declaration->name);
        if (!declaration->witness_count) { status = COMPUTE_SOURCE_QUALITY_FAILED; goto cleanup; }
        for (size_t witness = 0; witness < declaration->witness_count; ++witness) {
            HLSLSourceQualityFacts fact;
            hlsl_source_quality_facts_init(&fact);
            fact.known = true;
            fact.declaration_witness_record = true;
            fact.declaration_witness_count = witness == 0 ? (uint32_t)declaration->witness_count : 0;
            fact.declaration_variant_index = declaration->variant_witnesses[witness];
            fact.declaration_field_index = (uint32_t)resource;
            fact.declaration_witness_subprogram_index = 0;
            if (witness == 0) {
                fact.resource_declaration_kind = declaration->writable ? HLSL_SOURCE_RESOURCE_UAV : HLSL_SOURCE_RESOURCE_TEXTURE;
                fact.resource_binding_register = declaration->binding_register;
            }
            if (!hlsl_source_quality_analysis_emission(quality, &fact)) { status = COMPUTE_SOURCE_QUALITY_FAILED; goto cleanup; }
        }
    }
    bool structured_declarations = false;
    for (size_t resource = 0; resource < candidate.resource_count; ++resource)
        structured_declarations |= candidate.resources[resource].kind == COMPUTE_SOURCE_STRUCTURED_UINT4_BITS;
    /* Byte layout and unsigned execution are proved independently. The chosen
     * bit representation cannot supply the missing original element type. */
    if (structured_declarations && !hlsl_source_quality_analysis_mark_incomplete_unit(quality)) {
        status = COMPUTE_SOURCE_QUALITY_FAILED; goto cleanup;
    }
    sb_append(&candidate.source, "\n");
    for (size_t index = 0; index < candidate.variant_count; ++index) {
        const ComputeSourceVariant *evidence = &candidate.variants[index];
        if (keyword_count) {
            if (!append_condition(&candidate.source, &candidate, evidence->keyword_mask) || !configuration_event(quality)) {
                status = COMPUTE_SOURCE_QUALITY_FAILED; goto cleanup;
            }
            /* The source closing directive belongs to this same configuration
             * unit even though it follows the entry text in the final file. */
            if (!configuration_event(quality)) { status = COMPUTE_SOURCE_QUALITY_FAILED; goto cleanup; }
        }
        sb_append_len(&candidate.source, entries[index].buf, entries[index].len);
        if (keyword_count) sb_append(&candidate.source, "#endif\n");
        sb_append(&candidate.source, "\n");
        if (!sb_ok(&candidate.source) || candidate.source.len > COMPUTE_SOURCE_MAX_BYTES) { status = COMPUTE_SOURCE_LIMIT_EXCEEDED; goto cleanup; }
    }
    for (size_t index = 0; index < candidate.variant_count; ++index) {
        const ComputeSourceVariant *evidence = &candidate.variants[index];
        status = COMPUTE_SOURCE_QUALITY_FAILED;
        if (!hlsl_source_quality_analysis_begin_unit(quality, evidence->source_unit_id, HLSL_SOURCE_UNIT_ENTRY_POINT, true)) goto cleanup;
        for (size_t expression = 0; expression < evidence->expression_count; ++expression)
            if (!hlsl_source_quality_analysis_expression(quality, evidence->expressions[expression])) goto cleanup;
        for (size_t event = 0; event < evidence->emission_fact_count; ++event)
            if (!hlsl_source_quality_analysis_emission(quality, &evidence->emission_facts[event])) goto cleanup;
    }
    status = COMPUTE_SOURCE_QUALITY_FAILED;
    if (!hlsl_source_quality_analysis_finish(quality, HLSL_EMIT_STATUS_OK, candidate.variant_count + 1) ||
        (structured_declarations
            ? (candidate.source_quality.classification != HLSL_SOURCE_QUALITY_MIXED ||
               candidate.source_quality.reasons != HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE ||
               candidate.source_quality.counts.incomplete_units != 1u ||
               candidate.source_quality.counts.residual_total || candidate.source_quality.counts.unknown_provenance)
            : candidate.source_quality.classification != HLSL_SOURCE_QUALITY_CLEAN)) goto cleanup;
    common_sha256_final(&modeled, candidate.modeled_input_sha256);
    common_sha256(candidate.source.buf, candidate.source.len, candidate.source_sha256);
    candidate.domain_complete = true;
    candidate.status = COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
    status = COMPUTE_SOURCE_CANDIDATE_UNVERIFIED;
    compute_source_candidate_dispose(destination);
    *destination = candidate;
    compute_source_candidate_init(&candidate);
cleanup:
    if (entries) {
        for (size_t index = 0; index < diagnostic->requested_variants; ++index) sb_free(&entries[index]);
        free(entries);
    }
    hlsl_source_quality_analysis_destroy(quality);
    compute_source_candidate_dispose(&candidate);
    diagnostic->status = status;
    return status;
}

const char *compute_source_status_name(ComputeSourceStatus status) {
    static const char *const names[] = {
        "candidate-unverified", "invalid-argument", "layout-unavailable", "limit-exceeded",
        "platform-unsupported", "name-unrepresentable", "keyword-domain-unrepresentable",
        "resource-inverse-unavailable", "requirements-unsupported", "dxbc-unavailable",
        "stage-contract-failed", "thread-group-mismatch", "body-unsupported", "emission-failed",
        "allocation-failed", "quality-failed"
    };
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "invalid-status";
}
