// SPDX-License-Identifier: GPL-3.0-only

#ifndef SUBPROGRAM_METADATA_H
#define SUBPROGRAM_METADATA_H

#include "common/common.h"
#include "common/stream.h"

/* SaveGpuProgramToDataWithoutParameters / LoadVariantFromData wire dialect
 * verified for Unity 2021.3.35f1.  This is not a monotonic file-format range:
 * accepting a nearby integer under this layout would be a fail-open parse. */
#define UNITY_2021_3_PLAYER_BLOB_VERSION UINT32_C(202012090)

typedef enum {
    PLAYER_BLOB_DIALECT_INVALID = 0,
    PLAYER_BLOB_DIALECT_UNITY_2021_3_35F1 = 1
} PlayerBlobDialect;

/*
 * Explicit owner for dynamically sized serialized identifiers.  Model fields
 * borrow pointers from this pool (or from caller-owned literals in synthetic
 * data); disposing a model releases only strings registered here.
 */
typedef struct {
    size_t count;
    size_t capacity;
    char** strings;
} SerializedStringPool;

void serialized_string_pool_init(SerializedStringPool* pool);
void serialized_string_pool_dispose(SerializedStringPool* pool);
bool serialized_string_pool_copy(SerializedStringPool* pool,
                                 const char* source,
                                 const char** out_string);
/* On success the pool takes ownership of an exact mem_alloc C string. */
bool serialized_string_pool_take(SerializedStringPool* pool,
                                 char* owned_string,
                                 const char** out_string);

/* TypeTree representation of SerializedPlayerSubProgram.  These fields are
 * not read from the player blob wrapper. */
typedef struct {
    int32_t blob_index;
    bool has_hardware_tier;
    int32_t hardware_tier;
    int32_t program_type;
    /* Raw 64-bit ShaderRequirements mask transferred by Unity 2021.3 as
     * TypeTree SInt64.  Keep the complete two's-complement bit pattern: this
     * is identity-bearing authority, not a 32-bit projection. */
    uint64_t shader_requirements;
    int local_keyword_count;
    char** local_keywords;
    int global_keyword_count;
    char** global_keywords;
} SerializedSubProgram;

typedef struct {
    uint32_t channel;
    uint32_t component;
} PlayerSubProgramBindChannel;

/* Exact wire metadata consumed by UnityPlayer's LoadVariantFromData. */
typedef struct {
    uint32_t version;
    PlayerBlobDialect dialect;
    int32_t program_type;
    /*
     * Exact opaque words written immediately after program_type by
     * SaveGpuProgramToDataWithoutParameters and skipped by
     * LoadVariantFromData.  They are deliberately not assigned TypeTree
     * meanings: corpus values exceed the valid domains of blob indices and
     * hardware-tier scalars.
     */
    bool has_player_blob_header;
    uint32_t player_header_words[4];
    /* Exact word consumed into SerializedBindChannels::m_SourceMap. */
    uint32_t source_map;
    
    int local_keyword_count;
    char** local_keywords;
    
    int global_keyword_count;
    char** global_keywords;
    
    uint32_t bytecode_length;
    const uint8_t* bytecode; // Points into the stream payload
    
    int binding_count;
    PlayerSubProgramBindChannel* bindings;
} PlayerSubProgramMetadata;

// Structs representing parsed parameters block
typedef struct {
    const char* name;
    uint32_t layout[6];
} SerializedVariable;

typedef struct {
    const char* name;
    uint32_t layout[3];
    int member_count;
    SerializedVariable* members;
} SerializedStructParam;

/* Unity's binary parameter stream stores one distinguished loose-parameter
 * area before the named constant-buffer array.  The TypeTree represents the
 * same area as m_VectorParams/m_MatrixParams.  Its conventional name is
 * `$Globals`, but a real named `$Globals` buffer can immediately follow it;
 * the name alone therefore cannot identify the role.  Zero is NAMED so
 * ordinary zero-initialized fixtures retain their historical meaning. */
typedef enum {
    SERIALIZED_CBUFFER_NAMED = 0,
    SERIALIZED_CBUFFER_LOOSE_PARAMETERS = 1
} SerializedConstantBufferRole;

typedef struct {
    const char* name;
    SerializedConstantBufferRole role;
    uint32_t size;
    bool has_is_partial;
    bool is_partial;
    int var_count;
    SerializedVariable* variables;
    
    int struct_count;
    SerializedStructParam* struct_params;
} SerializedConstantBuffer;

/* Raw resource discriminator used by Unity's LoadParametersFromData. */
typedef enum {
    SERIALIZED_RESOURCE_TEXTURE = 0,
    SERIALIZED_RESOURCE_CONSTANT_BUFFER = 1,
    SERIALIZED_RESOURCE_BUFFER = 2,
    SERIALIZED_RESOURCE_UAV = 3,
    SERIALIZED_RESOURCE_SAMPLER = 4
} SerializedResourceType;

typedef struct {
    const char* name;
    SerializedResourceType bind_type;
    uint32_t bind_index;
    uint32_t array_size;
    uint32_t dimension;
    uint32_t sampler_index;
    bool multisampled;
    uint32_t original_index;
    uint32_t sampler_state;
    /* Exact words from the player parameter-blob wire representation. */
    uint32_t extra[2];
} SerializedResourceParam;

typedef struct {
    uint32_t version;
    PlayerBlobDialect dialect;
    bool is_binary;
    SerializedStringPool owned_strings;
    int cb_count;
    SerializedConstantBuffer* constant_buffers;
    
    int res_count;
    SerializedResourceParam* resources;
} SerializedProgramParameters;

/* SerializedProgramParameters owns every nested collection. Destinations
 * passed to parse/copy must first be initialized. Parse and copy provide a
 * strong output guarantee: on failure the previous destination is unchanged. */
void serialized_program_parameters_init(SerializedProgramParameters* params);
void serialized_program_parameters_free(SerializedProgramParameters* params);
bool serialized_program_parameters_copy(SerializedProgramParameters* dest,
                                        const SerializedProgramParameters* src);

// Deserializes a subprogram header from the stream
bool subprogram_metadata_parse_variant(ByteStream* stream,
                                       PlayerSubProgramMetadata* out_sub);
void subprogram_metadata_free_variant(PlayerSubProgramMetadata* sub);
/* Player blob keyword names and TypeTree subprogram keyword names describe
 * sets. Their storage orders are independent in Unity 2021.3. Exact matching
 * therefore requires duplicate-free set equality, not positional equality. */
bool subprogram_metadata_local_keyword_set_matches(
    const PlayerSubProgramMetadata* player,
    const SerializedSubProgram* serialized);

// Deserializes a parameters block from the stream
bool subprogram_metadata_parse_parameters(ByteStream* stream, SerializedProgramParameters* out_params);

#endif // SUBPROGRAM_METADATA_H
