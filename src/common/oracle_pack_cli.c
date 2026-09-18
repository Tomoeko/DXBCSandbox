// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "common/oracle_pack.h"
#include "common/windows_utf8.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* program) {
    fprintf(stderr,
            "Usage: %s validate PACK\n"
            "       %s inspect PACK [--entries]\n",
            program, program);
}

static void print_digest(const uint8_t digest[ORACLE_PACK_DIGEST_SIZE]) {
    for (size_t i = 0U; i < ORACLE_PACK_DIGEST_SIZE; ++i) {
        printf("%02x", digest[i]);
    }
}

static bool add_total(uint64_t* total, size_t amount) {
    uint64_t converted = (uint64_t)amount;
    if ((size_t)converted != amount ||
        *total > UINT64_MAX - converted) {
        return false;
    }
    *total += converted;
    return true;
}

static int inspect_pack(const OraclePack* pack, bool list_entries) {
    OraclePackAuthorityView authority;
    OraclePackStatus authority_status = oracle_pack_authority(
        pack, &authority);
    if (authority_status != ORACLE_PACK_OK) {
        fprintf(stderr, "OraclePack authority lookup failed: %s\n",
                oracle_pack_status_string(authority_status));
        return 1;
    }
    size_t entry_count = oracle_pack_entry_count(pack);
    size_t preprocess_count = oracle_pack_preprocess_count(pack);
    uint64_t dxbc_bytes = 0U;
    uint64_t glcore_bytes = 0U;
    uint64_t transcript_bytes = 0U;
    uint64_t metadata_bytes = 0U;
    uint64_t preprocess_request_bytes = 0U;
    uint64_t preprocess_result_bytes = 0U;
    size_t glcore_count = 0U;

    for (size_t i = 0U; i < entry_count; ++i) {
        OraclePackEntryView entry;
        OraclePackStatus status = oracle_pack_entry_at(pack, i, &entry);
        if (status != ORACLE_PACK_OK ||
            !add_total(&dxbc_bytes, entry.stripped_dxbc.size) ||
            !add_total(&glcore_bytes, entry.linked_glcore.size) ||
            !add_total(&transcript_bytes,
                       entry.compile_request_transcript.size) ||
            !add_total(&metadata_bytes,
                       entry.normalized_metadata_bytes.size)) {
            fprintf(stderr, "OraclePack entry accounting failed at %zu\n", i);
            return 1;
        }
        if (entry.linked_glcore.size != 0U) ++glcore_count;
        if (list_entries) {
            printf("entry[%zu] key=", i);
            print_digest(entry.variant_key_digest);
            printf(" request=");
            print_digest(entry.compile_request_digest);
            printf(" dxbc=%zu glcore=%zu transcript=%zu metadata=%zu\n",
                   entry.stripped_dxbc.size, entry.linked_glcore.size,
                   entry.compile_request_transcript.size,
                   entry.normalized_metadata_bytes.size);
        }
    }

    for (size_t i = 0U; i < preprocess_count; ++i) {
        OraclePackPreprocessView preprocess;
        OraclePackStatus status =
            oracle_pack_preprocess_at(pack, i, &preprocess);
        if (status != ORACLE_PACK_OK ||
            !add_total(&preprocess_request_bytes,
                       preprocess.request_transcript.size) ||
            !add_total(&preprocess_result_bytes,
                       preprocess.serialized_result.size)) {
            fprintf(stderr,
                    "OraclePack preprocess accounting failed at %zu\n", i);
            return 1;
        }
        if (list_entries) {
            printf("preprocess[%zu] request=", i);
            print_digest(preprocess.request_digest);
            printf(" result=");
            print_digest(preprocess.result_digest);
            printf(" transcript=%zu result_bytes=%zu\n",
                   preprocess.request_transcript.size,
                   preprocess.serialized_result.size);
        }
    }

    printf("format_version=%u\n", ORACLE_PACK_FORMAT_VERSION);
    printf("compiler_sha256=");
    print_digest(authority.compiler_fingerprint);
    printf("\nenvironment_sha256=");
    print_digest(authority.environment_fingerprint);
    printf("\n");
    printf("compile_entries=%zu\n", entry_count);
    printf("preprocess_entries=%zu\n", preprocess_count);
    printf("entries_with_glcore=%zu\n", glcore_count);
    printf("dxbc_bytes=%" PRIu64 "\n", dxbc_bytes);
    printf("glcore_bytes=%" PRIu64 "\n", glcore_bytes);
    printf("compile_transcript_bytes=%" PRIu64 "\n", transcript_bytes);
    printf("metadata_bytes=%" PRIu64 "\n", metadata_bytes);
    printf("preprocess_transcript_bytes=%" PRIu64 "\n",
           preprocess_request_bytes);
    printf("preprocess_result_bytes=%" PRIu64 "\n",
           preprocess_result_bytes);
    return 0;
}

static int oracle_pack_main(int argc, char** argv) {
    if (argc < 3 || argc > 4 ||
        (strcmp(argv[1], "validate") != 0 &&
         strcmp(argv[1], "inspect") != 0) ||
        (argc == 4 &&
         (strcmp(argv[1], "inspect") != 0 ||
          strcmp(argv[3], "--entries") != 0))) {
        print_usage(argv[0]);
        return 2;
    }

    CommonFileBytes file;
    CommonFileStatus file_status = common_file_read_regular(
        argv[2], SIZE_MAX, &file);
    if (file_status != COMMON_FILE_OK) {
        fprintf(stderr, "Could not read '%s': %s\n", argv[2],
                common_file_status_name(file_status));
        return 1;
    }
    OraclePack* pack = NULL;
    OraclePackStatus status =
        oracle_pack_open_memory(file.data, file.size, &pack);
    common_file_bytes_dispose(&file);
    if (status != ORACLE_PACK_OK) {
        fprintf(stderr, "Invalid OraclePack '%s': %s\n", argv[2],
                oracle_pack_status_string(status));
        return 1;
    }

    int result = 0;
    if (strcmp(argv[1], "inspect") == 0) {
        result = inspect_pack(pack, argc == 4);
    } else {
        OraclePackAuthorityView authority;
        status = oracle_pack_authority(pack, &authority);
        if (status != ORACLE_PACK_OK) {
            fprintf(stderr, "OraclePack authority lookup failed: %s\n",
                    oracle_pack_status_string(status));
            oracle_pack_free(pack);
            return 1;
        }
        printf("valid: compile_entries=%zu preprocess_entries=%zu "
               "compiler_sha256=",
               oracle_pack_entry_count(pack),
               oracle_pack_preprocess_count(pack));
        print_digest(authority.compiler_fingerprint);
        printf(" environment_sha256=");
        print_digest(authority.environment_fingerprint);
        printf("\n");
    }
    oracle_pack_free(pack);
    return result;
}

COMMON_DEFINE_UTF8_MAIN(oracle_pack_main)
