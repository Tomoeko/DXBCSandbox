// SPDX-License-Identifier: GPL-3.0-only

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "app/shader_catalog.h"
#include "app/shader_catalog_pptr.h"

#include "common/common.h"
#include "io/serialized_file.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

typedef struct {
    ShaderCatalog* catalog;
    const TypeTreeSchemaRegistry* registry;
    bool allocation_failed;
    bool saw_graphics_source;
    bool include_materials;
    const char* const* inputs;
    size_t input_count;
} CatalogVisitorContext;

static char* duplicate_string(const char* value) {
    if (!value) return NULL;
    size_t size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(size + 1U);
    if (!copy) return NULL;
    memcpy(copy, value, size + 1U);
    return copy;
}

/* Discovery, snapshot provenance, and PPtr scope checks must all compare the
 * same lexical path spelling.  UnityInput deliberately records an absolute
 * outer path, so bind every caller input to an absolute spelling before
 * discovery instead of trying to reconcile relative roots after the fact. */
static char* stable_absolute_catalog_path(const char* path) {
    if (!path || !path[0]) return NULL;
#ifdef _WIN32
    wchar_t* requested = common_windows_utf8_to_wide(path);
    if (!requested) return NULL;
    DWORD required = GetFullPathNameW(requested, 0U, NULL, NULL);
    if (required == 0U) {
        free(requested);
        return NULL;
    }
    wchar_t* absolute = (wchar_t*)malloc(
        (size_t)required * sizeof(*absolute));
    if (!absolute) {
        free(requested);
        return NULL;
    }
    DWORD written = GetFullPathNameW(requested, required, absolute, NULL);
    free(requested);
    if (written == 0U || written >= required) {
        free(absolute);
        return NULL;
    }
    char* result = common_windows_wide_to_utf8(absolute);
    free(absolute);
    return result;
#else
    if (path[0] == '/') return duplicate_string(path);
    size_t capacity = 256U;
    char* current = NULL;
    for (;;) {
        current = (char*)malloc(capacity);
        if (!current) return NULL;
        errno = 0;
        if (getcwd(current, capacity)) break;
        int error = errno;
        free(current);
        current = NULL;
        if (error != ERANGE || capacity > SIZE_MAX / 2U) return NULL;
        capacity *= 2U;
    }
    const size_t current_size = strlen(current);
    const size_t path_size = strlen(path);
    const bool separator = current_size == 0U ||
        current[current_size - 1U] != '/';
    if (current_size > SIZE_MAX - path_size -
            (separator ? 2U : 1U)) {
        free(current);
        return NULL;
    }
    char* absolute = (char*)malloc(
        current_size + (separator ? 1U : 0U) + path_size + 1U);
    if (!absolute) {
        free(current);
        return NULL;
    }
    memcpy(absolute, current, current_size);
    size_t at = current_size;
    if (separator) absolute[at++] = '/';
    memcpy(absolute + at, path, path_size + 1U);
    free(current);
    return absolute;
#endif
}

static void stable_catalog_inputs_dispose(char** inputs, size_t count) {
    if (!inputs) return;
    for (size_t i = 0U; i < count; ++i) free(inputs[i]);
    free(inputs);
}

static char* duplicate_byte_string(const uint8_t* bytes, size_t size) {
    if ((!bytes && size != 0U) || size == SIZE_MAX) return NULL;
    char* copy = (char*)malloc(size + 1U);
    if (!copy) return NULL;
    if (size != 0U) memcpy(copy, bytes, size);
    copy[size] = '\0';
    return copy;
}

static bool path_separator_byte(char value) {
#ifdef _WIN32
    return value == '/' || value == '\\';
#else
    return value == '/';
#endif
}

static size_t path_root_spelling_size(const char* path, size_t size) {
    if (!path || size == 0U) return 0U;
#ifdef _WIN32
    size_t root_size = 0U;
    if (common_windows_directory_root_length(
            path, size, &root_size)) {
        return root_size;
    }
#endif
    return path_separator_byte(path[0]) ? 1U : 0U;
}

static size_t path_without_redundant_trailing_separators(
    const char* path) {
    if (!path) return 0U;
    size_t size = strlen(path);
    size_t root_size = path_root_spelling_size(path, size);
    while (size > root_size && size > 0U &&
           path_separator_byte(path[size - 1U])) {
        --size;
    }
    return size;
}

static char* duplicate_parent_path(const char* path) {
    if (!path || !path[0]) return NULL;
    const char* last = NULL;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (path_separator_byte(*cursor)) last = cursor;
    }
    if (!last) return duplicate_string(".");
    size_t size = (size_t)(last - path);
    if (size == 0U) size = 1U;
#ifdef _WIN32
    if (size == 2U && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\')) {
        size = 3U;
    }
#endif
    char* parent = (char*)malloc(size + 1U);
    if (!parent) return NULL;
    memcpy(parent, path, size);
    parent[size] = '\0';
    return parent;
}

static bool lexical_path_has_root(const char* path, const char* root) {
    if (!path || !root || !root[0]) return false;
    size_t root_size =
        path_without_redundant_trailing_separators(root);
    if (root_size == 0U) return false;
    if (strncmp(path, root, root_size) != 0) return false;
    if (path_separator_byte(root[root_size - 1U])) return true;
    return path[root_size] == '\0' || path_separator_byte(path[root_size]);
}

static bool assign_source_scope_root(ShaderCatalogSource* source,
                                     const CatalogVisitorContext* context) {
    if (!source || !source->outer_path || !context) return false;
    const char* selected = NULL;
    size_t selected_size = 0U;
    for (size_t i = 0U; i < context->input_count; ++i) {
        const char* input = context->inputs[i];
        if (!input || !input[0]) continue;
        if (strcmp(source->outer_path, input) == 0) continue;
        if (!lexical_path_has_root(source->outer_path, input)) continue;
        size_t input_size =
            path_without_redundant_trailing_separators(input);
        if (input_size > selected_size) {
            selected = input;
            selected_size = input_size;
        }
    }
    if (!selected) return true;
    char* replacement = (char*)malloc(selected_size + 1U);
    if (!replacement) return false;
    memcpy(replacement, selected, selected_size);
    replacement[selected_size] = '\0';
    free(source->scope_root);
    source->scope_root = replacement;
    return true;
}

static void record_dispose(ShaderCatalogRecord* record) {
    if (!record) return;
    free(record->outer_path);
    free(record->member_name);
    free(record->unity_version);
    free(record->name);
    memset(record, 0, sizeof(*record));
}

static void material_record_dispose(ShaderCatalogMaterialRecord* record) {
    if (!record) return;
    free(record->outer_path);
    free(record->member_name);
    free(record->unity_version);
    free(record->name);
    material_object_dispose(&record->object);
    memset(record, 0, sizeof(*record));
}

static void source_dispose(ShaderCatalogSource* source) {
    if (!source) return;
    free(source->outer_path);
    free(source->scope_root);
    free(source->member_name);
    free(source->unity_version);
    for (size_t i = 0U; i < source->external_count; ++i) {
        free(source->externals[i].virtual_path);
        free(source->externals[i].path_name);
    }
    free(source->objects);
    free(source->externals);
    free(source->class_id_21_schema_keys);
    free(source->class_id_48_schema_keys);
    memset(source, 0, sizeof(*source));
}

static void issue_dispose(ShaderCatalogIssue* issue) {
    if (!issue) return;
    free(issue->outer_path);
    free(issue->member_name);
    memset(issue, 0, sizeof(*issue));
}

void shader_catalog_options_default(ShaderCatalogOptions* options) {
    if (!options) return;
    options->recursive = true;
    options->schema_registry = NULL;
    options->retain_source_snapshots = false;
    options->include_materials = false;
}

void shader_catalog_init(ShaderCatalog* catalog) {
    if (catalog) memset(catalog, 0, sizeof(*catalog));
}

void shader_catalog_dispose(ShaderCatalog* catalog) {
    if (!catalog) return;
    for (size_t i = 0U; i < catalog->retained_source_snapshot_count; ++i) {
        if (unity_input_snapshot_is_open(
                &catalog->retained_source_snapshots[i])) {
            (void)unity_input_snapshot_close(
                &catalog->retained_source_snapshots[i]);
        }
    }
    for (size_t i = 0U; i < catalog->record_count; ++i) {
        record_dispose(&catalog->records[i]);
    }
    for (size_t i = 0U; i < catalog->material_count; ++i) {
        material_record_dispose(&catalog->materials[i]);
    }
    for (size_t i = 0U; i < catalog->source_count; ++i) {
        source_dispose(&catalog->sources[i]);
    }
    for (size_t i = 0U; i < catalog->issue_count; ++i) {
        issue_dispose(&catalog->issues[i]);
    }
    free(catalog->records);
    free(catalog->materials);
    free(catalog->sources);
    free(catalog->issues);
    free(catalog->retained_source_snapshots);
    shader_catalog_init(catalog);
}

static bool append_retained_source_snapshot(
    ShaderCatalog* catalog, UnityInputSnapshot* snapshot) {
    if (!catalog || !snapshot || !unity_input_snapshot_is_open(snapshot) ||
        catalog->retained_source_snapshot_count == SIZE_MAX ||
        dxbc_size_multiply_overflows(
            catalog->retained_source_snapshot_count + 1U,
            sizeof(*catalog->retained_source_snapshots))) {
        return false;
    }
    size_t new_count = catalog->retained_source_snapshot_count + 1U;
    UnityInputSnapshot* snapshots = (UnityInputSnapshot*)realloc(
        catalog->retained_source_snapshots,
        new_count * sizeof(*catalog->retained_source_snapshots));
    if (!snapshots) return false;
    catalog->retained_source_snapshots = snapshots;
    snapshots[catalog->retained_source_snapshot_count] = *snapshot;
    unity_input_snapshot_init(snapshot);
    catalog->retained_source_snapshot_count = new_count;
    return true;
}

static bool append_issue(ShaderCatalog* catalog, const char* outer_path,
                         const char* member_name,
                         ShaderCatalogIssueCode code,
                         UnityInputStatus input_status) {
    if (!catalog || !outer_path || catalog->issue_count == SIZE_MAX ||
        dxbc_size_multiply_overflows(
            catalog->issue_count + 1U, sizeof(*catalog->issues))) {
        return false;
    }
    size_t new_count = catalog->issue_count + 1U;
    ShaderCatalogIssue* issues = (ShaderCatalogIssue*)realloc(
        catalog->issues, new_count * sizeof(*catalog->issues));
    if (!issues) return false;
    catalog->issues = issues;
    ShaderCatalogIssue* issue = &issues[catalog->issue_count];
    memset(issue, 0, sizeof(*issue));
    issue->outer_path = duplicate_string(outer_path);
    issue->member_name = duplicate_string(member_name);
    if (!issue->outer_path || (member_name && !issue->member_name)) {
        issue_dispose(issue);
        return false;
    }
    issue->code = code;
    issue->input_status = input_status;
    issue->discovery_status = COMMON_PATH_DISCOVERY_OK;
    catalog->issue_count = new_count;
    return true;
}

static bool append_discovery_issue(
    ShaderCatalog* catalog, const char* input,
    CommonPathDiscoveryStatus discovery_status) {
    if (!append_issue(catalog, input, NULL,
                      SHADER_CATALOG_ISSUE_DISCOVERY,
                      UNITY_INPUT_INVALID_ARGUMENT)) {
        return false;
    }
    catalog->issues[catalog->issue_count - 1U].discovery_status =
        discovery_status;
    return true;
}

static void store_le64(uint8_t bytes[8], uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8U));
    }
}

static void source_occurrence_digest(
    const UnitySerializedSource* source,
    const uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE],
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    static const char domain[] = "DXBCSandbox.ShaderOccurrence.v1";
    CommonSha256Context context;
    common_sha256_init(&context);
    common_sha256_update(&context, domain, sizeof(domain));
    common_sha256_update(
        &context, source->outer_path, strlen(source->outer_path) + 1U);
    if (source->member_name) {
        common_sha256_update(
            &context, source->member_name, strlen(source->member_name) + 1U);
    } else {
        const uint8_t no_member = 0U;
        common_sha256_update(&context, &no_member, sizeof(no_member));
    }
    uint8_t ordinal[8];
    store_le64(ordinal, (uint64_t)source->member_index);
    common_sha256_update(&context, ordinal, sizeof(ordinal));
    const uint8_t bundle_tag = source->is_bundle_member ? 1U : 0U;
    common_sha256_update(&context, &bundle_tag, sizeof(bundle_tag));
    common_sha256_update(&context, serialized_digest,
                         COMMON_SHA256_DIGEST_SIZE);
    common_sha256_final(&context, digest);
}

static bool initialize_source_identity(
    ShaderCatalogSource* record, const UnitySerializedSource* source,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!record || !source || !source->outer_path || !digest) return false;
    memset(record, 0, sizeof(*record));
    memcpy(record->serialized_digest, digest,
           sizeof(record->serialized_digest));
    common_sha256_digest_to_hex(digest, record->serialized_digest_hex);
    uint8_t occurrence_digest[COMMON_SHA256_DIGEST_SIZE];
    source_occurrence_digest(source, digest, occurrence_digest);
    common_sha256_digest_to_hex(occurrence_digest,
                                record->occurrence_digest_hex);
    int written = snprintf(record->occurrence_id,
                           sizeof(record->occurrence_id), "f:%s",
                           record->occurrence_digest_hex);
    if (written < 0 || (size_t)written >= sizeof(record->occurrence_id)) {
        return false;
    }
    record->outer_path = duplicate_string(source->outer_path);
    record->scope_root = duplicate_parent_path(source->outer_path);
    record->member_name = duplicate_string(source->member_name);
    if (!record->outer_path || !record->scope_root ||
        (source->member_name && !record->member_name)) {
        source_dispose(record);
        return false;
    }
    record->member_index = source->member_index;
    record->is_bundle_member = source->is_bundle_member;
    record->metadata_status = SHADER_CATALOG_SOURCE_METADATA_ERROR;
    record->class_id_48_schema_status = TYPETREE_SCHEMA_OK;
    return true;
}

static bool append_source(ShaderCatalog* catalog,
                          ShaderCatalogSource* pending) {
    if (!catalog || !pending || catalog->source_count == SIZE_MAX ||
        dxbc_size_multiply_overflows(
            catalog->source_count + 1U, sizeof(*catalog->sources))) {
        return false;
    }
    size_t new_count = catalog->source_count + 1U;
    ShaderCatalogSource* sources = (ShaderCatalogSource*)realloc(
        catalog->sources, new_count * sizeof(*catalog->sources));
    if (!sources) return false;
    catalog->sources = sources;
    sources[catalog->source_count] = *pending;
    memset(pending, 0, sizeof(*pending));
    catalog->source_count = new_count;
    return true;
}

static size_t class_schema_key_count(const TypeTreeType* types,
                                     int type_count, int32_t class_id) {
    size_t count = 0U;
    if (!types || type_count <= 0) return 0U;
    for (int index = 0; index < type_count; ++index) {
        if (types[index].type_id == class_id) ++count;
    }
    return count;
}

static void initialize_schema_key_record(
    ShaderCatalogSchemaKeyRecord* record, const SerializedFile* file,
    int32_t class_id, const TypeTreeType* type,
    const TypeTreeSchemaRegistry* registry) {
    memset(record, 0, sizeof(*record));
    record->serialized_file_version = file->version;
    record->class_id = class_id;
    record->serialized_type_id = type->type_id;
    record->script_type_index = type->script_type_index;
    record->is_stripped = type->is_stripped;
    record->is_ref_type = type->is_ref_type;
    record->lookup_required = type->node_count == 0 || !type->nodes;

    TypeTreeSchemaKey key;
    TypeTreeSchemaStatus status = typetree_schema_key_from_type(
        &key, file->version, file->unity_version, class_id, type);
    if (status != TYPETREE_SCHEMA_OK) {
        record->lookup_status = status;
        return;
    }
    record->has_script_id_hash = key.has_script_id_hash;
    memcpy(record->script_id_hash, key.script_id_hash,
           sizeof(record->script_id_hash));
    memcpy(record->type_hash, key.type_hash, sizeof(record->type_hash));

    if (!record->lookup_required) {
        record->lookup_status = TYPETREE_SCHEMA_OK;
        record->provenance =
            SHADER_CATALOG_SCHEMA_PROVENANCE_EMBEDDED_TYPETREE;
        record->profile = typetree_schema_validate_known_profile(
            key.unity_version, key.unity_version_size, key.class_id,
            key.type_hash, type);
        return;
    }
    if (!registry) {
        record->lookup_status = TYPETREE_SCHEMA_INVALID_ARGUMENT;
        return;
    }
    TypeTreeType resolved;
    status = typetree_schema_registry_lookup(registry, &key, &resolved);
    record->lookup_status = status;
    if (status == TYPETREE_SCHEMA_OK) {
        record->provenance =
            SHADER_CATALOG_SCHEMA_PROVENANCE_EXACT_REGISTRY;
        record->profile = typetree_schema_validate_known_profile(
            key.unity_version, key.unity_version_size, key.class_id,
            key.type_hash, &resolved);
        typetree_free_type(&resolved);
    }
}

static bool initialize_class_48_schema_keys(
    ShaderCatalogSource* source, const SerializedFile* file,
    const TypeTreeSchemaRegistry* registry) {
    size_t key_count = class_schema_key_count(
        file->types, file->type_count, 48);
    size_t ref_key_count = class_schema_key_count(
        file->ref_types, file->ref_type_count, 48);
    if (key_count > SIZE_MAX - ref_key_count) return false;
    key_count += ref_key_count;
    if (key_count == 0U) return true;
    if (dxbc_size_multiply_overflows(
            key_count, sizeof(*source->class_id_48_schema_keys))) {
        return false;
    }
    source->class_id_48_schema_keys =
        (ShaderCatalogSchemaKeyRecord*)calloc(
            key_count, sizeof(*source->class_id_48_schema_keys));
    if (!source->class_id_48_schema_keys) return false;
    source->class_id_48_schema_key_count = key_count;
    size_t output_index = 0U;
    for (int index = 0; index < file->type_count; ++index) {
        if (file->types[index].type_id != 48) continue;
        initialize_schema_key_record(
            &source->class_id_48_schema_keys[output_index++], file, 48,
            &file->types[index], registry);
    }
    for (int index = 0; index < file->ref_type_count; ++index) {
        if (file->ref_types[index].type_id != 48) continue;
        initialize_schema_key_record(
            &source->class_id_48_schema_keys[output_index++], file, 48,
            &file->ref_types[index], registry);
    }
    return output_index == key_count;
}

static bool initialize_class_21_schema_keys(
    ShaderCatalogSource* source, const SerializedFile* file,
    const TypeTreeSchemaRegistry* registry) {
    size_t key_count = class_schema_key_count(
        file->types, file->type_count, 21);
    size_t ref_key_count = class_schema_key_count(
        file->ref_types, file->ref_type_count, 21);
    if (key_count > SIZE_MAX - ref_key_count) return false;
    key_count += ref_key_count;
    if (key_count == 0U) return true;
    if (dxbc_size_multiply_overflows(
            key_count, sizeof(*source->class_id_21_schema_keys))) {
        return false;
    }
    source->class_id_21_schema_keys =
        (ShaderCatalogSchemaKeyRecord*)calloc(
            key_count, sizeof(*source->class_id_21_schema_keys));
    if (!source->class_id_21_schema_keys) return false;
    source->class_id_21_schema_key_count = key_count;
    size_t output_index = 0U;
    for (int index = 0; index < file->type_count; ++index) {
        if (file->types[index].type_id != 21) continue;
        initialize_schema_key_record(
            &source->class_id_21_schema_keys[output_index++], file, 21,
            &file->types[index], registry);
    }
    for (int index = 0; index < file->ref_type_count; ++index) {
        if (file->ref_types[index].type_id != 21) continue;
        initialize_schema_key_record(
            &source->class_id_21_schema_keys[output_index++], file, 21,
            &file->ref_types[index], registry);
    }
    return output_index == key_count;
}

static bool copy_source_metadata_tables(ShaderCatalogSource* destination,
                                        const SerializedFile* file) {
    if (!destination || !file || file->object_count < 0 ||
        file->external_count < 0) {
        return false;
    }
    size_t object_count = (size_t)file->object_count;
    size_t external_count = (size_t)file->external_count;
    if (dxbc_size_multiply_overflows(
            object_count, sizeof(*destination->objects)) ||
        dxbc_size_multiply_overflows(
            external_count, sizeof(*destination->externals))) {
        return false;
    }
    if (object_count != 0U) {
        destination->objects = (AssetObjectInfo*)calloc(
            object_count, sizeof(*destination->objects));
        if (!destination->objects) return false;
        destination->object_reference_count = object_count;
        for (size_t i = 0U; i < object_count; ++i) {
            destination->objects[i] = file->objects[i];
        }
    }
    if (external_count != 0U) {
        destination->externals = (AssetFileExternal*)calloc(
            external_count, sizeof(*destination->externals));
        if (!destination->externals) return false;
        destination->external_count = external_count;
        for (size_t i = 0U; i < external_count; ++i) {
            const AssetFileExternal* source = &file->externals[i];
            AssetFileExternal* target =
                &destination->externals[i];
            target->virtual_path = duplicate_string(source->virtual_path);
            target->path_name = duplicate_string(source->path_name);
            memcpy(target->guid, source->guid, sizeof(target->guid));
            target->type = source->type;
            if ((source->virtual_path && !target->virtual_path) ||
                (source->path_name && !target->path_name)) {
                return false;
            }
        }
    }
    return true;
}

static bool initialize_record_identity(
    ShaderCatalogRecord* record, const UnitySerializedSource* source,
    const SerializedFile* file, const AssetObjectInfo* object,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    memset(record, 0, sizeof(*record));
    memcpy(record->serialized_digest, digest,
           sizeof(record->serialized_digest));
    common_sha256_digest_to_hex(digest, record->serialized_digest_hex);
    int written = snprintf(record->content_id, sizeof(record->content_id),
                           "s:%s:%" PRId64,
                           record->serialized_digest_hex, object->path_id);
    if (written < 0 || (size_t)written >= sizeof(record->content_id)) {
        return false;
    }
    uint8_t occurrence_digest[COMMON_SHA256_DIGEST_SIZE];
    source_occurrence_digest(source, digest, occurrence_digest);
    common_sha256_digest_to_hex(occurrence_digest,
                                record->occurrence_digest_hex);
    written = snprintf(record->occurrence_id,
                       sizeof(record->occurrence_id),
                       "o:%s:%" PRId64,
                       record->occurrence_digest_hex, object->path_id);
    if (written < 0 || (size_t)written >= sizeof(record->occurrence_id)) {
        return false;
    }
    record->outer_path = duplicate_string(source->outer_path);
    record->member_name = duplicate_string(source->member_name);
    record->unity_version = duplicate_string(file->unity_version);
    if (!record->outer_path || !record->unity_version ||
        (source->member_name && !record->member_name)) {
        record_dispose(record);
        return false;
    }
    record->path_id = object->path_id;
    record->class_id = object->type_id;
    record->member_index = source->member_index;
    record->is_bundle_member = source->is_bundle_member;
    record->target_platform = file->target_platform;
    record->object_size = object->byte_size;
    record->schema_status = TYPETREE_SCHEMA_OK;
    record->object_status = SHADER_OBJECT_OK;
    return true;
}

static bool append_record(ShaderCatalog* catalog,
                          ShaderCatalogRecord* pending) {
    if (!catalog || !pending || catalog->record_count == SIZE_MAX ||
        dxbc_size_multiply_overflows(
            catalog->record_count + 1U, sizeof(*catalog->records))) {
        return false;
    }
    size_t new_count = catalog->record_count + 1U;
    ShaderCatalogRecord* records = (ShaderCatalogRecord*)realloc(
        catalog->records, new_count * sizeof(*catalog->records));
    if (!records) return false;
    catalog->records = records;
    records[catalog->record_count] = *pending;
    memset(pending, 0, sizeof(*pending));
    catalog->record_count = new_count;
    return true;
}

static bool initialize_material_record_identity(
    ShaderCatalogMaterialRecord* record,
    const UnitySerializedSource* source, const SerializedFile* file,
    const AssetObjectInfo* object,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!record || !source || !file || !object || !digest) return false;
    memset(record, 0, sizeof(*record));
    material_object_init(&record->object);
    record->source_index = SIZE_MAX;
    record->shader_record_index = SIZE_MAX;
    record->shader_link_status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
    memcpy(record->serialized_digest, digest,
           sizeof(record->serialized_digest));
    common_sha256_digest_to_hex(digest, record->serialized_digest_hex);
    int written = snprintf(record->content_id, sizeof(record->content_id),
                           "mc:%s:%" PRId64,
                           record->serialized_digest_hex, object->path_id);
    if (written < 0 || (size_t)written >= sizeof(record->content_id)) {
        return false;
    }
    uint8_t occurrence_digest[COMMON_SHA256_DIGEST_SIZE];
    source_occurrence_digest(source, digest, occurrence_digest);
    common_sha256_digest_to_hex(occurrence_digest,
                                record->occurrence_digest_hex);
    written = snprintf(record->occurrence_id,
                       sizeof(record->occurrence_id), "mo:%s:%" PRId64,
                       record->occurrence_digest_hex, object->path_id);
    if (written < 0 || (size_t)written >= sizeof(record->occurrence_id)) {
        return false;
    }
    record->outer_path = duplicate_string(source->outer_path);
    record->member_name = duplicate_string(source->member_name);
    record->unity_version = duplicate_string(file->unity_version);
    if (!record->outer_path || !record->unity_version ||
        (source->member_name && !record->member_name)) {
        material_record_dispose(record);
        return false;
    }
    record->member_index = source->member_index;
    record->is_bundle_member = source->is_bundle_member;
    record->path_id = object->path_id;
    record->object_size = object->byte_size;
    record->schema_status = TYPETREE_SCHEMA_OK;
    record->object_status = MATERIAL_OBJECT_NOT_DECODED;
    return true;
}

static bool append_material_record_storage(
    ShaderCatalog* catalog, ShaderCatalogMaterialRecord* pending) {
    if (!catalog || !pending || catalog->material_count == SIZE_MAX ||
        dxbc_size_multiply_overflows(
            catalog->material_count + 1U, sizeof(*catalog->materials))) {
        return false;
    }
    size_t new_count = catalog->material_count + 1U;
    ShaderCatalogMaterialRecord* records =
        (ShaderCatalogMaterialRecord*)realloc(
            catalog->materials, new_count * sizeof(*catalog->materials));
    if (!records) return false;
    catalog->materials = records;
    records[catalog->material_count] = *pending;
    memset(pending, 0, sizeof(*pending));
    catalog->material_count = new_count;
    return true;
}

static bool append_material_record(
    CatalogVisitorContext* context, const UnitySerializedSource* source,
    const SerializedFile* file, const AssetObjectInfo* object,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
    TypeTreeSchemaStatus schema_status) {
    ShaderCatalogMaterialRecord record;
    if (!initialize_material_record_identity(
            &record, source, file, object, digest)) {
        return false;
    }
    ++context->catalog->stats.material_objects;
    if (schema_status != TYPETREE_SCHEMA_OK) {
        record.status = SHADER_CATALOG_MATERIAL_SCHEMA_ERROR;
        record.schema_status = schema_status;
        ++context->catalog->stats.failed_materials;
    } else {
        record.object_status = material_object_decode(
            &record.object, file, object);
        if (record.object_status != MATERIAL_OBJECT_OK) {
            record.status = SHADER_CATALOG_MATERIAL_DECODE_ERROR;
            ++context->catalog->stats.failed_materials;
        } else {
            record.name = duplicate_byte_string(
                record.object.name.bytes, record.object.name.size);
            if (!record.name) {
                material_record_dispose(&record);
                return false;
            }
            record.status = SHADER_CATALOG_MATERIAL_LINK_ERROR;
            ++context->catalog->stats.decoded_materials;
        }
    }
    if (!append_material_record_storage(context->catalog, &record)) {
        material_record_dispose(&record);
        return false;
    }
    return true;
}

static void count_record_status(ShaderCatalog* catalog,
                                ShaderCatalogRecordStatus status) {
    if (status == SHADER_CATALOG_RECORD_READY) {
        ++catalog->stats.ready_shaders;
    } else if (status == SHADER_CATALOG_RECORD_D3D11_UNAVAILABLE) {
        ++catalog->stats.unavailable_shaders;
    } else {
        ++catalog->stats.failed_shaders;
    }
}

static bool append_shader_record(
    CatalogVisitorContext* context, const UnitySerializedSource* source,
    const SerializedFile* file, const AssetObjectInfo* object,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
    TypeTreeSchemaStatus schema_status) {
    ShaderCatalogRecord record;
    if (!initialize_record_identity(
            &record, source, file, object, digest)) {
        return false;
    }
    ++context->catalog->stats.shader_objects;
    if (schema_status != TYPETREE_SCHEMA_OK) {
        record.status = SHADER_CATALOG_RECORD_SCHEMA_ERROR;
        record.schema_status = schema_status;
        count_record_status(context->catalog, record.status);
        if (!append_record(context->catalog, &record)) {
            record_dispose(&record);
            return false;
        }
        return true;
    }

    ShaderObject decoded;
    shader_object_init(&decoded);
    ShaderObjectStatus object_status = shader_object_decode_borrowed(
        &decoded, file, object);
    if (object_status != SHADER_OBJECT_OK) {
        record.status = SHADER_CATALOG_RECORD_DECODE_ERROR;
        record.object_status = object_status;
    } else {
        record.name = duplicate_string(decoded.shader.name);
        if (!record.name) {
            shader_object_dispose(&decoded);
            record_dispose(&record);
            return false;
        }
        ++context->catalog->stats.decoded_shaders;
        object_status = shader_object_d3d11_platform_status(&decoded);
        if (object_status == SHADER_OBJECT_OK) {
            ShaderBlobArchiveInfo archive_info;
            if (!shader_blob_archive_inspect(
                    &decoded.root, 4, &archive_info)) {
                record.status = SHADER_CATALOG_RECORD_ARCHIVE_ERROR;
                record.object_status = SHADER_OBJECT_D3D11_ARCHIVE_INVALID;
            } else {
                record.status = SHADER_CATALOG_RECORD_READY;
                record.d3d11_blob_entries = archive_info.entry_count;
            }
        } else if (object_status ==
                   SHADER_OBJECT_D3D11_PLATFORM_ABSENT) {
            record.status = SHADER_CATALOG_RECORD_D3D11_UNAVAILABLE;
            record.object_status = object_status;
        } else {
            record.status = SHADER_CATALOG_RECORD_ARCHIVE_ERROR;
            record.object_status = object_status;
        }
    }
    shader_object_dispose(&decoded);
    count_record_status(context->catalog, record.status);
    if (!append_record(context->catalog, &record)) {
        record_dispose(&record);
        return false;
    }
    return true;
}

static bool append_compute_shader_record(
    CatalogVisitorContext* context, const UnitySerializedSource* source,
    const SerializedFile* file, const AssetObjectInfo* object,
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
    TypeTreeSchemaStatus schema_status) {
    ShaderCatalogRecord record;
    if (!initialize_record_identity(
            &record, source, file, object, digest)) {
        return false;
    }
    ++context->catalog->stats.shader_objects;
    ++context->catalog->stats.compute_shader_objects;
    record.status = SHADER_CATALOG_RECORD_DECODE_ERROR;
    record.schema_status = schema_status;
    record.object_status = SHADER_OBJECT_NOT_DECODED;
    ComputeShaderNameView name_view;
    record.compute_inventory_status = compute_shader_object_name_view(
        file, object, &name_view);
    if (record.compute_inventory_status == COMPUTE_SHADER_INVENTORY_OK) {
        record.name = duplicate_byte_string(name_view.name_bytes,
                                            name_view.name_size);
        if (!record.name) {
            record_dispose(&record);
            return false;
        }
        record.compute_platform_variants_declared =
            name_view.declared_platform_variant_count;
        ++context->catalog->stats.named_compute_shaders;
    }

    ComputeShaderObject decoded;
    compute_shader_object_init(&decoded);
    record.compute_object_status = compute_shader_object_decode_borrowed(
        &decoded, file, object);
    if (record.compute_object_status == COMPUTE_SHADER_OBJECT_OK) {
        if (decoded.platform_count > UINT32_MAX ||
            !compute_shader_object_summarize(
                &decoded, &record.compute_summary)) {
            record.compute_object_status = COMPUTE_SHADER_OBJECT_MODEL_INVALID;
        } else {
            record.compute_platform_variants_declared =
                (uint32_t)decoded.platform_count;
            record.compute_source_authority_status =
                compute_shader_object_source_authority(&decoded);
            record.status = SHADER_CATALOG_RECORD_READY;
            ++context->catalog->stats.decoded_shaders;
        }
    }
    compute_shader_object_dispose(&decoded);
    count_record_status(context->catalog, record.status);
    if (!append_record(context->catalog, &record)) {
        record_dispose(&record);
        return false;
    }
    return true;
}

static bool catalog_serialized_source(const UnitySerializedSource* source,
                                      void* opaque_context) {
    CatalogVisitorContext* context =
        (CatalogVisitorContext*)opaque_context;
    if (!source || !context || !context->catalog) return false;
    ++context->catalog->stats.visited_serialized_sources;
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(source->data, source->size, digest);
    ShaderCatalogSource catalog_source;
    if (!initialize_source_identity(&catalog_source, source, digest)) {
        context->allocation_failed = true;
        return false;
    }
    if (!assign_source_scope_root(&catalog_source, context)) {
        source_dispose(&catalog_source);
        context->allocation_failed = true;
        return false;
    }
    SerializedFile file;
    if (!serialized_file_open_metadata(&file, source->data, source->size)) {
        if (!append_source(context->catalog, &catalog_source)) {
            source_dispose(&catalog_source);
            context->allocation_failed = true;
            return false;
        }
        if (!append_issue(context->catalog, source->outer_path,
                          source->member_name,
                          SHADER_CATALOG_ISSUE_SERIALIZED_METADATA,
                          UNITY_INPUT_OK)) {
            context->allocation_failed = true;
            return false;
        }
        return true;
    }
    ++context->catalog->stats.serialized_sources;
    catalog_source.metadata_status = SHADER_CATALOG_SOURCE_METADATA_OK;
    catalog_source.serialized_file_version = file.version;
    catalog_source.unity_version = duplicate_string(file.unity_version);
    catalog_source.target_platform = file.target_platform;
    catalog_source.type_tree_enabled = file.type_tree_enabled;
    catalog_source.object_count = (size_t)file.object_count;
    if (!catalog_source.unity_version ||
        (context->include_materials &&
         !copy_source_metadata_tables(&catalog_source, &file))) {
        source_dispose(&catalog_source);
        serialized_file_close(&file);
        context->allocation_failed = true;
        return false;
    }

    size_t material_count = 0U;
    size_t shader_count = 0U;
    size_t compute_shader_count = 0U;
    for (int i = 0; i < file.object_count; ++i) {
        if (file.objects[i].type_id == 21) ++material_count;
        if (file.objects[i].type_id == 48) ++shader_count;
        if (file.objects[i].type_id == 72) ++compute_shader_count;
    }
    catalog_source.class_id_21_count = material_count;
    catalog_source.class_id_48_count = shader_count;
    catalog_source.class_id_72_count = compute_shader_count;
    catalog_source.class_id_21_schema_required =
        context->include_materials && material_count != 0U;
    TypeTreeSchemaStatus material_schema_status = TYPETREE_SCHEMA_OK;
    if (catalog_source.class_id_21_schema_required) {
        if (!initialize_class_21_schema_keys(
                &catalog_source, &file, context->registry)) {
            source_dispose(&catalog_source);
            serialized_file_close(&file);
            context->allocation_failed = true;
            return false;
        }
        material_schema_status = serialized_file_resolve_class_schema(
            &file, 21, context->registry);
    }
    catalog_source.class_id_21_schema_status = material_schema_status;
    catalog_source.class_id_48_schema_required = shader_count != 0U;
    if (shader_count != 0U) context->saw_graphics_source = true;
    TypeTreeSchemaStatus shader_schema_status = TYPETREE_SCHEMA_OK;
    if (shader_count != 0U) {
        if (!initialize_class_48_schema_keys(
                &catalog_source, &file, context->registry)) {
            source_dispose(&catalog_source);
            serialized_file_close(&file);
            context->allocation_failed = true;
            return false;
        }
        shader_schema_status = serialized_file_resolve_class_schema(
            &file, 48, context->registry);
    }
    catalog_source.class_id_48_schema_status = shader_schema_status;
    if (!append_source(context->catalog, &catalog_source)) {
        source_dispose(&catalog_source);
        serialized_file_close(&file);
        context->allocation_failed = true;
        return false;
    }
    if (shader_count == 0U && compute_shader_count == 0U &&
        (!context->include_materials || material_count == 0U)) {
        serialized_file_close(&file);
        return true;
    }
    TypeTreeSchemaStatus compute_shader_schema_status = TYPETREE_SCHEMA_OK;
    if (compute_shader_count != 0U) {
        /* Resolving records the exact schema authority independently of
         * parser support.  Failure is retained on every ClassID 72
         * occurrence rather than using the ClassID 48 schema or guessing. */
        compute_shader_schema_status = serialized_file_resolve_class_schema(
            &file, 72, context->registry);
    }
    for (int i = 0; i < file.object_count; ++i) {
        const AssetObjectInfo* object = &file.objects[i];
        bool appended = true;
        if (object->type_id == 21 && context->include_materials) {
            appended = append_material_record(
                context, source, &file, object, digest,
                material_schema_status);
        } else if (object->type_id == 48) {
            appended = append_shader_record(
                context, source, &file, object, digest,
                shader_schema_status);
        } else if (object->type_id == 72) {
            appended = append_compute_shader_record(
                context, source, &file, object, digest,
                compute_shader_schema_status);
        } else {
            continue;
        }
        if (!appended) {
            context->allocation_failed = true;
            serialized_file_close(&file);
            return false;
        }
    }
    serialized_file_close(&file);
    return true;
}

static ShaderCatalogIssueCode issue_code_from_input_status(
    UnityInputStatus status) {
    if (status == UNITY_INPUT_UNSUPPORTED) {
        return SHADER_CATALOG_ISSUE_UNSUPPORTED_INPUT;
    }
    if (status == UNITY_INPUT_FILE_ERROR) {
        return SHADER_CATALOG_ISSUE_INPUT_IO;
    }
    return SHADER_CATALOG_ISSUE_CONTAINER;
}

static int compare_records(const void* left, const void* right) {
    const ShaderCatalogRecord* a = (const ShaderCatalogRecord*)left;
    const ShaderCatalogRecord* b = (const ShaderCatalogRecord*)right;
    int comparison = strcmp(a->outer_path, b->outer_path);
    if (comparison != 0) return comparison;
    if (a->is_bundle_member != b->is_bundle_member) {
        return a->is_bundle_member ? 1 : -1;
    }
    if (a->member_index != b->member_index) {
        return a->member_index < b->member_index ? -1 : 1;
    }
    if (a->member_name || b->member_name) {
        if (!a->member_name) return -1;
        if (!b->member_name) return 1;
        comparison = strcmp(a->member_name, b->member_name);
        if (comparison != 0) return comparison;
    }
    if (a->path_id != b->path_id) {
        return a->path_id < b->path_id ? -1 : 1;
    }
    comparison = memcmp(a->serialized_digest, b->serialized_digest,
                        sizeof(a->serialized_digest));
    if (comparison != 0) return comparison;
    return strcmp(a->occurrence_id, b->occurrence_id);
}

static int compare_sources(const void* left, const void* right) {
    const ShaderCatalogSource* a = (const ShaderCatalogSource*)left;
    const ShaderCatalogSource* b = (const ShaderCatalogSource*)right;
    int comparison = strcmp(a->outer_path, b->outer_path);
    if (comparison != 0) return comparison;
    if (a->is_bundle_member != b->is_bundle_member) {
        return a->is_bundle_member ? 1 : -1;
    }
    if (a->member_index != b->member_index) {
        return a->member_index < b->member_index ? -1 : 1;
    }
    if (a->member_name || b->member_name) {
        if (!a->member_name) return -1;
        if (!b->member_name) return 1;
        comparison = strcmp(a->member_name, b->member_name);
        if (comparison != 0) return comparison;
    }
    comparison = memcmp(a->serialized_digest, b->serialized_digest,
                        sizeof(a->serialized_digest));
    if (comparison != 0) return comparison;
    return strcmp(a->occurrence_id, b->occurrence_id);
}

static int compare_material_records(const void* left, const void* right) {
    const ShaderCatalogMaterialRecord* a =
        (const ShaderCatalogMaterialRecord*)left;
    const ShaderCatalogMaterialRecord* b =
        (const ShaderCatalogMaterialRecord*)right;
    int comparison = strcmp(a->outer_path, b->outer_path);
    if (comparison != 0) return comparison;
    if (a->is_bundle_member != b->is_bundle_member) {
        return a->is_bundle_member ? 1 : -1;
    }
    if (a->member_index != b->member_index) {
        return a->member_index < b->member_index ? -1 : 1;
    }
    if (a->member_name || b->member_name) {
        if (!a->member_name) return -1;
        if (!b->member_name) return 1;
        comparison = strcmp(a->member_name, b->member_name);
        if (comparison != 0) return comparison;
    }
    if (a->path_id != b->path_id) {
        return a->path_id < b->path_id ? -1 : 1;
    }
    comparison = memcmp(a->serialized_digest, b->serialized_digest,
                        sizeof(a->serialized_digest));
    if (comparison != 0) return comparison;
    return strcmp(a->occurrence_id, b->occurrence_id);
}

static bool nullable_strings_match(const char* left, const char* right) {
    return (!left && !right) || (left && right && strcmp(left, right) == 0);
}

static bool material_matches_source(
    const ShaderCatalogMaterialRecord* material,
    const ShaderCatalogSource* source) {
    return material && source &&
        strcmp(material->outer_path, source->outer_path) == 0 &&
        nullable_strings_match(material->member_name, source->member_name) &&
        material->member_index == source->member_index &&
        material->is_bundle_member == source->is_bundle_member &&
        memcmp(material->serialized_digest, source->serialized_digest,
               COMMON_SHA256_DIGEST_SIZE) == 0;
}

static bool shader_matches_source_object(
    const ShaderCatalogRecord* shader, const ShaderCatalogSource* source,
    int64_t path_id) {
    return shader && source && shader->class_id == 48 &&
        shader->path_id == path_id &&
        strcmp(shader->outer_path, source->outer_path) == 0 &&
        nullable_strings_match(shader->member_name, source->member_name) &&
        shader->member_index == source->member_index &&
        shader->is_bundle_member == source->is_bundle_member &&
        memcmp(shader->serialized_digest, source->serialized_digest,
               COMMON_SHA256_DIGEST_SIZE) == 0;
}

static bool resolve_material_shader_links(ShaderCatalog* catalog) {
    if (!catalog) return false;
    if (catalog->material_count == 0U) return true;
    ShaderCatalogPPtrGraph graph;
    shader_catalog_pptr_graph_init(&graph);
    if (!shader_catalog_pptr_graph_build(catalog, &graph)) return false;
    for (size_t i = 0U; i < catalog->material_count; ++i) {
        ShaderCatalogMaterialRecord* material = &catalog->materials[i];
        if (material->object_status != MATERIAL_OBJECT_OK) continue;
        size_t source_index = SIZE_MAX;
        for (size_t j = 0U; j < catalog->source_count; ++j) {
            if (material_matches_source(material, &catalog->sources[j])) {
                if (source_index != SIZE_MAX) {
                    source_index = SIZE_MAX;
                    break;
                }
                source_index = j;
            }
        }
        if (source_index == SIZE_MAX) {
            material->status = SHADER_CATALOG_MATERIAL_LINK_ERROR;
            material->shader_link_status =
                UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
            ++catalog->stats.unresolved_material_shader_links;
            ++catalog->stats.failed_materials;
            continue;
        }
        material->source_index = source_index;
        UnityPPtrResolveResult resolved;
        bool called = unity_pptr_resolve(
            &graph.graph, source_index, &material->object.shader, 48,
            &resolved);
        material->shader_link_status = called
            ? resolved.status : UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
        if (called && resolved.status == UNITY_PPTR_RESOLVE_NULL) {
            material->status = SHADER_CATALOG_MATERIAL_SHADER_NULL;
            ++catalog->stats.null_material_shader_links;
            continue;
        }
        if (!called || !unity_pptr_resolve_status_is_success(
                           resolved.status)) {
            material->status = SHADER_CATALOG_MATERIAL_LINK_ERROR;
            ++catalog->stats.unresolved_material_shader_links;
            ++catalog->stats.failed_materials;
            continue;
        }
        size_t shader_index = SIZE_MAX;
        const ShaderCatalogSource* target =
            &catalog->sources[resolved.target_index];
        for (size_t j = 0U; j < catalog->record_count; ++j) {
            if (!shader_matches_source_object(
                    &catalog->records[j], target,
                    resolved.object->path_id)) {
                continue;
            }
            if (shader_index != SIZE_MAX) {
                shader_index = SIZE_MAX;
                break;
            }
            shader_index = j;
        }
        if (shader_index == SIZE_MAX) {
            material->status = SHADER_CATALOG_MATERIAL_LINK_ERROR;
            material->shader_link_status =
                UNITY_PPTR_RESOLVE_TARGET_MISSING;
            ++catalog->stats.unresolved_material_shader_links;
            ++catalog->stats.failed_materials;
            continue;
        }
        material->shader_record_index = shader_index;
        material->status = SHADER_CATALOG_MATERIAL_READY;
        ++catalog->stats.resolved_material_shader_links;
    }
    shader_catalog_pptr_graph_dispose(&graph);
    return true;
}

ShaderCatalogStatus shader_catalog_build(
    const char* const* inputs, size_t input_count,
    const ShaderCatalogOptions* options, ShaderCatalog* catalog) {
    if (!inputs || input_count == 0U || !catalog) {
        return SHADER_CATALOG_INVALID_ARGUMENT;
    }
    for (size_t i = 0U; i < input_count; ++i) {
        if (!inputs[i] || !inputs[i][0]) {
            return SHADER_CATALOG_INVALID_ARGUMENT;
        }
    }
    ShaderCatalogOptions defaults;
    shader_catalog_options_default(&defaults);
    if (!options) options = &defaults;

    if (dxbc_size_multiply_overflows(
            input_count, sizeof(char*))) {
        return SHADER_CATALOG_ALLOCATION_FAILED;
    }
    char** stable_inputs = (char**)calloc(
        input_count, sizeof(*stable_inputs));
    if (!stable_inputs) return SHADER_CATALOG_ALLOCATION_FAILED;
    for (size_t i = 0U; i < input_count; ++i) {
        stable_inputs[i] = stable_absolute_catalog_path(inputs[i]);
        if (!stable_inputs[i]) {
            stable_catalog_inputs_dispose(stable_inputs, input_count);
            return SHADER_CATALOG_ALLOCATION_FAILED;
        }
    }

    CommonPathDiscoveryOptions discovery_options;
    common_path_discovery_options_default(&discovery_options);
    discovery_options.recursive = options->recursive;
    ShaderCatalog pending;
    shader_catalog_init(&pending);
    pending.materials_included = options->include_materials;
    pending.stats.requested_inputs = input_count;
    CommonPathDiscoveryResult discovered;
    common_path_discovery_result_init(&discovered);
    for (size_t input_index = 0U; input_index < input_count; ++input_index) {
        CommonPathDiscoveryResult one;
        common_path_discovery_result_init(&one);
        const char* const one_input[] = {stable_inputs[input_index]};
        CommonPathDiscoveryStatus discovery_status = common_path_discover(
            one_input, 1U, &discovery_options, &one);
        if (discovery_status == COMMON_PATH_DISCOVERY_ALLOCATION_FAILED) {
            common_path_discovery_result_dispose(&one);
            common_path_discovery_result_dispose(&discovered);
            shader_catalog_dispose(&pending);
            stable_catalog_inputs_dispose(stable_inputs, input_count);
            return SHADER_CATALOG_ALLOCATION_FAILED;
        }
        if (discovery_status != COMMON_PATH_DISCOVERY_OK) {
            common_path_discovery_result_dispose(&one);
            if (!append_discovery_issue(
                    &pending, stable_inputs[input_index], discovery_status)) {
                common_path_discovery_result_dispose(&discovered);
                shader_catalog_dispose(&pending);
                stable_catalog_inputs_dispose(stable_inputs, input_count);
                return SHADER_CATALOG_ALLOCATION_FAILED;
            }
            continue;
        }
        discovery_status = common_path_discovery_result_merge(
            &discovered, &one);
        common_path_discovery_result_dispose(&one);
        if (discovery_status != COMMON_PATH_DISCOVERY_OK) {
            common_path_discovery_result_dispose(&discovered);
            shader_catalog_dispose(&pending);
            stable_catalog_inputs_dispose(stable_inputs, input_count);
            return discovery_status ==
                    COMMON_PATH_DISCOVERY_ALLOCATION_FAILED
                ? SHADER_CATALOG_ALLOCATION_FAILED
                : SHADER_CATALOG_INVALID_ARGUMENT;
        }
    }
    pending.stats.discovered_files = discovered.count;
    CatalogVisitorContext context = {
        &pending, options->schema_registry, false, false,
        options->include_materials,
        (const char* const*)stable_inputs, input_count
    };

    for (size_t i = 0U; i < discovered.count; ++i) {
        const CommonDiscoveredPath* path = &discovered.paths[i];
        UnityInputProbe probe;
        UnityInputStatus probe_status = unity_input_probe_path(
            path->path, &probe);
        if (probe_status == UNITY_INPUT_UNRELATED) {
            if (path->explicit_file) {
                if (!append_issue(
                        &pending, path->path, NULL,
                        SHADER_CATALOG_ISSUE_EXPLICIT_UNRELATED,
                        probe_status)) {
                    context.allocation_failed = true;
                    break;
                }
            } else {
                ++pending.stats.ignored_unrelated_files;
            }
            continue;
        }
        if (probe_status != UNITY_INPUT_OK) {
            if (!append_issue(&pending, path->path, NULL,
                              issue_code_from_input_status(probe_status),
                              probe_status)) {
                context.allocation_failed = true;
                break;
            }
            continue;
        }
        if (probe.kind == UNITY_INPUT_KIND_UNITYFS) {
            ++pending.stats.unity_container_files;
        } else {
            ++pending.stats.standalone_serialized_files;
        }
        UnityInputVisitStats visit_stats = {0};
        UnityInputStatus visit_status;
        context.saw_graphics_source = false;
        if (options->retain_source_snapshots) {
            UnityInputSnapshot snapshot;
            unity_input_snapshot_init(&snapshot);
            visit_status = unity_input_snapshot_open(
                path->path, &snapshot);
            if (visit_status == UNITY_INPUT_OK) {
                visit_status = unity_input_snapshot_visit(
                    &snapshot, catalog_serialized_source,
                    &context, &visit_stats);
            }
            if (visit_status == UNITY_INPUT_OK &&
                visit_stats.serialized_files != 0U) {
                visit_status = unity_input_snapshot_suspend_mapping(
                    &snapshot);
                if (visit_status == UNITY_INPUT_OK &&
                    !append_retained_source_snapshot(
                        &pending, &snapshot)) {
                    context.allocation_failed = true;
                }
            }
            if (unity_input_snapshot_is_open(&snapshot)) {
                UnityInputStatus close_status =
                    unity_input_snapshot_close(&snapshot);
                if (close_status != UNITY_INPUT_OK) {
                    visit_status = close_status;
                }
            }
        } else {
            visit_status = unity_input_visit_serialized(
                path->path, catalog_serialized_source,
                &context, &visit_stats);
        }
        if (context.allocation_failed) break;
        if (visit_status != UNITY_INPUT_OK) {
            if (!append_issue(&pending, path->path, NULL,
                              issue_code_from_input_status(visit_status),
                              visit_status)) {
                context.allocation_failed = true;
                break;
            }
        }
    }
    common_path_discovery_result_dispose(&discovered);
    if (context.allocation_failed) {
        shader_catalog_dispose(&pending);
        stable_catalog_inputs_dispose(stable_inputs, input_count);
        return SHADER_CATALOG_ALLOCATION_FAILED;
    }
    if (pending.record_count > 1U) {
        qsort(pending.records, pending.record_count,
              sizeof(*pending.records), compare_records);
    }
    if (pending.source_count > 1U) {
        qsort(pending.sources, pending.source_count,
              sizeof(*pending.sources), compare_sources);
    }
    if (pending.material_count > 1U) {
        qsort(pending.materials, pending.material_count,
              sizeof(*pending.materials), compare_material_records);
    }
    if (options->include_materials &&
        !resolve_material_shader_links(&pending)) {
        shader_catalog_dispose(&pending);
        stable_catalog_inputs_dispose(stable_inputs, input_count);
        return SHADER_CATALOG_ALLOCATION_FAILED;
    }
    shader_catalog_dispose(catalog);
    *catalog = pending;
    stable_catalog_inputs_dispose(stable_inputs, input_count);
    return SHADER_CATALOG_OK;
}

bool shader_catalog_is_complete(const ShaderCatalog* catalog) {
    return catalog && catalog->issue_count == 0U &&
        catalog->stats.failed_shaders == 0U &&
        (!catalog->materials_included ||
         (catalog->stats.failed_materials == 0U &&
          catalog->stats.material_objects == catalog->material_count)) &&
        catalog->stats.shader_objects == catalog->record_count &&
        catalog->stats.shader_objects ==
            catalog->stats.ready_shaders +
            catalog->stats.unavailable_shaders +
            catalog->stats.failed_shaders;
}

const char* shader_catalog_record_status_name(ShaderCatalogRecordStatus status) {
    switch (status) {
        case SHADER_CATALOG_RECORD_READY: return "ready";
        case SHADER_CATALOG_RECORD_D3D11_UNAVAILABLE:
            return "d3d11-unavailable";
        case SHADER_CATALOG_RECORD_SCHEMA_ERROR: return "schema-error";
        case SHADER_CATALOG_RECORD_DECODE_ERROR: return "decode-error";
        case SHADER_CATALOG_RECORD_ARCHIVE_ERROR: return "archive-error";
        case SHADER_CATALOG_RECORD_COMPUTE_SHADER_UNSUPPORTED:
            return "compute-shader-unsupported";
        default: return "unknown";
    }
}

const char* shader_catalog_material_status_name(
    ShaderCatalogMaterialStatus status) {
    switch (status) {
        case SHADER_CATALOG_MATERIAL_READY: return "ready";
        case SHADER_CATALOG_MATERIAL_SHADER_NULL: return "shader-null";
        case SHADER_CATALOG_MATERIAL_SCHEMA_ERROR: return "schema-error";
        case SHADER_CATALOG_MATERIAL_DECODE_ERROR: return "decode-error";
        case SHADER_CATALOG_MATERIAL_LINK_ERROR: return "link-error";
        default: return "unknown";
    }
}

const char* shader_catalog_issue_code_name(ShaderCatalogIssueCode code) {
    switch (code) {
        case SHADER_CATALOG_ISSUE_EXPLICIT_UNRELATED:
            return "explicit-unrelated";
        case SHADER_CATALOG_ISSUE_UNSUPPORTED_INPUT:
            return "unsupported-input";
        case SHADER_CATALOG_ISSUE_INPUT_IO: return "input-io";
        case SHADER_CATALOG_ISSUE_CONTAINER: return "container";
        case SHADER_CATALOG_ISSUE_SERIALIZED_METADATA:
            return "serialized-metadata";
        case SHADER_CATALOG_ISSUE_DISCOVERY: return "discovery";
        default: return "unknown";
    }
}

const char* shader_catalog_status_name(ShaderCatalogStatus status) {
    switch (status) {
        case SHADER_CATALOG_OK: return "ok";
        case SHADER_CATALOG_INVALID_ARGUMENT: return "invalid-argument";
        case SHADER_CATALOG_DISCOVERY_FAILED: return "discovery-failed";
        case SHADER_CATALOG_ALLOCATION_FAILED: return "allocation-failed";
        default: return "unknown";
    }
}

const char* shader_catalog_source_metadata_status_name(
    ShaderCatalogSourceMetadataStatus status) {
    switch (status) {
        case SHADER_CATALOG_SOURCE_METADATA_OK: return "ok";
        case SHADER_CATALOG_SOURCE_METADATA_ERROR:
            return "metadata-error";
        default: return "unknown";
    }
}

const char* shader_catalog_schema_provenance_name(
    ShaderCatalogSchemaProvenance provenance) {
    switch (provenance) {
        case SHADER_CATALOG_SCHEMA_PROVENANCE_NONE: return "none";
        case SHADER_CATALOG_SCHEMA_PROVENANCE_EMBEDDED_TYPETREE:
            return "embedded-typetree";
        case SHADER_CATALOG_SCHEMA_PROVENANCE_EXACT_REGISTRY:
            return "exact-schema-registry";
        default: return "unknown";
    }
}

const char* shader_catalog_schema_profile_name(
    TypeTreeSchemaProfileResult profile) {
    switch (profile) {
        case TYPETREE_SCHEMA_PROFILE_UNKNOWN: return "unknown";
        case TYPETREE_SCHEMA_PROFILE_VALID: return "known-valid";
        case TYPETREE_SCHEMA_PROFILE_INVALID: return "known-invalid";
        default: return "unknown-enum";
    }
}
