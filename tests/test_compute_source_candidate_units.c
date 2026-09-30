// SPDX-License-Identifier: GPL-3.0-only

#include "translation/compute_source_candidate.h"
#include "translation/hlsl_source_identifier.h"
#include "translation/hlsl_emitter_internal.h"
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
    CHECK(compute_source_candidate_build(&fixture->object, destination, &diagnostic) == expected);
    CHECK(diagnostic.status == expected);
    CHECK(memcmp(&before, destination, sizeof(before)) == 0);
    CHECK(destination->domain_complete && destination->source_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
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
    uint32_t changed[COUNT(typed_words) + 1];
    memcpy(changed, typed_words, sizeof(typed_words)); changed[8] = 0x5555u; /* Float UAV. */
    CHECK(typed_code(&fixture, changed, COUNT(typed_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    memcpy(changed, typed_words, sizeof(typed_words)); changed[41] = 0x001071b6u; /* Reversed Load lanes. */
    CHECK(typed_code(&fixture, changed, COUNT(typed_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    memcpy(changed, typed_words, sizeof(typed_words)); changed[54] = 0x0011e032u; /* Partial store. */
    CHECK(typed_code(&fixture, changed, COUNT(typed_words)));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* The same load cannot be emitted twice in an expression. */
    memcpy(changed, typed_words, sizeof(typed_words)); changed[43] = INSTRUCTION(30, 7);
    changed[48] = 0x00100e46u; changed[49] = 1u;
    memmove(changed + 50, changed + 53, (COUNT(typed_words) - 53) * sizeof(uint32_t));
    CHECK(typed_code(&fixture, changed, COUNT(typed_words) - 3));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    /* A retained device fence cannot become pure address computation. */
    memcpy(changed, typed_words, sizeof(typed_words));
    memmove(changed + 44, changed + 43, (COUNT(typed_words) - 43) * sizeof(uint32_t));
    changed[43] = INSTRUCTION(190, 1) | (8u << 11u);
    CHECK(typed_code(&fixture, changed, COUNT(typed_words) + 1));
    CHECK(expect_failure(&fixture, &candidate, COMPUTE_SOURCE_EMISSION_FAILED));
    compute_source_candidate_dispose(&candidate); return true;
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

int main(void) {
    return check_owned_quality_resolver() && check_typed_candidate() && check_typed_effect_rejections() && check_identifier_names() && check_complete_candidate() && check_transactional_failures() &&
        check_modeled_input_binding() && check_empty_keyword_domain_and_limits() &&
        check_modeled_program_binding() && check_barrier_candidates() ? 0 : 1;
}
