// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_SEMANTIC_H
#define HLSL_SEMANTIC_H

#include <stdbool.h>

typedef enum HLSLProvenanceKind {
    HLSL_PROVENANCE_UNDEFINED = 0,
    HLSL_PROVENANCE_INPUT,
    HLSL_PROVENANCE_CONSTANT,
    HLSL_PROVENANCE_TEXTURE,
    HLSL_PROVENANCE_COPY,
    HLSL_PROVENANCE_ARITHMETIC,
    HLSL_PROVENANCE_COMPARISON,
    HLSL_PROVENANCE_CONTROL
} HLSLProvenanceKind;

typedef struct HLSLComponentProvenance {
    HLSLProvenanceKind kind;
    int definition_instruction;
    int root_instruction;
    int root_register;
    int root_component;
    int vector_definition;
    unsigned char vector_mask;
} HLSLComponentProvenance;

typedef enum HLSLSemanticKind {
    HLSL_SEMANTIC_NONE = 0,
    HLSL_SEMANTIC_SCREEN_POSITION,
    HLSL_SEMANTIC_STATIC_UV_SELECTION,
    HLSL_SEMANTIC_SURFACE_CLIP_POSITION,
    HLSL_SEMANTIC_SURFACE_TANGENT_FRAME,
    HLSL_SEMANTIC_INDEXED_FACE_BASIS,
    HLSL_SEMANTIC_SHADOW_WORLD_POSITION,
    HLSL_SEMANTIC_DEFERRED_ALPHA_CLIP,
    HLSL_SEMANTIC_SPEEDTREE_GLOBAL_WIND,
    HLSL_SEMANTIC_TRUTHINESS_CONDITION
} HLSLSemanticKind;

typedef struct HLSLSemanticContract {
    HLSLSemanticKind kind;
    const char *name;
    int priority;
    bool library_semantic;
} HLSLSemanticContract;

typedef struct HLSLSemanticLift {
    const HLSLSemanticContract *contract;
    int trigger_instruction;
    int after_instruction;
    int claimed_count;
    int claimed[64];
    union {
        struct {
            int select;
            int lightmap_input;
            int dynamic_input;
        } static_uv;
        struct {
            int phase;
        } surface;
        struct {
            int output_register;
            int uv_input_register;
        } face_basis;
        struct {
            int output_register;
            int input_register;
            const char *matrix;
        } shadow_position;
        struct {
            int mad;
            int compare;
            int discard;
            int multiply;
            int move;
            int color_temp_operand;
            int first_operand_is_cb;
        } alpha_clip;
        struct {
            int start;
        } speedtree_wind;
        struct {
            int value_operand;
            unsigned char component_mask;
        } truthiness;
    } data;
} HLSLSemanticLift;

typedef struct HLSLSemanticProgram {
    HLSLSemanticLift *lifts;
    int lift_count;
    int lift_capacity;
    int *before_lift;
    int *after_lift;
    int *claim_owner;
    unsigned int *instruction_flags;
    signed char *binary_operand_order;
    int conflict_count;
} HLSLSemanticProgram;

enum {
    HLSL_SEMANTIC_FLAG_UNITY_SH_VECTOR_OUTPUT = 1u << 0
};

#endif
