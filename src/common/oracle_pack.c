// SPDX-License-Identifier: GPL-3.0-only

#include "common/oracle_pack.h"

#include "common/sha256.h"
#include "dxbc/dxbc_document.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define ORACLE_PACK_HEADER_SIZE 192U
#define ORACLE_PACK_DIRECTORY_ENTRY_SIZE 48U
#define ORACLE_PACK_ENTRY_HEADER_SIZE 136U
#define ORACLE_PACK_PREPROCESS_HEADER_SIZE 112U
#define ORACLE_PACK_METADATA_HEADER_SIZE 40U
#define ORACLE_PACK_PRECISION_HEADER_SIZE 16U
#define ORACLE_PACK_FLAG_GLCORE_PRESENT UINT32_C(1)
#define ORACLE_PACK_KNOWN_FLAGS ORACLE_PACK_FLAG_GLCORE_PRESENT

static const uint8_t k_pack_magic[8] = {
    'D', 'X', 'B', 'C', 'O', 'R', 'C', 'L'
};
static const uint8_t k_entry_magic[8] = {
    'D', 'X', 'B', 'C', 'E', 'N', 'T', 'R'
};
static const uint8_t k_preprocess_magic[8] = {
    'D', 'X', 'B', 'C', 'P', 'R', 'E', 'P'
};
static const uint8_t k_metadata_magic[8] = {
    'D', 'X', 'B', 'C', 'M', 'E', 'T', 'A'
};
static const uint8_t k_variant_key_magic[8] = {
    'D', 'X', 'B', 'C', 'V', 'K', 'E', 'Y'
};

typedef struct {
    uint8_t* data;
    size_t size;
} OwnedBytes;

typedef struct {
    OwnedBytes variant_key;
    uint8_t variant_key_digest[ORACLE_PACK_DIGEST_SIZE];
    OwnedBytes stripped_dxbc;
    OwnedBytes linked_glcore;
    OwnedBytes transcript;
    uint8_t transcript_digest[ORACLE_PACK_DIGEST_SIZE];
    OwnedBytes metadata;
} OwnedEntry;

typedef struct {
    OwnedBytes transcript;
    uint8_t request_digest[ORACLE_PACK_DIGEST_SIZE];
    OwnedBytes result;
    uint8_t result_digest[ORACLE_PACK_DIGEST_SIZE];
} OwnedPreprocess;

struct OraclePackWriter {
    uint8_t compiler_fingerprint[ORACLE_PACK_DIGEST_SIZE];
    uint8_t environment_fingerprint[ORACLE_PACK_DIGEST_SIZE];
    OwnedEntry* entries;
    size_t count;
    size_t capacity;
    OwnedPreprocess* preprocesses;
    size_t preprocess_count;
    size_t preprocess_capacity;
    bool finalized;
};

struct OraclePack {
    uint8_t* data;
    size_t size;
    size_t entry_count;
    size_t preprocess_count;
    size_t compile_directory_offset;
    size_t preprocess_directory_offset;
};

typedef struct {
    const OraclePackResourcePrecisionInput* input;
} PrecisionSortItem;

typedef struct {
    size_t canonical_offset;
    size_t canonical_size;
    size_t records_offset;
    size_t record_count;
} MetadataLayout;

static uint32_t load_le32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint64_t load_le64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= (uint64_t)data[i] << (i * 8U);
    }
    return value;
}

static void store_le32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static void store_le64(uint8_t* data, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        data[i] = (uint8_t)(value >> (i * 8U));
    }
}

static bool add_size(size_t left, size_t right, size_t* out) {
    if (left > SIZE_MAX - right) return false;
    *out = left + right;
    return true;
}

static bool multiply_size(size_t left, size_t right, size_t* out) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *out = left * right;
    return true;
}

static bool u64_to_size(uint64_t value, size_t* out) {
    if (value > (uint64_t)SIZE_MAX) return false;
    *out = (size_t)value;
    return true;
}

static bool bytes_are_consistent(OraclePackBytes bytes) {
    return (bytes.data == NULL) == (bytes.size == 0);
}

static bool fingerprint_has_value(const uint8_t* fingerprint) {
    if (!fingerprint) return false;
    uint8_t combined = 0U;
    for (size_t i = 0; i < ORACLE_PACK_DIGEST_SIZE; ++i) {
        combined |= fingerprint[i];
    }
    return combined != 0U;
}

static bool authority_is_valid(const OraclePackAuthorityInput* authority) {
    return authority &&
           fingerprint_has_value(authority->compiler_fingerprint) &&
           fingerprint_has_value(authority->environment_fingerprint);
}

static bool authority_matches_writer(
    const OraclePackWriter* writer,
    const OraclePackAuthorityInput* authority) {
    return writer && authority_is_valid(authority) &&
           memcmp(writer->compiler_fingerprint,
                  authority->compiler_fingerprint,
                  ORACLE_PACK_DIGEST_SIZE) == 0 &&
           memcmp(writer->environment_fingerprint,
                  authority->environment_fingerprint,
                  ORACLE_PACK_DIGEST_SIZE) == 0;
}

static bool precision_is_valid(OraclePackResourcePrecision precision) {
    return precision >= ORACLE_PACK_RESOURCE_PRECISION_UNKNOWN &&
           precision <= ORACLE_PACK_RESOURCE_PRECISION_HIGH;
}

static int compare_bytes(OraclePackBytes left, OraclePackBytes right) {
    size_t common = left.size < right.size ? left.size : right.size;
    int comparison = common == 0 ? 0 : memcmp(left.data, right.data, common);
    if (comparison != 0) return comparison;
    if (left.size < right.size) return -1;
    if (left.size > right.size) return 1;
    return 0;
}

static int compare_precision_items(const void* left, const void* right) {
    const PrecisionSortItem* a = (const PrecisionSortItem*)left;
    const PrecisionSortItem* b = (const PrecisionSortItem*)right;
    return compare_bytes(a->input->resource_key, b->input->resource_key);
}

static int compare_owned_entries(const void* left, const void* right) {
    const OwnedEntry* a = (const OwnedEntry*)left;
    const OwnedEntry* b = (const OwnedEntry*)right;
    return memcmp(a->variant_key_digest, b->variant_key_digest,
                  ORACLE_PACK_DIGEST_SIZE);
}

static int compare_owned_preprocesses(const void* left, const void* right) {
    const OwnedPreprocess* a = (const OwnedPreprocess*)left;
    const OwnedPreprocess* b = (const OwnedPreprocess*)right;
    return memcmp(a->request_digest, b->request_digest,
                  ORACLE_PACK_DIGEST_SIZE);
}

static void owned_bytes_free(OwnedBytes* bytes) {
    if (!bytes) return;
    free(bytes->data);
    bytes->data = NULL;
    bytes->size = 0;
}

static bool owned_bytes_copy(OwnedBytes* output, OraclePackBytes input) {
    output->data = NULL;
    output->size = 0;
    if (input.size == 0) return true;
    output->data = (uint8_t*)malloc(input.size);
    if (!output->data) return false;
    memcpy(output->data, input.data, input.size);
    output->size = input.size;
    return true;
}

static void owned_entry_free(OwnedEntry* entry) {
    if (!entry) return;
    owned_bytes_free(&entry->variant_key);
    owned_bytes_free(&entry->stripped_dxbc);
    owned_bytes_free(&entry->linked_glcore);
    owned_bytes_free(&entry->transcript);
    owned_bytes_free(&entry->metadata);
    memset(entry, 0, sizeof(*entry));
}

static void owned_preprocess_free(OwnedPreprocess* preprocess) {
    if (!preprocess) return;
    owned_bytes_free(&preprocess->transcript);
    owned_bytes_free(&preprocess->result);
    memset(preprocess, 0, sizeof(*preprocess));
}

static OraclePackStatus variant_status(VariantKeyStatus status) {
    switch (status) {
        case VARIANT_KEY_OK: return ORACLE_PACK_OK;
        case VARIANT_KEY_ALLOCATION_FAILED:
            return ORACLE_PACK_ALLOCATION_FAILED;
        case VARIANT_KEY_SIZE_OVERFLOW:
            return ORACLE_PACK_SIZE_OVERFLOW;
        default: return ORACLE_PACK_INVALID_VALUE;
    }
}

static bool validate_variant_key_bytes(
    const uint8_t* data, size_t size,
    const uint8_t expected_digest[ORACLE_PACK_DIGEST_SIZE]) {
    if (!data || size < 20U ||
        memcmp(data, k_variant_key_magic, sizeof(k_variant_key_magic)) != 0 ||
        load_le32(data + 8U) != VARIANT_KEY_FORMAT_VERSION ||
        load_le64(data + 12U) != (uint64_t)size) {
        return false;
    }
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(data, size, digest);
    return memcmp(digest, expected_digest, sizeof(digest)) == 0;
}

static bool validate_dxbc(const uint8_t* data, size_t size) {
    if (!data || size == 0U) return false;
    DXBCDocument document;
    DXBCDocumentDiagnostic diagnostic;
    dxbc_document_init(&document);
    if (!dxbc_document_parse(&document, data, size, &diagnostic)) {
        dxbc_document_free(&document);
        return false;
    }
    bool has_shader_code = false;
    for (uint32_t i = 0; i < document.chunk_count; ++i) {
        if (document.chunks[i].kind == DXBC_DOCUMENT_CHUNK_EXECUTABLE) {
            has_shader_code = true;
            break;
        }
    }
    dxbc_document_free(&document);
    return has_shader_code;
}

static OraclePackStatus parse_metadata(
    OraclePackBytes encoded, MetadataLayout* out_layout) {
    if (!bytes_are_consistent(encoded)) return ORACLE_PACK_INVALID_ARGUMENT;
    if (encoded.size < ORACLE_PACK_METADATA_HEADER_SIZE) {
        return ORACLE_PACK_INVALID_METADATA;
    }
    const uint8_t* data = encoded.data;
    if (memcmp(data, k_metadata_magic, sizeof(k_metadata_magic)) != 0 ||
        load_le32(data + 8U) != ORACLE_PACK_METADATA_FORMAT_VERSION ||
        load_le32(data + 12U) != ORACLE_PACK_METADATA_HEADER_SIZE ||
        load_le64(data + 16U) != (uint64_t)encoded.size) {
        return ORACLE_PACK_INVALID_METADATA;
    }
    size_t canonical_size;
    size_t record_count;
    if (!u64_to_size(load_le64(data + 24U), &canonical_size) ||
        !u64_to_size(load_le64(data + 32U), &record_count)) {
        return ORACLE_PACK_INVALID_METADATA;
    }
    size_t records_offset;
    if (!add_size(ORACLE_PACK_METADATA_HEADER_SIZE, canonical_size,
                  &records_offset) || records_offset > encoded.size) {
        return ORACLE_PACK_INVALID_METADATA;
    }
    /* Each record has a fixed header, so this is also an allocation/DoS cap. */
    if (record_count >
        (encoded.size - records_offset) / ORACLE_PACK_PRECISION_HEADER_SIZE) {
        return ORACLE_PACK_INVALID_METADATA;
    }

    size_t offset = records_offset;
    OraclePackBytes previous_key = {NULL, 0};
    for (size_t i = 0; i < record_count; ++i) {
        if (encoded.size - offset < ORACLE_PACK_PRECISION_HEADER_SIZE) {
            return ORACLE_PACK_INVALID_METADATA;
        }
        size_t key_size;
        if (!u64_to_size(load_le64(data + offset), &key_size) ||
            key_size == 0U || load_le32(data + offset + 12U) != 0U ||
            !precision_is_valid((OraclePackResourcePrecision)
                                load_le32(data + offset + 8U))) {
            return ORACLE_PACK_INVALID_METADATA;
        }
        offset += ORACLE_PACK_PRECISION_HEADER_SIZE;
        if (key_size > encoded.size - offset) {
            return ORACLE_PACK_INVALID_METADATA;
        }
        OraclePackBytes key = {data + offset, key_size};
        if (i > 0 && compare_bytes(previous_key, key) >= 0) {
            return ORACLE_PACK_INVALID_METADATA;
        }
        previous_key = key;
        offset += key_size;
    }
    if (offset != encoded.size) return ORACLE_PACK_INVALID_METADATA;
    if (out_layout) {
        out_layout->canonical_offset = ORACLE_PACK_METADATA_HEADER_SIZE;
        out_layout->canonical_size = canonical_size;
        out_layout->records_offset = records_offset;
        out_layout->record_count = record_count;
    }
    return ORACLE_PACK_OK;
}

static OraclePackStatus encode_metadata(
    const OraclePackNormalizedMetadataInput* input, OwnedBytes* output) {
    output->data = NULL;
    output->size = 0;
    if (!input || !bytes_are_consistent(input->canonical_bytes) ||
        (input->resource_precisions == NULL) !=
            (input->resource_precision_count == 0U)) {
        return ORACLE_PACK_INVALID_ARGUMENT;
    }

    PrecisionSortItem* sorted = NULL;
    if (input->resource_precision_count > 0U) {
        size_t allocation_size;
        if (!multiply_size(input->resource_precision_count,
                           sizeof(*sorted), &allocation_size)) {
            return ORACLE_PACK_SIZE_OVERFLOW;
        }
        sorted = (PrecisionSortItem*)malloc(allocation_size);
        if (!sorted) return ORACLE_PACK_ALLOCATION_FAILED;
        for (size_t i = 0; i < input->resource_precision_count; ++i) {
            const OraclePackResourcePrecisionInput* precision =
                &input->resource_precisions[i];
            if (!bytes_are_consistent(precision->resource_key) ||
                precision->resource_key.size == 0U ||
                !precision_is_valid(precision->precision)) {
                free(sorted);
                return ORACLE_PACK_INVALID_METADATA;
            }
            sorted[i].input = precision;
        }
        qsort(sorted, input->resource_precision_count, sizeof(*sorted),
              compare_precision_items);
        for (size_t i = 1; i < input->resource_precision_count; ++i) {
            if (compare_bytes(sorted[i - 1U].input->resource_key,
                              sorted[i].input->resource_key) == 0) {
                free(sorted);
                return ORACLE_PACK_DUPLICATE_KEY;
            }
        }
    }

    size_t total = ORACLE_PACK_METADATA_HEADER_SIZE;
    if (!add_size(total, input->canonical_bytes.size, &total)) {
        free(sorted);
        return ORACLE_PACK_SIZE_OVERFLOW;
    }
    for (size_t i = 0; i < input->resource_precision_count; ++i) {
        if (!add_size(total, ORACLE_PACK_PRECISION_HEADER_SIZE, &total) ||
            !add_size(total, sorted[i].input->resource_key.size, &total)) {
            free(sorted);
            return ORACLE_PACK_SIZE_OVERFLOW;
        }
    }
    uint8_t* data = (uint8_t*)malloc(total);
    if (!data) {
        free(sorted);
        return ORACLE_PACK_ALLOCATION_FAILED;
    }
    memcpy(data, k_metadata_magic, sizeof(k_metadata_magic));
    store_le32(data + 8U, ORACLE_PACK_METADATA_FORMAT_VERSION);
    store_le32(data + 12U, ORACLE_PACK_METADATA_HEADER_SIZE);
    store_le64(data + 16U, (uint64_t)total);
    store_le64(data + 24U, (uint64_t)input->canonical_bytes.size);
    store_le64(data + 32U, (uint64_t)input->resource_precision_count);
    size_t offset = ORACLE_PACK_METADATA_HEADER_SIZE;
    if (input->canonical_bytes.size > 0U) {
        memcpy(data + offset, input->canonical_bytes.data,
               input->canonical_bytes.size);
        offset += input->canonical_bytes.size;
    }
    for (size_t i = 0; i < input->resource_precision_count; ++i) {
        const OraclePackResourcePrecisionInput* precision = sorted[i].input;
        store_le64(data + offset, (uint64_t)precision->resource_key.size);
        store_le32(data + offset + 8U, (uint32_t)precision->precision);
        store_le32(data + offset + 12U, 0U);
        offset += ORACLE_PACK_PRECISION_HEADER_SIZE;
        memcpy(data + offset, precision->resource_key.data,
               precision->resource_key.size);
        offset += precision->resource_key.size;
    }
    free(sorted);
    output->data = data;
    output->size = total;
    return ORACLE_PACK_OK;
}

static void pack_digest(const uint8_t* data, size_t size,
                        uint8_t digest[ORACLE_PACK_DIGEST_SIZE]) {
    static const uint8_t zeros[ORACLE_PACK_DIGEST_SIZE] = {0};
    CommonSha256Context context;
    common_sha256_init(&context);
    common_sha256_update(&context, data, 152U);
    common_sha256_update(&context, zeros, sizeof(zeros));
    if (size > 184U) {
        common_sha256_update(&context, data + 184U, size - 184U);
    }
    common_sha256_final(&context, digest);
}

static OraclePackStatus parse_entry(
    const uint8_t* data, size_t size,
    const uint8_t expected_digest[ORACLE_PACK_DIGEST_SIZE],
    bool validate_payload, OraclePackEntryView* out_entry) {
    if (size < ORACLE_PACK_ENTRY_HEADER_SIZE ||
        memcmp(data, k_entry_magic, sizeof(k_entry_magic)) != 0 ||
        load_le32(data + 8U) != ORACLE_PACK_FORMAT_VERSION ||
        load_le32(data + 12U) != ORACLE_PACK_ENTRY_HEADER_SIZE ||
        load_le64(data + 16U) != (uint64_t)size ||
        load_le32(data + 28U) != 0U) {
        return ORACLE_PACK_CORRUPT;
    }
    uint32_t flags = load_le32(data + 24U);
    if ((flags & ~ORACLE_PACK_KNOWN_FLAGS) != 0U) {
        return ORACLE_PACK_CORRUPT;
    }
    size_t lengths[5];
    for (size_t i = 0; i < 5U; ++i) {
        if (!u64_to_size(load_le64(data + 32U + i * 8U), &lengths[i])) {
            return ORACLE_PACK_CORRUPT;
        }
    }
    bool glcore_present =
        (flags & ORACLE_PACK_FLAG_GLCORE_PRESENT) != 0U;
    if (lengths[0] == 0U || lengths[1] == 0U || lengths[3] == 0U ||
        lengths[4] == 0U || glcore_present != (lengths[2] != 0U) ||
        memcmp(data + 72U, expected_digest, ORACLE_PACK_DIGEST_SIZE) != 0) {
        return ORACLE_PACK_CORRUPT;
    }
    size_t offset = ORACLE_PACK_ENTRY_HEADER_SIZE;
    size_t starts[5];
    for (size_t i = 0; i < 5U; ++i) {
        starts[i] = offset;
        if (!add_size(offset, lengths[i], &offset) || offset > size) {
            return ORACLE_PACK_CORRUPT;
        }
    }
    if (offset != size) {
        return ORACLE_PACK_CORRUPT;
    }
    OraclePackBytes metadata = {data + starts[4], lengths[4]};
    if (validate_payload) {
        if (!validate_variant_key_bytes(data + starts[0], lengths[0],
                                        expected_digest) ||
            !validate_dxbc(data + starts[1], lengths[1])) {
            return ORACLE_PACK_CORRUPT;
        }
        uint8_t transcript_digest[COMMON_SHA256_DIGEST_SIZE];
        common_sha256(data + starts[3], lengths[3], transcript_digest);
        if (memcmp(transcript_digest, data + 104U,
                   sizeof(transcript_digest)) != 0 ||
            parse_metadata(metadata, NULL) != ORACLE_PACK_OK) {
            return ORACLE_PACK_CORRUPT;
        }
    }
    if (out_entry) {
        out_entry->variant_key_bytes =
            (OraclePackBytes){data + starts[0], lengths[0]};
        out_entry->variant_key_digest = data + 72U;
        out_entry->stripped_dxbc =
            (OraclePackBytes){data + starts[1], lengths[1]};
        out_entry->linked_glcore =
            (OraclePackBytes){lengths[2] ? data + starts[2] : NULL,
                              lengths[2]};
        out_entry->compile_request_transcript =
            (OraclePackBytes){data + starts[3], lengths[3]};
        out_entry->compile_request_digest = data + 104U;
        out_entry->normalized_metadata_bytes = metadata;
    }
    return ORACLE_PACK_OK;
}

static OraclePackStatus parse_preprocess(
    const uint8_t* data, size_t size,
    const uint8_t expected_digest[ORACLE_PACK_DIGEST_SIZE],
    bool validate_payload, OraclePackPreprocessView* out_preprocess) {
    if (size < ORACLE_PACK_PREPROCESS_HEADER_SIZE ||
        memcmp(data, k_preprocess_magic,
               sizeof(k_preprocess_magic)) != 0 ||
        load_le32(data + 8U) != ORACLE_PACK_FORMAT_VERSION ||
        load_le32(data + 12U) != ORACLE_PACK_PREPROCESS_HEADER_SIZE ||
        load_le64(data + 16U) != (uint64_t)size ||
        load_le32(data + 24U) != 0U ||
        load_le32(data + 28U) != 0U) {
        return ORACLE_PACK_CORRUPT;
    }
    size_t transcript_size;
    size_t result_size;
    if (!u64_to_size(load_le64(data + 32U), &transcript_size) ||
        !u64_to_size(load_le64(data + 40U), &result_size) ||
        transcript_size == 0U || result_size == 0U ||
        memcmp(data + 48U, expected_digest,
               ORACLE_PACK_DIGEST_SIZE) != 0) {
        return ORACLE_PACK_CORRUPT;
    }
    size_t result_offset;
    size_t end_offset;
    if (!add_size(ORACLE_PACK_PREPROCESS_HEADER_SIZE, transcript_size,
                  &result_offset) ||
        !add_size(result_offset, result_size, &end_offset) ||
        end_offset != size) {
        return ORACLE_PACK_CORRUPT;
    }
    if (validate_payload) {
        uint8_t digest[ORACLE_PACK_DIGEST_SIZE];
        common_sha256(data + ORACLE_PACK_PREPROCESS_HEADER_SIZE,
                      transcript_size, digest);
        if (memcmp(digest, data + 48U, sizeof(digest)) != 0) {
            return ORACLE_PACK_CORRUPT;
        }
        common_sha256(data + result_offset, result_size, digest);
        if (memcmp(digest, data + 80U, sizeof(digest)) != 0) {
            return ORACLE_PACK_CORRUPT;
        }
    }
    if (out_preprocess) {
        out_preprocess->request_transcript = (OraclePackBytes){
            data + ORACLE_PACK_PREPROCESS_HEADER_SIZE, transcript_size};
        out_preprocess->request_digest = data + 48U;
        out_preprocess->serialized_result = (OraclePackBytes){
            data + result_offset, result_size};
        out_preprocess->result_digest = data + 80U;
    }
    return ORACLE_PACK_OK;
}

static OraclePackStatus validate_pack(
    const uint8_t* data, size_t size, size_t* out_entry_count,
    size_t* out_preprocess_count, size_t* out_compile_directory_offset,
    size_t* out_preprocess_directory_offset) {
    /* Read the fixed prefix first so every older format, including a short
     * but otherwise valid v3 empty pack, is rejected explicitly rather than
     * being misreported as a truncated v4 pack. */
    if (size < 16U) return ORACLE_PACK_TRUNCATED;
    if (memcmp(data, k_pack_magic, sizeof(k_pack_magic)) != 0) {
        return ORACLE_PACK_BAD_MAGIC;
    }
    if (load_le32(data + 8U) != ORACLE_PACK_FORMAT_VERSION) {
        return ORACLE_PACK_UNSUPPORTED_VERSION;
    }
    if (size < ORACLE_PACK_HEADER_SIZE) return ORACLE_PACK_TRUNCATED;
    if (load_le32(data + 12U) != ORACLE_PACK_HEADER_SIZE) {
        return ORACLE_PACK_CORRUPT;
    }
    uint64_t declared_size = load_le64(data + 16U);
    if (declared_size > (uint64_t)size) return ORACLE_PACK_TRUNCATED;
    if (declared_size != (uint64_t)size) return ORACLE_PACK_CORRUPT;

    uint8_t digest[ORACLE_PACK_DIGEST_SIZE];
    pack_digest(data, size, digest);
    if (!fingerprint_has_value(data + 88U) ||
        !fingerprint_has_value(data + 120U) ||
        memcmp(digest, data + 152U, sizeof(digest)) != 0 ||
        load_le64(data + 184U) != 0U) {
        return ORACLE_PACK_CORRUPT;
    }

    size_t entry_count;
    size_t compile_directory_offset;
    size_t compile_directory_size;
    size_t preprocess_count;
    size_t preprocess_directory_offset;
    size_t preprocess_directory_size;
    size_t payload_offset;
    size_t payload_size;
    if (!u64_to_size(load_le64(data + 24U), &entry_count) ||
        !u64_to_size(load_le64(data + 32U), &compile_directory_offset) ||
        !u64_to_size(load_le64(data + 40U), &compile_directory_size) ||
        !u64_to_size(load_le64(data + 48U), &preprocess_count) ||
        !u64_to_size(load_le64(data + 56U), &preprocess_directory_offset) ||
        !u64_to_size(load_le64(data + 64U), &preprocess_directory_size) ||
        !u64_to_size(load_le64(data + 72U), &payload_offset) ||
        !u64_to_size(load_le64(data + 80U), &payload_size)) {
        return ORACLE_PACK_CORRUPT;
    }
    size_t expected_compile_directory_size;
    size_t expected_preprocess_directory_size;
    size_t expected_preprocess_directory_offset;
    size_t expected_payload_offset;
    if (!multiply_size(entry_count, ORACLE_PACK_DIRECTORY_ENTRY_SIZE,
                       &expected_compile_directory_size) ||
        !multiply_size(preprocess_count, ORACLE_PACK_DIRECTORY_ENTRY_SIZE,
                       &expected_preprocess_directory_size) ||
        !add_size(ORACLE_PACK_HEADER_SIZE,
                  expected_compile_directory_size,
                  &expected_preprocess_directory_offset) ||
        !add_size(expected_preprocess_directory_offset,
                  expected_preprocess_directory_size,
                  &expected_payload_offset) ||
        compile_directory_offset != ORACLE_PACK_HEADER_SIZE ||
        compile_directory_size != expected_compile_directory_size ||
        preprocess_directory_offset !=
            expected_preprocess_directory_offset ||
        preprocess_directory_size != expected_preprocess_directory_size ||
        payload_offset != expected_payload_offset || payload_offset > size ||
        payload_size != size - payload_offset) {
        return ORACLE_PACK_CORRUPT;
    }

    size_t payload_cursor = payload_offset;
    const uint8_t* previous_digest = NULL;
    for (size_t i = 0; i < entry_count; ++i) {
        const uint8_t* directory =
            data + compile_directory_offset +
            i * ORACLE_PACK_DIRECTORY_ENTRY_SIZE;
        if (previous_digest &&
            memcmp(previous_digest, directory, ORACLE_PACK_DIGEST_SIZE) >= 0) {
            return ORACLE_PACK_CORRUPT;
        }
        previous_digest = directory;
        size_t entry_offset;
        size_t entry_size;
        if (!u64_to_size(load_le64(directory + 32U), &entry_offset) ||
            !u64_to_size(load_le64(directory + 40U), &entry_size) ||
            entry_offset > size || entry_offset != payload_cursor ||
            entry_size > size - entry_offset) {
            return ORACLE_PACK_CORRUPT;
        }
        OraclePackStatus entry_status = parse_entry(
            data + entry_offset, entry_size, directory, true, NULL);
        if (entry_status != ORACLE_PACK_OK) return entry_status;
        if (!add_size(payload_cursor, entry_size, &payload_cursor) ||
            payload_cursor > size) {
            return ORACLE_PACK_CORRUPT;
        }
    }
    previous_digest = NULL;
    for (size_t i = 0; i < preprocess_count; ++i) {
        const uint8_t* directory =
            data + preprocess_directory_offset +
            i * ORACLE_PACK_DIRECTORY_ENTRY_SIZE;
        if (previous_digest &&
            memcmp(previous_digest, directory,
                   ORACLE_PACK_DIGEST_SIZE) >= 0) {
            return ORACLE_PACK_CORRUPT;
        }
        previous_digest = directory;
        size_t record_offset;
        size_t record_size;
        if (!u64_to_size(load_le64(directory + 32U), &record_offset) ||
            !u64_to_size(load_le64(directory + 40U), &record_size) ||
            record_offset > size || record_offset != payload_cursor ||
            record_size > size - record_offset) {
            return ORACLE_PACK_CORRUPT;
        }
        OraclePackStatus record_status = parse_preprocess(
            data + record_offset, record_size, directory, true, NULL);
        if (record_status != ORACLE_PACK_OK) return record_status;
        if (!add_size(payload_cursor, record_size, &payload_cursor) ||
            payload_cursor > size) {
            return ORACLE_PACK_CORRUPT;
        }
    }
    if (payload_cursor != size) return ORACLE_PACK_CORRUPT;
    *out_entry_count = entry_count;
    *out_preprocess_count = preprocess_count;
    *out_compile_directory_offset = compile_directory_offset;
    *out_preprocess_directory_offset = preprocess_directory_offset;
    return ORACLE_PACK_OK;
}

OraclePackStatus oracle_pack_writer_create(
    const OraclePackAuthorityInput* authority,
    OraclePackWriter** out_writer) {
    if (!out_writer) return ORACLE_PACK_INVALID_ARGUMENT;
    *out_writer = NULL;
    if (!authority || !authority->compiler_fingerprint ||
        !authority->environment_fingerprint) {
        return ORACLE_PACK_INVALID_ARGUMENT;
    }
    if (!authority_is_valid(authority)) return ORACLE_PACK_INVALID_VALUE;
    OraclePackWriter* writer =
        (OraclePackWriter*)calloc(1, sizeof(*writer));
    if (!writer) return ORACLE_PACK_ALLOCATION_FAILED;
    memcpy(writer->compiler_fingerprint, authority->compiler_fingerprint,
           ORACLE_PACK_DIGEST_SIZE);
    memcpy(writer->environment_fingerprint,
           authority->environment_fingerprint, ORACLE_PACK_DIGEST_SIZE);
    *out_writer = writer;
    return ORACLE_PACK_OK;
}

void oracle_pack_writer_free(OraclePackWriter* writer) {
    if (!writer) return;
    for (size_t i = 0; i < writer->count; ++i) {
        owned_entry_free(&writer->entries[i]);
    }
    for (size_t i = 0; i < writer->preprocess_count; ++i) {
        owned_preprocess_free(&writer->preprocesses[i]);
    }
    free(writer->entries);
    free(writer->preprocesses);
    free(writer);
}

OraclePackStatus oracle_pack_writer_add(
    OraclePackWriter* writer, const OraclePackEntryInput* input) {
    if (!writer || !input || !input->variant_key ||
        !bytes_are_consistent(input->stripped_dxbc) ||
        !bytes_are_consistent(input->linked_glcore) ||
        !bytes_are_consistent(input->compile_request_transcript) ||
        !input->authority.compiler_fingerprint ||
        !input->authority.environment_fingerprint) {
        return ORACLE_PACK_INVALID_ARGUMENT;
    }
    if (writer->finalized) return ORACLE_PACK_ALREADY_FINALIZED;
    if (!authority_matches_writer(writer, &input->authority)) {
        return ORACLE_PACK_AUTHORITY_MISMATCH;
    }
    if (input->stripped_dxbc.size == 0U ||
        input->compile_request_transcript.size == 0U) {
        return ORACLE_PACK_INVALID_VALUE;
    }
    if (!validate_dxbc(input->stripped_dxbc.data,
                       input->stripped_dxbc.size)) {
        return ORACLE_PACK_INVALID_DXBC;
    }

    OwnedEntry entry;
    memset(&entry, 0, sizeof(entry));
    uint8_t* variant_bytes = NULL;
    size_t variant_size = 0;
    VariantKeyStatus key_status = variant_key_serialize(
        input->variant_key, &variant_bytes, &variant_size);
    if (key_status != VARIANT_KEY_OK) return variant_status(key_status);
    entry.variant_key.data = variant_bytes;
    entry.variant_key.size = variant_size;
    key_status = variant_key_digest(input->variant_key,
                                    entry.variant_key_digest);
    if (key_status != VARIANT_KEY_OK) {
        owned_entry_free(&entry);
        return variant_status(key_status);
    }
    for (size_t i = 0; i < writer->count; ++i) {
        if (memcmp(writer->entries[i].variant_key_digest,
                   entry.variant_key_digest, ORACLE_PACK_DIGEST_SIZE) == 0) {
            owned_entry_free(&entry);
            return ORACLE_PACK_DUPLICATE_KEY;
        }
    }

    OraclePackStatus metadata_status = encode_metadata(
        &input->normalized_metadata, &entry.metadata);
    if (metadata_status != ORACLE_PACK_OK) {
        owned_entry_free(&entry);
        return metadata_status;
    }
    if (!owned_bytes_copy(&entry.stripped_dxbc, input->stripped_dxbc) ||
        !owned_bytes_copy(&entry.linked_glcore, input->linked_glcore) ||
        !owned_bytes_copy(&entry.transcript,
                          input->compile_request_transcript)) {
        owned_entry_free(&entry);
        return ORACLE_PACK_ALLOCATION_FAILED;
    }
    common_sha256(entry.transcript.data, entry.transcript.size,
                  entry.transcript_digest);

    if (writer->count == writer->capacity) {
        size_t new_capacity = writer->capacity == 0U ? 8U :
                              writer->capacity * 2U;
        if (new_capacity < writer->capacity ||
            new_capacity > SIZE_MAX / sizeof(*writer->entries)) {
            owned_entry_free(&entry);
            return ORACLE_PACK_SIZE_OVERFLOW;
        }
        OwnedEntry* resized = (OwnedEntry*)realloc(
            writer->entries, new_capacity * sizeof(*writer->entries));
        if (!resized) {
            owned_entry_free(&entry);
            return ORACLE_PACK_ALLOCATION_FAILED;
        }
        writer->entries = resized;
        writer->capacity = new_capacity;
    }
    writer->entries[writer->count++] = entry;
    return ORACLE_PACK_OK;
}

OraclePackStatus oracle_pack_writer_add_preprocess(
    OraclePackWriter* writer, const OraclePackPreprocessInput* input) {
    if (!writer || !input ||
        !bytes_are_consistent(input->request_transcript) ||
        !bytes_are_consistent(input->serialized_result) ||
        !input->authority.compiler_fingerprint ||
        !input->authority.environment_fingerprint) {
        return ORACLE_PACK_INVALID_ARGUMENT;
    }
    if (writer->finalized) return ORACLE_PACK_ALREADY_FINALIZED;
    if (!authority_matches_writer(writer, &input->authority)) {
        return ORACLE_PACK_AUTHORITY_MISMATCH;
    }
    if (input->request_transcript.size == 0U ||
        input->serialized_result.size == 0U) {
        return ORACLE_PACK_INVALID_VALUE;
    }

    OwnedPreprocess preprocess;
    memset(&preprocess, 0, sizeof(preprocess));
    common_sha256(input->request_transcript.data,
                  input->request_transcript.size,
                  preprocess.request_digest);
    common_sha256(input->serialized_result.data,
                  input->serialized_result.size,
                  preprocess.result_digest);
    for (size_t i = 0; i < writer->preprocess_count; ++i) {
        const OwnedPreprocess* existing = &writer->preprocesses[i];
        if (memcmp(existing->request_digest, preprocess.request_digest,
                   ORACLE_PACK_DIGEST_SIZE) != 0) {
            continue;
        }
        bool identical =
            existing->transcript.size == input->request_transcript.size &&
            existing->result.size == input->serialized_result.size &&
            memcmp(existing->transcript.data,
                   input->request_transcript.data,
                   input->request_transcript.size) == 0 &&
            memcmp(existing->result.data, input->serialized_result.data,
                   input->serialized_result.size) == 0;
        return identical ? ORACLE_PACK_OK : ORACLE_PACK_DUPLICATE_KEY;
    }
    if (!owned_bytes_copy(&preprocess.transcript,
                          input->request_transcript) ||
        !owned_bytes_copy(&preprocess.result,
                          input->serialized_result)) {
        owned_preprocess_free(&preprocess);
        return ORACLE_PACK_ALLOCATION_FAILED;
    }
    if (writer->preprocess_count == writer->preprocess_capacity) {
        size_t new_capacity = writer->preprocess_capacity == 0U ? 8U :
                              writer->preprocess_capacity * 2U;
        if (new_capacity < writer->preprocess_capacity ||
            new_capacity > SIZE_MAX / sizeof(*writer->preprocesses)) {
            owned_preprocess_free(&preprocess);
            return ORACLE_PACK_SIZE_OVERFLOW;
        }
        OwnedPreprocess* resized = (OwnedPreprocess*)realloc(
            writer->preprocesses,
            new_capacity * sizeof(*writer->preprocesses));
        if (!resized) {
            owned_preprocess_free(&preprocess);
            return ORACLE_PACK_ALLOCATION_FAILED;
        }
        writer->preprocesses = resized;
        writer->preprocess_capacity = new_capacity;
    }
    writer->preprocesses[writer->preprocess_count++] = preprocess;
    return ORACLE_PACK_OK;
}

OraclePackStatus oracle_pack_writer_finalize(
    OraclePackWriter* writer, uint8_t** out_data, size_t* out_size) {
    if (!writer || !out_data || !out_size) {
        return ORACLE_PACK_INVALID_ARGUMENT;
    }
    if (writer->finalized) return ORACLE_PACK_ALREADY_FINALIZED;
    *out_data = NULL;
    *out_size = 0;
    writer->finalized = true;
    if (writer->count > 1U) {
        qsort(writer->entries, writer->count, sizeof(*writer->entries),
              compare_owned_entries);
    }
    for (size_t i = 1; i < writer->count; ++i) {
        if (memcmp(writer->entries[i - 1U].variant_key_digest,
                   writer->entries[i].variant_key_digest,
                   ORACLE_PACK_DIGEST_SIZE) >= 0) {
            return ORACLE_PACK_DUPLICATE_KEY;
        }
    }
    if (writer->preprocess_count > 1U) {
        qsort(writer->preprocesses, writer->preprocess_count,
              sizeof(*writer->preprocesses), compare_owned_preprocesses);
    }
    for (size_t i = 1; i < writer->preprocess_count; ++i) {
        if (memcmp(writer->preprocesses[i - 1U].request_digest,
                   writer->preprocesses[i].request_digest,
                   ORACLE_PACK_DIGEST_SIZE) >= 0) {
            return ORACLE_PACK_DUPLICATE_KEY;
        }
    }

    size_t compile_directory_size;
    size_t preprocess_directory_size;
    size_t preprocess_directory_offset;
    size_t payload_offset;
    if (!multiply_size(writer->count, ORACLE_PACK_DIRECTORY_ENTRY_SIZE,
                       &compile_directory_size) ||
        !multiply_size(writer->preprocess_count,
                       ORACLE_PACK_DIRECTORY_ENTRY_SIZE,
                       &preprocess_directory_size) ||
        !add_size(ORACLE_PACK_HEADER_SIZE, compile_directory_size,
                  &preprocess_directory_offset) ||
        !add_size(preprocess_directory_offset, preprocess_directory_size,
                  &payload_offset)) {
        return ORACLE_PACK_SIZE_OVERFLOW;
    }
    size_t total_size = payload_offset;
    for (size_t i = 0; i < writer->count; ++i) {
        const OwnedEntry* entry = &writer->entries[i];
        size_t entry_size = ORACLE_PACK_ENTRY_HEADER_SIZE;
        if (!add_size(entry_size, entry->variant_key.size, &entry_size) ||
            !add_size(entry_size, entry->stripped_dxbc.size, &entry_size) ||
            !add_size(entry_size, entry->linked_glcore.size, &entry_size) ||
            !add_size(entry_size, entry->transcript.size, &entry_size) ||
            !add_size(entry_size, entry->metadata.size, &entry_size) ||
            !add_size(total_size, entry_size, &total_size)) {
            return ORACLE_PACK_SIZE_OVERFLOW;
        }
    }
    for (size_t i = 0; i < writer->preprocess_count; ++i) {
        const OwnedPreprocess* preprocess = &writer->preprocesses[i];
        size_t record_size = ORACLE_PACK_PREPROCESS_HEADER_SIZE;
        if (!add_size(record_size, preprocess->transcript.size,
                      &record_size) ||
            !add_size(record_size, preprocess->result.size,
                      &record_size) ||
            !add_size(total_size, record_size, &total_size)) {
            return ORACLE_PACK_SIZE_OVERFLOW;
        }
    }
    uint8_t* data = (uint8_t*)calloc(1, total_size);
    if (!data) return ORACLE_PACK_ALLOCATION_FAILED;

    memcpy(data, k_pack_magic, sizeof(k_pack_magic));
    store_le32(data + 8U, ORACLE_PACK_FORMAT_VERSION);
    store_le32(data + 12U, ORACLE_PACK_HEADER_SIZE);
    store_le64(data + 16U, (uint64_t)total_size);
    store_le64(data + 24U, (uint64_t)writer->count);
    store_le64(data + 32U, ORACLE_PACK_HEADER_SIZE);
    store_le64(data + 40U, (uint64_t)compile_directory_size);
    store_le64(data + 48U, (uint64_t)writer->preprocess_count);
    store_le64(data + 56U, (uint64_t)preprocess_directory_offset);
    store_le64(data + 64U, (uint64_t)preprocess_directory_size);
    store_le64(data + 72U, (uint64_t)payload_offset);
    store_le64(data + 80U, (uint64_t)(total_size - payload_offset));
    memcpy(data + 88U, writer->compiler_fingerprint,
           ORACLE_PACK_DIGEST_SIZE);
    memcpy(data + 120U, writer->environment_fingerprint,
           ORACLE_PACK_DIGEST_SIZE);

    size_t entry_offset = payload_offset;
    for (size_t i = 0; i < writer->count; ++i) {
        const OwnedEntry* entry = &writer->entries[i];
        uint8_t* directory = data + ORACLE_PACK_HEADER_SIZE +
                             i * ORACLE_PACK_DIRECTORY_ENTRY_SIZE;
        memcpy(directory, entry->variant_key_digest, ORACLE_PACK_DIGEST_SIZE);
        store_le64(directory + 32U, (uint64_t)entry_offset);

        size_t entry_size = ORACLE_PACK_ENTRY_HEADER_SIZE +
                            entry->variant_key.size +
                            entry->stripped_dxbc.size +
                            entry->linked_glcore.size +
                            entry->transcript.size + entry->metadata.size;
        store_le64(directory + 40U, (uint64_t)entry_size);

        uint8_t* encoded = data + entry_offset;
        memcpy(encoded, k_entry_magic, sizeof(k_entry_magic));
        store_le32(encoded + 8U, ORACLE_PACK_FORMAT_VERSION);
        store_le32(encoded + 12U, ORACLE_PACK_ENTRY_HEADER_SIZE);
        store_le64(encoded + 16U, (uint64_t)entry_size);
        store_le32(encoded + 24U,
                   entry->linked_glcore.size > 0U ?
                       ORACLE_PACK_FLAG_GLCORE_PRESENT : 0U);
        store_le32(encoded + 28U, 0U);
        store_le64(encoded + 32U, (uint64_t)entry->variant_key.size);
        store_le64(encoded + 40U, (uint64_t)entry->stripped_dxbc.size);
        store_le64(encoded + 48U, (uint64_t)entry->linked_glcore.size);
        store_le64(encoded + 56U, (uint64_t)entry->transcript.size);
        store_le64(encoded + 64U, (uint64_t)entry->metadata.size);
        memcpy(encoded + 72U, entry->variant_key_digest,
               ORACLE_PACK_DIGEST_SIZE);
        memcpy(encoded + 104U, entry->transcript_digest,
               ORACLE_PACK_DIGEST_SIZE);

        size_t offset = ORACLE_PACK_ENTRY_HEADER_SIZE;
#define COPY_ENTRY_BYTES(field) do { \
    if (entry->field.size > 0U) { \
        memcpy(encoded + offset, entry->field.data, entry->field.size); \
        offset += entry->field.size; \
    } \
} while (0)
        COPY_ENTRY_BYTES(variant_key);
        COPY_ENTRY_BYTES(stripped_dxbc);
        COPY_ENTRY_BYTES(linked_glcore);
        COPY_ENTRY_BYTES(transcript);
        COPY_ENTRY_BYTES(metadata);
#undef COPY_ENTRY_BYTES
        entry_offset += entry_size;
    }
    for (size_t i = 0; i < writer->preprocess_count; ++i) {
        const OwnedPreprocess* preprocess = &writer->preprocesses[i];
        uint8_t* directory = data + preprocess_directory_offset +
            i * ORACLE_PACK_DIRECTORY_ENTRY_SIZE;
        memcpy(directory, preprocess->request_digest,
               ORACLE_PACK_DIGEST_SIZE);
        store_le64(directory + 32U, (uint64_t)entry_offset);
        size_t record_size = ORACLE_PACK_PREPROCESS_HEADER_SIZE +
            preprocess->transcript.size + preprocess->result.size;
        store_le64(directory + 40U, (uint64_t)record_size);

        uint8_t* encoded = data + entry_offset;
        memcpy(encoded, k_preprocess_magic, sizeof(k_preprocess_magic));
        store_le32(encoded + 8U, ORACLE_PACK_FORMAT_VERSION);
        store_le32(encoded + 12U, ORACLE_PACK_PREPROCESS_HEADER_SIZE);
        store_le64(encoded + 16U, (uint64_t)record_size);
        store_le64(encoded + 32U, (uint64_t)preprocess->transcript.size);
        store_le64(encoded + 40U, (uint64_t)preprocess->result.size);
        memcpy(encoded + 48U, preprocess->request_digest,
               ORACLE_PACK_DIGEST_SIZE);
        memcpy(encoded + 80U, preprocess->result_digest,
               ORACLE_PACK_DIGEST_SIZE);
        memcpy(encoded + ORACLE_PACK_PREPROCESS_HEADER_SIZE,
               preprocess->transcript.data, preprocess->transcript.size);
        memcpy(encoded + ORACLE_PACK_PREPROCESS_HEADER_SIZE +
                   preprocess->transcript.size,
               preprocess->result.data, preprocess->result.size);
        entry_offset += record_size;
    }
    uint8_t digest[ORACLE_PACK_DIGEST_SIZE];
    pack_digest(data, total_size, digest);
    memcpy(data + 152U, digest, sizeof(digest));
    *out_data = data;
    *out_size = total_size;
    return ORACLE_PACK_OK;
}

void oracle_pack_bytes_free(uint8_t* data) {
    free(data);
}

OraclePackStatus oracle_pack_open_memory(
    const uint8_t* data, size_t size, OraclePack** out_pack) {
    if (!data || !out_pack) return ORACLE_PACK_INVALID_ARGUMENT;
    *out_pack = NULL;
    size_t entry_count = 0;
    size_t preprocess_count = 0;
    size_t compile_directory_offset = 0;
    size_t preprocess_directory_offset = 0;
    OraclePackStatus status = validate_pack(
        data, size, &entry_count, &preprocess_count,
        &compile_directory_offset, &preprocess_directory_offset);
    if (status != ORACLE_PACK_OK) return status;
    OraclePack* pack = (OraclePack*)calloc(1, sizeof(*pack));
    if (!pack) return ORACLE_PACK_ALLOCATION_FAILED;
    pack->data = (uint8_t*)malloc(size);
    if (!pack->data) {
        free(pack);
        return ORACLE_PACK_ALLOCATION_FAILED;
    }
    memcpy(pack->data, data, size);
    pack->size = size;
    pack->entry_count = entry_count;
    pack->preprocess_count = preprocess_count;
    pack->compile_directory_offset = compile_directory_offset;
    pack->preprocess_directory_offset = preprocess_directory_offset;
    *out_pack = pack;
    return ORACLE_PACK_OK;
}

void oracle_pack_free(OraclePack* pack) {
    if (!pack) return;
    free(pack->data);
    free(pack);
}

size_t oracle_pack_entry_count(const OraclePack* pack) {
    return pack ? pack->entry_count : 0U;
}

size_t oracle_pack_preprocess_count(const OraclePack* pack) {
    return pack ? pack->preprocess_count : 0U;
}

OraclePackStatus oracle_pack_authority(
    const OraclePack* pack, OraclePackAuthorityView* out_authority) {
    if (!out_authority) return ORACLE_PACK_INVALID_ARGUMENT;
    out_authority->compiler_fingerprint = NULL;
    out_authority->environment_fingerprint = NULL;
    if (!pack) return ORACLE_PACK_INVALID_ARGUMENT;
    out_authority->compiler_fingerprint = pack->data + 88U;
    out_authority->environment_fingerprint = pack->data + 120U;
    return ORACLE_PACK_OK;
}

OraclePackStatus oracle_pack_entry_at(
    const OraclePack* pack, size_t index, OraclePackEntryView* out_entry) {
    if (!pack || !out_entry) return ORACLE_PACK_INVALID_ARGUMENT;
    memset(out_entry, 0, sizeof(*out_entry));
    if (index >= pack->entry_count) return ORACLE_PACK_NOT_FOUND;
    const uint8_t* directory = pack->data + pack->compile_directory_offset +
                               index * ORACLE_PACK_DIRECTORY_ENTRY_SIZE;
    size_t entry_offset = (size_t)load_le64(directory + 32U);
    size_t entry_size = (size_t)load_le64(directory + 40U);
    return parse_entry(pack->data + entry_offset, entry_size, directory,
                       false, out_entry);
}

OraclePackStatus oracle_pack_lookup_digest(
    const OraclePack* pack,
    const uint8_t variant_key_digest[ORACLE_PACK_DIGEST_SIZE],
    OraclePackEntryView* out_entry) {
    if (!pack || !variant_key_digest || !out_entry) {
        return ORACLE_PACK_INVALID_ARGUMENT;
    }
    memset(out_entry, 0, sizeof(*out_entry));
    size_t low = 0;
    size_t high = pack->entry_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const uint8_t* directory = pack->data +
            pack->compile_directory_offset +
            middle * ORACLE_PACK_DIRECTORY_ENTRY_SIZE;
        int comparison = memcmp(variant_key_digest, directory,
                                ORACLE_PACK_DIGEST_SIZE);
        if (comparison < 0) {
            high = middle;
        } else if (comparison > 0) {
            low = middle + 1U;
        } else {
            return oracle_pack_entry_at(pack, middle, out_entry);
        }
    }
    return ORACLE_PACK_NOT_FOUND;
}

OraclePackStatus oracle_pack_lookup_variant_key(
    const OraclePack* pack, const VariantKey* key,
    OraclePackEntryView* out_entry) {
    if (!pack || !key || !out_entry) return ORACLE_PACK_INVALID_ARGUMENT;
    uint8_t digest[VARIANT_KEY_DIGEST_SIZE];
    VariantKeyStatus status = variant_key_digest(key, digest);
    if (status != VARIANT_KEY_OK) return variant_status(status);
    return oracle_pack_lookup_digest(pack, digest, out_entry);
}

OraclePackStatus oracle_pack_preprocess_at(
    const OraclePack* pack, size_t index,
    OraclePackPreprocessView* out_preprocess) {
    if (!pack || !out_preprocess) return ORACLE_PACK_INVALID_ARGUMENT;
    memset(out_preprocess, 0, sizeof(*out_preprocess));
    if (index >= pack->preprocess_count) return ORACLE_PACK_NOT_FOUND;
    const uint8_t* directory = pack->data +
        pack->preprocess_directory_offset +
        index * ORACLE_PACK_DIRECTORY_ENTRY_SIZE;
    size_t record_offset = (size_t)load_le64(directory + 32U);
    size_t record_size = (size_t)load_le64(directory + 40U);
    return parse_preprocess(pack->data + record_offset, record_size,
                            directory, false, out_preprocess);
}

OraclePackStatus oracle_pack_lookup_preprocess_digest(
    const OraclePack* pack,
    const uint8_t request_digest[ORACLE_PACK_DIGEST_SIZE],
    OraclePackPreprocessView* out_preprocess) {
    if (!pack || !request_digest || !out_preprocess) {
        return ORACLE_PACK_INVALID_ARGUMENT;
    }
    memset(out_preprocess, 0, sizeof(*out_preprocess));
    size_t low = 0U;
    size_t high = pack->preprocess_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const uint8_t* directory = pack->data +
            pack->preprocess_directory_offset +
            middle * ORACLE_PACK_DIRECTORY_ENTRY_SIZE;
        int comparison = memcmp(request_digest, directory,
                                ORACLE_PACK_DIGEST_SIZE);
        if (comparison < 0) {
            high = middle;
        } else if (comparison > 0) {
            low = middle + 1U;
        } else {
            return oracle_pack_preprocess_at(
                pack, middle, out_preprocess);
        }
    }
    return ORACLE_PACK_NOT_FOUND;
}

OraclePackStatus oracle_pack_metadata_view(
    OraclePackBytes encoded, OraclePackNormalizedMetadataView* out_metadata) {
    if (!out_metadata) return ORACLE_PACK_INVALID_ARGUMENT;
    memset(out_metadata, 0, sizeof(*out_metadata));
    MetadataLayout layout;
    OraclePackStatus status = parse_metadata(encoded, &layout);
    if (status != ORACLE_PACK_OK) return status;
    out_metadata->canonical_bytes = (OraclePackBytes){
        layout.canonical_size ? encoded.data + layout.canonical_offset : NULL,
        layout.canonical_size,
    };
    out_metadata->resource_precision_count = layout.record_count;
    return ORACLE_PACK_OK;
}

OraclePackStatus oracle_pack_metadata_precision_at(
    OraclePackBytes encoded, size_t index,
    OraclePackResourcePrecisionView* out_precision) {
    if (!out_precision) return ORACLE_PACK_INVALID_ARGUMENT;
    memset(out_precision, 0, sizeof(*out_precision));
    MetadataLayout layout;
    OraclePackStatus status = parse_metadata(encoded, &layout);
    if (status != ORACLE_PACK_OK) return status;
    if (index >= layout.record_count) return ORACLE_PACK_NOT_FOUND;
    size_t offset = layout.records_offset;
    for (size_t i = 0; i <= index; ++i) {
        size_t key_size = (size_t)load_le64(encoded.data + offset);
        OraclePackResourcePrecision precision =
            (OraclePackResourcePrecision)load_le32(encoded.data + offset + 8U);
        offset += ORACLE_PACK_PRECISION_HEADER_SIZE;
        if (i == index) {
            out_precision->resource_key =
                (OraclePackBytes){encoded.data + offset, key_size};
            out_precision->precision = precision;
            return ORACLE_PACK_OK;
        }
        offset += key_size;
    }
    return ORACLE_PACK_CORRUPT;
}

OraclePackStatus oracle_pack_metadata_matches_input(
    OraclePackBytes encoded,
    const OraclePackNormalizedMetadataInput* input,
    bool* out_equal) {
    if (!input || !out_equal || !bytes_are_consistent(encoded)) {
        return ORACLE_PACK_INVALID_ARGUMENT;
    }
    *out_equal = false;
    OraclePackStatus status = parse_metadata(encoded, NULL);
    if (status != ORACLE_PACK_OK) return status;

    OwnedBytes canonical;
    memset(&canonical, 0, sizeof(canonical));
    status = encode_metadata(input, &canonical);
    if (status != ORACLE_PACK_OK) return status;
    *out_equal = canonical.size == encoded.size &&
        memcmp(canonical.data, encoded.data, encoded.size) == 0;
    owned_bytes_free(&canonical);
    return ORACLE_PACK_OK;
}

const char* oracle_pack_status_string(OraclePackStatus status) {
    switch (status) {
        case ORACLE_PACK_OK: return "ok";
        case ORACLE_PACK_INVALID_ARGUMENT: return "invalid argument";
        case ORACLE_PACK_INVALID_VALUE: return "invalid value";
        case ORACLE_PACK_INVALID_DXBC: return "invalid DXBC container";
        case ORACLE_PACK_INVALID_METADATA: return "invalid metadata";
        case ORACLE_PACK_AUTHORITY_MISMATCH:
            return "authority mismatch";
        case ORACLE_PACK_DUPLICATE_KEY: return "duplicate key";
        case ORACLE_PACK_ALLOCATION_FAILED: return "allocation failed";
        case ORACLE_PACK_SIZE_OVERFLOW: return "size overflow";
        case ORACLE_PACK_ALREADY_FINALIZED: return "already finalized";
        case ORACLE_PACK_BAD_MAGIC: return "bad magic";
        case ORACLE_PACK_UNSUPPORTED_VERSION: return "unsupported version";
        case ORACLE_PACK_TRUNCATED: return "truncated";
        case ORACLE_PACK_CORRUPT: return "corrupt";
        case ORACLE_PACK_NOT_FOUND: return "not found";
        default: return "unknown";
    }
}
