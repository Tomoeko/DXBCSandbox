// SPDX-License-Identifier: GPL-3.0-only
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "app/shader_batch.h"
#include "common/file_io.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_compare.h"
#include "translation/usil_validation.h"
#include "test_support/file_mutation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define PROCESS_ID() ((unsigned long)_getpid())
#define REMOVE_DIRECTORY(path) _rmdir(path)
#else
#include <unistd.h>
#define PROCESS_ID() ((unsigned long)getpid())
#define REMOVE_DIRECTORY(path) rmdir(path)
#endif
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    goto cleanup; } } while (0)

typedef struct { uint8_t bytes[1024]; size_t size; } Bytes;
static bool append(Bytes *b, const void *value, size_t size) {
    if (size > sizeof(b->bytes) - b->size) return false;
    if (size) memcpy(b->bytes + b->size, value, size);
    b->size += size;
    return true;
}
static bool u32(Bytes *b, uint32_t value) {
    const uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    return append(b, bytes, sizeof(bytes));
}
static bool u64(Bytes *b, uint64_t value) { return u32(b, (uint32_t)value) && u32(b, (uint32_t)(value >> 32)); }
static bool align(Bytes *b, size_t boundary) {
    const uint8_t zero = 0;
    while (b->size % boundary) if (!append(b, &zero, 1)) return false;
    return true;
}
static bool string(Bytes *b, const char *value) {
    return u32(b, (uint32_t)strlen(value)) && append(b, value, strlen(value)) && align(b, 4);
}
static void big_endian(Bytes *b, size_t offset, uint64_t value, unsigned size) {
    for (unsigned index = 0; index < size; ++index) b->bytes[offset + index] = (uint8_t)(value >> (8u * (size - index - 1u)));
}

typedef enum {
    FIXTURE_RETURN_ONLY,
    FIXTURE_LITERAL_ATOMIC,
    FIXTURE_DISPATCH_ATOMIC
} FixtureCodeKind;

/* These token programs are authored here, not captured native containers.
 * The scalar atomic follows the observed raw173 no-result grammar. The normal
 * production parser supplies all Class72, stage, candidate and publication
 * checks; this fixture carries no compiler-exact or runtime claim. */
static bool fixture_code(Bytes *code, FixtureCodeKind kind) {
    static const uint8_t zero[16] = {0};
    static const uint32_t return_words[] = {
        155u | 4u << 24u, 8u, 4u, 1u, 62u | 1u << 24u
    };
    static const uint32_t atomic_words[] = {
        106u | 1u << 24u | 1u << 11u,
        156u | 4u << 24u | 3u << 11u, 0x0011e000u, 0u, 0x4444u,
        155u | 4u << 24u, 8u, 4u, 1u,
        173u | 10u << 24u, 0x0011e000u, 0u,
        0x4002u, 0u, 0u, 0u, 0u, 0x4001u, 1u,
        62u | 1u << 24u
    };
    static const uint32_t dispatch_words[] = {
        106u | 1u << 24u | 1u << 11u,
        156u | 4u << 24u | 3u << 11u, 0x0011e000u, 0u, 0x4444u,
        95u | 2u << 24u, 0x00020032u,
        155u | 4u << 24u, 8u, 4u, 1u,
        173u | 6u << 24u, 0x0011e000u, 0u, 0x00020046u, 0x4001u, 1u,
        62u | 1u << 24u
    };
    const uint32_t *words;
    size_t count;
    switch (kind) {
        case FIXTURE_RETURN_ONLY:
            words = return_words; count = sizeof(return_words) / sizeof(return_words[0]); break;
        case FIXTURE_LITERAL_ATOMIC:
            words = atomic_words; count = sizeof(atomic_words) / sizeof(atomic_words[0]); break;
        case FIXTURE_DISPATCH_ATOMIC:
            words = dispatch_words; count = sizeof(dispatch_words) / sizeof(dispatch_words[0]); break;
        default: return false;
    }
    const uint32_t payload_size = ((uint32_t)count + 2u) * 4u;
    const uint32_t total_size = 44u + payload_size;
    memset(code, 0, sizeof(*code));
    if (!append(code, "DXBC", 4) || !append(code, zero, 16) || !u32(code, 1) || !u32(code, total_size) ||
        !u32(code, 1) || !u32(code, 36) || !append(code, "SHEX", 4) || !u32(code, payload_size) ||
        !u32(code, 0x50050) || !u32(code, (uint32_t)count + 2u)) return false;
    for (size_t index = 0; index < count; ++index) if (!u32(code, words[index])) return false;
    return code->size == total_size && dxbc_compute_hash(code->bytes, code->size, code->bytes + 4);
}

static bool released_compute_program(Bytes *file, uint64_t requirements, FixtureCodeKind kind) {
    static const uint8_t zero[16] = {0};
    static const uint8_t type_hash[16] = {0xab,0xd9,0x13,0x5b,0x8c,0xe8,0x3d,0x04,
        0x3f,0xef,0x4e,0x9e,0xc7,0xf5,0x33,0x66};
    Bytes code;
    if (!fixture_code(&code, kind)) return false;
    const bool atomic = kind != FIXTURE_RETURN_ONLY;
    memset(file, 0, sizeof(*file));
    if (!append(file, zero, 16) || !append(file, zero, 16) || !append(file, zero, 16)) return false;
    file->bytes[11] = 22;
    const size_t metadata_start = file->size;
    const uint8_t type_tail[3] = {0, 255, 255};
    if (!append(file, "2021.3.35f1", sizeof("2021.3.35f1")) || !u32(file, 19) || !append(file, zero, 1) ||
        !u32(file, 1) || !u32(file, 72) || !append(file, type_tail, sizeof(type_tail)) ||
        !append(file, type_hash, sizeof(type_hash)) || !u32(file, 1) || !align(file, 4) ||
        !u64(file, 101) || !u64(file, 0)) return false;
    const size_t size_field = file->size;
    if (!u32(file, 0) || !u32(file, 0) || !u32(file, 0) || !u32(file, 0) || !u32(file, 0) ||
        !append(file, zero, 1)) return false;
    const size_t metadata_size = file->size - metadata_start;
    if (!align(file, 16)) return false;
    const size_t object_start = file->size;
    if (!string(file, "ComputeFixture") || !u32(file, 1) || !u32(file, 2) || !u32(file, 0) ||
        !u32(file, 1) || !string(file, atomic ? "AtomicKernel" : "ReturnOnly") || !u32(file, 1) || !string(file, "")) return false;
    for (unsigned index = 0; index < 5; ++index) if (!u32(file, 0)) return false;
    if (!u32(file, atomic ? 1u : 0u)) return false;
    if (atomic && (!string(file, "AtomicCounts") || !string(file, "") ||
        !u32(file, 0u) || !u32(file, UINT32_MAX) || !u32(file, 2u))) return false;
    if (!u32(file, (uint32_t)code.size) || !append(file, code.bytes, code.size) || !align(file, 4) ||
        !u32(file, 3) || !u32(file, 8) || !u32(file, 4) || !u32(file, 1) || !u64(file, requirements) ||
        !u32(file, 0) || !u32(file, 0) || !u32(file, 0) || !append(file, "\1", 1) || !align(file, 4)) return false;
    const uint32_t object_size = (uint32_t)(file->size - object_start);
    for (unsigned index = 0; index < 4; ++index) file->bytes[size_field + index] = (uint8_t)(object_size >> (8u * index));
    big_endian(file, 20, metadata_size, 4);
    big_endian(file, 24, file->size, 8);
    big_endian(file, 32, object_start, 8);
    return true;
}

static bool released_compute(Bytes *file, uint64_t requirements) {
    return released_compute_program(file, requirements, FIXTURE_RETURN_ONLY);
}

static bool contains(const CommonFileBytes *bytes, const char *text) {
    const size_t size = strlen(text);
    if (size > bytes->size) return false;
    for (size_t index = 0; index <= bytes->size - size; ++index)
        if (memcmp(bytes->data + index, text, size) == 0) return true;
    return false;
}

static char *copy_string(const char *value) {
    char *copy = malloc(strlen(value) + 1);
    if (copy) strcpy(copy, value);
    return copy;
}
static void cleanup_outputs(const char *root, const ShaderCatalog *catalog) {
    static const char *const names[] = {"compute_ComputeFixture__101.serialized-object.bin",
        "compute_ComputeFixture__101.p0.k0.v0.dxbc", "compute_ComputeFixture__101.compute.json",
        "compute_ComputeFixture__101_candidate.compute", "compute_ComputeFixture__101_candidate.json"};
    if (catalog->record_count != 1) return;
    char *directory = common_output_join_path(root, catalog->records[0].serialized_digest_hex);
    if (!directory) return;
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        char *path = common_output_join_path(directory, names[index]);
        if (path) { (void)remove(path); free(path); }
    }
    (void)REMOVE_DIRECTORY(directory);
    free(directory);
}
static bool catalog_input(const char *input, ShaderCatalog *catalog, bool retained) {
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.retain_source_snapshots = retained;
    const char *inputs[] = {input};
    return shader_catalog_build(inputs, 1, &options, catalog) == SHADER_CATALOG_OK &&
        shader_catalog_is_complete(catalog) && catalog->record_count == 1 && catalog->records[0].class_id == 72;
}

static bool check_publication(const char *input, const char *root) {
    bool passed = false, selected = true;
    ShaderCatalog catalog;
    ShaderBatchResult batch;
    shader_catalog_init(&catalog);
    shader_batch_result_init(&batch);
    CommonFileBytes manifest = {0}, source = {0}, evidence = {0}, actual = {0};
    char *source_path = NULL, *evidence_path = NULL, *binary_path = NULL;
    ShaderBatchOptions options;
    shader_batch_options_default(&options);
    CHECK(!options.emit_compute_source_candidate);
    CHECK(catalog_input(input, &catalog, false));
    CHECK(shader_batch_extract_ex(&catalog, &selected, NULL, root, &options, &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch) && batch.records[0].compute_artifact_publication_count == 3);
    CHECK(!batch.records[0].compute_source_candidate_attempted && !batch.records[0].compute_source_candidate_path);
    CHECK(common_file_read_regular(batch.records[0].output_path, 65536, &manifest) == COMMON_FILE_OK);
    options.emit_compute_source_candidate = true;
    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract_ex(&catalog, &selected, NULL, root, &options, &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch) && batch.records[0].status == SHADER_BATCH_EMITTED);
    const ShaderBatchRecordResult *record = &batch.records[0];
    CHECK(record->compute_source_candidate_attempted && record->compute_source_candidate_generated);
    CHECK(record->compute_source_candidate_status == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED);
    CHECK(record->compute_source_candidate_quality.classification == HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(record->compute_source_authority_status == COMPUTE_SHADER_SOURCE_AUTHORITY_DECLARATION_INVERSE_UNAVAILABLE);
    CHECK(record->compute_artifact_publication_count == 5 && record->publication_authorized);
    CHECK(common_file_read_regular(record->output_path, 65536, &actual) == COMMON_FILE_OK);
    CHECK(actual.size == manifest.size && memcmp(actual.data, manifest.data, actual.size) == 0);
    common_file_bytes_dispose(&actual);
    source_path = copy_string(record->compute_source_candidate_path);
    evidence_path = copy_string(record->compute_source_candidate_evidence_path);
    char *directory = common_output_join_path(root, catalog.records[0].serialized_digest_hex);
    CHECK(directory);
    binary_path = common_output_join_path(directory, "compute_ComputeFixture__101.p0.k0.v0.dxbc");
    free(directory);
    CHECK(source_path && evidence_path && binary_path);
    CHECK(common_file_read_regular(source_path, 65536, &source) == COMMON_FILE_OK);
    CHECK(common_file_read_regular(evidence_path, 65536, &evidence) == COMMON_FILE_OK);
    CHECK(contains(&evidence, "\"status\":\"candidate-unverified\""));
    CHECK(contains(&evidence, "\"compiler\":\"not-run\""));
    CHECK(contains(&evidence, "\"native\":\"not-run\""));
    CHECK(contains(&evidence, "\"version\":2") && contains(&evidence, "\"resource_declarations\":[]"));
    CHECK(contains(&evidence, "\"memory_effects\":[]"));
    CHECK(!contains(&evidence, "ReturnOnly") && !contains(&evidence, "ComputeFixture"));
    size_t source_members = 0, evidence_members = 0;
    for (size_t index = 0; index < record->compute_artifact_publication_count; ++index) {
        source_members += record->compute_artifact_publications[index].is_compute_source_candidate;
        evidence_members += record->compute_artifact_publications[index].is_compute_source_candidate_evidence;
    }
    CHECK(source_members == 1 && evidence_members == 1);
    batch.records[0].compute_artifact_publications[3].is_compute_source_candidate = false;
    CHECK(!shader_batch_is_complete(&batch));
    batch.records[0].compute_artifact_publications[3].is_compute_source_candidate = true;
    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract_ex(&catalog, &selected, NULL, root, &options, &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch) && batch.records[0].status == SHADER_BATCH_UNCHANGED);
    CHECK(batch.records[0].compute_artifact_publication_count == 5);
    for (unsigned collision = 0; collision < 2; ++collision) {
        const char *conflict_path = collision ? evidence_path : source_path;
        CHECK(remove(conflict_path) == 0);
        CHECK(common_file_write_new_atomic(conflict_path, "conflict\n", 9) == COMMON_FILE_OK);
        if (!collision) CHECK(remove(binary_path) == 0);
        shader_batch_result_dispose(&batch);
        CHECK(shader_batch_extract_ex(&catalog, &selected, NULL, root, &options, &batch) == SHADER_BATCH_OK);
        CHECK(!shader_batch_is_complete(&batch) && batch.records[0].failure == SHADER_BATCH_FAILURE_OUTPUT_COLLISION);
        CHECK(batch.records[0].compute_source_candidate_generated && !batch.records[0].publication_authorized);
        CHECK(!batch.records[0].compute_source_candidate_path && !batch.records[0].compute_source_candidate_evidence_path);
        CHECK(!batch.records[0].compute_publish_attempted && !batch.records[0].compute_publication_residue);
        for (size_t index = 0; index < batch.records[0].compute_artifact_publication_count; ++index) {
            CHECK(batch.records[0].compute_artifact_publications[index].preflight_attempted);
            CHECK(!batch.records[0].compute_artifact_publications[index].publish_attempted);
        }
        CHECK(common_file_read_regular(binary_path, 65536, &actual) != COMMON_FILE_OK);
        CHECK(common_file_read_regular(conflict_path, 65536, &actual) == COMMON_FILE_OK);
        CHECK(actual.size == 9 && memcmp(actual.data, "conflict\n", 9) == 0);
        common_file_bytes_dispose(&actual);
        CHECK(remove(conflict_path) == 0);
        const CommonFileBytes *original = collision ? &evidence : &source;
        CHECK(common_file_write_new_atomic(conflict_path, original->data, original->size) == COMMON_FILE_OK);
    }
    passed = true;
cleanup:
    common_file_bytes_dispose(&manifest);
    common_file_bytes_dispose(&source);
    common_file_bytes_dispose(&evidence);
    common_file_bytes_dispose(&actual);
    free(source_path); free(evidence_path); free(binary_path);
    cleanup_outputs(root, &catalog);
    shader_batch_result_dispose(&batch);
    shader_catalog_dispose(&catalog);
    return passed;
}

/* Exercise the actual released-input catalog/batch path, including its
 * original binary inventory and the public candidate evidence switch. This
 * authored Class72 fixture is never described as an observed native payload. */
static bool check_atomic_publication(const char *input, const char *root, FixtureCodeKind kind) {
    bool passed = false, selected = true;
    Bytes fixture, original_code;
    ShaderCatalog catalog;
    ShaderBatchResult batch;
    ShaderBatchOptions options;
    shader_catalog_init(&catalog);
    shader_batch_result_init(&batch);
    shader_batch_options_default(&options);
    CommonFileBytes manifest = {0}, source = {0}, evidence = {0}, actual = {0};
    char *directory = NULL, *binary_path = NULL;
    CHECK(kind == FIXTURE_LITERAL_ATOMIC || kind == FIXTURE_DISPATCH_ATOMIC);
    const bool dispatch = kind == FIXTURE_DISPATCH_ATOMIC;
    CHECK(released_compute_program(&fixture, 0x4001, kind) && fixture_code(&original_code, kind));
    CHECK(remove(input) == 0);
    CHECK(common_file_write_new_atomic(input, fixture.bytes, fixture.size) == COMMON_FILE_OK);
    CHECK(catalog_input(input, &catalog, true));
    CHECK(catalog.retained_source_snapshot_count == 1u && catalog.retained_source_snapshots);
    const ComputeShaderObjectSummary original_summary = catalog.records[0].compute_summary;
    CHECK(original_summary.platform_count == 1u && original_summary.kernel_parent_count == 1u &&
        original_summary.kernel_variant_count == 1u && original_summary.code_blob_count == 1u &&
        original_summary.dxbc_code_blob_count == 1u && original_summary.exact_thread_group_count == 1u &&
        original_summary.resource_count == 1u && !original_summary.constant_buffer_count);
    CHECK(shader_batch_extract_ex(&catalog, &selected, NULL, root, &options, &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch) && batch.records[0].compute_artifact_publication_count == 3u &&
        !batch.records[0].compute_source_candidate_attempted);
    CHECK(common_file_read_regular(batch.records[0].output_path, 65536, &manifest) == COMMON_FILE_OK);
    CHECK(contains(&manifest, "\"kernel_variants\":1,\"code_blobs\":1,\"dxbc_code_blobs\":1"));
    directory = common_output_join_path(root, catalog.records[0].serialized_digest_hex);
    CHECK(directory);
    binary_path = common_output_join_path(directory, "compute_ComputeFixture__101.p0.k0.v0.dxbc");
    CHECK(binary_path);
    CHECK(common_file_read_regular(binary_path, 65536, &actual) == COMMON_FILE_OK);
    DXBCCompareResult comparison;
    CHECK(dxbc_compare_exact(original_code.bytes, original_code.size, actual.data, actual.size, &comparison) ==
        DXBC_COMPARE_EQUAL);
    common_file_bytes_dispose(&actual);
    options.emit_compute_source_candidate = true;
    shader_batch_result_dispose(&batch);
    CHECK(shader_batch_extract_ex(&catalog, &selected, NULL, root, &options, &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch) && batch.records[0].status == SHADER_BATCH_EMITTED);
    const ShaderBatchRecordResult *record = &batch.records[0];
    CHECK(record->compute_source_candidate_attempted && record->compute_source_candidate_generated &&
        record->compute_source_candidate_status == COMPUTE_SOURCE_CANDIDATE_UNVERIFIED &&
        record->publication_authorized && !record->source_identity_close_deferred &&
        !record->compute_publication_residue && record->compute_artifact_publication_count == 5u);
    CHECK(record->compute_source_authority_status == COMPUTE_SHADER_SOURCE_AUTHORITY_DECLARATION_INVERSE_UNAVAILABLE);
    CHECK(record->compute_source_candidate_diagnostic.requested_counts_known &&
        record->compute_source_candidate_diagnostic.requested_kernels == 1u &&
        record->compute_source_candidate_diagnostic.requested_variants == 1u &&
        record->compute_source_candidate_diagnostic.examined_variants == 1u &&
        record->compute_source_candidate_diagnostic.represented_variants == 1u);
    CHECK(record->compute_source_candidate_quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
        !record->compute_source_candidate_quality.reasons &&
        !record->compute_source_candidate_quality.counts.incomplete_units &&
        !record->compute_source_candidate_quality.counts.unknown_provenance &&
        !record->compute_source_candidate_quality.counts.residual_total);
    CHECK(common_file_read_regular(record->compute_source_candidate_path, 65536, &source) == COMMON_FILE_OK);
    CHECK(contains(&source, "RWTexture2D<uint> AtomicCounts : register(u0);"));
    /* The owned binding expression retains the formatter's parentheses. */
    if (dispatch) {
        CHECK(contains(&source, "uint3 dispatchThreadId : SV_DispatchThreadID"));
        CHECK(contains(&source, "InterlockedAdd((AtomicCounts)[int2((dispatchThreadId.xy))], 1u);"));
    } else {
        CHECK(!contains(&source, "SV_DispatchThreadID") && !contains(&source, "dispatchThreadId"));
        CHECK(contains(&source, "InterlockedAdd((AtomicCounts)[int2(0, 0)], 1u);"));
    }
    CHECK(contains(&source, "[numthreads(8, 4, 1)]") && !contains(&source, "uint4") && !contains(&source, "].x"));
    uint8_t source_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(source.data, source.size, source_digest);
    CHECK(!memcmp(source_digest, record->compute_source_candidate_source_sha256, sizeof(source_digest)));
    CHECK(common_file_read_regular(record->compute_source_candidate_evidence_path, 65536, &evidence) == COMMON_FILE_OK);
    CHECK(contains(&evidence, "\"status\":\"candidate-unverified\"") &&
        contains(&evidence, "\"compiler\":\"not-run\"") && contains(&evidence, "\"import\":\"not-run\"") &&
        contains(&evidence, "\"native\":\"not-run\"") && contains(&evidence, "\"semantic\":\"not-run\"") &&
        contains(&evidence, "\"domain_complete\":true") && contains(&evidence, "\"kernels\":1,\"variants\":1"));
    CHECK(contains(&evidence, "\"representation\":\"texture2d-uint-scalar-atomic\"") &&
        contains(&evidence, "\"binding_register\":0,\"writable\":true") &&
        contains(&evidence, "\"original_element_type_known\":true,\"variant_witnesses\":[0]") &&
        contains(&evidence, "\"classification\":\"clean\"") && !contains(&evidence, "\"classification\":\"mixed\""));
    char effect[256];
    const uint32_t effects = (uint32_t)(USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_EXTERNAL_WRITE | USIL_EFFECT_ATOMIC);
    const int effect_size = snprintf(effect, sizeof(effect),
        "\"memory_effects\":[{\"opcode\":%u,\"effect_flags\":%u,\"instruction_index\":0,"
        "\"source_instruction_index\":%u,\"binding_register\":0}]", (unsigned)USIL_OP_ATOMIC_IADD, effects,
        dispatch ? 4u : 3u);
    CHECK(effect_size > 0 && (size_t)effect_size < sizeof(effect) && contains(&evidence, effect));
    size_t source_members = 0u, evidence_members = 0u;
    for (size_t index = 0; index < record->compute_artifact_publication_count; ++index) {
        const ShaderBatchComputeArtifactPublication *member = &record->compute_artifact_publications[index];
        CHECK(member->preflight_attempted && member->publish_attempted && !member->publication_residue);
        source_members += member->is_compute_source_candidate;
        evidence_members += member->is_compute_source_candidate_evidence;
    }
    CHECK(source_members == 1u && evidence_members == 1u);
    CHECK(common_file_read_regular(record->output_path, 65536, &actual) == COMMON_FILE_OK);
    CHECK(actual.size == manifest.size && !memcmp(actual.data, manifest.data, actual.size));
    common_file_bytes_dispose(&actual);
    CHECK(common_file_read_regular(binary_path, 65536, &actual) == COMMON_FILE_OK);
    CHECK(dxbc_compare_exact(original_code.bytes, original_code.size, actual.data, actual.size, &comparison) ==
        DXBC_COMPARE_EQUAL);
    CHECK(catalog.record_count == 1u && catalog.records[0].compute_summary.kernel_variant_count ==
        original_summary.kernel_variant_count && catalog.records[0].compute_summary.resource_count == original_summary.resource_count);
    passed = true;
cleanup:
    common_file_bytes_dispose(&manifest); common_file_bytes_dispose(&source);
    common_file_bytes_dispose(&evidence); common_file_bytes_dispose(&actual);
    free(directory); free(binary_path);
    cleanup_outputs(root, &catalog);
    shader_batch_result_dispose(&batch);
    shader_catalog_dispose(&catalog);
    return passed;
}

static bool check_unavailable_and_identity(const char *input, const char *root) {
    bool passed = false, selected = true;
    Bytes fixture;
    ShaderCatalog catalog;
    ShaderBatchResult batch;
    ShaderBatchOptions options;
    shader_catalog_init(&catalog);
    shader_batch_result_init(&batch);
    shader_batch_options_default(&options);
    options.emit_compute_source_candidate = true;
    CHECK(released_compute(&fixture, 0x4003));
    CHECK(remove(input) == 0);
    CHECK(common_file_write_new_atomic(input, fixture.bytes, fixture.size) == COMMON_FILE_OK);
    CHECK(catalog_input(input, &catalog, false));
    CHECK(shader_batch_extract_ex(&catalog, &selected, NULL, root, &options, &batch) == SHADER_BATCH_OK);
    CHECK(shader_batch_is_complete(&batch) && batch.records[0].compute_artifact_publication_count == 3);
    CHECK(batch.records[0].compute_source_candidate_attempted && !batch.records[0].compute_source_candidate_generated);
    CHECK(batch.records[0].compute_source_candidate_status == COMPUTE_SOURCE_REQUIREMENTS_UNSUPPORTED);
    CHECK(batch.records[0].compute_source_candidate_diagnostic.requested_variants == 1);
    CHECK(batch.records[0].compute_source_candidate_quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    CHECK(!batch.records[0].compute_source_candidate_path && !batch.records[0].compute_source_candidate_evidence_path);
    cleanup_outputs(root, &catalog);
    shader_batch_result_dispose(&batch);
    shader_catalog_dispose(&catalog);
    CHECK(released_compute(&fixture, 0x4001));
    CHECK(remove(input) == 0);
    CHECK(common_file_write_new_atomic(input, fixture.bytes, fixture.size) == COMMON_FILE_OK);
    CHECK(catalog_input(input, &catalog, true));
    CHECK(test_replace_regular_file(input, fixture.bytes, fixture.size));
    CHECK(shader_batch_extract_ex(&catalog, &selected, NULL, root, &options, &batch) == SHADER_BATCH_OK);
    CHECK(!shader_batch_is_complete(&batch) && batch.records[0].status == SHADER_BATCH_FAILED);
    CHECK(!batch.records[0].publication_authorized && !batch.records[0].compute_preflight_attempted);
    CHECK(!batch.records[0].compute_source_candidate_path && !batch.records[0].compute_source_candidate_evidence_path);
    passed = true;
cleanup:
    cleanup_outputs(root, &catalog);
    shader_batch_result_dispose(&batch);
    shader_catalog_dispose(&catalog);
    return passed;
}

int main(void) {
    char root[160], input[200];
    if (snprintf(root, sizeof(root), "dxbc_compute_candidate_publication_%lu", PROCESS_ID()) <= 0) return 1;
    if (snprintf(input, sizeof(input), "%s.assets", root) <= 0) return 1;
    Bytes fixture;
    (void)remove(input);
    if (!released_compute(&fixture, 0x4001) ||
        common_file_write_new_atomic(input, fixture.bytes, fixture.size) != COMMON_FILE_OK) return 1;
    const bool passed = check_publication(input, root) && check_unavailable_and_identity(input, root) &&
        check_atomic_publication(input, root, FIXTURE_LITERAL_ATOMIC) &&
        check_atomic_publication(input, root, FIXTURE_DISPATCH_ATOMIC);
    (void)remove(input);
    (void)REMOVE_DIRECTORY(root);
    return passed ? 0 : 1;
}
