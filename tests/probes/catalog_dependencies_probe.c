// SPDX-License-Identifier: GPL-3.0-only

#include "app/shader_catalog_dependencies_internal.h"
#include "common/file_io.h"
#include "common/sha256.h"

#include <stdio.h>
#include <string.h>

/* Manual observation from one small released input, never authored source.
 * Private mutations exercise independent replay without changing input files.
 * This program exports summaries only, not source or captured object bytes. */
enum { PROBE_INPUT_LIMIT = 1024 * 1024, PROBE_RECORD_LIMIT = 256 };

static bool reject_observation(void *context, const ShaderCatalogDependenciesSummary *summary) {
    size_t *calls = context;
    ++*calls;
    return summary == NULL;
}

static bool source_mutations(const ShaderCatalogDependenciesInput *input,
                             ShaderCatalogDependencies *owned) {
    if (!owned->summary.has_emitted_source || !sb_ok(&owned->source) || !owned->source.len ||
        !owned->inventory.complete || !owned->inventory.receipt_count ||
        !owned->inventory.receipts || !shader_catalog_dependencies_replay(input, owned))
        return false;

    const size_t position = owned->source.len / 2;
    const char original = owned->source.buf[position];
    owned->source.buf[position] ^= 1;
    bool accepted = shader_catalog_dependencies_replay(input, owned);
    owned->source.buf[position] = original;
    if (accepted || !shader_catalog_dependencies_replay(input, owned)) return false;

    ShaderLabSourceSyntaxReceipt *receipt = &owned->inventory.receipts[0];
    const size_t original_end = receipt->source_end;
    receipt->source_end = receipt->source_begin;
    accepted = shader_catalog_dependencies_replay(input, owned);
    receipt->source_end = original_end;
    if (accepted || !shader_catalog_dependencies_replay(input, owned)) return false;

    const uint32_t gaps = owned->summary.base_quality.gaps;
    owned->summary.base_quality.gaps ^= SHADERLAB_SOURCE_GAP_EXTERNAL_INCLUDE;
    accepted = shader_catalog_dependencies_replay(input, owned);
    owned->summary.base_quality.gaps = gaps;
    if (accepted || !shader_catalog_dependencies_replay(input, owned)) return false;

    owned->root_digest[0] ^= 1;
    accepted = shader_catalog_dependencies_replay(input, owned);
    owned->root_digest[0] ^= 1;
    if (accepted || !shader_catalog_dependencies_replay(input, owned)) return false;

    size_t calls = 0;
    ShaderCatalogDependenciesInput rejected = *input;
    rejected.observer = reject_observation;
    rejected.observer_context = &calls;
    ShaderCatalogDependencies *destination = NULL;
    ShaderCatalogDependenciesStatus status =
        shader_catalog_dependencies_capture(&rejected, &destination, NULL);
    const bool rejected_cleanly = status == SHADER_CATALOG_DEPENDENCIES_OBSERVER_REJECTED &&
        calls == 1 && destination == NULL;
    shader_catalog_dependencies_free(destination);
    return rejected_cleanly && shader_catalog_dependencies_replay(input, owned);
}

static bool inspect_record(const ShaderCatalogDependenciesInput *input, size_t ordinal,
                           const uint8_t input_digest[COMMON_SHA256_DIGEST_SIZE],
                           size_t *observed, size_t *sources, size_t *unavailable) {
    ShaderCatalogDependencies *owned = NULL;
    ShaderCatalogDependenciesDiagnostic diagnostic;
    ShaderCatalogDependenciesStatus status =
        shader_catalog_dependencies_capture(input, &owned, &diagnostic);
    if (status != SHADER_CATALOG_DEPENDENCIES_OK) {
        ++*unavailable;
        printf("{\"record\":%zu,\"status\":\"%s\",\"object_status\":%u,"
               "\"source_status\":%u,\"published\":false}\n",
               ordinal, shader_catalog_dependencies_status_name(status),
               (unsigned)diagnostic.object_status, (unsigned)diagnostic.source.status);
        const bool unpublished = owned == NULL;
        shader_catalog_dependencies_free(owned);
        return unpublished;
    }
    ShaderCatalogDependenciesSummary summary = {0};
    bool valid = shader_catalog_dependencies_describe(owned, &summary) &&
        memcmp(summary.release.source_artifact_digest, input_digest,
               COMMON_SHA256_DIGEST_SIZE) == 0 &&
        shader_catalog_dependencies_replay(input, owned);
    char release_hash[COMMON_SHA256_DIGEST_SIZE * 2 + 1];
    common_sha256_digest_to_hex(summary.release.release_digest, release_hash);
    if (valid && summary.has_emitted_source) valid = source_mutations(input, owned);
    if (valid) {
        ++*observed;
        if (summary.has_emitted_source) ++*sources;
        printf("{\"record\":%zu,\"status\":\"ok\",\"release_sha256\":\"%s\","
               "\"has_source\":%s,\"source_bytes\":%zu,\"base_class\":\"%s\","
               "\"base_gaps\":%u,\"receipts\":%zu,\"linked_entries\":%zu,"
               "\"independent_replay\":true,\"source_mutations\":%s}\n",
               ordinal, release_hash, summary.has_emitted_source ? "true" : "false",
               owned->source.len, summary.has_emitted_source
                   ? hlsl_source_quality_class_name(summary.base_quality.classification) : "unobserved",
               summary.base_quality.gaps, owned->inventory.receipt_count,
               owned->inventory.entries.count, summary.has_emitted_source ? "true" : "null");
    } else {
        printf("{\"record\":%zu,\"status\":\"replay-check-failed\"}\n", ordinal);
    }
    shader_catalog_dependencies_free(owned);
    return valid;
}

int main(int argc, char **argv) {
    if (argc != 2 || !strcmp(argv[1], "--help")) {
        fprintf(argc == 2 ? stdout : stderr,
                "usage: catalog_dependencies_probe RELEASED_INPUT\n"
                "One regular input <=1MiB, <=256 records, retained embedded schema.\n"
                "Summary-only empty-table/source replay; no compiler or runtime certificate.\n");
        return argc == 2 ? 0 : 1;
    }
    CommonFileView file = {0};
    if (common_file_view_open_regular(argv[1], PROBE_INPUT_LIMIT, &file) != COMMON_FILE_OK) {
        fprintf(stderr, "Small regular input unavailable.\n");
        return 1;
    }
    uint8_t input_digest[COMMON_SHA256_DIGEST_SIZE] = {0};
    bool valid = common_file_view_sha256(&file, input_digest);
    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.retain_source_snapshots = true;
    const char *paths[] = {argv[1]};
    valid = valid && shader_catalog_build(paths, 1, &options, &catalog) == SHADER_CATALOG_OK &&
        shader_catalog_is_complete(&catalog) && catalog.record_count <= PROBE_RECORD_LIMIT;
    size_t requested = 0, observed = 0, sources = 0, unavailable = 0, failed = 0;
    if (valid) {
        for (size_t index = 0; index < catalog.record_count; ++index) {
            if (catalog.records[index].class_id != 48) continue;
            ++requested;
            ShaderCatalogDependenciesInput input = {.catalog = &catalog,
                .record = &catalog.records[index]};
            if (!inspect_record(&input, index, input_digest, &observed, &sources, &unavailable))
                ++failed;
        }
    }
    shader_catalog_dispose(&catalog);
    if (common_file_view_close(&file) != COMMON_FILE_OK) valid = false;
    char hash[COMMON_SHA256_DIGEST_SIZE * 2 + 1];
    common_sha256_digest_to_hex(input_digest, hash);
    printf("{\"event\":\"summary\",\"input_sha256\":\"%s\",\"requested\":%zu,"
           "\"observed\":%zu,\"source_positive\":%zu,\"unavailable\":%zu,\"failed\":%zu,"
           "\"input_valid\":%s,\"compiler\":\"not-run\",\"native_D3D11\":\"not-run\"}\n",
           hash, requested, observed, sources, unavailable, failed, valid ? "true" : "false");
    return valid && requested && sources && !failed ? 0 : 1;
}
