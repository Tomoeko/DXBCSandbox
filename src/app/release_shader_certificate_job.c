// SPDX-License-Identifier: GPL-3.0-only

#include "app/release_shader_certificate_job.h"

#include "common/file_io.h"
#include "common/sha256.h"
#include "io/serialized_file.h"
#include "io/unity_input.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CERTIFICATE_TABLE_MAX_SIZE (64U * 1024U * 1024U)
#define CERTIFICATE_TABLE_MAX_ROWS 1048576U

static const char k_pairs_header[] =
    "expected_occurrence_id\texpected_content_id\t"
    "expected_serialized_sha256\tactual_occurrence_id\t"
    "actual_content_id\tactual_serialized_sha256";
static const char k_references_header[] =
    "side\towner_serialized_sha256\tfile_id\tpath_id\tstable_id_sha256";

void release_shader_certificate_pair_table_init(
    ReleaseShaderCertificatePairTable* table) {
    if (table) memset(table, 0, sizeof(*table));
}

void release_shader_certificate_pair_table_dispose(
    ReleaseShaderCertificatePairTable* table) {
    if (!table) return;
    free(table->rows);
    release_shader_certificate_pair_table_init(table);
}

void release_shader_certificate_reference_map_init(
    ReleaseShaderCertificateReferenceMap* map) {
    if (map) memset(map, 0, sizeof(*map));
}

void release_shader_certificate_reference_map_dispose(
    ReleaseShaderCertificateReferenceMap* map) {
    if (!map) return;
    free(map->rows);
    release_shader_certificate_reference_map_init(map);
}

static int lower_hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    return -1;
}

static bool parse_lower_hex(const char* text, size_t byte_count,
                            uint8_t* output) {
    if (!text || !output || strlen(text) != byte_count * 2U) return false;
    for (size_t index = 0U; index < byte_count; ++index) {
        int high = lower_hex_value((unsigned char)text[index * 2U]);
        int low = lower_hex_value((unsigned char)text[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        output[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool decimal_is_canonical(const char* text, bool allow_negative) {
    if (!text || !text[0]) return false;
    const char* digits = text;
    if (*digits == '-') {
        if (!allow_negative) return false;
        ++digits;
        if (!*digits) return false;
    }
    if (digits[0] == '0' && digits[1] != '\0') return false;
    for (const char* cursor = digits; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    return true;
}

static bool parse_int64_canonical(const char* text, int64_t* output) {
    if (!decimal_is_canonical(text, true) || !output) return false;
    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' ||
        value < INT64_MIN || value > INT64_MAX) {
        return false;
    }
    *output = (int64_t)value;
    return true;
}

static bool parse_int32_canonical(const char* text, int32_t* output) {
    int64_t value = 0;
    if (!parse_int64_canonical(text, &value) || value < INT32_MIN ||
        value > INT32_MAX || !output) {
        return false;
    }
    *output = (int32_t)value;
    return true;
}

static bool parse_identity(const char* text, char prefix,
                           int64_t* path_id, const char** digest_start) {
    if (!text || text[0] != prefix || text[1] != ':') return false;
    const char* digest = text + 2U;
    const char* separator = strchr(digest, ':');
    if (!separator || (size_t)(separator - digest) != 64U) return false;
    for (const char* cursor = digest; cursor < separator; ++cursor) {
        if (lower_hex_value((unsigned char)*cursor) < 0) return false;
    }
    if (!parse_int64_canonical(separator + 1U, path_id)) return false;
    if (digest_start) *digest_start = digest;
    return true;
}

static size_t split_tabs(char* line, char** fields, size_t capacity) {
    if (!line || !fields || capacity == 0U) return 0U;
    size_t count = 1U;
    fields[0] = line;
    for (char* cursor = line; *cursor; ++cursor) {
        if (*cursor != '\t') continue;
        *cursor = '\0';
        if (count >= capacity) return capacity + 1U;
        fields[count++] = cursor + 1U;
    }
    return count;
}

static bool pair_row_is_valid(char** fields,
                              ReleaseShaderCertificatePair* row) {
    int64_t expected_occurrence_path = 0;
    int64_t expected_content_path = 0;
    int64_t actual_occurrence_path = 0;
    int64_t actual_content_path = 0;
    const char* expected_content_digest = NULL;
    const char* actual_content_digest = NULL;
    uint8_t ignored[COMMON_SHA256_DIGEST_SIZE];
    if (!parse_identity(fields[0], 'o', &expected_occurrence_path, NULL) ||
        !parse_identity(fields[1], 's', &expected_content_path,
                        &expected_content_digest) ||
        !parse_lower_hex(fields[2], COMMON_SHA256_DIGEST_SIZE, ignored) ||
        !parse_identity(fields[3], 'o', &actual_occurrence_path, NULL) ||
        !parse_identity(fields[4], 's', &actual_content_path,
                        &actual_content_digest) ||
        !parse_lower_hex(fields[5], COMMON_SHA256_DIGEST_SIZE, ignored) ||
        expected_occurrence_path != expected_content_path ||
        actual_occurrence_path != actual_content_path ||
        strncmp(expected_content_digest, fields[2], 64U) != 0 ||
        strncmp(actual_content_digest, fields[5], 64U) != 0 ||
        strlen(fields[0]) >= sizeof(row->expected_occurrence_id) ||
        strlen(fields[1]) >= sizeof(row->expected_content_id) ||
        strlen(fields[3]) >= sizeof(row->actual_occurrence_id) ||
        strlen(fields[4]) >= sizeof(row->actual_content_id)) {
        return false;
    }
    memcpy(row->expected_occurrence_id, fields[0], strlen(fields[0]) + 1U);
    memcpy(row->expected_content_id, fields[1], strlen(fields[1]) + 1U);
    memcpy(row->expected_serialized_sha256, fields[2], 65U);
    memcpy(row->actual_occurrence_id, fields[3], strlen(fields[3]) + 1U);
    memcpy(row->actual_content_id, fields[4], strlen(fields[4]) + 1U);
    memcpy(row->actual_serialized_sha256, fields[5], 65U);
    return true;
}

static bool pair_is_duplicate(const ReleaseShaderCertificatePairTable* table,
                              const ReleaseShaderCertificatePair* row) {
    for (size_t index = 0U; index < table->count; ++index) {
        if (strcmp(table->rows[index].expected_occurrence_id,
                   row->expected_occurrence_id) == 0 ||
            strcmp(table->rows[index].actual_occurrence_id,
                   row->actual_occurrence_id) == 0) {
            return true;
        }
    }
    return false;
}

static ReleaseShaderCertificateTableStatus append_pair(
    ReleaseShaderCertificatePairTable* table,
    const ReleaseShaderCertificatePair* row) {
    if (table->count >= CERTIFICATE_TABLE_MAX_ROWS) {
        return RELEASE_SHADER_CERTIFICATE_TABLE_LIMIT_EXCEEDED;
    }
    if (dxbc_size_multiply_overflows(
            table->count + 1U, sizeof(*table->rows))) {
        return RELEASE_SHADER_CERTIFICATE_TABLE_LIMIT_EXCEEDED;
    }
    ReleaseShaderCertificatePair* rows =
        (ReleaseShaderCertificatePair*)realloc(
            table->rows, (table->count + 1U) * sizeof(*table->rows));
    if (!rows) return RELEASE_SHADER_CERTIFICATE_TABLE_ALLOCATION_FAILED;
    table->rows = rows;
    rows[table->count++] = *row;
    return RELEASE_SHADER_CERTIFICATE_TABLE_OK;
}

static ReleaseShaderCertificateTableStatus copy_table_text(
    const uint8_t* data, size_t size, char** output) {
    if ((!data && size != 0U) || !output) {
        return RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (size == 0U || size > CERTIFICATE_TABLE_MAX_SIZE ||
        data[size - 1U] != '\n' || memchr(data, '\0', size)) {
        return size > CERTIFICATE_TABLE_MAX_SIZE
            ? RELEASE_SHADER_CERTIFICATE_TABLE_LIMIT_EXCEEDED
            : RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW;
    }
    char* copy = (char*)malloc(size + 1U);
    if (!copy) return RELEASE_SHADER_CERTIFICATE_TABLE_ALLOCATION_FAILED;
    memcpy(copy, data, size);
    copy[size] = '\0';
    *output = copy;
    return RELEASE_SHADER_CERTIFICATE_TABLE_OK;
}

ReleaseShaderCertificateTableStatus release_shader_certificate_pairs_parse(
    const uint8_t* data, size_t size,
    ReleaseShaderCertificatePairTable* table, size_t* error_line) {
    if (error_line) *error_line = 0U;
    if (!table) return RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ARGUMENT;
    ReleaseShaderCertificatePairTable pending;
    release_shader_certificate_pair_table_init(&pending);
    char* text = NULL;
    ReleaseShaderCertificateTableStatus status =
        copy_table_text(data, size, &text);
    if (status != RELEASE_SHADER_CERTIFICATE_TABLE_OK) return status;
    char* cursor = text;
    size_t line_number = 0U;
    while (*cursor) {
        char* newline = strchr(cursor, '\n');
        if (!newline) {
            status = RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW;
            break;
        }
        *newline = '\0';
        if (newline > cursor && newline[-1] == '\r') newline[-1] = '\0';
        ++line_number;
        if (line_number == 1U) {
            if (strcmp(cursor, k_pairs_header) != 0) {
                status = RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_HEADER;
                break;
            }
        } else {
            char* fields[6];
            ReleaseShaderCertificatePair row;
            memset(&row, 0, sizeof(row));
            row.line_number = line_number;
            if (split_tabs(cursor, fields, 6U) != 6U ||
                !pair_row_is_valid(fields, &row)) {
                status = RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW;
                break;
            }
            if (pair_is_duplicate(&pending, &row)) {
                status = RELEASE_SHADER_CERTIFICATE_TABLE_DUPLICATE_ROW;
                break;
            }
            status = append_pair(&pending, &row);
            if (status != RELEASE_SHADER_CERTIFICATE_TABLE_OK) break;
        }
        cursor = newline + 1U;
    }
    if (status == RELEASE_SHADER_CERTIFICATE_TABLE_OK && pending.count == 0U) {
        status = RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW;
        line_number = 1U;
    }
    if (status != RELEASE_SHADER_CERTIFICATE_TABLE_OK) {
        if (error_line) *error_line = line_number;
        release_shader_certificate_pair_table_dispose(&pending);
    } else {
        release_shader_certificate_pair_table_dispose(table);
        *table = pending;
    }
    free(text);
    return status;
}

static bool reference_is_duplicate(
    const ReleaseShaderCertificateReferenceMap* map,
    const ReleaseShaderCertificateReference* row) {
    for (size_t index = 0U; index < map->count; ++index) {
        const ReleaseShaderCertificateReference* existing =
            &map->rows[index];
        if (existing->side == row->side &&
            existing->file_id == row->file_id &&
            existing->path_id == row->path_id &&
            memcmp(existing->owner_serialized_sha256,
                   row->owner_serialized_sha256,
                   sizeof(row->owner_serialized_sha256)) == 0) {
            return true;
        }
    }
    return false;
}

static ReleaseShaderCertificateTableStatus append_reference(
    ReleaseShaderCertificateReferenceMap* map,
    const ReleaseShaderCertificateReference* row) {
    if (map->count >= CERTIFICATE_TABLE_MAX_ROWS ||
        dxbc_size_multiply_overflows(
            map->count + 1U, sizeof(*map->rows))) {
        return RELEASE_SHADER_CERTIFICATE_TABLE_LIMIT_EXCEEDED;
    }
    ReleaseShaderCertificateReference* rows =
        (ReleaseShaderCertificateReference*)realloc(
            map->rows, (map->count + 1U) * sizeof(*map->rows));
    if (!rows) return RELEASE_SHADER_CERTIFICATE_TABLE_ALLOCATION_FAILED;
    map->rows = rows;
    rows[map->count++] = *row;
    return RELEASE_SHADER_CERTIFICATE_TABLE_OK;
}

ReleaseShaderCertificateTableStatus
release_shader_certificate_reference_map_parse(
    const uint8_t* data, size_t size,
    ReleaseShaderCertificateReferenceMap* map, size_t* error_line) {
    if (error_line) *error_line = 0U;
    if (!map) return RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ARGUMENT;
    ReleaseShaderCertificateReferenceMap pending;
    release_shader_certificate_reference_map_init(&pending);
    char* text = NULL;
    ReleaseShaderCertificateTableStatus status =
        copy_table_text(data, size, &text);
    if (status != RELEASE_SHADER_CERTIFICATE_TABLE_OK) return status;
    char* cursor = text;
    size_t line_number = 0U;
    while (*cursor) {
        char* newline = strchr(cursor, '\n');
        if (!newline) {
            status = RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW;
            break;
        }
        *newline = '\0';
        if (newline > cursor && newline[-1] == '\r') newline[-1] = '\0';
        ++line_number;
        if (line_number == 1U) {
            if (strcmp(cursor, k_references_header) != 0) {
                status = RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_HEADER;
                break;
            }
        } else {
            char* fields[5];
            ReleaseShaderCertificateReference row;
            memset(&row, 0, sizeof(row));
            if (split_tabs(cursor, fields, 5U) != 5U ||
                (strcmp(fields[0], "expected") != 0 &&
                 strcmp(fields[0], "actual") != 0) ||
                !parse_lower_hex(fields[1], COMMON_SHA256_DIGEST_SIZE,
                                 row.owner_serialized_sha256) ||
                !parse_int32_canonical(fields[2], &row.file_id) ||
                !parse_int64_canonical(fields[3], &row.path_id) ||
                (row.file_id == 0 && row.path_id == 0) ||
                !parse_lower_hex(fields[4],
                                 RELEASE_SHADER_REFERENCE_ID_SIZE,
                                 row.stable_id)) {
                status = RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW;
                break;
            }
            row.side = strcmp(fields[0], "expected") == 0
                ? RELEASE_SHADER_SIDE_EXPECTED : RELEASE_SHADER_SIDE_ACTUAL;
            if (reference_is_duplicate(&pending, &row)) {
                status = RELEASE_SHADER_CERTIFICATE_TABLE_DUPLICATE_ROW;
                break;
            }
            status = append_reference(&pending, &row);
            if (status != RELEASE_SHADER_CERTIFICATE_TABLE_OK) break;
        }
        cursor = newline + 1U;
    }
    /* Header-only is a valid explicit empty map. */
    if (status != RELEASE_SHADER_CERTIFICATE_TABLE_OK) {
        if (error_line) *error_line = line_number;
        release_shader_certificate_reference_map_dispose(&pending);
    } else {
        release_shader_certificate_reference_map_dispose(map);
        *map = pending;
    }
    free(text);
    return status;
}

static ReleaseShaderCertificateTableStatus load_table_bytes(
    const char* path, CommonFileBytes* file) {
    if (!path || !file) {
        return RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ARGUMENT;
    }
    CommonFileStatus status = common_file_read_regular(
        path, CERTIFICATE_TABLE_MAX_SIZE, file);
    if (status == COMMON_FILE_OK) return RELEASE_SHADER_CERTIFICATE_TABLE_OK;
    if (status == COMMON_FILE_TOO_LARGE) {
        return RELEASE_SHADER_CERTIFICATE_TABLE_LIMIT_EXCEEDED;
    }
    if (status == COMMON_FILE_ALLOCATION_FAILED) {
        return RELEASE_SHADER_CERTIFICATE_TABLE_ALLOCATION_FAILED;
    }
    return RELEASE_SHADER_CERTIFICATE_TABLE_IO_ERROR;
}

ReleaseShaderCertificateTableStatus release_shader_certificate_pairs_load(
    const char* path, ReleaseShaderCertificatePairTable* table,
    size_t* error_line) {
    CommonFileBytes file = {0};
    ReleaseShaderCertificateTableStatus status =
        load_table_bytes(path, &file);
    if (status == RELEASE_SHADER_CERTIFICATE_TABLE_OK) {
        status = release_shader_certificate_pairs_parse(
            file.data, file.size, table, error_line);
    }
    common_file_bytes_dispose(&file);
    return status;
}

ReleaseShaderCertificateTableStatus
release_shader_certificate_reference_map_load(
    const char* path, ReleaseShaderCertificateReferenceMap* map,
    size_t* error_line) {
    CommonFileBytes file = {0};
    ReleaseShaderCertificateTableStatus status =
        load_table_bytes(path, &file);
    if (status == RELEASE_SHADER_CERTIFICATE_TABLE_OK) {
        status = release_shader_certificate_reference_map_parse(
            file.data, file.size, map, error_line);
    }
    common_file_bytes_dispose(&file);
    return status;
}

static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    char* result = (char*)malloc(size + 1U);
    if (result) memcpy(result, value, size + 1U);
    return result;
}

static void endpoint_dispose(ReleaseShaderCertificateEndpoint* endpoint) {
    if (!endpoint) return;
    free(endpoint->name);
    free(endpoint->outer_path);
    free(endpoint->member_name);
    memset(endpoint, 0, sizeof(*endpoint));
}

void release_shader_certificate_compare_result_init(
    ReleaseShaderCertificateCompareResult* result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->status = RELEASE_SHADER_CERTIFICATE_COMPARE_INVALID_ARGUMENT;
    result->expected_catalog_status = SHADER_CATALOG_INVALID_ARGUMENT;
    result->actual_catalog_status = SHADER_CATALOG_INVALID_ARGUMENT;
}

void release_shader_certificate_compare_result_dispose(
    ReleaseShaderCertificateCompareResult* result) {
    if (!result) return;
    for (size_t index = 0U; index < result->pair_count; ++index) {
        endpoint_dispose(&result->pairs[index].expected);
        endpoint_dispose(&result->pairs[index].actual);
    }
    free(result->pairs);
    release_shader_certificate_compare_result_init(result);
}

static bool initialize_endpoint(
    ReleaseShaderCertificateEndpoint* endpoint,
    const ShaderCatalogRecord* record) {
    if (!endpoint || !record) return false;
    memset(endpoint, 0, sizeof(*endpoint));
    memcpy(endpoint->occurrence_id, record->occurrence_id,
           sizeof(endpoint->occurrence_id));
    memcpy(endpoint->content_id, record->content_id,
           sizeof(endpoint->content_id));
    memcpy(endpoint->serialized_sha256, record->serialized_digest_hex,
           sizeof(endpoint->serialized_sha256));
    endpoint->path_id = record->path_id;
    endpoint->name = duplicate_string(record->name);
    endpoint->outer_path = duplicate_string(record->outer_path);
    endpoint->member_name = duplicate_string(record->member_name);
    endpoint->member_index = record->member_index;
    endpoint->is_bundle_member = record->is_bundle_member;
    if (!endpoint->outer_path || (record->name && !endpoint->name) ||
        (record->member_name && !endpoint->member_name)) {
        endpoint_dispose(endpoint);
        return false;
    }
    return true;
}

static const ShaderCatalogRecord* find_occurrence(
    const ShaderCatalog* catalog, const char* occurrence_id,
    size_t* match_count) {
    const ShaderCatalogRecord* result = NULL;
    *match_count = 0U;
    for (size_t index = 0U; index < catalog->record_count; ++index) {
        if (strcmp(catalog->records[index].occurrence_id,
                   occurrence_id) == 0) {
            result = &catalog->records[index];
            ++*match_count;
        }
    }
    return result;
}

typedef struct {
    const ShaderCatalogRecord* record;
    const TypeTreeSchemaRegistry* registry;
    ShaderObject* output;
    TypeTreeSchemaStatus schema_status;
    ShaderObjectStatus object_status;
    size_t source_matches;
} DecodeVisitorContext;

static bool serialized_source_matches_record(
    const UnitySerializedSource* source, const ShaderCatalogRecord* record,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (strcmp(source->outer_path, record->outer_path) != 0 ||
        source->member_index != record->member_index ||
        source->is_bundle_member != record->is_bundle_member ||
        memcmp(digest, record->serialized_digest,
               COMMON_SHA256_DIGEST_SIZE) != 0) {
        return false;
    }
    if (!source->member_name || !record->member_name) {
        return source->member_name == record->member_name;
    }
    return strcmp(source->member_name, record->member_name) == 0;
}

static bool decode_source_visitor(const UnitySerializedSource* source,
                                  void* opaque) {
    DecodeVisitorContext* context = (DecodeVisitorContext*)opaque;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(source->data, source->size, digest);
    if (!serialized_source_matches_record(
            source, context->record, digest)) {
        return true;
    }
    ++context->source_matches;
    if (context->source_matches != 1U) return true;

    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        context->object_status = SHADER_OBJECT_INVALID_SOURCE_FILE;
        return true;
    }
    context->schema_status = serialized_file_resolve_class_schema(
        &file, 48, context->registry);
    if (context->schema_status == TYPETREE_SCHEMA_OK) {
        const AssetObjectInfo* asset = serialized_file_get_object(
            &file, context->record->path_id);
        if (!asset || asset->type_id != 48) {
            context->object_status = SHADER_OBJECT_OBJECT_NOT_OWNED;
        } else {
            context->object_status = shader_object_decode(
                context->output, &file, asset);
        }
    }
    serialized_file_close(&file);
    return true;
}

static UnityInputSnapshot* snapshot_for_path(
    ShaderCatalog* catalog, const char* path) {
    for (size_t index = 0U;
         index < catalog->retained_source_snapshot_count; ++index) {
        const char* snapshot_path = unity_input_snapshot_path(
            &catalog->retained_source_snapshots[index]);
        if (snapshot_path && strcmp(snapshot_path, path) == 0) {
            return &catalog->retained_source_snapshots[index];
        }
    }
    return NULL;
}

static bool decode_catalog_record(
    ShaderCatalog* catalog, const ShaderCatalogRecord* record,
    const TypeTreeSchemaRegistry* registry, ShaderObject* output,
    TypeTreeSchemaStatus* schema_status,
    ShaderObjectStatus* object_status) {
    *schema_status = TYPETREE_SCHEMA_INVALID_ARGUMENT;
    *object_status = SHADER_OBJECT_NOT_DECODED;
    UnityInputSnapshot* snapshot = snapshot_for_path(
        catalog, record->outer_path);
    if (!snapshot) return false;
    DecodeVisitorContext context;
    memset(&context, 0, sizeof(context));
    context.record = record;
    context.registry = registry;
    context.output = output;
    context.schema_status = TYPETREE_SCHEMA_INVALID_ARGUMENT;
    context.object_status = SHADER_OBJECT_NOT_DECODED;
    UnityInputVisitStats stats;
    UnityInputStatus visit_status = unity_input_snapshot_visit(
        snapshot, decode_source_visitor, &context, &stats);
    *schema_status = context.schema_status;
    *object_status = context.object_status;
    return visit_status == UNITY_INPUT_OK && context.source_matches == 1U &&
        context.schema_status == TYPETREE_SCHEMA_OK &&
        context.object_status == SHADER_OBJECT_OK;
}

typedef struct {
    const ReleaseShaderCertificateReferenceMap* map;
    uint8_t owner_sha256[2][COMMON_SHA256_DIGEST_SIZE];
} ReferenceResolverContext;

static bool certificate_reference_resolver(
    void* opaque, ReleaseShaderObjectSide side, int32_t file_id,
    int64_t path_id,
    uint8_t stable_id[RELEASE_SHADER_REFERENCE_ID_SIZE]) {
    ReferenceResolverContext* context = (ReferenceResolverContext*)opaque;
    if (!context || !context->map || side < RELEASE_SHADER_SIDE_EXPECTED ||
        side > RELEASE_SHADER_SIDE_ACTUAL) {
        return false;
    }
    for (size_t index = 0U; index < context->map->count; ++index) {
        const ReleaseShaderCertificateReference* row =
            &context->map->rows[index];
        if (row->side == side && row->file_id == file_id &&
            row->path_id == path_id &&
            memcmp(row->owner_serialized_sha256,
                   context->owner_sha256[side],
                   COMMON_SHA256_DIGEST_SIZE) == 0) {
            memcpy(stable_id, row->stable_id,
                   RELEASE_SHADER_REFERENCE_ID_SIZE);
            return true;
        }
    }
    return false;
}

static bool pair_coordinates_match(
    const ReleaseShaderCertificatePair* pair,
    const ShaderCatalogRecord* expected,
    const ShaderCatalogRecord* actual,
    ReleaseShaderCertificateDiagnostic* diagnostic) {
    if (strcmp(expected->content_id, pair->expected_content_id) != 0 ||
        strcmp(expected->serialized_digest_hex,
               pair->expected_serialized_sha256) != 0) {
        *diagnostic =
            RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_COORDINATE_MISMATCH;
        return false;
    }
    if (strcmp(actual->content_id, pair->actual_content_id) != 0 ||
        strcmp(actual->serialized_digest_hex,
               pair->actual_serialized_sha256) != 0) {
        *diagnostic =
            RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_COORDINATE_MISMATCH;
        return false;
    }
    return true;
}

static void count_pair_result(ReleaseShaderCertificateCompareResult* result,
                              ReleaseShaderCertificatePairStatus status) {
    switch (status) {
        case RELEASE_SHADER_CERTIFICATE_PAIR_EXACT:
            ++result->exact_count;
            break;
        case RELEASE_SHADER_CERTIFICATE_PAIR_MISMATCH:
            ++result->mismatch_count;
            break;
        case RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_UNAVAILABLE:
            ++result->unavailable_count;
            break;
        case RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_INVALID:
            ++result->invalid_count;
            break;
    }
}

ReleaseShaderCertificateCompareStatus release_shader_certificate_compare(
    const char* expected_input, const char* actual_input,
    const ReleaseShaderCertificatePairTable* pairs,
    const ReleaseShaderCertificateReferenceMap* references,
    const TypeTreeSchemaRegistry* schema_registry,
    ReleaseShaderCertificateCompareResult* result) {
    if (!expected_input || !expected_input[0] || !actual_input ||
        !actual_input[0] || !pairs || pairs->count == 0U || !pairs->rows ||
        !references || !schema_registry || !result) {
        return RELEASE_SHADER_CERTIFICATE_COMPARE_INVALID_ARGUMENT;
    }
    ReleaseShaderCertificateCompareResult pending;
    release_shader_certificate_compare_result_init(&pending);
    ShaderCatalog expected_catalog;
    ShaderCatalog actual_catalog;
    shader_catalog_init(&expected_catalog);
    shader_catalog_init(&actual_catalog);
    ShaderCatalogOptions options;
    shader_catalog_options_default(&options);
    options.schema_registry = schema_registry;
    options.retain_source_snapshots = true;
    const char* expected_inputs[] = {expected_input};
    const char* actual_inputs[] = {actual_input};
    pending.expected_catalog_status = shader_catalog_build(
        expected_inputs, 1U, &options, &expected_catalog);
    pending.actual_catalog_status = shader_catalog_build(
        actual_inputs, 1U, &options, &actual_catalog);
    pending.expected_catalog_issues = expected_catalog.issue_count;
    pending.actual_catalog_issues = actual_catalog.issue_count;
    if (pending.expected_catalog_status != SHADER_CATALOG_OK ||
        pending.actual_catalog_status != SHADER_CATALOG_OK ||
        expected_catalog.issue_count != 0U ||
        actual_catalog.issue_count != 0U) {
        pending.status = RELEASE_SHADER_CERTIFICATE_COMPARE_CATALOG_FAILED;
        goto finish;
    }
    if (dxbc_size_multiply_overflows(
            pairs->count, sizeof(*pending.pairs))) {
        pending.status = RELEASE_SHADER_CERTIFICATE_COMPARE_ALLOCATION_FAILED;
        goto finish;
    }
    pending.pairs = (ReleaseShaderCertificatePairResult*)calloc(
        pairs->count, sizeof(*pending.pairs));
    if (!pending.pairs) {
        pending.status = RELEASE_SHADER_CERTIFICATE_COMPARE_ALLOCATION_FAILED;
        goto finish;
    }
    pending.pair_count = pairs->count;

    for (size_t index = 0U; index < pairs->count; ++index) {
        const ReleaseShaderCertificatePair* pair = &pairs->rows[index];
        ReleaseShaderCertificatePairResult* pair_result =
            &pending.pairs[index];
        pair_result->line_number = pair->line_number;
        pair_result->status = RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_INVALID;
        pair_result->expected_decode_status = SHADER_OBJECT_NOT_DECODED;
        pair_result->actual_decode_status = SHADER_OBJECT_NOT_DECODED;
        pair_result->expected_schema_status = TYPETREE_SCHEMA_INVALID_ARGUMENT;
        pair_result->actual_schema_status = TYPETREE_SCHEMA_INVALID_ARGUMENT;
        memcpy(pair_result->expected.occurrence_id,
               pair->expected_occurrence_id,
               sizeof(pair_result->expected.occurrence_id));
        memcpy(pair_result->expected.content_id,
               pair->expected_content_id,
               sizeof(pair_result->expected.content_id));
        memcpy(pair_result->expected.serialized_sha256,
               pair->expected_serialized_sha256,
               sizeof(pair_result->expected.serialized_sha256));
        memcpy(pair_result->actual.occurrence_id,
               pair->actual_occurrence_id,
               sizeof(pair_result->actual.occurrence_id));
        memcpy(pair_result->actual.content_id,
               pair->actual_content_id,
               sizeof(pair_result->actual.content_id));
        memcpy(pair_result->actual.serialized_sha256,
               pair->actual_serialized_sha256,
               sizeof(pair_result->actual.serialized_sha256));
        (void)parse_identity(pair->expected_content_id, 's',
                             &pair_result->expected.path_id, NULL);
        (void)parse_identity(pair->actual_content_id, 's',
                             &pair_result->actual.path_id, NULL);
        release_shader_object_certificate_report_init(
            &pair_result->certificate);

        size_t expected_matches = 0U;
        size_t actual_matches = 0U;
        const ShaderCatalogRecord* expected = find_occurrence(
            &expected_catalog, pair->expected_occurrence_id,
            &expected_matches);
        const ShaderCatalogRecord* actual = find_occurrence(
            &actual_catalog, pair->actual_occurrence_id, &actual_matches);
        if (!expected || expected_matches != 1U) {
            pair_result->diagnostic =
                RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_RECORD_NOT_FOUND;
            count_pair_result(&pending, pair_result->status);
            continue;
        }
        if (!actual || actual_matches != 1U) {
            pair_result->diagnostic =
                RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_RECORD_NOT_FOUND;
            count_pair_result(&pending, pair_result->status);
            continue;
        }
        if (!initialize_endpoint(&pair_result->expected, expected) ||
            !initialize_endpoint(&pair_result->actual, actual)) {
            pair_result->diagnostic =
                RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ALLOCATION_FAILED;
            count_pair_result(&pending, pair_result->status);
            continue;
        }
        if (!pair_coordinates_match(
                pair, expected, actual, &pair_result->diagnostic)) {
            count_pair_result(&pending, pair_result->status);
            continue;
        }
        if (expected->class_id != 48) {
            pair_result->diagnostic =
                RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_NOT_GRAPHICS;
            count_pair_result(&pending, pair_result->status);
            continue;
        }
        if (actual->class_id != 48) {
            pair_result->diagnostic =
                RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_NOT_GRAPHICS;
            count_pair_result(&pending, pair_result->status);
            continue;
        }

        ShaderObject expected_object;
        ShaderObject actual_object;
        shader_object_init(&expected_object);
        shader_object_init(&actual_object);
        bool expected_decoded = decode_catalog_record(
            &expected_catalog, expected, schema_registry, &expected_object,
            &pair_result->expected_schema_status,
            &pair_result->expected_decode_status);
        bool actual_decoded = decode_catalog_record(
            &actual_catalog, actual, schema_registry, &actual_object,
            &pair_result->actual_schema_status,
            &pair_result->actual_decode_status);
        if (!expected_decoded || !actual_decoded) {
            pair_result->status =
                RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_UNAVAILABLE;
            pair_result->diagnostic = !expected_decoded
                ? RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_DECODE_UNAVAILABLE
                : RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_DECODE_UNAVAILABLE;
        } else {
            ReferenceResolverContext resolver;
            memset(&resolver, 0, sizeof(resolver));
            resolver.map = references;
            (void)parse_lower_hex(
                pair->expected_serialized_sha256,
                COMMON_SHA256_DIGEST_SIZE,
                resolver.owner_sha256[RELEASE_SHADER_SIDE_EXPECTED]);
            (void)parse_lower_hex(
                pair->actual_serialized_sha256,
                COMMON_SHA256_DIGEST_SIZE,
                resolver.owner_sha256[RELEASE_SHADER_SIDE_ACTUAL]);
            ReleaseShaderObjectCertificateOptions certificate_options;
            release_shader_object_certificate_options_init(
                &certificate_options);
            certificate_options.resolve_reference =
                certificate_reference_resolver;
            certificate_options.reference_user_data = &resolver;
            ReleaseShaderObjectCertificateStatus certificate_status =
                release_shader_object_certify_equal(
                    &expected_object, &actual_object, &certificate_options,
                    &pair_result->certificate);
            if (certificate_status == RELEASE_SHADER_OBJECT_CERTIFICATE_OK) {
                pair_result->status = RELEASE_SHADER_CERTIFICATE_PAIR_EXACT;
            } else if (certificate_status ==
                       RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER) {
                pair_result->status =
                    RELEASE_SHADER_CERTIFICATE_PAIR_MISMATCH;
            } else if (certificate_status ==
                       RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_UNAVAILABLE) {
                pair_result->status =
                    RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_UNAVAILABLE;
            } else {
                pair_result->status =
                    RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_INVALID;
            }
        }
        shader_object_dispose(&actual_object);
        shader_object_dispose(&expected_object);
        count_pair_result(&pending, pair_result->status);
    }

    if (pending.invalid_count != 0U) {
        pending.status = RELEASE_SHADER_CERTIFICATE_COMPARE_INPUT_INVALID;
    } else if (pending.unavailable_count != 0U) {
        pending.status =
            RELEASE_SHADER_CERTIFICATE_COMPARE_AUTHORITY_UNAVAILABLE;
    } else if (pending.mismatch_count != 0U) {
        pending.status = RELEASE_SHADER_CERTIFICATE_COMPARE_MISMATCH;
    } else {
        pending.status = RELEASE_SHADER_CERTIFICATE_COMPARE_EXACT;
    }

finish:
    shader_catalog_dispose(&actual_catalog);
    shader_catalog_dispose(&expected_catalog);
    release_shader_certificate_compare_result_dispose(result);
    *result = pending;
    return result->status;
}

const char* release_shader_certificate_table_status_name(
    ReleaseShaderCertificateTableStatus status) {
    switch (status) {
        case RELEASE_SHADER_CERTIFICATE_TABLE_OK: return "ok";
        case RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ARGUMENT:
            return "invalid-argument";
        case RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_HEADER:
            return "invalid-header";
        case RELEASE_SHADER_CERTIFICATE_TABLE_INVALID_ROW:
            return "invalid-row";
        case RELEASE_SHADER_CERTIFICATE_TABLE_DUPLICATE_ROW:
            return "duplicate-row";
        case RELEASE_SHADER_CERTIFICATE_TABLE_ALLOCATION_FAILED:
            return "allocation-failed";
        case RELEASE_SHADER_CERTIFICATE_TABLE_IO_ERROR: return "io-error";
        case RELEASE_SHADER_CERTIFICATE_TABLE_LIMIT_EXCEEDED:
            return "limit-exceeded";
        default: return "unknown";
    }
}

const char* release_shader_certificate_pair_status_name(
    ReleaseShaderCertificatePairStatus status) {
    switch (status) {
        case RELEASE_SHADER_CERTIFICATE_PAIR_EXACT: return "exact";
        case RELEASE_SHADER_CERTIFICATE_PAIR_MISMATCH: return "mismatch";
        case RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_UNAVAILABLE:
            return "authority-unavailable";
        case RELEASE_SHADER_CERTIFICATE_PAIR_AUTHORITY_INVALID:
            return "authority-invalid";
        default: return "unknown";
    }
}

const char* release_shader_certificate_diagnostic_name(
    ReleaseShaderCertificateDiagnostic diagnostic) {
    switch (diagnostic) {
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_NONE: return "none";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_RECORD_NOT_FOUND:
            return "expected-record-not-found";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_RECORD_NOT_FOUND:
            return "actual-record-not-found";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_COORDINATE_MISMATCH:
            return "expected-coordinate-mismatch";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_COORDINATE_MISMATCH:
            return "actual-coordinate-mismatch";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_NOT_GRAPHICS:
            return "expected-not-graphics";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_NOT_GRAPHICS:
            return "actual-not-graphics";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_SOURCE_UNAVAILABLE:
            return "expected-source-unavailable";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_SOURCE_UNAVAILABLE:
            return "actual-source-unavailable";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_EXPECTED_DECODE_UNAVAILABLE:
            return "expected-decode-unavailable";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ACTUAL_DECODE_UNAVAILABLE:
            return "actual-decode-unavailable";
        case RELEASE_SHADER_CERTIFICATE_DIAGNOSTIC_ALLOCATION_FAILED:
            return "allocation-failed";
        default: return "unknown";
    }
}

const char* release_shader_certificate_compare_status_name(
    ReleaseShaderCertificateCompareStatus status) {
    switch (status) {
        case RELEASE_SHADER_CERTIFICATE_COMPARE_EXACT: return "exact";
        case RELEASE_SHADER_CERTIFICATE_COMPARE_MISMATCH: return "mismatch";
        case RELEASE_SHADER_CERTIFICATE_COMPARE_AUTHORITY_UNAVAILABLE:
            return "authority-unavailable";
        case RELEASE_SHADER_CERTIFICATE_COMPARE_INVALID_ARGUMENT:
            return "invalid-argument";
        case RELEASE_SHADER_CERTIFICATE_COMPARE_INPUT_INVALID:
            return "input-invalid";
        case RELEASE_SHADER_CERTIFICATE_COMPARE_CATALOG_FAILED:
            return "catalog-failed";
        case RELEASE_SHADER_CERTIFICATE_COMPARE_ALLOCATION_FAILED:
            return "allocation-failed";
        default: return "unknown";
    }
}
