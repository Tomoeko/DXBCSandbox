#include "common/common.h"
#include "common/sha256.h"
#include "io/bundle_archive.h"
#include "io/serialized_file.h"
#include "io/shader_blob_archive.h"
#include "io/serialized_shader.h"
#include "io/subprogram_metadata.h"
#include "io/lz4_decompress.h"
#include "dxbc/dxbc_parser.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <stdalign.h>

#ifndef DXBCSANDBOX_PINNED_UNITY_PLAYER_PATH
#define DXBCSANDBOX_PINNED_UNITY_PLAYER_PATH ""
#endif

/*
 * Every function address below is relative to this exact Unity 2021.3.35f1
 * arm64 image. A successful dlopen is not ABI authority; applying these
 * offsets to another build can crash.
 */
static const uint8_t k_pinned_unity_player_sha256[COMMON_SHA256_DIGEST_SIZE] = {
    0x9e, 0x5a, 0x2b, 0xa3, 0x53, 0xe9, 0x8d, 0x0a,
    0xe2, 0xb6, 0x61, 0xba, 0xd5, 0x35, 0xb9, 0x94,
    0x3b, 0x6b, 0xbe, 0x83, 0x13, 0x4b, 0x27, 0xa3,
    0x77, 0xb4, 0xea, 0x7c, 0x3c, 0xd0, 0x20, 0x6d,
};

static bool hash_file_sha256(
    const char* path, uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!path || !digest) return false;
    FILE* file = fopen(path, "rb");
    if (!file) return false;

    CommonSha256Context context;
    common_sha256_init(&context);
    uint8_t buffer[64 * 1024];
    bool success = true;
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0) common_sha256_update(&context, buffer, count);
        if (count != sizeof(buffer)) {
            if (ferror(file)) success = false;
            break;
        }
    }
    if (fclose(file) != 0) success = false;
    if (!success) return false;
    common_sha256_final(&context, digest);
    return true;
}

static void format_sha256(
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE],
    char output[COMMON_SHA256_DIGEST_SIZE * 2 + 1]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < COMMON_SHA256_DIGEST_SIZE; ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    output[COMMON_SHA256_DIGEST_SIZE * 2] = '\0';
}

// --- Helper Structures and Functions ---



static const TypeTreeValue* find_field(const TypeTreeValue* root, const TypeTreeValue* parsed_form, const char* name) {
    const TypeTreeValue* val = typetree_find_child(parsed_form, name);
    if (!val) val = typetree_find_child(root, name);
    return val;
}

static uint8_t* read_file_to_buffer(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) { fclose(f); return NULL; }
    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);
    if (read_bytes != (size_t)size) { free(buf); return NULL; }
    *out_size = (size_t)size;
    return buf;
}

// --- Unity C++ Type Definitions ---

typedef struct {
    char data[48];
} UnityString;

static const char* get_unity_string(const UnityString* str) {
    // Offset 32 is the flag (var1): 1 means stack allocated (inline), 0 means heap allocated.
    unsigned char flag = (unsigned char)str->data[32];
    if (flag == 1) {
        return str->data;
    } else {
        uintptr_t ptr = *(const uintptr_t*)str->data;
        return (const char*)ptr;
    }
}



typedef struct {
    void* m_data;
    uint64_t m_label_salt;
    uint32_t m_label_id;
    uint32_t m_padding;
    size_t m_size;
    size_t m_capacity;
} UnityDynamicArray;

typedef struct {
    uint32_t channel;
    uint32_t component;
} UnitySerializedBindChannel;

typedef struct {
    UnityString name;
    uint32_t layout[6];
    uint32_t padding[2];
} UnityVariable;

typedef struct {
    UnityString name;                // offset 0 (48 bytes)
    uint32_t size;                   // offset 48 (4 bytes)
    uint32_t padding_0[1];           // offset 52 (4 bytes)
    UnityDynamicArray matrix_params; // offset 56 (40 bytes)
    UnityDynamicArray vector_params; // offset 96 (40 bytes)
    UnityDynamicArray struct_params; // offset 136 (40 bytes)
    uint64_t padding_1;              // offset 176 (8 bytes)
} UnityConstantBuffer;               // size 184


typedef struct {
    UnityString name;                // offset 0 (48 bytes)
    int32_t name_hash;               // offset 48 (4 bytes)
    uint32_t index;                  // offset 52 (4 bytes)
    uint32_t size;                   // offset 56 (4 bytes)
    uint32_t member_count;           // offset 60 (4 bytes)
    UnityDynamicArray vector_params; // offset 64 (40 bytes)
    UnityDynamicArray matrix_params; // offset 104 (40 bytes)
} UnityStructParameter;              // size 144

typedef struct {
    UnityString name;   // offset 0 (48 bytes)
    int32_t name_hash;   // offset 48 (4 bytes)
    uint32_t index;     // offset 52 (4 bytes)
    uint32_t type;      // offset 56 (4 bytes)
    uint32_t extra;     // offset 60 (4 bytes)
    uint32_t padding[2]; // offset 64 (8 bytes)
} UnityTextureParameter; // size 72

typedef struct {
    UnityString name;   // offset 0 (48 bytes)
    int32_t name_hash;   // offset 48 (4 bytes)
    uint32_t index;     // offset 52 (4 bytes)
    uint32_t type;      // offset 56 (4 bytes)
    uint32_t padding[3]; // offset 60 (12 bytes)
} UnityUAVParameter; // size 72

typedef struct {
    UnityString name;   // offset 0 (48 bytes)
    int32_t name_hash;   // offset 48 (4 bytes)
    uint32_t index;     // offset 52 (4 bytes)
    uint32_t padding[5]; // offset 56 (20 bytes)
} UnityBufferBinding; // size 72


typedef struct {
    uint32_t type;       // offset 0 (4 bytes)
    uint32_t index;      // offset 4 (4 bytes)
} UnitySamplerParameter; // size 8

typedef struct {
    UnityString name;   // offset 0 (48 bytes)
    int32_t name_hash;   // offset 48 (4 bytes)
    uint32_t index;     // offset 52 (4 bytes)
    uint32_t size;      // offset 56 (4 bytes)
    uint32_t type;      // offset 60 (4 bytes)
    uint8_t rows;       // offset 64 (1 byte)
    uint8_t padding[7]; // offset 65-71 (7 bytes)
} UnityMatrixParameter; // size 72

typedef struct {
    UnityString name;   // offset 0 (48 bytes)
    int32_t name_hash;   // offset 48 (4 bytes)
    uint32_t index;     // offset 52 (4 bytes)
    uint32_t size;      // offset 56 (4 bytes)
    uint32_t type;      // offset 60 (4 bytes)
    uint8_t dim;        // offset 64 (1 byte)
    uint8_t padding[7]; // offset 65-71 (7 bytes)
} UnityVectorParameter; // size 72


// --- Function Pointers to Unity dylib ---

typedef void* (*LocalSpace_ctor_fn)(void* self, const void* label_id);
typedef void (*LocalSpace_dtor_fn)(void* self);
typedef bool (*LoadVariantFromData_fn)(const uint8_t** stream_ptr, const uint8_t* stream_end, void* local_space, uint32_t version, void* serialized_subprogram, uint32_t* version_out);
typedef bool (*LoadParametersFromData_fn)(const uint8_t** stream_ptr, const uint8_t* stream_end, uint32_t version, void* serialized_program_parameters);

// Destructors for memory safety
typedef void (*dtor_uint8_array_fn)(void* self);
typedef void (*dtor_channels_array_fn)(void* self);
typedef void (*dtor_keyword_state_fn)(void* self);
typedef void (*dtor_cb_array_fn)(void* self);
typedef void (*dtor_matrix_array_fn)(void* self);
typedef void (*dtor_vector_array_fn)(void* self);
typedef void (*dtor_sampler_array_fn)(void* self);
typedef void (*dtor_texture_array_fn)(void* self);
typedef void (*dtor_buffer_array_fn)(void* self);
typedef void (*dtor_uav_array_fn)(void* self);

static uintptr_t g_unity_base = 0;

static uintptr_t get_unity_player_base(void) {
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const char* name = _dyld_get_image_name(i);
        if (name && strstr(name, "UnityPlayer.dylib")) {
            return (uintptr_t)_dyld_get_image_header(i);
        }
    }
    return 0;
}

static void destruct_params(void* params) {
    if (!g_unity_base) return;
    
    ((dtor_cb_array_fn)(g_unity_base + 0x8c1054))((char*)params + 200);
    ((dtor_matrix_array_fn)(g_unity_base + 0x8c0ec8))((char*)params + 40);
    ((dtor_vector_array_fn)(g_unity_base + 0x8c0e30))((char*)params + 0);
    ((dtor_sampler_array_fn)(g_unity_base + 0x8c0f74))((char*)params + 280);
    ((dtor_texture_array_fn)(g_unity_base + 0x8c119c))((char*)params + 80);
    ((dtor_buffer_array_fn)(g_unity_base + 0x8c0fbc))((char*)params + 160);
    ((dtor_uav_array_fn)(g_unity_base + 0x8c1104))((char*)params + 120);
    ((dtor_buffer_array_fn)(g_unity_base + 0x8c0fbc))((char*)params + 240);
}

static void destruct_subprogram(void* subprogram) {
    if (!g_unity_base) return;
    
    // Offset destructors
    ((dtor_uint8_array_fn)(g_unity_base + 0x0cfcec))((char*)subprogram + 32);
    ((dtor_channels_array_fn)(g_unity_base + 0x8c1238))((char*)subprogram + 80);
    ((dtor_keyword_state_fn)(g_unity_base + 0x916308))((char*)subprogram + 168);
    
    char* params = (char*)subprogram + 216;
    destruct_params(params);
}

// --- Comparison Engine ---

static bool match_matrix_param(const UnityDynamicArray* arr, const char* name, uint32_t index, uint32_t type, uint32_t size, uint32_t rows, uint32_t cols) {
    const UnityMatrixParameter* data = (const UnityMatrixParameter*)arr->m_data;
    for (size_t i = 0; i < arr->m_size; i++) {
        const char* p_name = get_unity_string(&data[i].name);
        if (strcmp(p_name, name) == 0) {
            if (data[i].index == index && data[i].type == type && data[i].size == size && data[i].rows == rows) {
                return true;
            }
            LOG_ERROR("Matrix param '%s' match failed: Ours={idx:%u,type:%u,sz:%u,r:%u,c:%u}, Theirs={idx:%u,type:%u,sz:%u,r:%u}",
                      name, index, type, size, rows, cols,
                      data[i].index, data[i].type, data[i].size, data[i].rows);
            return false;
        }
    }
    LOG_ERROR("Matrix param '%s' not found in Theirs!", name);
    return false;
}

static bool match_vector_param(const UnityDynamicArray* arr, const char* name, uint32_t index, uint32_t type, uint32_t size, uint32_t dim) {
    const UnityVectorParameter* data = (const UnityVectorParameter*)arr->m_data;
    for (size_t i = 0; i < arr->m_size; i++) {
        const char* p_name = get_unity_string(&data[i].name);
        if (strcmp(p_name, name) == 0) {
            if (data[i].index == index && data[i].type == type && data[i].size == size && data[i].dim == dim) {
                return true;
            }
            LOG_ERROR("Vector param '%s' match failed: Ours={idx:%u,type:%u,sz:%u,d:%u}, Theirs={idx:%u,type:%u,sz:%u,d:%u}",
                      name, index, type, size, dim,
                      data[i].index, data[i].type, data[i].size, data[i].dim);
            return false;
        }
    }
    LOG_ERROR("Vector param '%s' not found in Theirs!", name);
    return false;
}

static bool match_struct_param(const UnityDynamicArray* arr, const char* name, uint32_t index, uint32_t size, uint32_t member_count) {
    const UnityStructParameter* data = (const UnityStructParameter*)arr->m_data;
    for (size_t i = 0; i < arr->m_size; i++) {
        const char* p_name = get_unity_string(&data[i].name);
        if (strcmp(p_name, name) == 0) {
            if (data[i].index == index && data[i].size == size && data[i].member_count == member_count) {
                return true;
            }
            LOG_ERROR("Struct param '%s' match failed: Ours={idx:%u,sz:%u,m_cnt:%u}, Theirs={idx:%u,sz:%u,m_cnt:%u}",
                      name, index, size, member_count,
                      data[i].index, data[i].size, data[i].member_count);
            return false;
        }
    }
    LOG_ERROR("Struct param '%s' not found in Theirs!", name);
    return false;
}

static bool compare_cb_variables(const SerializedConstantBuffer* ours_cb, const UnityDynamicArray* their_matrices, const UnityDynamicArray* their_vectors, const UnityDynamicArray* their_structs) {
    // 1. Compare flat variables
    for (int j = 0; j < ours_cb->var_count; j++) {
        const SerializedVariable* var = &ours_cb->variables[j];
        uint32_t type = var->layout[0];
        uint32_t rows = var->layout[1];  // layout[1] is rows in C++ MatrixParameter
        uint32_t dim = var->layout[2];   // layout[2] is dim in C++ VectorParameter
        uint32_t is_matrix = var->layout[3]; // layout[3] is non-zero flag for matrix
        uint32_t index = var->layout[5]; // Swapped index -> layout[5]
        uint32_t size = var->layout[4];  // Swapped size -> layout[4]
        
        if (is_matrix != 0) {
            if (!match_matrix_param(their_matrices, var->name, index, type, size, rows, 0)) {
                return false;
            }
        } else {
            if (!match_vector_param(their_vectors, var->name, index, type, size, dim)) {
                return false;
            }
        }
    }
    
    // 2. Compare nested struct parameter variables and struct info
    for (int sc = 0; sc < ours_cb->struct_count; sc++) {
        const SerializedStructParam* sp = &ours_cb->struct_params[sc];
        uint32_t index = sp->layout[0];
        uint32_t size = sp->layout[1];
        uint32_t member_count = sp->layout[2];
        
        if (!match_struct_param(their_structs, sp->name, index, size, member_count)) {
            return false;
        }
        
        // Find the matching StructParameter in their_structs to search its member arrays
        const UnityStructParameter* struct_data = (const UnityStructParameter*)their_structs->m_data;
        const UnityStructParameter* target_struct = NULL;
        for (size_t i = 0; i < their_structs->m_size; i++) {
            if (strcmp(get_unity_string(&struct_data[i].name), sp->name) == 0) {
                target_struct = &struct_data[i];
                break;
            }
        }
        if (!target_struct) {
            LOG_ERROR("Struct parameter '%s' not found in Theirs for member lookup", sp->name);
            return false;
        }
        
        // Members of the struct are flattened in C++ and named sp->name + "." + member->name
        for (int m = 0; m < sp->member_count; m++) {
            const SerializedVariable* mbr = &sp->members[m];
            char full_name[256];
            snprintf(full_name, sizeof(full_name), "%s.%s", sp->name, mbr->name);
            
            uint32_t type = mbr->layout[0];
            uint32_t rows = mbr->layout[1];
            uint32_t dim = mbr->layout[2];
            uint32_t is_matrix = mbr->layout[3];
            uint32_t m_index = mbr->layout[5]; // Swapped index -> layout[5]
            uint32_t m_size = mbr->layout[4];  // Swapped size -> layout[4]
            
            if (is_matrix != 0) {
                if (!match_matrix_param(&target_struct->matrix_params, full_name, m_index, type, m_size, rows, 0)) {
                    return false;
                }
            } else {
                if (!match_vector_param(&target_struct->vector_params, full_name, m_index, type, m_size, dim)) {
                    return false;
                }
            }
        }
    }

    
    return true;
}

static bool compare_params(const SerializedProgramParameters* ours, const void* theirs_raw) {
    const char* theirs = (const char*)theirs_raw;
    
    // A. Constant Buffers Count Check
    const UnityDynamicArray* their_cbs = (const UnityDynamicArray*)(theirs + 200);
    const UnityConstantBuffer* cb_data = (const UnityConstantBuffer*)their_cbs->m_data;
    int expected_their_cbs = (ours->cb_count > 0) ? (ours->cb_count - 1) : 0;
    if ((size_t)expected_their_cbs != their_cbs->m_size) {
        LOG_ERROR("Constant buffers count mismatch: Ours (total)=%d, Expected Theirs=%d, Actual Theirs=%zu", 
                  ours->cb_count, expected_their_cbs, their_cbs->m_size);
        return false;
    }
    
    // B. Check cb 0 (which is `$Globals` or empty)
    if (ours->cb_count > 0) {
        const SerializedConstantBuffer* cb0 = &ours->constant_buffers[0];
        const UnityDynamicArray* outer_vectors = (const UnityDynamicArray*)(theirs + 0);    // Offset 0
        const UnityDynamicArray* outer_matrices = (const UnityDynamicArray*)(theirs + 40);  // Offset 40
        UnityDynamicArray empty_structs = {0};
        
        if (!compare_cb_variables(cb0, outer_matrices, outer_vectors, &empty_structs)) {
            LOG_ERROR("CB 0 variables comparison failed!");
            return false;
        }
    }
    
    // C. Check cb i (where i > 0)
    for (int i = 1; i < ours->cb_count; i++) {
        const SerializedConstantBuffer* ours_cb = &ours->constant_buffers[i];
        const UnityConstantBuffer* theirs_cb = &cb_data[i - 1];
        
        const char* cb_name = get_unity_string(&theirs_cb->name);
        if (strcmp(ours_cb->name, cb_name) != 0) {
            LOG_ERROR("CB %d name mismatch: Ours=\"%s\", Theirs=\"%s\"", i, ours_cb->name, cb_name);
            return false;
        }
        
        if (!compare_cb_variables(ours_cb, &theirs_cb->matrix_params, &theirs_cb->vector_params, &theirs_cb->struct_params)) {
            LOG_ERROR("CB %d variables comparison failed!", i);
            return false;
        }
    }
    
    // D. Textures
    const UnityDynamicArray* their_textures = (const UnityDynamicArray*)(theirs + 80); // Offset 80
    int expected_tex_count = 0;
    for (int i = 0; i < ours->res_count; i++) {
        if (ours->resources[i].bind_type == SERIALIZED_RESOURCE_TEXTURE) expected_tex_count++;
    }
    if ((size_t)expected_tex_count != their_textures->m_size) {
        LOG_ERROR("Texture count mismatch: Ours=%d, Theirs=%zu", expected_tex_count, their_textures->m_size);
        return false;
    }
    const UnityTextureParameter* tex_data = (const UnityTextureParameter*)their_textures->m_data;
    int tex_idx = 0;
    for (int i = 0; i < ours->res_count; i++) {
        if (ours->resources[i].bind_type == SERIALIZED_RESOURCE_TEXTURE) {
            const char* tex_name = get_unity_string(&tex_data[tex_idx].name);
            if (strcmp(ours->resources[i].name, tex_name) != 0) {
                LOG_ERROR("Texture %d name mismatch: Ours=\"%s\", Theirs=\"%s\"", tex_idx, ours->resources[i].name, tex_name);
                return false;
            }
            if (ours->resources[i].bind_index != tex_data[tex_idx].index) {
                LOG_ERROR("Texture %d bind index mismatch: Ours=%u, Theirs=%u", tex_idx, ours->resources[i].bind_index, tex_data[tex_idx].index);
                return false;
            }
            tex_idx++;
        }
    }
    
    // E. Samplers
    const UnityDynamicArray* their_samplers = (const UnityDynamicArray*)(theirs + 280); // Offset 280
    int expected_sampler_count = 0;
    for (int i = 0; i < ours->res_count; i++) {
        if (ours->resources[i].bind_type == SERIALIZED_RESOURCE_SAMPLER) expected_sampler_count++;
    }
    if ((size_t)expected_sampler_count != their_samplers->m_size) {
        LOG_ERROR("Sampler count mismatch: Ours=%d, Theirs=%zu", expected_sampler_count, their_samplers->m_size);
        return false;
    }
    const UnitySamplerParameter* sampler_data = (const UnitySamplerParameter*)their_samplers->m_data;
    int sampler_idx = 0;
    for (int i = 0; i < ours->res_count; i++) {
        if (ours->resources[i].bind_type == SERIALIZED_RESOURCE_SAMPLER) {
            if (ours->resources[i].bind_index != sampler_data[sampler_idx].index) {
                LOG_ERROR("Sampler %d bind index mismatch: Ours=%u, Theirs=%u", sampler_idx, ours->resources[i].bind_index, sampler_data[sampler_idx].index);
                return false;
            }
            sampler_idx++;
        }
    }
    
    // F. UAVs
    const UnityDynamicArray* their_uavs = (const UnityDynamicArray*)(theirs + 120); // Offset 120
    int expected_uav_count = 0;
    for (int i = 0; i < ours->res_count; i++) {
        if (ours->resources[i].bind_type == SERIALIZED_RESOURCE_UAV) expected_uav_count++;
    }
    if ((size_t)expected_uav_count != their_uavs->m_size) {
        LOG_ERROR("UAV count mismatch: Ours=%d, Theirs=%zu", expected_uav_count, their_uavs->m_size);
        return false;
    }
    const UnityUAVParameter* uav_data = (const UnityUAVParameter*)their_uavs->m_data;
    int uav_idx = 0;
    for (int i = 0; i < ours->res_count; i++) {
        if (ours->resources[i].bind_type == SERIALIZED_RESOURCE_UAV) {
            const char* uav_name = get_unity_string(&uav_data[uav_idx].name);
            if (strcmp(ours->resources[i].name, uav_name) != 0) {
                LOG_ERROR("UAV %d name mismatch: Ours=\"%s\", Theirs=\"%s\"", uav_idx, ours->resources[i].name, uav_name);
                return false;
            }
            if (ours->resources[i].bind_index != uav_data[uav_idx].index) {
                LOG_ERROR("UAV %d bind index mismatch: Ours=%u, Theirs=%u", uav_idx, ours->resources[i].bind_index, uav_data[uav_idx].index);
                return false;
            }
            uav_idx++;
        }
    }
    
    // G. Buffers
    const UnityDynamicArray* their_buffers = (const UnityDynamicArray*)(theirs + 160); // Offset 160
    int expected_buffer_count = 0;
    for (int i = 0; i < ours->res_count; i++) {
        if (ours->resources[i].bind_type == SERIALIZED_RESOURCE_BUFFER) expected_buffer_count++;
    }
    if ((size_t)expected_buffer_count != their_buffers->m_size) {
        LOG_ERROR("Buffer count mismatch: Ours=%d, Theirs=%zu", expected_buffer_count, their_buffers->m_size);
        return false;
    }
    const UnityBufferBinding* buffer_data = (const UnityBufferBinding*)their_buffers->m_data;
    int buffer_idx = 0;
    for (int i = 0; i < ours->res_count; i++) {
        if (ours->resources[i].bind_type == SERIALIZED_RESOURCE_BUFFER) {
            const char* buffer_name = get_unity_string(&buffer_data[buffer_idx].name);
            if (strcmp(ours->resources[i].name, buffer_name) != 0) {
                LOG_ERROR("Buffer %d name mismatch: Ours=\"%s\", Theirs=\"%s\"", buffer_idx, ours->resources[i].name, buffer_name);
                return false;
            }
            if (ours->resources[i].bind_index != buffer_data[buffer_idx].index) {
                LOG_ERROR("Buffer %d bind index mismatch: Ours=%u, Theirs=%u", buffer_idx, ours->resources[i].bind_index, buffer_data[buffer_idx].index);
                return false;
            }
            buffer_idx++;
        }
    }
    
    return true;
}

/* Compare the independently parsed player-blob wrapper with the runtime
 * SerializedSubProgram populated by UnityPlayer.  Do not use the TypeTree
 * SerializedSubProgram here: blob_index, hardware tier, and requirements are
 * serialized-object authority and are intentionally a separate domain. */
static bool compare_player_subprogram(const PlayerSubProgramMetadata* ours,
                                      const void* theirs_raw) {
    const char* theirs = (const char*)theirs_raw;
    
    // 1. Program Type
    int32_t their_program_type = *(const int32_t*)(theirs + 16);
    if (ours->program_type != their_program_type) {
        LOG_ERROR("Program type mismatch: Ours=%d, Theirs=%d", ours->program_type, their_program_type);
        return false;
    }
    
    // 2. SerializedBindChannels source map
    uint32_t their_source_map = *(const uint32_t*)(theirs + 120);
    if (ours->source_map != their_source_map) {
        LOG_ERROR("Source map mismatch: Ours=0x%x, Theirs=0x%x",
                  ours->source_map, their_source_map);
        return false;
    }
    
    // 3. Bytecode
    const UnityDynamicArray* their_bytecode = (const UnityDynamicArray*)(theirs + 32);
    if ((size_t)ours->bytecode_length != their_bytecode->m_size) {
        LOG_ERROR("Bytecode length mismatch: Ours=%u, Theirs=%zu", ours->bytecode_length, their_bytecode->m_size);
        return false;
    }
    if (ours->bytecode_length > 0) {
        if (memcmp(ours->bytecode, their_bytecode->m_data, ours->bytecode_length) != 0) {
            LOG_ERROR("Bytecode content mismatch!");
            return false;
        }
    }
    
    // 4. Vertex Bindings / Channels
    const UnityDynamicArray* their_channels = (const UnityDynamicArray*)(theirs + 80);
    if ((size_t)ours->binding_count != their_channels->m_size) {
        LOG_ERROR("Binding count mismatch: Ours=%d, Theirs=%zu", ours->binding_count, their_channels->m_size);
        return false;
    }
    const UnitySerializedBindChannel* channel_data = (const UnitySerializedBindChannel*)their_channels->m_data;
    for (int i = 0; i < ours->binding_count; i++) {
        if (ours->bindings[i].channel != channel_data[i].channel || ours->bindings[i].component != channel_data[i].component) {
            LOG_ERROR("Binding %d mismatch: Ours={ch:%u,comp:%u}, Theirs={ch:%u,comp:%u}", 
                      i, ours->bindings[i].channel, ours->bindings[i].component, 
                      channel_data[i].channel, channel_data[i].component);
            return false;
        }
    }
    
    return true;
}

// --- Main Verification Flow ---

static int g_subprogram_checked = 0;
static int g_subprogram_passed = 0;
static int g_params_checked = 0;
static int g_params_passed = 0;

static void verify_shader_roundtrip(
    const SerializedShader* shader, 
    const BlobEntry* blob_entries, 
    int entry_count, 
    uint8_t** segments, 
    int segment_count, 
    void* local_space, 
    LoadVariantFromData_fn load_variant, 
    LoadParametersFromData_fn load_params
) {
    if (shader->subshader_count == 0) return;
    
    for (int subshader_idx = 0; subshader_idx < shader->subshader_count; subshader_idx++) {
        const SerializedSubShader* subshader = &shader->subshaders[subshader_idx];
        for (int pass_idx = 0; pass_idx < subshader->pass_count; pass_idx++) {
            const SerializedPass* pass = &subshader->passes[pass_idx];
            
            for (int stage_idx = 0; stage_idx < 6; stage_idx++) {
                for (int sub_idx = 0; sub_idx < pass->subprogram_count[stage_idx]; sub_idx++) {
                    if (!serialized_pass_subprogram_is_platform(
                            pass, stage_idx, sub_idx, 4)) {
                        continue;
                    }
                    const SerializedSubProgram* ours_sub = &pass->subprograms[stage_idx][sub_idx];
                    
                    int32_t blob_idx = ours_sub->blob_index;
                    if (blob_idx < 0 || blob_idx >= entry_count) continue;
                    
                    BlobEntry entry = blob_entries[blob_idx];
                    if (entry.segment < 0 || entry.segment >= segment_count || !segments[entry.segment]) continue;
                    
                    const uint8_t* payload = segments[entry.segment] + entry.offset;
                    size_t payload_len = entry.length;
                    
                    // A. Parse Subprogram Variant Metadata
                    ByteStream stream;
                    stream_init(&stream, payload, payload_len);
                    stream_set_endian(&stream, false);
                    
                    PlayerSubProgramMetadata ours;
                    bool parse_ok = subprogram_metadata_parse_variant(&stream, &ours);
                    
                    alignas(16) char theirs[4096];
                    memset(theirs, 0, sizeof(theirs));
                    
                    const uint8_t* stream_ptr = payload;
                    const uint8_t* stream_end = payload + payload_len;
                    
                    // Read version (first 4 bytes)
                    uint32_t ver = 0;
                    memcpy(&ver, stream_ptr, 4);
                    stream_ptr += 4;
                    
                    uint32_t version_out = 0;
                    bool theirs_ok = load_variant(&stream_ptr, stream_end, local_space, ver, theirs, &version_out);
                    
                    g_subprogram_checked++;
                    
                    if (parse_ok && theirs_ok) {
                        if (compare_player_subprogram(&ours, theirs)) {
                            g_subprogram_passed++;
                        }
                    } else if (!parse_ok && !theirs_ok) {
                        g_subprogram_passed++;
                    } else {
                        LOG_ERROR("Subprogram variant success mismatch: Ours=%d, Theirs=%d", parse_ok, theirs_ok);
                    }
                    
                    if (parse_ok) subprogram_metadata_free_variant(&ours);
                    if (theirs_ok) destruct_subprogram(theirs);
                    
                    // B. Parse Subprogram Parameters Blob (if it exists)
                    int param_blob_idx = pass->subprogram_param_blob_indices[stage_idx][sub_idx];
                    if (param_blob_idx >= 0 && param_blob_idx < entry_count) {
                        BlobEntry p_entry = blob_entries[param_blob_idx];
                        if (p_entry.segment >= 0 && p_entry.segment < segment_count && segments[p_entry.segment]) {
                            const uint8_t* p_payload = segments[p_entry.segment] + p_entry.offset;
                            size_t p_len = p_entry.length;
                            
                            ByteStream p_stream;
                            stream_init(&p_stream, p_payload, p_len);
                            stream_set_endian(&p_stream, false);
                            
                            SerializedProgramParameters ours_params;
                            serialized_program_parameters_init(&ours_params);
                            bool p_parse_ok = subprogram_metadata_parse_parameters(&p_stream, &ours_params);
                            
                            const uint8_t* p_stream_ptr = p_payload;
                            const uint8_t* p_stream_end = p_payload + p_len;
                            
                            // Read parameter version (first 4 bytes)
                            uint32_t p_ver = 0;
                            memcpy(&p_ver, p_stream_ptr, 4);
                            p_stream_ptr += 4;
                            
                            alignas(16) char theirs_params[4096];
                            memset(theirs_params, 0, sizeof(theirs_params));
                            
                            bool theirs_params_ok = load_params(&p_stream_ptr, p_stream_end, p_ver, theirs_params);
                            
                            g_params_checked++;
                            
                            if (p_parse_ok && theirs_params_ok) {
                                if (compare_params(&ours_params, theirs_params)) {
                                    g_params_passed++;
                                }
                            } else if (!p_parse_ok && !theirs_params_ok) {
                                g_params_passed++;
                            } else {
                                LOG_ERROR("Parameters success mismatch: Ours=%d, Theirs=%d", p_parse_ok, theirs_params_ok);
                            }
                            
                            if (theirs_params_ok) destruct_params(theirs_params);
                            serialized_program_parameters_free(&ours_params);
                        }
                    }
                }
            }
        }
    }
}

static void process_shader_object(SerializedFile* file, const AssetObjectInfo* obj, void* local_space, LoadVariantFromData_fn load_variant, LoadParametersFromData_fn load_params) {
    size_t shader_size = 0;
    const uint8_t* shader_data = serialized_file_get_object_data(file, obj, &shader_size);
    if (!shader_data || shader_size == 0) return;
    
    TypeTreeValue shader_value;
    int node_idx = 0;
    ByteStream stream;
    stream_init(&stream, shader_data, shader_size);
    stream_set_endian(&stream, file->big_endian);
    if (!typetree_parse_value_ex(
            &file->types[obj->type_id_or_index], &node_idx, &stream,
            &shader_value, TYPETREE_PARSE_PACK_COMPRESSED_BLOB)) return;
    
    if (stream.position != shader_size ||
        node_idx != file->types[obj->type_id_or_index].node_count) {
        typetree_free_value(&shader_value);
        return;
    }
    SerializedShaderSchemaProfile shader_profile;
    if (!serialized_shader_profile_from_unity_version(
            file->unity_version, &shader_profile)) {
        typetree_free_value(&shader_value);
        return;
    }
    SerializedShader shader;
    serialized_shader_init(&shader);
    if (!serialized_shader_parse_with_profile(
            &shader, &shader_value, shader_profile)) {
        typetree_free_value(&shader_value);
        return;
    }
    
    const TypeTreeValue* parsed_form = typetree_find_path(&shader_value, "m_ParsedForm");
    if (!parsed_form) parsed_form = &shader_value;
    
    const TypeTreeValue* platforms_val = find_field(&shader_value, parsed_form, "platforms");
    const TypeTreeValue* offsets_val = find_field(&shader_value, parsed_form, "offsets");
    const TypeTreeValue* comp_lens_val = find_field(&shader_value, parsed_form, "compressedLengths");
    const TypeTreeValue* decomp_lens_val = find_field(&shader_value, parsed_form, "decompressedLengths");
    const TypeTreeValue* blob_val = find_field(&shader_value, parsed_form, "compressedBlob");
    
    if (!platforms_val || !offsets_val || !comp_lens_val || !decomp_lens_val || !blob_val) {
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        return;
    }
    
    int platform_idx = -1;
    const TypeTreeValue* arr_platforms = typetree_get_array(platforms_val);
    if (arr_platforms) {
        for (int i = 0; i < arr_platforms->array_val.count; i++) {
            if (arr_platforms->array_val.elements[i].int_val == 4) { // 4 = d3d11
                platform_idx = i;
                break;
            }
        }
    }
    if (platform_idx == -1) platform_idx = 0;
    
    const TypeTreeValue* arr_offsets_outer = typetree_get_array(offsets_val);
    const TypeTreeValue* arr_comp_lens_outer = typetree_get_array(comp_lens_val);
    const TypeTreeValue* arr_decomp_lens_outer = typetree_get_array(decomp_lens_val);
    
    if (!arr_offsets_outer || platform_idx >= arr_offsets_outer->array_val.count ||
        !arr_comp_lens_outer || platform_idx >= arr_comp_lens_outer->array_val.count ||
        !arr_decomp_lens_outer || platform_idx >= arr_decomp_lens_outer->array_val.count) {
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        return;
    }
    
    const TypeTreeValue* platform_offsets = &arr_offsets_outer->array_val.elements[platform_idx];
    const TypeTreeValue* platform_comp_lens = &arr_comp_lens_outer->array_val.elements[platform_idx];
    const TypeTreeValue* platform_decomp_lens = &arr_decomp_lens_outer->array_val.elements[platform_idx];
    
    const TypeTreeValue* arr_offsets = typetree_get_array(platform_offsets);
    const TypeTreeValue* arr_comp_lens = typetree_get_array(platform_comp_lens);
    const TypeTreeValue* arr_decomp_lens = typetree_get_array(platform_decomp_lens);
    if (!arr_offsets || !arr_comp_lens || !arr_decomp_lens) {
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        return;
    }
    
    const TypeTreeValue* flat_blob = typetree_get_array(blob_val);
    if (!flat_blob) {
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        return;
    }
    
    int blob_len = flat_blob->array_val.count;
    uint8_t* compressed_data_flat = (uint8_t*)malloc(blob_len);
    if (!compressed_data_flat || !typetree_array_copy_bytes(
            flat_blob, 0, compressed_data_flat, (size_t)blob_len)) {
        free(compressed_data_flat);
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        return;
    }
    
    int segment_count = arr_offsets->array_val.count;
    uint8_t** segments = (uint8_t**)malloc(segment_count * sizeof(uint8_t*));
    int* segment_lens = (int*)malloc(segment_count * sizeof(int));
    memset(segments, 0, segment_count * sizeof(uint8_t*));
    
    bool decompress_ok = true;
    for (int i = 0; i < segment_count; i++) {
        uint32_t offset = (uint32_t)arr_offsets->array_val.elements[i].int_val;
        uint32_t comp_len = (uint32_t)arr_comp_lens->array_val.elements[i].int_val;
        uint32_t decomp_len = (uint32_t)arr_decomp_lens->array_val.elements[i].int_val;
        segments[i] = (uint8_t*)malloc(decomp_len);
        segment_lens[i] = decomp_len;
        int dec_res = lz4_decompress_safe(compressed_data_flat + offset, segments[i], comp_len, decomp_len);
        if (dec_res < 0) { decompress_ok = false; break; }
    }
    
    if (decompress_ok && segment_count >= 1 && segment_lens[0] >= 4) {
        int32_t entry_count;
        memcpy(&entry_count, segments[0], 4);
        entry_count = read_le32(entry_count);
        BlobEntry* blob_entries = (BlobEntry*)malloc(entry_count * sizeof(BlobEntry));
        int entry_offset = 4;
        for (int k = 0; k < entry_count && entry_offset + 12 <= segment_lens[0]; k++) {
            int32_t off = 0, len = 0, seg = 0;
            memcpy(&off, segments[0] + entry_offset, 4);
            memcpy(&len, segments[0] + entry_offset + 4, 4);
            memcpy(&seg, segments[0] + entry_offset + 8, 4);
            blob_entries[k].offset = read_le32(off);
            blob_entries[k].length = read_le32(len);
            blob_entries[k].segment = read_le32(seg);
            entry_offset += 12;
        }
        
        verify_shader_roundtrip(&shader, blob_entries, entry_count, segments, segment_count, local_space, load_variant, load_params);
        
        free(blob_entries);
    }
    
    for (int i = 0; i < segment_count; i++) {
        if (segments[i]) free(segments[i]);
    }
    free(segments);
    free(segment_lens);
    free(compressed_data_flat);
    serialized_shader_free(&shader);
    typetree_free_value(&shader_value);
}

static void* try_load_unity_player(const char* path) {
    return dlopen(path, RTLD_NOW);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2 || argc > 3) {
        printf("Usage: %s <path_to_bundle> [pinned_UnityPlayer.dylib]\n",
               argv[0]);
        printf("The optional path can also be set with "
               "DXBC_UNITY_PLAYER_PATH.\n");
        return 1;
    }

    const char* unity_player_path = argc == 3 ? argv[2] : NULL;
    if (!unity_player_path || unity_player_path[0] == '\0') {
        unity_player_path = getenv("DXBC_UNITY_PLAYER_PATH");
    }
    if (!unity_player_path || unity_player_path[0] == '\0') {
        unity_player_path = DXBCSANDBOX_PINNED_UNITY_PLAYER_PATH;
    }

    uint8_t unity_player_digest[COMMON_SHA256_DIGEST_SIZE];
    if (!hash_file_sha256(unity_player_path, unity_player_digest)) {
        printf("[FAIL] Failed to hash UnityPlayer.dylib: %s\n",
               unity_player_path);
        return 1;
    }
    if (memcmp(unity_player_digest, k_pinned_unity_player_sha256,
               sizeof(unity_player_digest)) != 0) {
        char actual_sha256[COMMON_SHA256_DIGEST_SIZE * 2 + 1];
        format_sha256(unity_player_digest, actual_sha256);
        printf("[FAIL] Refusing version-specific ABI offsets for unpinned "
               "UnityPlayer.dylib: %s\n", unity_player_path);
        printf("[FAIL] Actual SHA-256: %s\n", actual_sha256);
        return 1;
    }
    printf("[INFO] UnityPlayer ABI authority SHA-256 is pinned: %s\n",
           unity_player_path);
    
    // 1. Load UnityPlayer.dylib
    void* handle = try_load_unity_player(unity_player_path);
    if (!handle) {
        printf("[FAIL] Failed to dlopen UnityPlayer.dylib from %s: %s\n",
               unity_player_path, dlerror());
        return 1;
    }
    
    g_unity_base = get_unity_player_base();
    if (!g_unity_base) {
        printf("[FAIL] Failed to locate base address of UnityPlayer.dylib in memory!\n");
        dlclose(handle);
        return 1;
    }
    printf("[INFO] Loaded UnityPlayer.dylib at base address: 0x%lx\n", g_unity_base);
    
    // 2. Instantiate keywords::LocalSpace
    alignas(16) char local_space_buf[1024];
    memset(local_space_buf, 0, sizeof(local_space_buf));
    
    LocalSpace_ctor_fn local_space_ctor = (LocalSpace_ctor_fn)(g_unity_base + 0x9175bc);
    LocalSpace_dtor_fn local_space_dtor = (LocalSpace_dtor_fn)(g_unity_base + 0x8eeb30);
    const void* p_kMemTempAlloc = (const void*)(g_unity_base + 0x24dac88);
    
    local_space_ctor(local_space_buf, p_kMemTempAlloc);
    printf("[INFO] Constructed keywords::LocalSpace successfully.\n");
    
    // 3. Resolve function pointers
    LoadVariantFromData_fn load_variant = (LoadVariantFromData_fn)(g_unity_base + 0x94b118);
    LoadParametersFromData_fn load_params = (LoadParametersFromData_fn)(g_unity_base + 0x94b758);
    
    // 4. Load bundle archive
    size_t file_size = 0;
    uint8_t* file_data = read_file_to_buffer(argv[1], &file_size);
    if (!file_data) {
        printf("[FAIL] Failed to read bundle: %s\n", argv[1]);
        local_space_dtor(local_space_buf);
        dlclose(handle);
        return 1;
    }
    
    BundleArchive archive;
    if (!bundle_open(&archive, file_data, file_size)) {
        printf("[FAIL] Failed to open BundleArchive\n");
        free(file_data);
        local_space_dtor(local_space_buf);
        dlclose(handle);
        return 1;
    }
    
    // 5. Traverse and verify
    for (int i = 0; i < archive.directory_count; i++) {
        size_t sub_file_size = 0;
        const uint8_t* sub_file_data = bundle_get_file(&archive, archive.directories[i].name, &sub_file_size);
        if (sub_file_data && sub_file_size > 0) {
            SerializedFile file;
            if (serialized_file_open(&file, sub_file_data, sub_file_size)) {
                for (int obj_idx = 0; obj_idx < file.object_count; obj_idx++) {
                    const AssetObjectInfo* obj = &file.objects[obj_idx];
                    if (obj->type_id == 48) { // 48 = Shader
                        process_shader_object(&file, obj, local_space_buf, load_variant, load_params);
                    }
                }
                serialized_file_close(&file);
            }
        }
    }
    
    // Cleanup
    bundle_close(&archive);
    free(file_data);
    local_space_dtor(local_space_buf);
    dlclose(handle);
    
    printf("\n=== Roundtrip Verification Summary ===\n");
    printf("Total subprograms checked: %d\n", g_subprogram_checked);
    printf("Total subprograms passed:  %d\n", g_subprogram_passed);
    printf("Total parameter blobs checked: %d\n", g_params_checked);
    printf("Total parameter blobs passed:  %d\n", g_params_passed);
    
    if (g_subprogram_passed == g_subprogram_checked && g_subprogram_checked > 0 &&
        g_params_passed == g_params_checked && g_params_checked > 0) {
        printf("[SUCCESS] All subprogram and parameter metadata fields matched 1:1 perfectly with the macOS dylib!\n");
        return 0;
    } else {
        printf("[FAIL] Roundtrip verification mismatches or no tests run!\n");
        return 1;
    }
}
