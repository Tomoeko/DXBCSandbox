// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_CATALOG_H
#define SHADER_CATALOG_H

#include "common/path_discovery.h"
#include "common/sha256.h"
#include "io/compute_shader_object.h"
#include "io/material_object.h"
#include "io/shader_object.h"
#include "io/typetree_schema_profile.h"
#include "io/typetree_schema_registry.h"
#include "io/unity_input.h"
#include "io/unity_pptr_resolver.h"

typedef enum {
    SHADER_CATALOG_SOURCE_METADATA_OK = 0,
    SHADER_CATALOG_SOURCE_METADATA_ERROR,
} ShaderCatalogSourceMetadataStatus;

typedef enum {
    SHADER_CATALOG_SCHEMA_PROVENANCE_NONE = 0,
    /* The schema was carried by this exact SerializedFile. It is usable for
     * this file, but is not registry authority. */
    SHADER_CATALOG_SCHEMA_PROVENANCE_EMBEDDED_TYPETREE,
    /* The TypeTree-disabled key was matched byte-for-byte in the caller's
     * exact schema registry. */
    SHADER_CATALOG_SCHEMA_PROVENANCE_EXACT_REGISTRY,
} ShaderCatalogSchemaProvenance;

typedef struct {
    uint32_t serialized_file_version;
    int32_t class_id;
    int32_t serialized_type_id;
    uint16_t script_type_index;
    bool has_script_id_hash;
    bool is_stripped;
    bool is_ref_type;
    uint8_t script_id_hash[16];
    uint8_t type_hash[16];

    /* Embedded TypeTrees do not perform a registry lookup. In that case
     * lookup_required is false and lookup_status is OK only as a neutral C
     * value; reports render the lookup status as null. */
    bool lookup_required;
    TypeTreeSchemaStatus lookup_status;
    ShaderCatalogSchemaProvenance provenance;
    TypeTreeSchemaProfileResult profile;
} ShaderCatalogSchemaKeyRecord;

typedef struct {
    char occurrence_id[96];
    char occurrence_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE];
    char serialized_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];

    char* outer_path;
    char* scope_root;
    char* member_name;
    size_t member_index;
    bool is_bundle_member;

    ShaderCatalogSourceMetadataStatus metadata_status;
    uint32_t serialized_file_version;
    char* unity_version;
    uint32_t target_platform;
    bool type_tree_enabled;
    size_t object_count;
    size_t class_id_21_count;
    size_t class_id_48_count;
    size_t class_id_72_count;

    /* Exact metadata needed to resolve PPtrs without asset-name heuristics.
     * These tables retain the SerializedFile order because fileID indexes the
     * external table and pathID addresses the object table. */
    AssetObjectInfo* objects;
    size_t object_reference_count;
    AssetFileExternal* externals;
    size_t external_count;

    bool class_id_21_schema_required;
    TypeTreeSchemaStatus class_id_21_schema_status;
    ShaderCatalogSchemaKeyRecord* class_id_21_schema_keys;
    size_t class_id_21_schema_key_count;

    bool class_id_48_schema_required;
    TypeTreeSchemaStatus class_id_48_schema_status;
    ShaderCatalogSchemaKeyRecord* class_id_48_schema_keys;
    size_t class_id_48_schema_key_count;
} ShaderCatalogSource;

typedef enum {
    SHADER_CATALOG_RECORD_READY = 0,
    SHADER_CATALOG_RECORD_D3D11_UNAVAILABLE,
    SHADER_CATALOG_RECORD_SCHEMA_ERROR,
    SHADER_CATALOG_RECORD_DECODE_ERROR,
    SHADER_CATALOG_RECORD_ARCHIVE_ERROR,
    /* Legacy status retained for report compatibility. New ClassID 72
     * records use READY after complete layout decode because their exact
     * binary/manifest authority is extractable; .compute source authority is
     * reported independently and remains fail-closed. */
    SHADER_CATALOG_RECORD_COMPUTE_SHADER_UNSUPPORTED,
} ShaderCatalogRecordStatus;

typedef struct {
    /* Occurrence identity includes the supplied outer pathname and exact
     * bundle member ordinal. Content identity remains relocation-stable and
     * may intentionally have aliases in copied files. */
    char occurrence_id[96];
    char occurrence_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char content_id[96];
    uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE];
    char serialized_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];

    char* outer_path;
    char* member_name;
    size_t member_index;
    bool is_bundle_member;
    char* unity_version;
    char* name;
    int64_t path_id;
    int32_t class_id;
    uint32_t target_platform;
    uint32_t object_size;
    size_t d3d11_blob_entries;
    /* ClassID 72 inventory, complete layout decode and source reconstruction
     * are three separate authority planes. */
    uint32_t compute_platform_variants_declared;
    ComputeShaderObjectSummary compute_summary;

    ShaderCatalogRecordStatus status;
    TypeTreeSchemaStatus schema_status;
    ShaderObjectStatus object_status;
    ComputeShaderInventoryStatus compute_inventory_status;
    ComputeShaderObjectStatus compute_object_status;
    ComputeShaderSourceAuthorityStatus compute_source_authority_status;
} ShaderCatalogRecord;

typedef enum {
    SHADER_CATALOG_MATERIAL_READY = 0,
    SHADER_CATALOG_MATERIAL_SHADER_NULL,
    SHADER_CATALOG_MATERIAL_SCHEMA_ERROR,
    SHADER_CATALOG_MATERIAL_DECODE_ERROR,
    SHADER_CATALOG_MATERIAL_LINK_ERROR,
} ShaderCatalogMaterialStatus;

typedef struct {
    char occurrence_id[96];
    char occurrence_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char content_id[96];
    uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE];
    char serialized_digest_hex[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];

    char* outer_path;
    char* member_name;
    size_t member_index;
    bool is_bundle_member;
    char* unity_version;
    char* name;
    int64_t path_id;
    uint32_t object_size;

    ShaderCatalogMaterialStatus status;
    TypeTreeSchemaStatus schema_status;
    MaterialObjectStatus object_status;
    UnityPPtrResolveStatus shader_link_status;
    size_t source_index;
    size_t shader_record_index;
    MaterialObject object;
} ShaderCatalogMaterialRecord;

typedef enum {
    SHADER_CATALOG_ISSUE_EXPLICIT_UNRELATED = 0,
    SHADER_CATALOG_ISSUE_UNSUPPORTED_INPUT,
    SHADER_CATALOG_ISSUE_INPUT_IO,
    SHADER_CATALOG_ISSUE_CONTAINER,
    SHADER_CATALOG_ISSUE_SERIALIZED_METADATA,
    SHADER_CATALOG_ISSUE_DISCOVERY,
} ShaderCatalogIssueCode;

typedef struct {
    char* outer_path;
    char* member_name;
    ShaderCatalogIssueCode code;
    UnityInputStatus input_status;
    CommonPathDiscoveryStatus discovery_status;
} ShaderCatalogIssue;

typedef struct {
    size_t requested_inputs;
    size_t discovered_files;
    size_t ignored_unrelated_files;
    size_t unity_container_files;
    size_t standalone_serialized_files;
    /* Every source delivered by unity_input, including sources whose strict
     * SerializedFile metadata later fails to parse. */
    size_t visited_serialized_sources;
    /* Backward-compatible count of successfully parsed metadata sources. */
    size_t serialized_sources;
    /* All shader-bearing Unity asset classes currently inventoried: Shader
     * (ClassID 48) and ComputeShader (ClassID 72). */
    size_t shader_objects;
    size_t compute_shader_objects;
    size_t material_objects;
    size_t decoded_materials;
    size_t resolved_material_shader_links;
    size_t unresolved_material_shader_links;
    size_t null_material_shader_links;
    size_t failed_materials;
    size_t named_compute_shaders;
    size_t decoded_shaders;
    size_t ready_shaders;
    size_t unavailable_shaders;
    size_t failed_shaders;
} ShaderCatalogStats;

typedef struct {
    bool materials_included;
    ShaderCatalogSource* sources;
    size_t source_count;
    ShaderCatalogRecord* records;
    size_t record_count;
    ShaderCatalogMaterialRecord* materials;
    size_t material_count;
    ShaderCatalogIssue* issues;
    size_t issue_count;
    ShaderCatalogStats stats;
    /* Optional exact source identities retained for one extraction pass.
     * These are app-owned implementation state, not catalog identity. */
    UnityInputSnapshot* retained_source_snapshots;
    size_t retained_source_snapshot_count;
} ShaderCatalog;

typedef struct {
    bool recursive;
    const TypeTreeSchemaRegistry* schema_registry;
    /* Preserve the exact already-hashed source object through extraction.
     * Listing leaves this false and closes every source during cataloging. */
    bool retain_source_snapshots;
    /* Decode and retain exact ClassID 21 Material state and PPtr evidence.
     * This is opt-in so ordinary shader inventory keeps its prior memory
     * boundary. */
    bool include_materials;
} ShaderCatalogOptions;

typedef enum {
    SHADER_CATALOG_OK = 0,
    SHADER_CATALOG_INVALID_ARGUMENT,
    SHADER_CATALOG_DISCOVERY_FAILED,
    SHADER_CATALOG_ALLOCATION_FAILED,
} ShaderCatalogStatus;

void shader_catalog_options_default(ShaderCatalogOptions* options);
void shader_catalog_init(ShaderCatalog* catalog);
void shader_catalog_dispose(ShaderCatalog* catalog);

/*
 * Builds a complete, deterministic occurrence catalog. Unrelated descendants
 * are counted and ignored; an explicitly supplied unrelated file becomes an
 * issue. Per-input and per-shader-asset failures are retained in the result
 * and do not stop the scan. The operation has a strong output guarantee.
 */
ShaderCatalogStatus shader_catalog_build(
    const char* const* inputs, size_t input_count,
    const ShaderCatalogOptions* options, ShaderCatalog* catalog);

bool shader_catalog_is_complete(const ShaderCatalog* catalog);
const char* shader_catalog_record_status_name(ShaderCatalogRecordStatus status);
const char* shader_catalog_material_status_name(
    ShaderCatalogMaterialStatus status);
const char* shader_catalog_issue_code_name(ShaderCatalogIssueCode code);
const char* shader_catalog_status_name(ShaderCatalogStatus status);
const char* shader_catalog_source_metadata_status_name(
    ShaderCatalogSourceMetadataStatus status);
const char* shader_catalog_schema_provenance_name(
    ShaderCatalogSchemaProvenance provenance);
const char* shader_catalog_schema_profile_name(
    TypeTreeSchemaProfileResult profile);

#endif /* SHADER_CATALOG_H */
