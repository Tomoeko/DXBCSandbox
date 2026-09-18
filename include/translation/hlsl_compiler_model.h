// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_COMPILER_MODEL_H
#define HLSL_COMPILER_MODEL_H

#include <stdbool.h>

typedef struct HLSLCompilerTangentFrame {
    bool valid;
    int end_instruction;
    int normal_register;
    int tangent_register;
    int binormal_register;
    int tangent_input_register;
    int output_registers[3];
} HLSLCompilerTangentFrame;

typedef struct HLSLCompilerScreenPosition {
    bool valid;
    unsigned char phase;
    unsigned char pattern;
    int clip_instruction;
    int screen_instruction;
    int screen_instruction_count;
} HLSLCompilerScreenPosition;

/* Exact inverse of D3DCompiler's partial two-component projection pack.  The
 * physical stream scales one source lane in place, packs three half-scaled
 * lanes into another temporary, permits independent straight-line work, and
 * later adds the packed homogeneous offset into an output pair.  Keeping this
 * separate from the source-facing readable ScreenPos lift makes the authority
 * explicit: it is selected only by the decoded instruction/def-use graph. */
typedef struct HLSLCompilerInterleavedProjectionPack {
    bool valid;
    unsigned char phase;
    int scale_instruction;
    int pack_instruction;
    int add_instruction;
} HLSLCompilerInterleavedProjectionPack;

typedef struct HLSLCompilerSplitMatrixTransform {
    bool valid;
    unsigned char phase;
    int clip_instruction;
    int world_instruction;
    int position_input_register;
    int object_matrix_buffer;
    int object_matrix_first_row;
    int clip_matrix_buffer;
    int clip_matrix_first_row;
} HLSLCompilerSplitMatrixTransform;

typedef struct HLSLCompilerModelProgram {
    signed char *swap_binary_operands;
    unsigned char *preserve_vector_output;
    HLSLCompilerTangentFrame *tangent_frames;
    HLSLCompilerScreenPosition *screen_positions;
    HLSLCompilerInterleavedProjectionPack *interleaved_projection_packs;
    HLSLCompilerSplitMatrixTransform *split_matrix_transforms;
    int *claim_owner;
    int replacement_count;
    int conflict_count;
} HLSLCompilerModelProgram;

#endif
