// SPDX-License-Identifier: GPL-3.0-only

#include "translation/compute_source_candidate.h"
#include "translation/hlsl_source_identifier.h"
#include "translation/hlsl_emitter_internal.h"
#include "translation/hlsl_compute_source_internal.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/usil_validation.h"
#include "dxbc/dxbc_hash.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return false; } } while (0)
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define INSTRUCTION(op, length) ((uint32_t)(op) | (uint32_t)(length) << 24u)

typedef struct {
    uint8_t bytes[8192];
    size_t used;
    ComputeShaderObject object;
    ComputeShaderPlatformVariant platforms[2];
    ComputeShaderKernelParent kernels[2];
    ComputeShaderKernelVariant variants[2][4];
    ComputeShaderStringView global[2][1];
    ComputeShaderStringView local[2][1];
    uint32_t groups[2][4][3];
    ComputeShaderResource textures[2][4];
    ComputeShaderResource outputs[2][4];
} Fixture;

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes[index] = (uint8_t)(value >> (8u * index));
}

static ComputeShaderStringView text(Fixture *fixture, const char *value) {
    const size_t size = strlen(value);
    if (size > sizeof(fixture->bytes) - fixture->used) return (ComputeShaderStringView){0};
    const ComputeShaderStringView view = {fixture->bytes + fixture->used, size};
    memcpy(fixture->bytes + fixture->used, value, size);
    fixture->used += size;
    return view;
}

/* Deliberately caller-owned controlled model: decoded/layout identity alone
 * supplies no assertion that these fields equal an original serialization. */
static bool fixture_init(Fixture *fixture, const uint8_t *flags, size_t flag_count) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->object.name = text(fixture, "ControlledCompute");
    const ComputeShaderStringView names[] = {text(fixture, "KernelFirst"), text(fixture, "KernelSecond")};
    const ComputeShaderStringView global = text(fixture, "KEY_A");
    const ComputeShaderStringView local = text(fixture, "KEY_B");
    const ComputeShaderStringView keys[] = {text(fixture, ""), text(fixture, "KEY_A"),
        text(fixture, "KEY_A KEY_B"), text(fixture, "KEY_B")};
    const size_t offset = fixture->used;
    const size_t word_count = 5u + flag_count;
    const size_t size = 52u + word_count * 4u;
    CHECK(size <= sizeof(fixture->bytes) - offset);
    uint8_t *code = fixture->bytes + offset;
    memset(code, 0, size);
    memcpy(code, "DXBC", 4);
    write_u32(code + 20, 1);
    write_u32(code + 24, (uint32_t)size);
    write_u32(code + 28, 1);
    write_u32(code + 32, 36);
    memcpy(code + 36, "SHEX", 4);
    write_u32(code + 40, (uint32_t)size - 44u);
    write_u32(code + 44, 0x00050050u);
    write_u32(code + 48, (uint32_t)word_count + 2u);
    write_u32(code + 52, INSTRUCTION(155, 4));
    write_u32(code + 56, 8);
    write_u32(code + 60, 4);
    write_u32(code + 64, 1);
    for (size_t index = 0; index < flag_count; ++index)
        write_u32(code + 68 + 4u * index, INSTRUCTION(190, 1) | (uint32_t)flags[index] << 11u);
    write_u32(code + 68 + flag_count * 4u, INSTRUCTION(62, 1));
    CHECK(dxbc_compute_hash(code, size, code + 4));
    fixture->used += size;
    for (size_t kernel = 0; kernel < 2; ++kernel) {
        fixture->global[kernel][0] = global;
        fixture->local[kernel][0] = local;
        fixture->kernels[kernel] = (ComputeShaderKernelParent){.name = names[kernel],
            .global_keywords = fixture->global[kernel], .global_keyword_count = 1,
            .local_keywords = fixture->local[kernel], .local_keyword_count = 1,
            .variants = fixture->variants[kernel], .variant_count = 4};
        for (size_t variant = 0; variant < 4; ++variant) {
            fixture->groups[kernel][variant][0] = 8;
            fixture->groups[kernel][variant][1] = 4;
            fixture->groups[kernel][variant][2] = 1;
            fixture->variants[kernel][variant] = (ComputeShaderKernelVariant){.keyword_key = keys[variant],
                .code = code, .code_size = size, .thread_group_size = fixture->groups[kernel][variant],
                .thread_group_size_count = 3, .requirements = 0x4001};
        }
    }
    fixture->platforms[0] = (ComputeShaderPlatformVariant){.target_renderer = 2, .target_level = 0,
        .kernels = fixture->kernels, .kernel_count = 2, .resources_resolved = true};
    fixture->platforms[1] = fixture->platforms[0];
    fixture->object.unity_version = (ComputeShaderStringView){(const uint8_t *)"2021.3.35f1", sizeof("2021.3.35f1") - 1};
    fixture->object.serialized_object_bytes = fixture->bytes;
    fixture->object.serialized_object_size = sizeof(fixture->bytes);
    fixture->object.platforms = fixture->platforms;
    fixture->object.platform_count = 1;
    fixture->object.target_platform = 19;
    fixture->object.serialized_file_version = 22;
    fixture->object.decoded = true;
    const uint8_t identity[16] = {0xab,0xd9,0x13,0x5b,0x8c,0xe8,0x3d,0x04,
        0x3f,0xef,0x4e,0x9e,0xc7,0xf5,0x33,0x66};
    memcpy(fixture->object.serialized_type_hash, identity, sizeof(identity));
    return true;
}

static bool expect_failure(Fixture *fixture, ComputeSourceCandidate *destination,
                           ComputeSourceStatus expected) {
    const ComputeSourceCandidate before = *destination;
    ComputeSourceDiagnostic diagnostic;
    const ComputeSourceStatus status = compute_source_candidate_build(&fixture->object, destination, &diagnostic);
    if (status != expected) fprintf(stderr, "expected=%s actual=%s\n", compute_source_status_name(expected), compute_source_status_name(status));
    CHECK(status == expected);
    CHECK(diagnostic.status == expected);
    CHECK(memcmp(&before, destination, sizeof(before)) == 0);
    CHECK(destination->domain_complete && destination->source_quality.classification == before.source_quality.classification);
    return true;
}

static bool check_identifier_names(void) {
    CHECK(hlsl_source_identifier_valid("Kernel_01"));
    CHECK(hlsl_source_identifier_valid("KEY_A"));
    const char *invalid[] = {"", "_", "0first", "a b", "a\nb", "float", "uint3", "float2x4",
        "uint4x4", "void", "defined", "GroupMemoryBarrier", "InputPatch", "__RESERVED", "UNITY_TEST", "SV_GroupID"};
    for (size_t index = 0; index < COUNT(invalid); ++index) CHECK(!hlsl_source_identifier_valid(invalid[index]));
    char name[257];
    memset(name, 'A', 256); name[256] = 0;
    CHECK(!hlsl_source_identifier_valid(name));
    name[255] = 0;
    CHECK(hlsl_source_identifier_valid(name));
    return true;
}

static bool check_complete_candidate(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, NULL, 0));
    ComputeSourceCandidate candidate;
    ComputeSourceDiagnostic diagnostic;
    compute_source_candidate_init(&candidate);
    CHECK(candidate.source_quality.classification == HLSL_SOURCE_QUALITY_UNSUPPORTED);
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(diagnostic.requested_counts_known && diagnostic.requested_kernels == 2 && diagnostic.requested_variants == 8);
    CHECK(diagnostic.examined_variants == 8 && diagnostic.represented_variants == 8);
    CHECK(candidate.status == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED && candidate.domain_complete);
    CHECK(candidate.kernel_count == 2 && candidate.variant_count == 8);
    CHECK(candidate.source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(candidate.source_quality.counts.inspected_units == 9 && !candidate.source_quality.counts.incomplete_units);
    CHECK(!candidate.source_quality.counts.unknown_provenance && !candidate.source_quality.counts.residual_total);
    CHECK(strstr(candidate.source.buf, "#pragma kernel KernelFirst\n"));
    CHECK(strstr(candidate.source.buf, "#pragma multi_compile _ KEY_A\n"));
    CHECK(strstr(candidate.source.buf, "#pragma multi_compile_local _ KEY_B\n"));
    CHECK(!strstr(candidate.source.buf, "#pragma require"));
    CHECK(strstr(candidate.source.buf, "#if defined(KEY_A) && !defined(KEY_B)\n"));
    for (size_t index = 0; index < candidate.variant_count; ++index) {
        const ComputeSourceVariant *variant = &candidate.variants[index];
        CHECK(variant->source_unit_id == index + 1 && variant->platform_index == 0);
        CHECK(variant->emission_fact_count == 4 && variant->emission_facts);
        CHECK(variant->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
        CHECK(variant->requirements == 0x4001 && variant->thread_group_size[0] == 8);
    }
    CHECK(compute_shader_object_source_authority(&fixture.object) == COMPUTE_SHADER_SOURCE_AUTHORITY_DECLARATION_INVERSE_UNAVAILABLE);
    memset(fixture.bytes, 0, sizeof(fixture.bytes));
    CHECK(strcmp(candidate.variants[0].kernel_name, "KernelFirst") == 0);
    CHECK(strcmp(candidate.keywords[1], "KEY_B") == 0 && strstr(candidate.source.buf, "void KernelFirst()"));
    compute_source_candidate_dispose(&candidate);
    CHECK(!candidate.source.buf && !candidate.variants && !candidate.keywords);
    return true;
}

static bool check_transactional_failures(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, NULL, 0));
    ComputeSourceCandidate candidate;
    ComputeSourceDiagnostic diagnostic;
    compute_source_candidate_init(&candidate);
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    fixture.kernels[1].variant_count = 3;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE));
    CHECK(compute_source_candidate_build(&fixture.object, &(ComputeSourceCandidate){0}, &diagnostic) == COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE);
    CHECK(diagnostic.requested_variants == 7 && diagnostic.represented_variants == 4);
    fixture.kernels[1].variant_count = 4;
    fixture.variants[0][1].keyword_key = fixture.variants[0][0].keyword_key;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE));
    CHECK(fixture_init(&fixture, NULL, 0));
    const char *bad_keys[] = {"KEY_A ", " KEY_A", "KEY_A  KEY_B", "KEY_A\tKEY_B", "UNKNOWN", "KEY_A KEY_A"};
    for (size_t index = 0; index < COUNT(bad_keys); ++index) {
        fixture.variants[0][1].keyword_key = text(&fixture, bad_keys[index]);
        CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE));
    }
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.kernels[1].name = fixture.kernels[0].name;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.kernels[0].name = fixture.global[0][0];
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.global[1][0] = fixture.local[1][0];
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_KEYWORD_DOMAIN_UNREPRESENTABLE));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.global[0][0] = text(&fixture, "float4");
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.kernels[0].name = (ComputeShaderStringView){fixture.bytes + sizeof(fixture.bytes) - 1, 4};
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE));
    fixture.kernels[0].name = (ComputeShaderStringView){NULL, 4};
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.kernels[0].name = (ComputeShaderStringView){fixture.bytes + fixture.used, 4};
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.variants[0][0].code = fixture.bytes + sizeof(fixture.bytes) - 1;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_DXBC_UNAVAILABLE));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.groups[0][0][0] = 9;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_THREAD_GROUP_MISMATCH));
    fixture.groups[0][0][0] = 8;
    fixture.variants[0][0].thread_group_size_count = 2;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_THREAD_GROUP_MISMATCH));
    fixture.variants[0][0].thread_group_size_count = 3;
    const int64_t requirements[] = {-1, 0, 0x4000, 0x4003, INT64_MAX};
    for (size_t index = 0; index < COUNT(requirements); ++index) {
        fixture.variants[0][0].requirements = requirements[index];
        CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_REQUIREMENTS_UNSUPPORTED));
    }
    fixture.variants[0][0].requirements = 0x4001;
    fixture.variants[0][0].input_buffer_count = 1;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE));
    fixture.variants[0][0].input_buffer_count = 0;
    fixture.platforms[0].target_renderer = 4;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_PLATFORM_UNSUPPORTED));
    fixture.platforms[0].target_renderer = 2;
    fixture.object.platform_count = 2;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_PLATFORM_UNSUPPORTED));
    CHECK(compute_source_candidate_build(&fixture.object, &(ComputeSourceCandidate){0}, &diagnostic) == COMPUTE_SOURCE_PLATFORM_UNSUPPORTED);
    CHECK(diagnostic.requested_counts_known && diagnostic.requested_kernels == 4 && diagnostic.requested_variants == 16);
    fixture.object.platform_count = 1;
    fixture.object.unity_version = (ComputeShaderStringView){NULL, sizeof("2021.3.35f1") - 1};
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_LAYOUT_UNAVAILABLE));
    compute_source_candidate_dispose(&candidate);
    return true;
}

static bool check_modeled_input_binding(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, NULL, 0));
    const ComputeShaderStringView renamed = text(&fixture, "RenamedKernel");
    ComputeSourceCandidate first, second;
    compute_source_candidate_init(&first);
    compute_source_candidate_init(&second);
    CHECK(compute_source_candidate_build(&fixture.object, &first, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    fixture.kernels[0].name = renamed;
    CHECK(compute_source_candidate_build(&fixture.object, &second, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(memcmp(first.serialized_object_sha256, second.serialized_object_sha256, COMMON_SHA256_DIGEST_SIZE) == 0);
    CHECK(memcmp(first.modeled_input_sha256, second.modeled_input_sha256, COMMON_SHA256_DIGEST_SIZE) != 0);
    CHECK(memcmp(first.source_sha256, second.source_sha256, COMMON_SHA256_DIGEST_SIZE) != 0);
    CHECK(first.status == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED && second.status == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    compute_source_candidate_dispose(&first);
    compute_source_candidate_dispose(&second);
    return true;
}

static bool check_empty_keyword_domain_and_limits(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.platforms[0].kernel_count = 1;
    fixture.kernels[0].global_keyword_count = fixture.kernels[0].local_keyword_count = 0;
    fixture.kernels[0].global_keywords = fixture.kernels[0].local_keywords = NULL;
    fixture.kernels[0].variant_count = 1;
    ComputeSourceCandidate candidate;
    ComputeSourceDiagnostic diagnostic;
    compute_source_candidate_init(&candidate);
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(candidate.variant_count == 1 && candidate.domain_complete && !candidate.keywords);
    CHECK(candidate.source_quality.counts.inspected_units == 2);
    CHECK(!strstr(candidate.source.buf, "#if") && !strstr(candidate.source.buf, "multi_compile"));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.kernels[0].variant_count = COMPUTE_SOURCE_MAX_VARIANTS + 1;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_LIMIT_EXCEEDED));
    CHECK(compute_source_candidate_build(&fixture.object, &(ComputeSourceCandidate){0}, &diagnostic) == COMPUTE_SOURCE_LIMIT_EXCEEDED);
    CHECK(diagnostic.requested_counts_known && diagnostic.requested_variants == COMPUTE_SOURCE_MAX_VARIANTS + 5);
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.kernels[0].global_keyword_count = COMPUTE_SOURCE_MAX_KEYWORDS + 1;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_LIMIT_EXCEEDED));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.platforms[0].kernel_count = COMPUTE_SOURCE_MAX_KERNELS + 1;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_LIMIT_EXCEEDED));
    CHECK(fixture_init(&fixture, NULL, 0));
    fixture.object.platform_count = 17;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_LIMIT_EXCEEDED));
    CHECK(strcmp(compute_source_status_name((ComputeSourceStatus)-1), "invalid-status") == 0);
    compute_source_candidate_dispose(&candidate);
    return true;
}

static bool check_modeled_program_binding(void) {
    Fixture fixture;
    CHECK(fixture_init(&fixture, NULL, 0));
    const size_t code_size = fixture.variants[0][0].code_size;
    CHECK(code_size <= sizeof(fixture.bytes) - fixture.used);
    uint8_t *alternate = fixture.bytes + fixture.used;
    memcpy(alternate, fixture.variants[0][0].code, code_size);
    write_u32(alternate + 56, 4);
    write_u32(alternate + 60, 2);
    write_u32(alternate + 64, 2);
    CHECK(dxbc_compute_hash(alternate, code_size, alternate + 4));
    ComputeSourceCandidate first, second;
    compute_source_candidate_init(&first);
    compute_source_candidate_init(&second);
    CHECK(compute_source_candidate_build(&fixture.object, &first, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    fixture.variants[0][0].code = alternate;
    fixture.groups[0][0][0] = 4;
    fixture.groups[0][0][1] = 2;
    fixture.groups[0][0][2] = 2;
    CHECK(compute_source_candidate_build(&fixture.object, &second, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(memcmp(first.serialized_object_sha256, second.serialized_object_sha256, COMMON_SHA256_DIGEST_SIZE) == 0);
    CHECK(memcmp(first.modeled_input_sha256, second.modeled_input_sha256, COMMON_SHA256_DIGEST_SIZE) != 0);
    CHECK(memcmp(first.variants[0].serialized_program_sha256, second.variants[0].serialized_program_sha256, COMMON_SHA256_DIGEST_SIZE) != 0);
    CHECK(memcmp(first.source_sha256, second.source_sha256, COMMON_SHA256_DIGEST_SIZE) != 0);
    compute_source_candidate_dispose(&first);
    compute_source_candidate_dispose(&second);
    return true;
}

static bool check_barrier_candidates(void) {
    Fixture fixture;
    const uint8_t flags[] = {3, 2, 8, 9, 10, 11};
    CHECK(fixture_init(&fixture, flags, COUNT(flags)));
    ComputeSourceCandidate candidate;
    compute_source_candidate_init(&candidate);
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(candidate.variants[0].emission_fact_count == 10 && candidate.source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(strstr(candidate.source.buf, "GroupMemoryBarrierWithGroupSync();\n    GroupMemoryBarrier();\n"));
    CHECK(strstr(candidate.source.buf, "DeviceMemoryBarrier();\n    DeviceMemoryBarrierWithGroupSync();\n"));
    CHECK(strstr(candidate.source.buf, "AllMemoryBarrier();\n    AllMemoryBarrierWithGroupSync();\n"));
    const uint8_t unsupported = 4;
    CHECK(fixture_init(&fixture, &unsupported, 1));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    const uint8_t invalid = 1;
    CHECK(fixture_init(&fixture, &invalid, 1));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_STAGE_CONTRACT_FAILED));
    compute_source_candidate_dispose(&candidate);
    return true;
}

/* Controlled typed texture vocabulary in a synthetic container. These units
 * establish decoder/emitter boundaries, not selected compiler equality. */
static const uint32_t typed_words[] = {
    0x0100086au, 0x04001858u, 0x00107000u, 0x00000000u, 0x00004444u, 0x0400189cu,
    0x0011e000u, 0x00000000u, 0x00004444u, 0x0200005fu, 0x00020032u, 0x02000068u,
    0x00000002u, 0x0400009bu, 0x00000004u, 0x00000004u, 0x00000001u, 0x08000036u,
    0x001000c2u, 0x00000000u, 0x00004002u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x0900001eu, 0x00100032u, 0x00000000u, 0x00020046u, 0x00004002u,
    0x00000001u, 0x00000002u, 0x00000000u, 0x00000000u, 0x8900002du, 0x800000c2u,
    0x00111103u, 0x001000f2u, 0x00000001u, 0x00100e46u, 0x00000000u, 0x00107e46u,
    0x00000000u, 0x0a00001eu, 0x001000f2u, 0x00000001u, 0x00100e46u, 0x00000001u,
    0x00004002u, 0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u, 0x070000a4u,
    0x0011e0f2u, 0x00000000u, 0x00100546u, 0x00000000u, 0x00100e46u, 0x00000001u,
    0x0100003eu,
};

static bool typed_code(Fixture *fixture, const uint32_t *words, size_t count) {
    const size_t size = 52u + count * 4u;
    CHECK(size <= sizeof(fixture->bytes) - fixture->used);
    uint8_t *code = fixture->bytes + fixture->used;
    memset(code, 0, size); memcpy(code, "DXBC", 4);
    write_u32(code + 20, 1); write_u32(code + 24, (uint32_t)size);
    write_u32(code + 28, 1); write_u32(code + 32, 36);
    memcpy(code + 36, "SHEX", 4); write_u32(code + 40, (uint32_t)size - 44u);
    write_u32(code + 44, 0x00050050u); write_u32(code + 48, (uint32_t)count + 2u);
    for (size_t index = 0; index < count; ++index) write_u32(code + 52u + 4u * index, words[index]);
    CHECK(dxbc_compute_hash(code, size, code + 4)); fixture->used += size;
    for (size_t k = 0; k < 2; ++k) for (size_t v = 0; v < 4; ++v) {
        fixture->variants[k][v].code = code; fixture->variants[k][v].code_size = size;
    }
    return true;
}

static bool typed_fixture(Fixture *fixture) {
    CHECK(fixture_init(fixture, NULL, 0));
    CHECK(typed_code(fixture, typed_words, COUNT(typed_words)));
    const ComputeShaderStringView input = text(fixture, "InputTexels"), output = text(fixture, "OutputTexels");
    for (size_t k = 0; k < 2; ++k) for (size_t v = 0; v < 4; ++v) {
        fixture->groups[k][v][0] = fixture->groups[k][v][1] = 4;
        fixture->textures[k][v] = (ComputeShaderResource){.name = input, .bind_point = 0, .sampler_bind_point = -1, .texture_dimension = 2};
        fixture->outputs[k][v] = (ComputeShaderResource){.name = output, .bind_point = 0, .sampler_bind_point = -1, .texture_dimension = 2};
        fixture->variants[k][v].textures = &fixture->textures[k][v]; fixture->variants[k][v].texture_count = 1;
        fixture->variants[k][v].output_buffers = &fixture->outputs[k][v]; fixture->variants[k][v].output_buffer_count = 1;
    }
    return true;
}

static bool check_typed_candidate(void) {
    Fixture fixture; CHECK(typed_fixture(&fixture));
    const ComputeShaderStringView renamed = text(&fixture, "RenamedInput");
    ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
    ComputeSourceDiagnostic diagnostic;
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(candidate.variant_count == 8 && candidate.resource_count == 2 && candidate.domain_complete);
    CHECK(candidate.source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(candidate.source_quality.counts.resource_declarations == 2);
    CHECK(candidate.source_quality.counts.sibling_declarations == 2 &&
          candidate.source_quality.counts.sibling_declaration_witnesses == 16);
    for (size_t resource = 0; resource < 2; ++resource) {
        CHECK(candidate.resources[resource].witness_count == 8);
        for (size_t witness = 0; witness < 8; ++witness) CHECK(candidate.resources[resource].variant_witnesses[witness] == witness);
    }
    CHECK(candidate.source_quality.counts.ast_expressions && candidate.source_quality.counts.semantic_projections);
    CHECK(!candidate.source_quality.counts.residual_total && !candidate.source_quality.counts.unknown_provenance);
    CHECK(strstr(candidate.source.buf, "Texture2D<uint4> InputTexels;\n"));
    CHECK(strstr(candidate.source.buf, "RWTexture2D<uint4> OutputTexels;\n"));
    CHECK(strstr(candidate.source.buf, "InputTexels.Load(int3("));
    CHECK(strstr(candidate.source.buf, "+ uint4(1u, 2u, 3u, 4u)"));
    for (size_t row = 0; row < candidate.variant_count; ++row) {
        CHECK(candidate.variants[row].expression_count == 3);
        CHECK(candidate.variants[row].memory_effect_count == 2);
        CHECK(candidate.variants[row].memory_effects[0].opcode == USIL_OP_LD &&
              candidate.variants[row].memory_effects[1].opcode == USIL_OP_STORE_UAV_TYPED);
        CHECK(candidate.variants[row].memory_effects[0].instruction_index < candidate.variants[row].memory_effects[1].instruction_index);
        CHECK(candidate.variants[row].memory_effects[0].source_instruction_index < candidate.variants[row].memory_effects[1].source_instruction_index);
        CHECK(candidate.variants[row].entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    }
    ComputeShaderResource saved = fixture.outputs[0][0];
    fixture.outputs[0][0].texture_dimension = -1;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][0] = saved;
    fixture.outputs[0][0].bind_point = 1;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED)); fixture.outputs[0][0] = saved;
    fixture.outputs[0][0].sampler_bind_point = 0;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][0] = saved;
    fixture.outputs[0][0].generated_name = text(&fixture, "GeneratedDifferentName");
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][0] = saved;
    fixture.outputs[0][1].name = text(&fixture, "DifferentOutput");
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][1] = saved;
    fixture.outputs[0][0].name = (ComputeShaderStringView){fixture.bytes + sizeof(fixture.bytes), 1};
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE)); fixture.outputs[0][0] = saved;
    fixture.outputs[0][0].name = text(&fixture, "dispatchThreadId");
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE)); fixture.outputs[0][0] = saved;
    fixture.outputs[0][0].name = fixture.global[0][0];
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_NAME_UNREPRESENTABLE)); fixture.outputs[0][0] = saved;
    fixture.outputs[0][0].name = fixture.textures[0][0].name;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][0] = saved;
    fixture.variants[0][0].textures = NULL;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.variants[0][0].textures = &fixture.textures[0][0];
    /* Mutating modeled names changes the digest while the held span stays
     * identical; roots and source names remain deeply owned. */
    ComputeSourceCandidate first, second; compute_source_candidate_init(&first); compute_source_candidate_init(&second);
    CHECK(compute_source_candidate_build(&fixture.object, &first, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    for (size_t k = 0; k < 2; ++k) for (size_t v = 0; v < 4; ++v) fixture.textures[k][v].name = renamed;
    CHECK(compute_source_candidate_build(&fixture.object, &second, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(memcmp(first.modeled_input_sha256, second.modeled_input_sha256, 32));
    CHECK(!memcmp(first.serialized_object_sha256, second.serialized_object_sha256, 32));
    CHECK(strstr(first.source.buf, "InputTexels.Load") && strstr(second.source.buf, "RenamedInput.Load"));
    memset(fixture.bytes, 0, sizeof(fixture.bytes));
    StringBuilder retained; sb_init(&retained); ast_format_expr(first.variants[0].expressions[2], &retained);
    CHECK(sb_ok(&retained) && strstr(retained.buf, "InputTexels.Load")); sb_free(&retained);
    compute_source_candidate_dispose(&first); compute_source_candidate_dispose(&second); compute_source_candidate_dispose(&candidate);
    return true;
}

static bool check_typed_effect_rejections(void) {
    Fixture fixture; CHECK(typed_fixture(&fixture));
    ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    uint32_t changed[COUNT(typed_words) + 7];
    memcpy(changed, typed_words, sizeof(typed_words)); changed[8] = 0x5555u; /* Float UAV. */
    CHECK(typed_code(&fixture, changed, COUNT(typed_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    memcpy(changed, typed_words, sizeof(typed_words)); changed[41] = 0x001071b6u; /* Reversed Load lanes. */
    CHECK(typed_code(&fixture, changed, COUNT(typed_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    memcpy(changed, typed_words, sizeof(typed_words)); changed[54] = 0x0011e032u; /* Partial store. */
    CHECK(typed_code(&fixture, changed, COUNT(typed_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* A dead read remains an effect; overwriting its result cannot remove it. */
    memcpy(changed, typed_words, sizeof(typed_words));
    const uint32_t overwrite[] = {0x08000036u, 0x001000f2u, 1u, 0x00004002u, 1u, 2u, 3u, 4u};
    memcpy(changed + 43, overwrite, sizeof(overwrite));
    memmove(changed + 51, typed_words + 53, (COUNT(typed_words) - 53) * sizeof(uint32_t));
    CHECK(typed_code(&fixture, changed, COUNT(typed_words) - 2));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* The same load cannot be emitted twice in an expression. */
    memcpy(changed, typed_words, sizeof(typed_words)); changed[43] = INSTRUCTION(30, 7);
    changed[48] = 0x00100e46u; changed[49] = 1u;
    memmove(changed + 50, changed + 53, (COUNT(typed_words) - 53) * sizeof(uint32_t));
    CHECK(typed_code(&fixture, changed, COUNT(typed_words) - 3));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* A shared pure intermediate must not expand the sole load twice. */
    memcpy(changed, typed_words, sizeof(typed_words));
    const uint32_t twice[] = {0x0700001eu, 0x001000f2u, 1u, 0x00100e46u, 1u, 0x00100e46u, 1u};
    memcpy(changed + 53, twice, sizeof(twice));
    memcpy(changed + 60, typed_words + 53, (COUNT(typed_words) - 53) * sizeof(uint32_t));
    CHECK(typed_code(&fixture, changed, COUNT(typed_words) + 7));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* A retained device fence cannot become pure address computation. */
    memcpy(changed, typed_words, sizeof(typed_words));
    memmove(changed + 44, changed + 43, (COUNT(typed_words) - 43) * sizeof(uint32_t));
    changed[43] = INSTRUCTION(190, 1) | (8u << 11u);
    CHECK(typed_code(&fixture, changed, COUNT(typed_words) + 1));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    compute_source_candidate_dispose(&candidate); return true;
}

/* An independently authored raw token grammar retains one actual UINT4 UAV
 * read and final write. Same-address authority comes from SSA, not spelling. */
static const uint32_t uav_read_words[] = {
    INSTRUCTION(106, 1) | (1u << 11u),
    INSTRUCTION(156, 4) | (3u << 11u), UINT32_C(0x0011e000), 0, UINT32_C(0x4444),
    INSTRUCTION(95, 2), UINT32_C(0x00020032),
    INSTRUCTION(104, 2), 2,
    INSTRUCTION(155, 4), 4, 4, 1,
    INSTRUCTION(30, 9), UINT32_C(0x00100032), 0, UINT32_C(0x00020046),
    UINT32_C(0x00004002), 1, 2, 0, 0,
    INSTRUCTION(163, 7), UINT32_C(0x001000f2), 1, UINT32_C(0x00100e46), 0,
    UINT32_C(0x0011ee46), 0,
    INSTRUCTION(30, 10), UINT32_C(0x001000f2), 1, UINT32_C(0x00100e46), 1,
    UINT32_C(0x00004002), 1, 2, 3, 4,
    INSTRUCTION(164, 7), UINT32_C(0x0011e0f2), 0, UINT32_C(0x00100e46), 0,
    UINT32_C(0x00100e46), 1,
    INSTRUCTION(62, 1)
};

static bool uav_read_fixture(Fixture *fixture) {
    CHECK(typed_fixture(fixture));
    CHECK(typed_code(fixture, uav_read_words, COUNT(uav_read_words)));
    for (size_t kernel = 0; kernel < 2; ++kernel) {
        for (size_t variant = 0; variant < 4; ++variant) {
            fixture->variants[kernel][variant].textures = NULL;
            fixture->variants[kernel][variant].texture_count = 0;
        }
    }
    return true;
}

static bool check_uav_read_candidate(void) {
    const struct { uint32_t opcode; const char *syntax; bool unary; } operations[] = {
        {30, " + ", false}, {1, " & ", false}, {60, " | ", false},
        {87, " ^ ", false}, {59, "~", true}, {41, " << ", false}, {85, " >> ", false}
    };
    for (size_t index = 0; index < COUNT(operations); ++index) {
        Fixture fixture; CHECK(uav_read_fixture(&fixture));
        uint32_t words[COUNT(uav_read_words)]; memcpy(words, uav_read_words, sizeof(words));
        size_t count = COUNT(words);
        words[29] = INSTRUCTION(operations[index].opcode, operations[index].unary ? 5 : 10);
        if (operations[index].unary) {
            memmove(words + 34, uav_read_words + 39, (COUNT(words) - 39) * sizeof(*words));
            count -= 5;
        }
        CHECK(typed_code(&fixture, words, count));
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        CHECK(candidate.domain_complete && candidate.variant_count == 8 && candidate.resource_count == 1);
        CHECK(candidate.source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
            candidate.source_quality.counts.resource_declarations == 1 &&
            candidate.source_quality.counts.sibling_declaration_witnesses == 8);
        CHECK(strstr(candidate.source.buf, "RWTexture2D<uint4> OutputTexels;") &&
            strstr(candidate.source.buf, "OutputTexels.Load(int2(") &&
            strstr(candidate.source.buf, operations[index].syntax));
        CHECK(!strstr(candidate.source.buf, "InputTexels") && !strstr(candidate.source.buf, "int3("));
        for (size_t row = 0; row < candidate.variant_count; ++row) {
            const ComputeSourceVariant *entry = &candidate.variants[row];
            CHECK(entry->expression_count == 3 && entry->memory_effect_count == 2 &&
                entry->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
            CHECK(entry->memory_effects[0].opcode == USIL_OP_LD_UAV_TYPED &&
                entry->memory_effects[1].opcode == USIL_OP_STORE_UAV_TYPED &&
                entry->memory_effects[0].binding_register == 0 && entry->memory_effects[1].binding_register == 0 &&
                entry->memory_effects[0].instruction_index < entry->memory_effects[1].instruction_index);
        }
        memset(fixture.bytes, 0, sizeof(fixture.bytes));
        StringBuilder retained; sb_init(&retained); ast_format_expr(candidate.variants[0].expressions[2], &retained);
        CHECK(sb_ok(&retained) && strstr(retained.buf, "OutputTexels.Load(int2(")); sb_free(&retained);
        compute_source_candidate_dispose(&candidate);
    }
    /* Copying a retained resource value does not require an arithmetic node. */
    Fixture copy; CHECK(uav_read_fixture(&copy));
    uint32_t copy_words[COUNT(uav_read_words)]; memcpy(copy_words, uav_read_words, sizeof(copy_words));
    memmove(copy_words + 29, uav_read_words + 39, (COUNT(copy_words) - 39) * sizeof(*copy_words));
    CHECK(typed_code(&copy, copy_words, COUNT(copy_words) - 10));
    ComputeSourceCandidate copied; compute_source_candidate_init(&copied);
    CHECK(compute_source_candidate_build(&copy.object, &copied, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(copied.variants[0].memory_effect_count == 2 && strstr(copied.source.buf, "OutputTexels.Load(int2("));
    compute_source_candidate_dispose(&copied);
    const struct { size_t word; uint32_t value; } negatives[] = {
        {4, UINT32_C(0x5555)}, /* A float declaration grants no UINT4 view. */
        {1, INSTRUCTION(156, 4) | (3u << 11u) | (1u << 16u)}, /* Coherent UAV. */
        {23, UINT32_C(0x00100072)}, /* Partial resource result. */
        {27, UINT32_C(0x0011e006)}, /* Broadcast the same resource word. */
        {27, UINT32_C(0x0011e1b6)}, /* Reverse actual component order. */
        {40, UINT32_C(0x0011e032)}, /* Partial final store. */
        {28, 1}, /* Foreign read binding. */
        {22, INSTRUCTION(163, 7) | (1u << 13u)}, /* Saturation. */
        {25, UINT32_C(0x00100416)}, /* Same register, different read address. */
        {43, 1}, /* Resource value cannot impersonate its coordinate producer. */
        {29, INSTRUCTION(0, 10)}, /* Float arithmetic. */
        {29, INSTRUCTION(42, 10)} /* Signed shift. */
    };
    for (size_t index = 0; index < COUNT(negatives); ++index) {
        Fixture fixture; CHECK(uav_read_fixture(&fixture));
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        uint32_t words[COUNT(uav_read_words)]; memcpy(words, uav_read_words, sizeof(words));
        words[negatives[index].word] = negatives[index].value;
        CHECK(typed_code(&fixture, words, COUNT(words)));
        CHECK(expect_failure(&fixture, &candidate, index == 6 || index == 7 ?
            COMPUTE_SOURCE_STAGE_CONTRACT_FAILED : COMPUTE_SOURCE_EMISSION_FAILED));
        compute_source_candidate_dispose(&candidate);
    }
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        Fixture fixture; CHECK(uav_read_fixture(&fixture));
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        uint32_t words[COUNT(uav_read_words) + 10];
        size_t count = COUNT(uav_read_words);
        memcpy(words, uav_read_words, sizeof(uav_read_words));
        if (scenario == 0) {
            /* A retained read cannot disappear behind a pure redefinition. */
            const uint32_t overwrite[] = {INSTRUCTION(54, 8), UINT32_C(0x001000f2), 1,
                UINT32_C(0x00004002), 1, 2, 3, 4};
            memcpy(words + 29, overwrite, sizeof(overwrite));
            memmove(words + 37, uav_read_words + 39, (count - 39) * sizeof(*words)); count -= 2;
        } else if (scenario == 1) {
            /* A shared read cannot be expanded twice within one expression. */
            words[29] = INSTRUCTION(30, 7); words[34] = UINT32_C(0x00100e46); words[35] = 1;
            memmove(words + 36, uav_read_words + 39, (count - 39) * sizeof(*words)); count -= 3;
        } else if (scenario == 2) {
            memcpy(words + 29, uav_read_words + 22, 7 * sizeof(*words));
            memcpy(words + 36, uav_read_words + 29, (count - 29) * sizeof(*words)); count += 7;
        } else if (scenario == 3) {
            /* Physical coordinate register equality does not survive a new
             * SSA definition between the read and write. */
            const uint32_t overwrite[] = {INSTRUCTION(30, 10), UINT32_C(0x00100032), 0,
                UINT32_C(0x00100e46), 0, UINT32_C(0x00004002), 1, 2, 0, 0};
            memcpy(words + 29, overwrite, sizeof(overwrite));
            memcpy(words + 39, uav_read_words + 29, (count - 29) * sizeof(*words)); count += 10;
        } else {
            memmove(words + 30, uav_read_words + 29, (count - 29) * sizeof(*words));
            words[29] = INSTRUCTION(190, 1) | (8u << 11u); ++count;
        }
        CHECK(typed_code(&fixture, words, count));
        CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
        compute_source_candidate_dispose(&candidate);
    }
    return true;
}


/* Controlled structured declaration/load/store words. The byte stride does
 * not retain the original element type, so the source exposes uint4 bits and
 * its whole declaration unit remains incomplete rather than asserting CLEAN. */
static const uint32_t structured_words[] = {
    0x0100086au, 0x040000a2u, 0x00107000u, 0x00000000u, 0x00000010u, 0x0400009eu,
    0x0011e000u, 0x00000000u, 0x00000010u, 0x0200005fu, 0x00020012u, 0x02000068u,
    0x00000002u, 0x0400009bu, 0x00000004u, 0x00000001u, 0x00000001u, 0x0600001eu,
    0x00100012u, 0x00000000u, 0x0002000au, 0x00004001u, 0x00000001u, 0x8b0000a7u,
    0x80008302u, 0x00199983u, 0x001000f2u, 0x00000001u, 0x0010000au, 0x00000000u,
    0x00004001u, 0x00000000u, 0x00107e46u, 0x00000000u, 0x0a00001eu, 0x001000f2u,
    0x00000001u, 0x00100e46u, 0x00000001u, 0x00004002u, 0x0000000bu, 0x00000016u,
    0x00000021u, 0x0000002cu, 0x090000a8u, 0x0011e0f2u, 0x00000000u, 0x0010000au,
    0x00000000u, 0x00004001u, 0x00000000u, 0x00100e46u, 0x00000001u, 0x0100003eu
};

static bool structured_fixture(Fixture *fixture) {
    CHECK(fixture_init(fixture, NULL, 0));
    CHECK(typed_code(fixture, structured_words, COUNT(structured_words)));
    const ComputeShaderStringView input = text(fixture, "SourceElements"), output = text(fixture, "DestinationElements");
    for (size_t k = 0; k < 2; ++k) for (size_t v = 0; v < 4; ++v) {
        fixture->groups[k][v][0] = 4; fixture->groups[k][v][1] = fixture->groups[k][v][2] = 1;
        fixture->textures[k][v] = (ComputeShaderResource){.name = input, .bind_point = 0, .sampler_bind_point = -1, .texture_dimension = -1};
        fixture->outputs[k][v] = (ComputeShaderResource){.name = output, .bind_point = 0, .sampler_bind_point = -1, .texture_dimension = -1};
        fixture->variants[k][v].input_buffers = &fixture->textures[k][v]; fixture->variants[k][v].input_buffer_count = 1;
        fixture->variants[k][v].output_buffers = &fixture->outputs[k][v]; fixture->variants[k][v].output_buffer_count = 1;
    }
    return true;
}

static bool check_structured_candidate(void) {
    Fixture fixture; CHECK(structured_fixture(&fixture));
    ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
    ComputeSourceDiagnostic diagnostic;
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(candidate.domain_complete && candidate.variant_count == 8 && candidate.resource_count == 2);
    CHECK(candidate.source_quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
          candidate.source_quality.reasons == HLSL_SOURCE_QUALITY_REASON_INCOMPLETE_SOURCE &&
          candidate.source_quality.counts.incomplete_units == 1);
    CHECK(!candidate.source_quality.counts.residual_total && !candidate.source_quality.counts.unknown_provenance &&
          !candidate.source_quality.counts.raw_buffer_reconstruction && !candidate.source_quality.counts.storage_bitcasts);
    CHECK(strstr(candidate.source.buf, "StructuredBuffer<uint4> SourceElements;\n"));
    CHECK(strstr(candidate.source.buf, "RWStructuredBuffer<uint4> DestinationElements;\n"));
    CHECK(strstr(candidate.source.buf, "SourceElements.Load(((dispatchThreadId.x) + 1u))"));
    CHECK(strstr(candidate.source.buf, "+ uint4(11u, 22u, 33u, 44u)"));
    CHECK(!strstr(candidate.source.buf, "asuint") && !strstr(candidate.source.buf, "asfloat"));
    for (size_t resource = 0; resource < 2; ++resource) {
        CHECK(candidate.resources[resource].kind == COMPUTE_SOURCE_STRUCTURED_UINT4_BITS &&
              candidate.resources[resource].byte_stride == 16 && !candidate.resources[resource].original_element_type_known &&
              candidate.resources[resource].witness_count == 8);
    }
    for (size_t row = 0; row < candidate.variant_count; ++row) {
        const ComputeSourceVariant *entry = &candidate.variants[row];
        CHECK(entry->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && entry->expression_count == 3);
        CHECK(entry->memory_effect_count == 2 && entry->memory_effects[0].opcode == USIL_OP_LD_STRUCTURED &&
              entry->memory_effects[1].opcode == USIL_OP_STORE_STRUCTURED &&
              entry->memory_effects[0].instruction_index < entry->memory_effects[1].instruction_index);
    }
    /* Scalar semantic coordinates and high unsigned bits retain their owned
     * arithmetic domain rather than passing through float storage. */
    for (unsigned axis = 1; axis <= 2; ++axis) {
        uint32_t coordinate_words[COUNT(structured_words)];
        memcpy(coordinate_words, structured_words, sizeof(coordinate_words));
        coordinate_words[10] = axis == 1 ? 0x00020022u : 0x00020042u;
        coordinate_words[20] = axis == 1 ? 0x0002001au : 0x0002002au;
        coordinate_words[40] = UINT32_MAX; coordinate_words[41] = UINT32_C(0x80000000);
        CHECK(typed_code(&fixture, coordinate_words, COUNT(coordinate_words)));
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        CHECK(strstr(candidate.source.buf, axis == 1 ? "dispatchThreadId.y" : "dispatchThreadId.z"));
        CHECK(strstr(candidate.source.buf, "uint4(4294967295u, 2147483648u, 33u, 44u)"));
        CHECK(candidate.source_quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
              !candidate.source_quality.counts.residual_total && !candidate.source_quality.counts.unknown_provenance);
    }
    CHECK(typed_code(&fixture, structured_words, COUNT(structured_words)));
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    ComputeShaderResource saved = fixture.textures[0][0];
    fixture.textures[0][0].texture_dimension = 2;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.textures[0][0] = saved;
    fixture.variants[0][0].input_buffers = NULL;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.variants[0][0].input_buffers = &fixture.textures[0][0];
    fixture.variants[0][0].texture_count = 1; fixture.variants[0][0].textures = &fixture.textures[0][0];
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.variants[0][0].texture_count = 0;
    fixture.textures[0][0].bind_point = 1;
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED)); fixture.textures[0][0] = saved;
    fixture.textures[0][1].name = text(&fixture, "DifferentElements");
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.textures[0][1] = saved;
    uint32_t changed[COUNT(structured_words) + 7];
    const struct { size_t word; uint32_t value; } negatives[] = {
        {4, 32}, {8, 32}, /* Changed retained stride, not a uint4 element. */
        {31, 4}, {50, 4}, /* A byte-offset field cannot become an element index. */
        {26, 0x00100032u}, {45, 0x0011e032u}, /* Partial load/store. */
        {32, 0x001071b6u}, {51, 0x001001b6u}, /* Reversed resource/value lanes. */
        {5, 0x0401009eu}, /* Coherence cannot be dropped. */
        {34, 0x0a000000u}, /* Float ADD does not authorize uint arithmetic. */
        {38, 2}, {20, 0x0002003au} /* Missing SSA owner or widened scalar address. */
    };
    for (size_t n = 0; n < COUNT(negatives); ++n) {
        memcpy(changed, structured_words, sizeof(structured_words)); changed[negatives[n].word] = negatives[n].value;
        CHECK(typed_code(&fixture, changed, COUNT(structured_words)));
        CHECK(expect_failure(&fixture, &candidate, n == 0 ? COMPUTE_SOURCE_STAGE_CONTRACT_FAILED : COMPUTE_SOURCE_EMISSION_FAILED));
    }
    /* Preserve the original structured read even if its result is overwritten. */
    memcpy(changed, structured_words, sizeof(structured_words));
    const uint32_t overwrite[] = {0x08000036u, 0x001000f2u, 1u, 0x00004002u, 11u, 22u, 33u, 44u};
    memcpy(changed + 34, overwrite, sizeof(overwrite));
    memmove(changed + 42, structured_words + 44, (COUNT(structured_words) - 44) * sizeof(uint32_t));
    CHECK(typed_code(&fixture, changed, COUNT(structured_words) - 2));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* The same indirect effect reuse is rejected for structured elements. */
    memcpy(changed, structured_words, sizeof(structured_words));
    const uint32_t twice[] = {0x0700001eu, 0x001000f2u, 1u, 0x00100e46u, 1u, 0x00100e46u, 1u};
    memcpy(changed + 44, twice, sizeof(twice));
    memcpy(changed + 51, structured_words + 44, (COUNT(structured_words) - 44) * sizeof(uint32_t));
    CHECK(typed_code(&fixture, changed, COUNT(structured_words) + 7));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* Matched wider declarations are still an unsupported element layout. */
    memcpy(changed, structured_words, sizeof(structured_words)); changed[4] = changed[8] = 32;
    changed[24] = 0x80010302u;
    CHECK(typed_code(&fixture, changed, COUNT(structured_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    CHECK(typed_code(&fixture, structured_words, COUNT(structured_words)));
    ComputeSourceCandidate renamed; compute_source_candidate_init(&renamed);
    const ComputeShaderStringView name = text(&fixture, "RenamedSourceElements");
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    for (size_t k = 0; k < 2; ++k) for (size_t v = 0; v < 4; ++v) fixture.textures[k][v].name = name;
    CHECK(compute_source_candidate_build(&fixture.object, &renamed, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(memcmp(candidate.modeled_input_sha256, renamed.modeled_input_sha256, 32));
    CHECK(!memcmp(candidate.serialized_object_sha256, renamed.serialized_object_sha256, 32));
    CHECK(strstr(candidate.source.buf, "SourceElements.Load") && strstr(renamed.source.buf, "RenamedSourceElements.Load"));
    memset(fixture.bytes, 0, sizeof(fixture.bytes));
    StringBuilder retained; sb_init(&retained); ast_format_expr(candidate.variants[0].expressions[2], &retained);
    CHECK(sb_ok(&retained) && strstr(retained.buf, "SourceElements.Load")); sb_free(&retained);
    CHECK(!strcmp(candidate.resources[0].name, "SourceElements"));
    compute_source_candidate_dispose(&candidate); compute_source_candidate_dispose(&renamed);
    return true;
}

/* Controlled unsigned bit operations use the same one-read/one-write
 * effect plan. Typed texture and unknown structured element provenance remain
 * distinct even when their entry expression vocabulary agrees. */
static bool check_typed_uint_operations(void) {
    const struct { uint32_t opcode; const char *syntax; bool unary; } operations[] = {
        {1, " & ", false}, {60, " | ", false}, {87, " ^ ", false},
        {59, "~", true}, {41, " << ", false}, {85, " >> ", false}
    };
    for (unsigned structured = 0; structured < 2; ++structured) {
        const uint32_t *base = structured ? structured_words : typed_words;
        const size_t count = structured ? COUNT(structured_words) : COUNT(typed_words);
        const size_t operation = structured ? 34 : 43;
        for (size_t index = 0; index < COUNT(operations); ++index) {
            Fixture fixture;
            CHECK(structured ? structured_fixture(&fixture) : typed_fixture(&fixture));
            uint32_t changed[COUNT(typed_words)];
            memcpy(changed, base, count * sizeof(*changed));
            size_t changed_count = count;
            if (operations[index].unary) {
                changed[operation] = INSTRUCTION(operations[index].opcode, 5);
                memmove(changed + operation + 5, base + operation + 10,
                    (count - operation - 10) * sizeof(*changed));
                changed_count -= 5;
            } else {
                changed[operation] = INSTRUCTION(operations[index].opcode, 10);
                /* Unsigned high bits are actual bit values, never float
                 * storage; different shift counts retain their lane owners. */
                if (operations[index].opcode != 41 && operations[index].opcode != 85) {
                    changed[operation + 6] = UINT32_MAX;
                    changed[operation + 7] = UINT32_C(0x80000000);
                }
            }
            CHECK(typed_code(&fixture, changed, changed_count));
            ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
            CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) ==
                COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
            CHECK(candidate.domain_complete && candidate.variant_count == 8);
            CHECK(strstr(candidate.source.buf, operations[index].syntax));
            CHECK(!strstr(candidate.source.buf, "asfloat") && !strstr(candidate.source.buf, "asuint"));
            CHECK(!candidate.source_quality.counts.residual_total &&
                  !candidate.source_quality.counts.unknown_provenance);
            CHECK(candidate.source_quality.classification == (structured ?
                HLSL_SOURCE_QUALITY_MIXED : HLSL_SOURCE_QUALITY_CLEAN));
            for (size_t row = 0; row < candidate.variant_count; ++row) {
                const ComputeSourceVariant *entry = &candidate.variants[row];
                CHECK(entry->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                    entry->memory_effect_count == 2 && entry->expression_count == 3);
                CHECK(entry->memory_effects[0].instruction_index < entry->memory_effects[1].instruction_index);
            }
            changed[operation] |= UINT32_C(1) << 13; /* Saturation cannot become a bit operation. */
            CHECK(typed_code(&fixture, changed, changed_count));
            CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
            compute_source_candidate_dispose(&candidate);
        }
        Fixture fixture;
        CHECK(structured ? structured_fixture(&fixture) : typed_fixture(&fixture));
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) ==
            COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        uint32_t changed[COUNT(typed_words)];
        const uint32_t unsupported[] = {42, 0}; /* Signed right shift and float ADD. */
        for (size_t index = 0; index < COUNT(unsupported); ++index) {
            memcpy(changed, base, count * sizeof(*changed));
            changed[operation] = INSTRUCTION(unsupported[index], 10);
            CHECK(typed_code(&fixture, changed, count));
            CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
        }
        /* XOR cannot hide duplication of the same retained resource read. */
        memcpy(changed, base, count * sizeof(*changed));
        changed[operation] = INSTRUCTION(87, 7);
        changed[operation + 5] = UINT32_C(0x00100e46);
        changed[operation + 6] = 1;
        memmove(changed + operation + 7, base + operation + 10,
            (count - operation - 10) * sizeof(*changed));
        CHECK(typed_code(&fixture, changed, count - 3));
        CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
        compute_source_candidate_dispose(&candidate);
    }
    return true;
}


/* Build controlled partial structured reads from the declared sixteen-byte
 * element. A scalar/vector constant owns the prefix, and a single retained
 * read owns the suffix; these are distinct SSA producers, not lane locals. */
static void partial_structured_words(uint32_t *words, unsigned width) {
    memcpy(words, structured_words, 44u * sizeof(*words));
    const unsigned prefix = 4u - width;
    const uint8_t load_lanes = (uint8_t)(15u & ~((1u << prefix) - 1u));
    words[26] = UINT32_C(0x00100002) | (uint32_t)load_lanes << 4u;
    words[31] = prefix * 4u;
    words[32] = UINT32_C(0x00107006);
    for (unsigned component = 0; component < width; ++component)
        words[32] |= component << (4u + 2u * (component + prefix));
    words[34] = INSTRUCTION(60, 10); /* OR in the actual UINT32 domain. */
    words[35] = words[26];
    const uint32_t prefix_words[] = {INSTRUCTION(54, 8),
        UINT32_C(0x00100002) | ((1u << prefix) - 1u) << 4u,
        1u, UINT32_C(0x00004002), UINT32_MAX, UINT32_C(0x80000000), 23u, 41u};
    memcpy(words + 44, prefix_words, sizeof(prefix_words));
    memcpy(words + 52, structured_words + 44,
        (COUNT(structured_words) - 44u) * sizeof(*words));
}

static bool check_partial_structured_candidate(void) {
    Fixture fixture; CHECK(structured_fixture(&fixture));
    ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
    uint32_t words[COUNT(structured_words) + 8];
    const char *selections[] = {NULL, ".w", ".zw", ".yzw"};
    for (unsigned width = 1; width <= 3; ++width) {
        partial_structured_words(words, width);
        CHECK(typed_code(&fixture, words, COUNT(words)));
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) ==
            COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        CHECK(candidate.domain_complete && candidate.variant_count == 8 &&
            candidate.source_quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
            candidate.source_quality.counts.incomplete_units == 1 &&
            !candidate.source_quality.counts.residual_total &&
            !candidate.source_quality.counts.unknown_provenance);
        CHECK(strstr(candidate.source.buf, selections[width]) &&
            strstr(candidate.source.buf, "uint4(") && strstr(candidate.source.buf, "4294967295u"));
        CHECK(!strstr(candidate.source.buf, "asfloat") && !strstr(candidate.source.buf, "asuint"));
        for (size_t row = 0; row < candidate.variant_count; ++row) {
            const ComputeSourceVariant *entry = &candidate.variants[row];
            CHECK(entry->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                entry->expression_count == 3 && entry->memory_effect_count == 2);
            CHECK(entry->memory_effects[0].opcode == USIL_OP_LD_STRUCTURED &&
                entry->memory_effects[1].opcode == USIL_OP_STORE_STRUCTURED &&
                entry->memory_effects[0].instruction_index < entry->memory_effects[1].instruction_index);
            const ASTExpr *composition = entry->expressions[2];
            CHECK(composition->kind == AST_EXPR_CALL && !strcmp(composition->u.call.name, "uint4") &&
                composition->u.call.arg_count == 2 && composition->logical_origin.complete &&
                composition->logical_origin.scalar_type == AST_SCALAR_UINT32 &&
                composition->logical_origin.components == 4);
            const ASTExpr *operation = composition->u.call.args[1];
            CHECK(operation->kind == AST_EXPR_BINARY && operation->u.binary.op == USIL_OP_OR &&
                operation->logical_origin.components == width);
            const ASTExpr *projection = operation->u.binary.left;
            CHECK(projection->kind == AST_EXPR_SWIZZLE && projection->logical_origin.complete &&
                projection->logical_origin.semantic_projection &&
                projection->logical_origin.components == width &&
                projection->u.swizzle.swizzle_count == (int)width &&
                projection->logical_origin.instruction_index == (int)entry->memory_effects[0].instruction_index);
            for (unsigned component = 0; component < width; ++component)
                CHECK(projection->u.swizzle.swizzle[component] == (int)(4u - width + component));
            CHECK(projection->u.swizzle.sub->kind == AST_EXPR_CALL &&
                !strcmp(projection->u.swizzle.sub->u.call.name, "SourceElements.Load") &&
                projection->u.swizzle.sub->logical_origin.components == 4);
        }
    }
    /* Exact byte windows cannot cross the element, repeat/reverse components,
     * or consume an undefined or dead read lane. Partial stores remain out. */
    const struct { size_t word; uint32_t value; } negatives[] = {
        {31, 1}, {31, 16}, {31, UINT32_MAX}, {31, 8},
        {32, UINT32_C(0x00107e46)}, /* Offset four plus identity crosses stride. */
        {32, UINT32_C(0x00107006)}, /* Repeated memory words. */
        {32, UINT32_C(0x00107306)}, /* Reverse the first two retained words. */
        {26, UINT32_C(0x00100062)}, /* Missing w owner. */
        {35, UINT32_C(0x00100062)}, /* Retained w read becomes dead. */
        {38, 2}, {53, UINT32_C(0x0011e0e2)}, /* Missing SSA owner/partial store. */
        {23, UINT32_C(0x8b0020a7)}, /* Saturated load. */
        {34, INSTRUCTION(60, 10) | (UINT32_C(1) << 13)}
    };
    for (size_t n = 0; n < COUNT(negatives); ++n) {
        partial_structured_words(words, 3); words[negatives[n].word] = negatives[n].value;
        CHECK(typed_code(&fixture, words, COUNT(words)));
        CHECK(expect_failure(&fixture, &candidate, n == 11 ?
            COMPUTE_SOURCE_STAGE_CONTRACT_FAILED : COMPUTE_SOURCE_EMISSION_FAILED));
    }
    /* Duplicating the resource result through the binary operator retains
     * only one read instruction and must fail instead of creating two loads. */
    partial_structured_words(words, 3);
    words[34] = INSTRUCTION(60, 7); words[39] = UINT32_C(0x00100e46); words[40] = 1;
    memmove(words + 41, words + 44, (COUNT(words) - 44u) * sizeof(*words));
    CHECK(typed_code(&fixture, words, COUNT(words) - 3u));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* A retained MOV chain can supply the byte offset without turning the
     * byte coordinate into an element index or trusting a register spelling. */
    partial_structured_words(words, 3);
    uint32_t offset_words[COUNT(words) + 5];
    memcpy(offset_words, words, 23u * sizeof(*words));
    const uint32_t offset_definition[] = {INSTRUCTION(54, 5), UINT32_C(0x00100022),
        0u, UINT32_C(0x00004001), 4u};
    memcpy(offset_words + 23, offset_definition, sizeof(offset_definition));
    memcpy(offset_words + 28, words + 23, (COUNT(words) - 23u) * sizeof(*words));
    offset_words[35] = UINT32_C(0x0010001a); offset_words[36] = 0u;
    CHECK(typed_code(&fixture, offset_words, COUNT(offset_words)));
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(strstr(candidate.source.buf, ".yzw") && candidate.variants[0].memory_effect_count == 2);
    offset_words[27] = 8u;
    CHECK(typed_code(&fixture, offset_words, COUNT(offset_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    offset_words[27] = 4u; offset_words[36] = 1u; /* No preceding offset owner in r1. */
    CHECK(typed_code(&fixture, offset_words, COUNT(offset_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* Non-contiguous DXBC destinations may own naturally ordered distinct
     * memory words. Real use/def selections compress those words once. */
    partial_structured_words(words, 2);
    words[26] = UINT32_C(0x00100052); /* Load x,z. */
    words[31] = 4u; words[32] = UINT32_C(0x00107106); /* Physical y,z. */
    words[35] = UINT32_C(0x00100032); /* Operation writes x,y. */
    words[37] = UINT32_C(0x00100086); /* Its source reads x,z. */
    words[45] = UINT32_C(0x001000c2); /* Constant owns z,w. */
    CHECK(typed_code(&fixture, words, COUNT(words)));
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(strstr(candidate.source.buf, ".yz") && strstr(candidate.source.buf, "uint2(23u, 41u)"));
    CHECK(candidate.variants[0].entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    /* A producer cannot recur in separate constructor groups A/B/A/B,
     * including a pure literal producer; no per-lane expansion is permitted. */
    partial_structured_words(words, 2);
    words[26] = words[35] = UINT32_C(0x001000a2);
    words[32] = UINT32_C(0x00107406); /* Physical z,w for y,w. */
    words[45] = UINT32_C(0x00100052);
    CHECK(typed_code(&fixture, words, COUNT(words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    partial_structured_words(words, 3);
    CHECK(typed_code(&fixture, words, COUNT(words)));
    const ComputeShaderStringView renamed = text(&fixture, "RenamedProjectedElements");
    for (size_t k = 0; k < 2; ++k) for (size_t v = 0; v < 4; ++v) fixture.textures[k][v].name = renamed;
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(strstr(candidate.source.buf, "RenamedProjectedElements.Load") &&
        candidate.source_quality.classification == HLSL_SOURCE_QUALITY_MIXED);
    compute_source_candidate_dispose(&candidate);
    return true;
}

static bool check_owned_quality_resolver(void) {
    HLSLSourceQualityFacts facts;
    hlsl_source_quality_facts_init(&facts);
    ASTExpr *unknown = ast_create_emitter_operand("opaqueUnknown");
    CHECK(unknown && !hlsl_source_quality_owned_expression_facts(NULL, 0, unknown, &facts));
    ast_free_expr(unknown);
    ASTOperandProvenance provenance;
    ast_operand_provenance_init(&provenance);
    provenance.complete = true; provenance.value_role = AST_OPERAND_VALUE_LOGICAL;
    provenance.logical_value_id = 7; provenance.natural_components = provenance.result_components = 2;
    provenance.instruction_index = 3; provenance.source_instruction_index = 9;
    provenance.operand_index = 1; provenance.destination_lanes = 3;
    ASTExpr *owned = ast_create_emitter_operand_with_provenance("knownLogicalValue", &provenance);
    CHECK(owned && hlsl_source_quality_owned_expression_facts(NULL, 0, owned, &facts));
    CHECK(facts.known && facts.value_kind == HLSL_SOURCE_VALUE_LOGICAL && facts.components == 2);
    CHECK(facts.instruction_index == 3 && facts.source_instruction_index == 9);
    owned->operand_provenance.complete = false;
    hlsl_source_quality_facts_init(&facts);
    CHECK(!hlsl_source_quality_owned_expression_facts(NULL, 0, owned, &facts));
    ast_free_expr(owned);
    provenance.value_role = AST_OPERAND_VALUE_UNKNOWN;
    CHECK(ast_create_emitter_operand_with_provenance("untrusted", &provenance) == NULL);
    const uint32_t one = 1, two = 2;
    ASTExpr *left = ast_create_literal_bits(&one, 1, AST_SCALAR_UINT32);
    ASTExpr *right = ast_create_literal_bits(&two, 1, AST_SCALAR_UINT32);
    ASTExpr *operation = ast_create_binary(USIL_OP_IADD, left, right);
    CHECK(operation);
    const ASTExpr *roots[] = {operation};
    const HLSLSourceQualityUnit unit = {.source_unit_id = 0, .kind = HLSL_SOURCE_UNIT_ENTRY_POINT,
        .coverage_complete = true, .expressions = roots, .expression_count = 1};
    const HLSLSourceQualityRequest request = {.stage = DXBC_PROGRAM_TYPE_COMPUTE,
        .units = &unit, .unit_count = 1, .expected_unit_count = 1,
        .expression_facts = hlsl_source_quality_owned_expression_facts};
    HLSLSourceQualityResult result;
    CHECK(hlsl_source_quality_analyze(&request, &result));
    CHECK(result.classification != HLSL_SOURCE_QUALITY_CLEAN && result.counts.unknown_provenance == 1);
    ast_free_expr(operation);
    return true;
}

/* Independently varied address ownership cases qualify the initial same-UAV
 * boundary. Equal values do not replace the actual SSA coordinate contract. */
static bool check_same_uav_address_ownership(void) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Fixture fixture; CHECK(uav_read_fixture(&fixture));
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        uint32_t words[COUNT(uav_read_words) + 10];
        memcpy(words, uav_read_words, sizeof(uav_read_words));
        size_t count = COUNT(uav_read_words);
        if (scenario < 2) {
            /* Only one consumed coordinate lane receives a new SSA owner.
             * The other lane still has its original definition. */
            const uint32_t overwrite[] = {INSTRUCTION(30, 10),
                scenario ? UINT32_C(0x00100022) : UINT32_C(0x00100012), 0,
                UINT32_C(0x00100e46), 0, UINT32_C(0x00004002), 1, 2, 0, 0};
            memcpy(words + 29, overwrite, sizeof(overwrite));
            memcpy(words + 39, uav_read_words + 29, (count - 29) * sizeof(*words)); count += 10;
        } else if (scenario == 2) {
            /* A distinct MOV alias is not the unchanged address producer. */
            const uint32_t alias[] = {INSTRUCTION(54, 5), UINT32_C(0x00100032), 2,
                UINT32_C(0x00100e46), 0};
            words[8] = 3;
            memcpy(words + 29, alias, sizeof(alias));
            memcpy(words + 34, uav_read_words + 29, (count - 29) * sizeof(*words)); count += 5;
            words[48] = 2; /* Final store address r2.xy, read address r0.xy. */
        } else {
            /* The UAV result overwrites the original coordinate register.
             * Its resource-value owner cannot become the store address. */
            words[24] = 0; words[33] = 0;
        }
        CHECK(typed_code(&fixture, words, count));
        CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
        compute_source_candidate_dispose(&candidate);
    }
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        Fixture fixture; CHECK(uav_read_fixture(&fixture));
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        uint32_t words[80]; size_t count = 13;
        memcpy(words, uav_read_words, count * sizeof(*words));
        const bool immediate = scenario >= 2;
        if (immediate) {
            const uint32_t load[] = {INSTRUCTION(163, 10), UINT32_C(0x001000f2), 1,
                UINT32_C(0x00004002), 11, 17, 0, 0, UINT32_C(0x0011ee46), 0};
            memcpy(words + count, load, sizeof(load)); count += COUNT(load);
        } else {
            const uint32_t load[] = {INSTRUCTION(163, 6), UINT32_C(0x001000f2), 1,
                UINT32_C(0x00020046), UINT32_C(0x0011ee46), 0};
            memcpy(words + count, load, sizeof(load)); count += COUNT(load);
        }
        memcpy(words + count, uav_read_words + 29, 10 * sizeof(*words)); count += 10;
        if (immediate) {
            const uint32_t store[] = {INSTRUCTION(164, 10), UINT32_C(0x0011e0f2), 0,
                UINT32_C(0x00004002), scenario == 3 ? 12u : 11u, scenario == 4 ? 18u : 17u, 0, 0,
                UINT32_C(0x00100e46), 1};
            memcpy(words + count, store, sizeof(store)); count += COUNT(store);
        } else {
            const uint32_t store[] = {INSTRUCTION(164, 6), UINT32_C(0x0011e0f2), 0,
                scenario == 1 ? UINT32_C(0x00020016) : UINT32_C(0x00020046),
                UINT32_C(0x00100e46), 1};
            memcpy(words + count, store, sizeof(store)); count += COUNT(store);
        }
        words[count++] = INSTRUCTION(62, 1);
        CHECK(typed_code(&fixture, words, count));
        if (scenario == 1 || scenario >= 3) {
            /* Builtin xy/yx or one immediate coordinate word differs. */
            CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
        } else {
            CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
            CHECK(candidate.source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
            CHECK(candidate.variants[0].memory_effect_count == 2 && candidate.resource_count == 1);
            CHECK(strstr(candidate.source.buf, "OutputTexels.Load(int2("));
        }
        compute_source_candidate_dispose(&candidate);
    }
    return true;
}

static bool check_replicated_uav_address_lanes(void) {
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        Fixture fixture; CHECK(uav_read_fixture(&fixture));
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        uint32_t words[COUNT(uav_read_words) + 8];
        memcpy(words, uav_read_words, sizeof(uav_read_words));
        size_t count = COUNT(uav_read_words);
        /* One real IADD writes xyzw from threadId.xyyy + uint4(1,2,2,2).
         * The load selects xw (or xz); the store selects xy. */
        words[14] = UINT32_C(0x001000f2);
        words[16] = UINT32_C(0x00020546);
        words[20] = 2; words[21] = 2;
        words[25] = scenario == 1 ? UINT32_C(0x00100a86) : UINT32_C(0x00100fc6);
        if (scenario == 2) words[21] = 3; /* Unequal literal bits. */
        if (scenario == 3) words[16] = UINT32_C(0x00020946); /* W consumes threadId.z. */
        if (scenario == 4) words[14] = UINT32_C(0x00100072); /* W has no definition. */
        if (scenario == 5) {
            /* A new owner for only Y breaks the address despite equal bits. */
            const uint32_t overwrite[] = {INSTRUCTION(54, 8), UINT32_C(0x00100022), 0,
                UINT32_C(0x00004002), 0, 2, 0, 0};
            memcpy(words + 29, overwrite, sizeof(overwrite));
            memcpy(words + 37, uav_read_words + 29, (count - 29) * sizeof(*words)); count += 8;
        }
        if (scenario == 6) words[25] = UINT32_C(0x00100006); /* XX differs from XY. */
        if (scenario == 7 || scenario == 8) {
            words[13] = INSTRUCTION(41, 9);
            if (scenario == 8) words[21] = 3; /* Only the shift count differs. */
        }
        if (scenario == 9 || scenario == 10) {
            const uint32_t address[] = {
                INSTRUCTION(54, 4), UINT32_C(0x001000f2), 2, UINT32_C(0x00020546),
                INSTRUCTION(30, 10), UINT32_C(0x001000f2), 0,
                scenario == 9 ? UINT32_C(0x00100546) : UINT32_C(0x00100e46), 2,
                UINT32_C(0x00004002), 1, 2, 2, 2
            };
            /* The same selected TEMP input and SSA owner qualify. Different
             * selected input lanes remain outside this one-producer proof,
             * even when the earlier MOV happens to replicate their values. */
            words[8] = 3;
            memcpy(words + 13, address, sizeof(address));
            memcpy(words + 27, uav_read_words + 22, (count - 22) * sizeof(*words)); count += 5;
            words[30] = UINT32_C(0x00100fc6);
        }
        CHECK(typed_code(&fixture, words, count));
        if (scenario != 0 && scenario != 1 && scenario != 7 && scenario != 9) {
            CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
        } else {
            CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
            CHECK(candidate.source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                candidate.variant_count == 8 && candidate.variants[0].memory_effect_count == 2);
            CHECK(strstr(candidate.source.buf, "OutputTexels.Load(int2(") &&
                !strstr(candidate.source.buf, ".xwww") && !strstr(candidate.source.buf, ".xyzw"));
        }
        compute_source_candidate_dispose(&candidate);
    }
    return true;
}

/* Independent raw words describe a partial identity-selected typed read, one
 * UINT32 operation and a complementary constant definition. The final store
 * owns all four words, without inventing missing components of the read. */
static void partial_uav_words(uint32_t *words, unsigned width, bool suffix) {
    memcpy(words, uav_read_words, 39u * sizeof(*words));
    const unsigned first = suffix ? 4u - width : 0u;
    const uint8_t read_lanes = (uint8_t)(((1u << width) - 1u) << first);
    const uint8_t constant_lanes = (uint8_t)(15u ^ read_lanes);
    words[23] = UINT32_C(0x00100002) | (uint32_t)read_lanes << 4u;
    words[29] = INSTRUCTION(60, 10); /* OR retains unsigned raw words. */
    words[30] = words[23];
    const uint32_t constants[] = {
        INSTRUCTION(54, 8), UINT32_C(0x00100002) | (uint32_t)constant_lanes << 4u,
        1u, UINT32_C(0x00004002), UINT32_MAX, UINT32_C(0x80000000), 23u, 41u
    };
    memcpy(words + 39, constants, sizeof(constants));
    memcpy(words + 47, uav_read_words + 39,
        (COUNT(uav_read_words) - 39u) * sizeof(*words));
}

static bool check_partial_uav_candidate(void) {
    const char *prefix_selections[] = {NULL, ".x", ".xy", ".xyz"};
    const char *suffix_selections[] = {NULL, ".w", ".zw", ".yzw"};
    uint32_t words[COUNT(uav_read_words) + 8];
    for (unsigned width = 1; width <= 3; ++width) {
        for (unsigned suffix = 0; suffix < 2; ++suffix) {
            Fixture fixture;
            CHECK(uav_read_fixture(&fixture));
            partial_uav_words(words, width, suffix != 0);
            CHECK(typed_code(&fixture, words, COUNT(words)));
            ComputeSourceCandidate candidate;
            compute_source_candidate_init(&candidate);
            CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) ==
                COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
            CHECK(candidate.domain_complete && candidate.variant_count == 8 &&
                candidate.resource_count == 1 &&
                candidate.source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
            CHECK(strstr(candidate.source.buf, "RWTexture2D<uint4> OutputTexels;") &&
                strstr(candidate.source.buf, suffix ? suffix_selections[width] : prefix_selections[width]) &&
                strstr(candidate.source.buf, "uint4(") && !strstr(candidate.source.buf, ".xyzw"));
            for (size_t row = 0; row < candidate.variant_count; ++row) {
                const ComputeSourceVariant *entry = &candidate.variants[row];
                CHECK(entry->expression_count == 3 && entry->memory_effect_count == 2 &&
                    entry->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
                CHECK(entry->memory_effects[0].opcode == USIL_OP_LD_UAV_TYPED &&
                    entry->memory_effects[1].opcode == USIL_OP_STORE_UAV_TYPED &&
                    entry->memory_effects[0].instruction_index < entry->memory_effects[1].instruction_index);
                const ASTExpr *composition = entry->expressions[2];
                CHECK(composition->kind == AST_EXPR_CALL && !strcmp(composition->u.call.name, "uint4") &&
                    composition->u.call.arg_count == 2 && composition->logical_origin.complete &&
                    composition->logical_origin.scalar_type == AST_SCALAR_UINT32 &&
                    composition->logical_origin.components == 4);
                const ASTExpr *operation = composition->u.call.args[suffix ? 1 : 0];
                CHECK(operation->kind == AST_EXPR_BINARY && operation->u.binary.op == USIL_OP_OR &&
                    operation->logical_origin.components == width);
                const ASTExpr *projection = operation->u.binary.left;
                CHECK(projection->kind == AST_EXPR_SWIZZLE && projection->logical_origin.complete &&
                    projection->logical_origin.semantic_projection &&
                    projection->logical_origin.scalar_type == AST_SCALAR_UINT32 &&
                    projection->logical_origin.components == width &&
                    projection->u.swizzle.swizzle_count == (int)width &&
                    projection->logical_origin.instruction_index == entry->memory_effects[0].instruction_index);
                for (unsigned component = 0; component < width; ++component)
                    CHECK(projection->u.swizzle.swizzle[component] ==
                        (int)((suffix ? 4u - width : 0u) + component));
                const ASTExpr *load = projection->u.swizzle.sub;
                CHECK(load->kind == AST_EXPR_CALL && !strcmp(load->u.call.name, "OutputTexels.Load") &&
                    load->logical_origin.scalar_type == AST_SCALAR_UINT32 &&
                    load->logical_origin.components == 4 &&
                    load->logical_origin.destination_lanes == projection->logical_origin.destination_lanes);
            }
            StringBuilder before, after;
            sb_init(&before); sb_init(&after);
            ast_format_expr(candidate.variants[0].expressions[2], &before);
            memset(fixture.bytes, 0, sizeof(fixture.bytes));
            ast_format_expr(candidate.variants[0].expressions[2], &after);
            CHECK(sb_ok(&before) && sb_ok(&after) && before.len == after.len &&
                !memcmp(before.buf, after.buf, before.len));
            sb_free(&before); sb_free(&after);
            compute_source_candidate_dispose(&candidate);
        }
    }
    /* Begin each malformed case with a retained accepted candidate. Rejection
     * must clear its source, owned expressions and evidence transactionally. */
    const struct { size_t word; uint32_t value; } negatives[] = {
        {23, UINT32_C(0x00100082)}, /* Operation demands undefined Z. */
        {30, UINT32_C(0x00100082)}, /* Retained Z read becomes dead. */
        {40, UINT32_C(0x00100072)}, /* Constant definition overwrites Z. */
        {27, UINT32_C(0x0011e006)}, /* Broadcast resource words. */
        {27, UINT32_C(0x0011e1b6)}, /* Reverse resource order. */
        {27, UINT32_C(0x0011e046)}, /* Same component set with different identity. */
        {48, UINT32_C(0x0011e032)}, /* Partial final store. */
        {52, UINT32_C(0x00100546)}, /* Duplicate demanded loaded word. */
        {52, UINT32_C(0x00100b46)}, /* Reverse demanded loaded words. */
        {25, UINT32_C(0x00100416)}, /* Changed read coordinates. */
        {4, UINT32_C(0x5555)}, /* Float resource declaration. */
        {29, INSTRUCTION(0, 10)}, /* Float operator on UINT32 read. */
        {29, INSTRUCTION(60, 10) | (UINT32_C(1) << 13)} /* Saturated UINT32 operator. */
    };
    for (size_t index = 0; index < COUNT(negatives); ++index) {
        Fixture fixture;
        CHECK(uav_read_fixture(&fixture));
        partial_uav_words(words, 2, true);
        CHECK(typed_code(&fixture, words, COUNT(words)));
        ComputeSourceCandidate candidate;
        compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) ==
            COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        words[negatives[index].word] = negatives[index].value;
        CHECK(typed_code(&fixture, words, COUNT(words)));
        CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
        compute_source_candidate_dispose(&candidate);
    }
    Fixture fixture;
    CHECK(uav_read_fixture(&fixture));
    partial_uav_words(words, 2, true);
    /* Replicated address lanes are proved by the same existing SSA producer. */
    words[14] = UINT32_C(0x001000f2); words[16] = UINT32_C(0x00020546);
    words[20] = 2; words[21] = 2; words[25] = UINT32_C(0x00100fc6);
    CHECK(typed_code(&fixture, words, COUNT(words)));
    ComputeSourceCandidate candidate;
    compute_source_candidate_init(&candidate);
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) ==
        COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    words[21] = 3;
    CHECK(typed_code(&fixture, words, COUNT(words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    partial_uav_words(words, 2, true);
    /* One instruction read used on both sides of an operator is two expanded
     * effects, so direct instruction counts alone cannot grant admission. */
    words[29] = INSTRUCTION(60, 7); words[34] = UINT32_C(0x00100e46); words[35] = 1;
    memmove(words + 36, words + 39, (COUNT(words) - 39u) * sizeof(*words));
    CHECK(typed_code(&fixture, words, COUNT(words) - 3u));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    compute_source_candidate_dispose(&candidate);
    return true;
}

/* FLOAT data uses the current UAV declaration. Unsigned addresses remain a
 * separate SSA domain, including the existing same-producer replication proof. */
static void float_uav_words(uint32_t words[COUNT(uav_read_words)], const uint32_t bits[4]) {
    memcpy(words, uav_read_words, sizeof(uav_read_words));
    words[4] = UINT32_C(0x5555);
    words[29] = INSTRUCTION(0, 10);
    memcpy(words + 35, bits, 4 * sizeof(*bits));
}

static bool check_float_uav_candidate(void) {
    const uint32_t literals[][4] = {
        {UINT32_C(0x3e800000), UINT32_C(0x3f000000), UINT32_C(0x3f400000), UINT32_C(0x3f800000)},
        {UINT32_C(0x00000000), UINT32_C(0x80000000), UINT32_C(0x7f800000), UINT32_C(0x7fc00013)},
        {UINT32_C(0x00000001), UINT32_C(0xbf000000), UINT32_C(0x3eaaaaab), UINT32_C(0xff800000)}
    };
    for (size_t set = 0; set < COUNT(literals); ++set) {
        for (unsigned address = 0; address < 3; ++address) {
            Fixture fixture;
            CHECK(uav_read_fixture(&fixture));
            uint32_t words[COUNT(uav_read_words)];
            float_uav_words(words, literals[set]);
            size_t count = COUNT(words);
            if (address == 1) {
                words[14] = UINT32_C(0x001000f2);
                words[16] = UINT32_C(0x00020546);
                words[20] = words[21] = 2;
                words[25] = UINT32_C(0x00100fc6);
            } else if (address == 2) {
                memmove(words + 13, words + 22, (COUNT(words) - 22) * sizeof(*words));
                count -= 9;
                words[16] = words[33] = UINT32_C(0x00020046);
                memmove(words + 17, words + 18, (count - 18) * sizeof(*words)); --count;
                /* Removing the load's register word shifts the store token. */
                memmove(words + 33, words + 34, (count - 34) * sizeof(*words)); --count;
                words[13] = INSTRUCTION(163, 6);
                words[29] = INSTRUCTION(164, 6);
            }
            CHECK(typed_code(&fixture, words, count));
            ComputeSourceCandidate candidate;
            compute_source_candidate_init(&candidate);
            CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
            CHECK(candidate.resource_count == 1 && candidate.variant_count == 8 && candidate.domain_complete &&
                candidate.source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                candidate.resources[0].kind == COMPUTE_SOURCE_TEXTURE2D_FLOAT4 &&
                candidate.resources[0].original_element_type_known && candidate.resources[0].witness_count == 8);
            CHECK(strstr(candidate.source.buf, "RWTexture2D<float4> OutputTexels;") &&
                !strstr(candidate.source.buf, "asuint") && !strstr(candidate.source.buf, ".xyzw") &&
                !strstr(candidate.source.buf, ".xwww"));
            for (size_t row = 0; row < candidate.variant_count; ++row) {
                const ComputeSourceVariant *entry = &candidate.variants[row];
                CHECK(entry->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                    entry->expression_count == 3 && entry->memory_effect_count == 2);
                const ASTExpr *value = entry->expressions[2];
                CHECK(value->kind == AST_EXPR_BINARY && value->u.binary.op == USIL_OP_ADD &&
                    value->logical_origin.complete && value->logical_origin.scalar_type == AST_SCALAR_FLOAT32 &&
                    value->logical_origin.components == 4 && value->logical_origin.destination_lanes == 15);
                const ASTExpr *load = value->u.binary.left, *literal = value->u.binary.right;
                CHECK(load->kind == AST_EXPR_CALL && !strcmp(load->u.call.name, "OutputTexels.Load") &&
                    load->logical_origin.complete && load->logical_origin.scalar_type == AST_SCALAR_FLOAT32 &&
                    load->logical_origin.components == 4 && load->logical_origin.destination_lanes == 15 &&
                    load->logical_origin.instruction_index == entry->memory_effects[0].instruction_index);
                CHECK(literal->kind == AST_EXPR_LITERAL && literal->u.literal.scalar_type == AST_SCALAR_FLOAT32 &&
                    literal->u.literal.components == 4 && !memcmp(literal->u.literal.val, literals[set], sizeof(literals[set])));
                CHECK(load->u.call.args[0]->logical_origin.scalar_type == AST_SCALAR_SINT32);
                if (address < 2)
                    CHECK(entry->expressions[1]->logical_origin.complete &&
                        entry->expressions[1]->logical_origin.scalar_type == AST_SCALAR_UINT32);
            }
            StringBuilder before, after;
            sb_init(&before); sb_init(&after);
            ast_format_expr(candidate.variants[0].expressions[2], &before);
            memset(fixture.bytes, 0, sizeof(fixture.bytes));
            ast_format_expr(candidate.variants[0].expressions[2], &after);
            CHECK(sb_ok(&before) && sb_ok(&after) && before.len == after.len && !memcmp(before.buf, after.buf, before.len));
            sb_free(&before); sb_free(&after);
            compute_source_candidate_dispose(&candidate);
        }
    }
    const struct { size_t word; uint32_t value; } negatives[] = {
        {4, UINT32_C(0x4555)}, /* Mixed return tuple. */
        {4, UINT32_C(0x3333)}, /* Signed resource. */
        {29, INSTRUCTION(30, 10)}, /* UINT data cannot become FLOAT. */
        {13, INSTRUCTION(0, 9)}, /* FLOAT address cannot become UINT. */
        {29, INSTRUCTION(0, 10) | (UINT32_C(1) << 13)}, /* Saturation. */
        {29, INSTRUCTION(0, 10) | (UINT32_C(15) << 19)}, /* Precision. */
        {23, UINT32_C(0x00100032)}, /* Partial FLOAT read. */
        {30, UINT32_C(0x00100032)}, /* Partial FLOAT addition. */
        {40, UINT32_C(0x0011e032)}, /* Partial final store. */
        {32, UINT32_C(0x001001b6)}, /* Reverse FLOAT read components. */
        {44, UINT32_C(0x001001b6)}, /* Reverse final store components. */
        {25, UINT32_C(0x00100416)}, /* Changed physical address. */
        {1, INSTRUCTION(156, 4) | (3u << 11u) | (1u << 16u)} /* Coherence. */
    };
    for (size_t index = 0; index < COUNT(negatives); ++index) {
        Fixture fixture;
        CHECK(uav_read_fixture(&fixture));
        uint32_t words[COUNT(uav_read_words)]; float_uav_words(words, literals[0]);
        CHECK(typed_code(&fixture, words, COUNT(words)));
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        words[negatives[index].word] = negatives[index].value;
        CHECK(typed_code(&fixture, words, COUNT(words)));
        CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
        compute_source_candidate_dispose(&candidate);
    }
    for (unsigned order = 0; order < 2; ++order) {
        Fixture fixture; CHECK(uav_read_fixture(&fixture));
        uint32_t words[COUNT(uav_read_words)]; float_uav_words(words, literals[0]);
        CHECK(typed_code(&fixture, words, COUNT(words)));
        const uint8_t *float_code = fixture.variants[0][0].code;
        CHECK(typed_code(&fixture, uav_read_words, COUNT(uav_read_words)));
        const uint8_t *uint_code = fixture.variants[0][0].code;
        for (size_t k = 0; k < 2; ++k) for (size_t v = 0; v < 4; ++v)
            fixture.variants[k][v].code = order ? float_code : uint_code;
        fixture.variants[0][0].code = order ? uint_code : float_code;
        ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
        ComputeSourceDiagnostic diagnostic;
        CHECK(compute_source_candidate_build(&fixture.object, &candidate, &diagnostic) == COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE);
        CHECK(diagnostic.requested_variants == 8 && diagnostic.represented_variants == 1 &&
            !candidate.domain_complete && !candidate.source.buf && !candidate.variants);
        compute_source_candidate_dispose(&candidate);
    }
    Fixture fixture; CHECK(uav_read_fixture(&fixture));
    uint32_t words[COUNT(uav_read_words)]; float_uav_words(words, literals[0]);
    /* Commutativity does not grant a source operand-order rewrite. */
    words[32] = UINT32_C(0x00004002);
    memcpy(words + 33, literals[0], sizeof(literals[0]));
    words[37] = UINT32_C(0x00100e46); words[38] = 1;
    CHECK(typed_code(&fixture, words, COUNT(words)));
    ComputeSourceCandidate candidate; compute_source_candidate_init(&candidate);
    CHECK(compute_source_candidate_build(&fixture.object, &candidate, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    const ASTExpr *value = candidate.variants[0].expressions[2];
    CHECK(value->kind == AST_EXPR_BINARY && value->u.binary.op == USIL_OP_ADD &&
        value->u.binary.left->kind == AST_EXPR_LITERAL && value->u.binary.right->kind == AST_EXPR_CALL);
    /* A UINT address producer cannot become FLOAT data through a TEMP
     * identity. A retained effect is independently required to have one use. */
    float_uav_words(words, literals[0]);
    words[14] = UINT32_C(0x001000f2); words[16] = UINT32_C(0x00020546);
    words[20] = words[21] = 2; words[33] = 0;
    CHECK(typed_code(&fixture, words, COUNT(words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    float_uav_words(words, literals[0]);
    words[29] = INSTRUCTION(0, 7); words[34] = UINT32_C(0x00100e46); words[35] = 1;
    memmove(words + 36, words + 39, (COUNT(words) - 39u) * sizeof(*words));
    CHECK(typed_code(&fixture, words, COUNT(words) - 3u));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    compute_source_candidate_dispose(&candidate);
    return true;
}

/* Literal-only scalar atomic grammar, authored through the same complete
 * token/container path as the existing typed memory fixtures. */
static const uint32_t atomic_candidate_words[] = {
    INSTRUCTION(106, 1) | (1u << 11u),
    INSTRUCTION(156, 4) | (3u << 11u), UINT32_C(0x0011e000), 0u, UINT32_C(0x4444),
    INSTRUCTION(155, 4), 1u, 1u, 1u,
    INSTRUCTION(173, 10), UINT32_C(0x0011e000), 0u,
    UINT32_C(0x00004002), 0u, 0u, 0u, 0u,
    UINT32_C(0x00004001), 1u,
    INSTRUCTION(62, 1)
};

static bool atomic_candidate_fixture_at_group(Fixture *fixture, const uint32_t *words, size_t count,
                                              size_t group_offset) {
    CHECK(group_offset < count && count - group_offset >= 3u);
    CHECK(fixture_init(fixture, NULL, 0));
    CHECK(typed_code(fixture, words, count));
    const ComputeShaderStringView name = text(fixture, "AtomicCounts");
    for (size_t kernel = 0; kernel < 2; ++kernel) for (size_t variant = 0; variant < 4; ++variant) {
        for (size_t axis = 0; axis < 3; ++axis) fixture->groups[kernel][variant][axis] = words[group_offset + axis];
        fixture->outputs[kernel][variant] = (ComputeShaderResource){.name = name,
            .bind_point = (int)words[3], .sampler_bind_point = -1, .texture_dimension = 2};
        fixture->variants[kernel][variant].output_buffers = &fixture->outputs[kernel][variant];
        fixture->variants[kernel][variant].output_buffer_count = 1;
    }
    return true;
}

static bool atomic_candidate_fixture(Fixture *fixture, const uint32_t *words, size_t count) {
    return atomic_candidate_fixture_at_group(fixture, words, count, 6u);
}

static bool check_atomic_candidate_evidence(const ComputeSourceCandidate *candidate,
                                           uint32_t binding, uint32_t increment, uint32_t x, uint32_t y) {
    CHECK(candidate->status == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED && candidate->domain_complete &&
        candidate->kernel_count == 2u && candidate->variant_count == 8u && candidate->resource_count == 1u);
    CHECK(candidate->source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
        !candidate->source_quality.reasons && !candidate->source_quality.counts.incomplete_units &&
        !candidate->source_quality.counts.unknown_provenance && !candidate->source_quality.counts.residual_total);
    const ComputeSourceTypedResource *resource = &candidate->resources[0];
    CHECK(resource->kind == COMPUTE_SOURCE_TEXTURE2D_UINT_SCALAR_ATOMIC &&
        resource->writable && resource->binding_register == binding &&
        resource->original_element_type_known && resource->witness_count == 8u);
    char declaration[96];
    const int declaration_size = snprintf(declaration, sizeof(declaration),
        "RWTexture2D<uint> AtomicCounts : register(u%u);\n", binding);
    CHECK(declaration_size > 0 && (size_t)declaration_size < sizeof(declaration));
    CHECK(strstr(candidate->source.buf, declaration) &&
        strstr(candidate->source.buf, "InterlockedAdd(") && !strstr(candidate->source.buf, "].x") &&
        !strstr(candidate->source.buf, "dispatchThreadId") && !strstr(candidate->source.buf, "uint4"));
    for (size_t row = 0; row < candidate->variant_count; ++row) {
        const ComputeSourceVariant *entry = &candidate->variants[row];
        CHECK(resource->variant_witnesses[row] == (uint32_t)row && entry->source_unit_id == (uint32_t)row + 1u &&
            entry->kernel_index == row / 4u && entry->variant_index == row % 4u);
        CHECK(entry->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
            !entry->entry_quality.reasons && !entry->entry_quality.counts.incomplete_units &&
            !entry->entry_quality.counts.unknown_provenance && !entry->entry_quality.counts.residual_total);
        CHECK(entry->expression_count == 3u && entry->memory_effect_count == 1u);
        const ComputeSourceMemoryEffect *effect = &entry->memory_effects[0];
        CHECK(effect->opcode == USIL_OP_ATOMIC_IADD && effect->instruction_index == 0 &&
            effect->source_instruction_index == 3u && effect->binding_register == binding &&
            effect->effect_flags == (uint32_t)(USIL_EFFECT_ATOMIC | USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_EXTERNAL_WRITE));
        CHECK(entry->expressions[0]->kind == AST_EXPR_EMITTER_OPERAND &&
            entry->expressions[0]->operand_provenance.complete &&
            entry->expressions[0]->operand_provenance.instruction_index == 0 &&
            entry->expressions[0]->operand_provenance.source_instruction_index == 3u &&
            entry->expressions[0]->operand_provenance.operand_index == 0);
        for (size_t root = 1; root < 3; ++root) CHECK(entry->expressions[root]->logical_origin.complete &&
            entry->expressions[root]->logical_origin.scalar_type == (root == 1u ? AST_SCALAR_SINT32 : AST_SCALAR_UINT32) &&
            entry->expressions[root]->logical_origin.components == (root == 1u ? 2u : 1u) &&
            entry->expressions[root]->logical_origin.destination_lanes == (root == 1u ? 3u : 1u) &&
            entry->expressions[root]->logical_origin.instruction_index == 0 &&
            entry->expressions[root]->logical_origin.source_instruction_index == 3u);
        const ASTExpr *coordinates = entry->expressions[1];
        CHECK(coordinates->kind == AST_EXPR_LITERAL && coordinates->u.literal.scalar_type == AST_SCALAR_SINT32 &&
            coordinates->u.literal.components == 2 && coordinates->u.literal.val[0] == x && coordinates->u.literal.val[1] == y);
        const ASTExpr *value = entry->expressions[2];
        CHECK(value->kind == AST_EXPR_LITERAL && value->u.literal.scalar_type == AST_SCALAR_UINT32 &&
            value->u.literal.components == 1 && value->u.literal.val[0] == increment);
        size_t effect_events = 0;
        for (size_t event = 0; event < entry->emission_fact_count; ++event) {
            const HLSLSourceQualityFacts *fact = &entry->emission_facts[event];
            if (fact->instruction_index < 0) continue;
            CHECK(effect_events < 2u && fact->known && fact->logical_operation && !fact->artifacts &&
                fact->instruction_index == (int)effect_events && fact->source_instruction_index == (uint32_t)effect_events + 3u);
            ++effect_events;
        }
        CHECK(effect_events == 2u);
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(candidate->source.buf, candidate->source.len, digest);
    CHECK(!memcmp(digest, candidate->source_sha256, sizeof(digest)));
    return true;
}

static bool check_literal_atomic_candidate(void) {
    const uint32_t configurations[][7] = {
        /* x,y,value,groupX,groupY,groupZ,binding */
        {0u, 0u, 1u, 1u, 1u, 1u, 0u},
        {3u, 5u, UINT32_MAX, 4u, 2u, 1u, 0u},
        {2u, 7u, 0u, 8u, 4u, 1u, 3u}
    };
    for (size_t configuration = 0; configuration < COUNT(configurations); ++configuration) {
        uint32_t words[COUNT(atomic_candidate_words)]; memcpy(words, atomic_candidate_words, sizeof(words));
        const uint32_t *values = configurations[configuration];
        words[13] = values[0]; words[14] = values[1]; words[18] = values[2];
        words[6] = values[3]; words[7] = values[4]; words[8] = values[5]; words[3] = words[11] = values[6];
        Fixture fixture; CHECK(atomic_candidate_fixture(&fixture, words, COUNT(words)));
        ComputeSourceCandidate first, second;
        ComputeSourceDiagnostic diagnostic;
        compute_source_candidate_init(&first); compute_source_candidate_init(&second);
        CHECK(compute_source_candidate_build(&fixture.object, &first, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        CHECK(diagnostic.requested_counts_known && diagnostic.requested_kernels == 2u &&
            diagnostic.requested_variants == 8u && diagnostic.examined_variants == 8u && diagnostic.represented_variants == 8u);
        CHECK(check_atomic_candidate_evidence(&first, values[6], values[2], values[0], values[1]));
        CHECK(compute_source_candidate_build(&fixture.object, &second, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        CHECK(first.source.len == second.source.len && !memcmp(first.source.buf, second.source.buf, first.source.len) &&
            hlsl_source_quality_results_equal(&first.source_quality, &second.source_quality));
        CHECK(!memcmp(first.modeled_input_sha256, second.modeled_input_sha256, COMMON_SHA256_DIGEST_SIZE));
        for (size_t row = 0; row < first.variant_count; ++row) CHECK(
            first.variants[row].expressions != second.variants[row].expressions &&
            first.variants[row].expressions[0] != second.variants[row].expressions[0] &&
            hlsl_source_quality_results_equal(&first.variants[row].entry_quality, &second.variants[row].entry_quality));
        memset(fixture.bytes, 0, sizeof(fixture.bytes));
        CHECK(check_atomic_candidate_evidence(&first, values[6], values[2], values[0], values[1]));
        StringBuilder held; sb_init(&held); ast_format_expr(first.variants[7].expressions[0], &held);
        CHECK(sb_ok(&held) && strstr(held.buf, "AtomicCounts")); sb_free(&held);
        compute_source_candidate_dispose(&first); compute_source_candidate_dispose(&second);
    }
    Fixture fixture; CHECK(atomic_candidate_fixture(&fixture, atomic_candidate_words, COUNT(atomic_candidate_words)));
    ComputeSourceCandidate baseline, renamed; compute_source_candidate_init(&baseline); compute_source_candidate_init(&renamed);
    /* Both candidate names belong to the same held serialized span before
     * hashing it; only the modeled selection changes between these builds. */
    const ComputeShaderStringView changed_name = text(&fixture, "RenamedCounts");
    CHECK(compute_source_candidate_build(&fixture.object, &baseline, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    for (size_t kernel = 0; kernel < 2; ++kernel) for (size_t variant = 0; variant < 4; ++variant)
        fixture.outputs[kernel][variant].name = changed_name;
    CHECK(compute_source_candidate_build(&fixture.object, &renamed, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(memcmp(baseline.modeled_input_sha256, renamed.modeled_input_sha256, COMMON_SHA256_DIGEST_SIZE) &&
        !memcmp(baseline.serialized_object_sha256, renamed.serialized_object_sha256, COMMON_SHA256_DIGEST_SIZE) &&
        strstr(baseline.source.buf, "AtomicCounts") && strstr(renamed.source.buf, "RenamedCounts"));
    compute_source_candidate_dispose(&renamed);
    const ComputeShaderResource original = fixture.outputs[0][0];
    fixture.outputs[0][0].texture_dimension = -1;
    CHECK(expect_failure(&fixture, &baseline, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][0] = original;
    fixture.outputs[0][0].bind_point = 1;
    CHECK(expect_failure(&fixture, &baseline, COMPUTE_SOURCE_EMISSION_FAILED)); fixture.outputs[0][0] = original;
    fixture.outputs[0][0].sampler_bind_point = 0;
    CHECK(expect_failure(&fixture, &baseline, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][0] = original;
    fixture.outputs[0][0].generated_name = changed_name;
    CHECK(expect_failure(&fixture, &baseline, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][0] = original;
    fixture.outputs[0][0].name = fixture.global[0][0];
    CHECK(expect_failure(&fixture, &baseline, COMPUTE_SOURCE_NAME_UNREPRESENTABLE)); fixture.outputs[0][0] = original;
    fixture.outputs[0][1].name = text(&fixture, "SiblingConflict");
    CHECK(expect_failure(&fixture, &baseline, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE)); fixture.outputs[0][1] = original;
    /* Genuine token mutations stay in the complete requested row domain;
     * a rejected body cannot become an absent successful variant. */
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        uint32_t words[COUNT(atomic_candidate_words) + 1u]; memcpy(words, atomic_candidate_words, sizeof(atomic_candidate_words));
        size_t count = COUNT(atomic_candidate_words);
        switch (mutation) {
            case 0: words[4] = UINT32_C(0x3333); break; /* Signed resource. */
            case 1: words[1] = INSTRUCTION(156, 4) | (5u << 11u); break; /* 3D. */
            case 2: words[1] |= 1u << 16u; break; /* Globally coherent. */
            case 3: words[15] = 1u; break; /* Unused address word cannot disappear. */
            case 4: words[6] = 0u; break; /* Invalid execution contract. */
            case 5: words[9] = INSTRUCTION(171, 10); break; /* Another actual atomic opcode. */
            case 6:
                words[count] = words[count - 1u]; words[count - 1u] = INSTRUCTION(190, 1) | (8u << 11u);
                ++count; break;
        }
        CHECK(atomic_candidate_fixture(&fixture, words, count));
        CHECK(expect_failure(&fixture, &baseline,
            mutation == 4u ? COMPUTE_SOURCE_STAGE_CONTRACT_FAILED : COMPUTE_SOURCE_EMISSION_FAILED));
    }
    compute_source_candidate_dispose(&baseline);
    return true;
}

static bool check_atomic_descriptor_boundary(void) {
    Fixture fixture; CHECK(uav_read_fixture(&fixture));
    DXBCDocument document; dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract; dxbc_stage_contract_init(&contract);
    USILProgram program = {0};
    CHECK(dxbc_document_parse(&document, fixture.variants[0][0].code, fixture.variants[0][0].code_size, NULL));
    CHECK(dxbc_document_decode_semantic(&document, &semantic));
    CHECK(dxbc_stage_contract_decode(&document, &semantic, &contract, NULL));
    CHECK(usil_translate_with_stage_contract(&program, &semantic, &contract));
    HLSLComputeTypedResource resource = {.name = "OutputTexels", .binding_register = 0,
        .writable = true, .scalar_type = AST_SCALAR_UINT32};
    HLSLComputeTypedSource typed = {.resources = &resource, .resource_count = 1u, .emit_declarations = true};
    HLSLSourceQualityResult quality;
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT; options.source_quality = &quality;
    HLSLEmitDiagnostic diagnostic;
    StringBuilder source; sb_init(&source);
    CHECK(hlsl_emit_compute_typed_stage(&program, &source, NULL, &options, &typed, &diagnostic));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && strstr(source.buf, "RWTexture2D<uint4>"));
    sb_free(&source);
    /* Identical 4444 return formats do not authorize a scalar view of an
     * ordinary load/store program. The whole atomic-use proof is required. */
    resource.scalar_atomic = true;
    sb_init(&source); sb_append(&source, "prefix");
    CHECK(!hlsl_emit_compute_typed_stage(&program, &source, NULL, &options, &typed, &diagnostic));
    CHECK(source.failed && source.len == 6u && !strcmp(source.buf, "prefix") &&
        diagnostic.status != HLSL_EMIT_STATUS_OK && quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    usil_free(&program); dxbc_stage_contract_free(&contract); dxbc_free(&semantic); dxbc_document_free(&document);
    /* Each row must authorize the same declaration. The same UINT return
     * formats, name and binding cannot merge a scalar atomic resource view
     * with a genuinely decoded four-component load/store sibling. */
    CHECK(atomic_candidate_fixture(&fixture, atomic_candidate_words, COUNT(atomic_candidate_words)));
    ComputeSourceCandidate held, restored; compute_source_candidate_init(&held); compute_source_candidate_init(&restored);
    CHECK(compute_source_candidate_build(&fixture.object, &held, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    const uint8_t *atomic_code = fixture.variants[0][0].code;
    const size_t atomic_size = fixture.variants[0][0].code_size;
    CHECK(typed_code(&fixture, uav_read_words, COUNT(uav_read_words)));
    const uint8_t *legacy_code = fixture.variants[0][0].code;
    const size_t legacy_size = fixture.variants[0][0].code_size;
    for (size_t kernel = 0; kernel < 2; ++kernel) for (size_t variant = 0; variant < 4; ++variant) {
        fixture.variants[kernel][variant].code = atomic_code;
        fixture.variants[kernel][variant].code_size = atomic_size;
    }
    fixture.variants[0][3].code = legacy_code; fixture.variants[0][3].code_size = legacy_size;
    fixture.groups[0][3][0] = fixture.groups[0][3][1] = 4u;
    CHECK(expect_failure(&fixture, &held, COMPUTE_SOURCE_RESOURCE_INVERSE_UNAVAILABLE));
    fixture.variants[0][3].code = atomic_code; fixture.variants[0][3].code_size = atomic_size;
    fixture.groups[0][3][0] = fixture.groups[0][3][1] = 1u;
    CHECK(compute_source_candidate_build(&fixture.object, &restored, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(held.source.len == restored.source.len && !memcmp(held.source.buf, restored.source.buf, held.source.len) &&
        hlsl_source_quality_results_equal(&held.source_quality, &restored.source_quality));
    compute_source_candidate_dispose(&held); compute_source_candidate_dispose(&restored);
    return true;
}

static const uint32_t dispatch_atomic_candidate_words[] = {
    INSTRUCTION(106, 1) | (1u << 11u),
    INSTRUCTION(156, 4) | (3u << 11u), UINT32_C(0x0011e000), 0u, UINT32_C(0x4444),
    INSTRUCTION(95, 2), UINT32_C(0x00020032),
    INSTRUCTION(155, 4), 1u, 1u, 1u,
    INSTRUCTION(173, 6), UINT32_C(0x0011e000), 0u,
    UINT32_C(0x00020046), UINT32_C(0x00004001), 1u,
    INSTRUCTION(62, 1)
};

static bool check_dispatch_atomic_candidate_evidence(const ComputeSourceCandidate *candidate,
                                                     uint32_t binding, uint32_t increment) {
    CHECK(candidate->status == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED && candidate->domain_complete &&
        candidate->kernel_count == 2u && candidate->variant_count == 8u && candidate->resource_count == 1u);
    CHECK(candidate->source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !candidate->source_quality.reasons &&
        !candidate->source_quality.counts.incomplete_units && !candidate->source_quality.counts.unknown_provenance &&
        !candidate->source_quality.counts.residual_total && candidate->source_quality.counts.semantic_projections);
    const ComputeSourceTypedResource *resource = &candidate->resources[0];
    CHECK(resource->kind == COMPUTE_SOURCE_TEXTURE2D_UINT_SCALAR_ATOMIC && resource->writable &&
        resource->binding_register == binding && resource->original_element_type_known && resource->witness_count == 8u);
    char declaration[96];
    const int declaration_size = snprintf(declaration, sizeof(declaration),
        "RWTexture2D<uint> AtomicCounts : register(u%u);\n", binding);
    CHECK(declaration_size > 0 && (size_t)declaration_size < sizeof(declaration));
    CHECK(strstr(candidate->source.buf, declaration) &&
        strstr(candidate->source.buf, "uint3 dispatchThreadId : SV_DispatchThreadID") &&
        strstr(candidate->source.buf, "InterlockedAdd((AtomicCounts)[int2((dispatchThreadId.xy))], ") &&
        !strstr(candidate->source.buf, "].x") && !strstr(candidate->source.buf, "uint4") && !strstr(candidate->source.buf, "asint("));
    for (size_t row = 0; row < candidate->variant_count; ++row) {
        const ComputeSourceVariant *entry = &candidate->variants[row];
        CHECK(resource->variant_witnesses[row] == (uint32_t)row && entry->source_unit_id == (uint32_t)row + 1u &&
            entry->kernel_index == row / 4u && entry->variant_index == row % 4u && entry->expression_count == 3u &&
            entry->memory_effect_count == 1u && entry->entry_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
            !entry->entry_quality.reasons && !entry->entry_quality.counts.incomplete_units &&
            !entry->entry_quality.counts.residual_total && !entry->entry_quality.counts.unknown_provenance);
        const ComputeSourceMemoryEffect *effect = &entry->memory_effects[0];
        CHECK(effect->opcode == USIL_OP_ATOMIC_IADD && effect->instruction_index == 0 &&
            effect->source_instruction_index == 4u && effect->binding_register == binding &&
            effect->effect_flags == (uint32_t)(USIL_EFFECT_ATOMIC | USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_EXTERNAL_WRITE));
        const ASTExpr *aggregate = entry->expressions[0];
        CHECK(aggregate->kind == AST_EXPR_EMITTER_OPERAND && aggregate->operand_provenance.complete &&
            aggregate->operand_provenance.instruction_index == 0 && aggregate->operand_provenance.source_instruction_index == 4u &&
            aggregate->operand_provenance.operand_index == 0);
        const ASTExpr *coordinates = entry->expressions[1];
        CHECK(coordinates->kind == AST_EXPR_CALL && !strcmp(coordinates->u.call.name, "int2") &&
            coordinates->u.call.arg_count == 1 && coordinates->u.call.args && coordinates->logical_origin.complete &&
            coordinates->logical_origin.scalar_type == AST_SCALAR_SINT32 && coordinates->logical_origin.components == 2u &&
            coordinates->logical_origin.destination_lanes == 3u && coordinates->logical_origin.instruction_index == 0 &&
            coordinates->logical_origin.logical_value_id == 0u && coordinates->logical_origin.source_instruction_index == 4u);
        const ASTExpr *dispatch = coordinates->u.call.args[0];
        const ASTOperandProvenance *origin = &dispatch->operand_provenance;
        CHECK(dispatch->kind == AST_EXPR_EMITTER_OPERAND && !strcmp(dispatch->u.emitter_operand, "dispatchThreadId.xy") &&
            origin->complete && origin->value_role == AST_OPERAND_VALUE_LOGICAL &&
            origin->logical_value_id == (UINT64_C(0x100000000) | USIL_COMPUTE_DISPATCH_THREAD_ID) &&
            origin->natural_components == 3u && origin->result_components == 2u &&
            origin->selection_role == AST_COMPONENT_SELECTION_SEMANTIC && origin->selected_components[0] == 0u &&
            origin->selected_components[1] == 1u && origin->bitcast_role == AST_OPERAND_BITCAST_NONE &&
            !origin->raw_buffer_reconstruction && !origin->synthetic_interface &&
            origin->instruction_index == 0 && origin->source_instruction_index == 4u &&
            origin->operand_index == 1 && origin->destination_lanes == 3u);
        const ASTExpr *value = entry->expressions[2];
        CHECK(value->kind == AST_EXPR_LITERAL && value->u.literal.scalar_type == AST_SCALAR_UINT32 &&
            value->u.literal.components == 1 && value->u.literal.val[0] == increment && value->logical_origin.complete &&
            value->logical_origin.scalar_type == AST_SCALAR_UINT32 && value->logical_origin.components == 1u &&
            value->logical_origin.destination_lanes == 1u && value->logical_origin.instruction_index == 0 &&
            value->logical_origin.source_instruction_index == 4u);
        size_t effects = 0;
        for (size_t event = 0; event < entry->emission_fact_count; ++event) {
            const HLSLSourceQualityFacts *fact = &entry->emission_facts[event];
            if (fact->instruction_index < 0) continue;
            CHECK(effects < 2u && fact->known && fact->logical_operation && !fact->artifacts &&
                fact->instruction_index == (int)effects && fact->source_instruction_index == (uint32_t)effects + 4u);
            ++effects;
        }
        CHECK(effects == 2u);
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(candidate->source.buf, candidate->source.len, digest);
    CHECK(!memcmp(digest, candidate->source_sha256, sizeof(digest)));
    return true;
}

static bool check_dispatch_atomic_candidate(void) {
    const uint32_t configurations[][5] = {
        /* value,groupX,groupY,groupZ,binding */
        {1u, 1u, 1u, 1u, 0u}, {UINT32_MAX, 4u, 2u, 1u, 3u}
    };
    for (size_t configuration = 0; configuration < COUNT(configurations); ++configuration) {
        uint32_t words[COUNT(dispatch_atomic_candidate_words)];
        memcpy(words, dispatch_atomic_candidate_words, sizeof(words));
        const uint32_t *values = configurations[configuration];
        words[16] = values[0]; words[8] = values[1]; words[9] = values[2]; words[10] = values[3];
        words[3] = words[13] = values[4];
        Fixture fixture; CHECK(atomic_candidate_fixture_at_group(&fixture, words, COUNT(words), 8u));
        ComputeSourceCandidate first, second; compute_source_candidate_init(&first); compute_source_candidate_init(&second);
        ComputeSourceDiagnostic diagnostic;
        CHECK(compute_source_candidate_build(&fixture.object, &first, &diagnostic) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        CHECK(diagnostic.requested_counts_known && diagnostic.requested_kernels == 2u && diagnostic.requested_variants == 8u &&
            diagnostic.examined_variants == 8u && diagnostic.represented_variants == 8u);
        CHECK(check_dispatch_atomic_candidate_evidence(&first, values[4], values[0]));
        CHECK(compute_source_candidate_build(&fixture.object, &second, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
        CHECK(first.source.len == second.source.len && !memcmp(first.source.buf, second.source.buf, first.source.len) &&
            hlsl_source_quality_results_equal(&first.source_quality, &second.source_quality) &&
            !memcmp(first.modeled_input_sha256, second.modeled_input_sha256, COMMON_SHA256_DIGEST_SIZE));
        for (size_t row = 0; row < first.variant_count; ++row) CHECK(
            first.variants[row].expressions[1] != second.variants[row].expressions[1] &&
            first.variants[row].expressions[1]->u.call.args[0] != second.variants[row].expressions[1]->u.call.args[0] &&
            hlsl_source_quality_results_equal(&first.variants[row].entry_quality, &second.variants[row].entry_quality));
        memset(fixture.bytes, 0, sizeof(fixture.bytes));
        CHECK(check_dispatch_atomic_candidate_evidence(&first, values[4], values[0]));
        StringBuilder held; sb_init(&held); ast_format_expr(first.variants[7].expressions[1], &held);
        CHECK(sb_ok(&held) && !strcmp(held.buf, "int2((dispatchThreadId.xy))")); sb_free(&held);
        compute_source_candidate_dispose(&first); compute_source_candidate_dispose(&second);
    }
    Fixture fixture;
    CHECK(atomic_candidate_fixture_at_group(&fixture, dispatch_atomic_candidate_words, COUNT(dispatch_atomic_candidate_words), 8u));
    ComputeSourceCandidate baseline, restored; compute_source_candidate_init(&baseline); compute_source_candidate_init(&restored);
    CHECK(compute_source_candidate_build(&fixture.object, &baseline, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    const uint8_t *original_code = fixture.variants[0][0].code;
    const size_t original_size = fixture.variants[0][0].code_size;
    /* Keep seven admitted rows and one rejected final row. Requested coverage
     * is still eight; partial construction may not modify the held candidate. */
    uint32_t changed[COUNT(dispatch_atomic_candidate_words)];
    memcpy(changed, dispatch_atomic_candidate_words, sizeof(changed)); changed[14] = UINT32_C(0x00020016);
    CHECK(typed_code(&fixture, changed, COUNT(changed)));
    const uint8_t *changed_code = fixture.variants[0][0].code;
    const size_t changed_size = fixture.variants[0][0].code_size;
    for (size_t kernel = 0; kernel < 2; ++kernel) for (size_t variant = 0; variant < 4; ++variant) {
        fixture.variants[kernel][variant].code = original_code;
        fixture.variants[kernel][variant].code_size = original_size;
    }
    fixture.variants[1][3].code = changed_code; fixture.variants[1][3].code_size = changed_size;
    const ComputeSourceCandidate before = baseline;
    ComputeSourceDiagnostic diagnostic;
    CHECK(compute_source_candidate_build(&fixture.object, &baseline, &diagnostic) == COMPUTE_SOURCE_EMISSION_FAILED);
    CHECK(!memcmp(&before, &baseline, sizeof(before)) && diagnostic.requested_counts_known &&
        diagnostic.requested_kernels == 2u && diagnostic.requested_variants == 8u && diagnostic.examined_variants == 8u &&
        diagnostic.represented_variants == 7u && diagnostic.kernel_index == 1u && diagnostic.variant_index == 3u);
    CHECK(check_dispatch_atomic_candidate_evidence(&baseline, 0u, 1u));
    fixture.variants[1][3].code = original_code; fixture.variants[1][3].code_size = original_size;
    CHECK(compute_source_candidate_build(&fixture.object, &restored, NULL) == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(restored.source.len == baseline.source.len && !memcmp(restored.source.buf, baseline.source.buf, baseline.source.len) &&
        hlsl_source_quality_results_equal(&restored.source_quality, &baseline.source_quality));
    compute_source_candidate_dispose(&restored);
    /* The shared memory projection rejects undeclared reads before emitter
     * admission. Other rows have valid decoded operands but lie outside the
     * deliberately narrow DispatchXY producer contract. */
    static const ComputeSourceStatus failures[] = {
        COMPUTE_SOURCE_EMISSION_FAILED, COMPUTE_SOURCE_STAGE_CONTRACT_FAILED,
        COMPUTE_SOURCE_EMISSION_FAILED, COMPUTE_SOURCE_EMISSION_FAILED,
        COMPUTE_SOURCE_EMISSION_FAILED, COMPUTE_SOURCE_EMISSION_FAILED,
        COMPUTE_SOURCE_EMISSION_FAILED, COMPUTE_SOURCE_STAGE_CONTRACT_FAILED
    };
    for (unsigned mutation = 0; mutation < 8u; ++mutation) {
        uint32_t words[COUNT(dispatch_atomic_candidate_words) + 4u];
        memcpy(words, dispatch_atomic_candidate_words, sizeof(dispatch_atomic_candidate_words));
        size_t count = COUNT(dispatch_atomic_candidate_words);
        switch (mutation) {
            case 0: words[6] = UINT32_C(0x00020072); break; /* Undemanded Z declaration is outside this exact route. */
            case 1: words[14] = UINT32_C(0x00020086); break; /* XZ is not the declared XY address. */
            case 2: words[14] = UINT32_C(0x00020006); break; /* Replicated X. */
            case 3: words[6] = UINT32_C(0x00021032); words[14] = UINT32_C(0x00021046); break; /* A different builtin. */
            case 4:
                memmove(words + 9, words + 7, (count - 7u) * sizeof(words[0]));
                words[7] = INSTRUCTION(95, 2); words[8] = UINT32_C(0x00021032); count += 2u; break;
            case 5:
                words[count] = words[count - 1u]; words[count - 1u] = INSTRUCTION(190, 1) | (8u << 11u); ++count; break;
            case 6:
                words[11] = INSTRUCTION(173, 10); words[14] = UINT32_C(0x00004002);
                words[15] = words[16] = words[17] = words[18] = 0u;
                words[19] = UINT32_C(0x00004001); words[20] = 1u; words[21] = INSTRUCTION(62, 1); count += 4u; break;
            case 7:
                memmove(words + 5, words + 7, (count - 7u) * sizeof(words[0]));
                count -= 2u; break; /* The actual address now lacks its declaration. */
        }
        /* Added declarations move numthreads; the remaining authored shapes
         * retain group words[8..10]. No parser or count-derived fallback is used. */
        CHECK(atomic_candidate_fixture_at_group(&fixture, words, count,
            mutation == 4u ? 10u : mutation == 7u ? 6u : 8u));
        const ComputeSourceCandidate original = baseline;
        ComputeSourceDiagnostic boundary;
        const ComputeSourceStatus status = compute_source_candidate_build(&fixture.object, &baseline, &boundary);
        if (status != failures[mutation]) fprintf(stderr,
            "dispatch atomic candidate boundary mutation=%u expected=%s actual=%s\n", mutation,
            compute_source_status_name(failures[mutation]), compute_source_status_name(status));
        CHECK(status == failures[mutation] && boundary.status == status && !memcmp(&original, &baseline, sizeof(original)));
        CHECK(boundary.requested_counts_known && boundary.requested_kernels == 2u && boundary.requested_variants == 8u &&
            boundary.examined_variants == 1u && !boundary.represented_variants &&
            boundary.kernel_index == 0u && boundary.variant_index == 0u);
        CHECK(check_dispatch_atomic_candidate_evidence(&baseline, 0u, 1u));
    }
    compute_source_candidate_dispose(&baseline);
    return true;
}

int main(void) {
    return check_literal_atomic_candidate() && check_dispatch_atomic_candidate() && check_atomic_descriptor_boundary() && check_float_uav_candidate() && check_partial_uav_candidate() && check_replicated_uav_address_lanes() && check_same_uav_address_ownership() && check_uav_read_candidate() && check_partial_structured_candidate() && check_typed_uint_operations() && check_structured_candidate() && check_owned_quality_resolver() && check_typed_candidate() && check_typed_effect_rejections() && check_identifier_names() && check_complete_candidate() && check_transactional_failures() &&
        check_modeled_input_binding() && check_empty_keyword_domain_and_limits() &&
        check_modeled_program_binding() && check_barrier_candidates() ? 0 : 1;
}
