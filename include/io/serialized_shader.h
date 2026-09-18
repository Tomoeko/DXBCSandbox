// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_SHADER_H
#define SERIALIZED_SHADER_H

#include "common/common.h"
#include "io/typetree.h"
#include "io/subprogram_metadata.h"

typedef struct {
    const char* name;
    const char* description;
    int type;
    uint32_t flags;
    float def_value[4];
    const char* def_texture_name;
    int def_texture_dim;
    int attribute_count;
    char** attributes;
} ParsedShaderProperty;

typedef struct {
    float val;
    const char* name;
    /* Distinguishes an absent TypeTree field from an explicitly serialized
     * numeric zero.  Render-state zero is meaningful for several commands. */
    bool present;
} SerializedShaderFloatValue;

typedef struct {
    SerializedShaderFloatValue srcBlend;
    SerializedShaderFloatValue destBlend;
    SerializedShaderFloatValue srcBlendAlpha;
    SerializedShaderFloatValue destBlendAlpha;
    SerializedShaderFloatValue blendOp;
    SerializedShaderFloatValue blendOpAlpha;
    SerializedShaderFloatValue colMask;
} SerializedShaderRTBlendState;

typedef struct {
    SerializedShaderFloatValue pass;
    SerializedShaderFloatValue fail;
    SerializedShaderFloatValue zFail;
    SerializedShaderFloatValue comp;
} SerializedStencilOp;

typedef struct {
    const char* name;
    SerializedShaderRTBlendState rtBlend[8];
    bool rtSeparateBlend;
    SerializedShaderFloatValue zClip;
    SerializedShaderFloatValue zTest;
    SerializedShaderFloatValue zWrite;
    SerializedShaderFloatValue culling;
    SerializedShaderFloatValue conservative;
    SerializedShaderFloatValue offsetFactor;
    SerializedShaderFloatValue offsetUnits;
    SerializedShaderFloatValue alphaToMask;
    SerializedStencilOp stencilOp;
    SerializedStencilOp stencilOpFront;
    SerializedStencilOp stencilOpBack;
    SerializedShaderFloatValue stencilReadMask;
    SerializedShaderFloatValue stencilWriteMask;
    SerializedShaderFloatValue stencilRef;
    SerializedShaderFloatValue fogStart;
    SerializedShaderFloatValue fogEnd;
    SerializedShaderFloatValue fogDensity;
    struct {
        SerializedShaderFloatValue x, y, z, w;
        const char* name;
    } fogColor;
    int fogMode;
    int gpuProgramID;
    int lod;
    bool lighting;
} SerializedShaderState;

typedef struct {
    const char* key;
    const char* value;
} SerializedTag;

typedef struct {
    int tag_count;
    SerializedTag* tags;
} SerializedTagMap;

typedef struct {
    const char* name;
    int index;
} SerializedNameTableEntry;

typedef struct {
    int count;
    SerializedNameTableEntry* entries;
} SerializedNameTable;

typedef struct {
    int count;
    char** keywords;
} SerializedKeywordList;

/*
 * m_PlayerSubPrograms is grouped by hardware tier.  Slots 0..2 are
 * tier-specific and slot 3 is the generic group selected independently of
 * the graphics tier.  Compiler platforms come from SerializedPass.m_Platforms
 * and must never be inferred from this group index.
 */
typedef struct {
    int hardware_tier_group;
    int inner_subprogram_index;

    bool keyword_scopes_are_explicit;
    int global_keyword_index_count;
    int* global_keyword_indices;
    int local_keyword_index_count;
    int* local_keyword_indices;
} SerializedSubProgramIdentity;

typedef struct {
    int version;
    bool has_serialized_platforms;
    int platform_count;
    int* platforms;
    uint32_t program_mask;
    SerializedNameTable name_table;
    int pass_type; // m_Type
    SerializedShaderState state;
    SerializedProgramParameters common_parameters[6]; // vertex=0, fragment=1, geometry=2, hull=3, domain=4, raytracing=5
    int subprogram_count[6];
    SerializedSubProgram* subprograms[6];
    SerializedSubProgramIdentity* subprogram_identities[6];
    int* subprogram_param_blob_indices[6];
    bool has_instancing_variant;
    bool has_procedural_instancing_variant;
    const char* use_name;
    const char* name;
    const char* texture_name;
    SerializedTagMap tags;
    int serialized_keyword_state_mask_count;
    uint16_t* serialized_keyword_state_mask;
} SerializedPass;

typedef struct {
    int pass_count;
    SerializedPass* passes;
    SerializedTagMap tags;
    int lod;
} SerializedSubShader;

typedef struct {
    const char* from;
    const char* to;
} SerializedShaderDependency;

typedef struct {
    const char* custom_editor_name;
    const char* render_pipeline_type;
} SerializedCustomEditorForRenderPipeline;

typedef struct {
    const char* name;
    const char* fallback_name;
    const char* custom_editor_name;
    SerializedStringPool owned_strings;
    int property_count;
    ParsedShaderProperty* properties;
    int subshader_count;
    SerializedSubShader* subshaders;
    SerializedKeywordList keyword_names;
    uint8_t* keyword_flags;
    int dependency_count;
    SerializedShaderDependency* dependencies;
    int custom_editor_for_render_pipeline_count;
    SerializedCustomEditorForRenderPipeline*
        custom_editors_for_render_pipelines;
    bool disable_no_subshaders_message;
    int archive_platform_count;
    int* archive_platforms;
} SerializedShader;

typedef enum {
    SERIALIZED_SHADER_PROFILE_UNITY_2021_3_35F1 = 1,
    /* Exact player-resource schema retained by Unity 2021.3.35f1 players.
     * This is not a generic 2021.3 patch-compatibility rule. */
    SERIALIZED_SHADER_PROFILE_UNITY_2021_3_29F1_PLAYER_RESOURCES = 2
} SerializedShaderSchemaProfile;

/* Maps only an exactly supported SerializedFile Unity version. Compatibility
 * across patch/layout revisions is never inferred from a shared major/minor. */
bool serialized_shader_profile_from_unity_version(
    const char* unity_version, SerializedShaderSchemaProfile* out_profile);

// Parses a complete Shader object from the TypeTree.
/* Projection-only compatibility wrapper pinned to Unity 2021.3.35f1.  It is
 * retained for synthetic unit trees and does not establish an authoritative
 * wire-shape contract. Bundle readers must use the exact profile entry point
 * below. The destination must be initialized. */
bool serialized_shader_parse(SerializedShader* shader, const TypeTreeValue* value);

/* Exact profile entry point. Unknown profiles and incomplete, reordered, or
 * wrong-shaped TypeTree values fail closed before projection. The destination
 * must first be initialized; parsing has a strong output guarantee. */
bool serialized_shader_parse_with_profile(
    SerializedShader* shader, const TypeTreeValue* value,
    SerializedShaderSchemaProfile profile);

/* Initializes an empty shader model suitable for serialized_shader_free(). */
void serialized_shader_init(SerializedShader* shader);

/* Projects a Unity 2021 SerializedProgramParameters TypeTree losslessly enough
 * to compare it with the corresponding player parameter blob. */
bool serialized_program_parameters_parse_typetree(
    SerializedProgramParameters* parameters, const TypeTreeValue* value,
    const SerializedNameTable* name_table);

/* Unity 2021.3 ShaderCompilerPlatform/m_GpuProgramType compatibility.
 * Unknown platform or program-type values fail closed. */
bool serialized_gpu_program_type_is_platform(int program_type, int platform);
/* Exact Unity 2021.3 ShaderCompilerPlatform domain retained by the parser.
 * Unknown integer values never establish exclusion authority. */
bool serialized_shader_platform_is_known(int platform);

/*
 * Tests both authorities required to select a flattened player subprogram:
 * the compiler platform must be explicitly listed by the pass and the
 * subprogram's m_GpuProgramType must belong to that platform. Hardware-tier
 * grouping is orthogonal. Synthetic passes with neither serialized platform
 * nor identity metadata remain platform-agnostic for unit-level API
 * compatibility only.
 */
bool serialized_pass_subprogram_is_platform(const SerializedPass* pass,
                                            int stage, int subprogram_index,
                                            int platform);

// Frees all allocated memory in a SerializedShader.
void serialized_shader_free(SerializedShader* shader);

#endif // SERIALIZED_SHADER_H
