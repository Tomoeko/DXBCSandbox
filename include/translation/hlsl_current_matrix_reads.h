// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_CURRENT_MATRIX_READS_H
#define HLSL_CURRENT_MATRIX_READS_H

#include "translation/usil.h"
#include "io/subprogram_metadata.h"

enum { HLSL_CURRENT_MATRIX_NAME_LIMIT = 96,
       HLSL_CURRENT_MATRIX_FIELD_LIMIT = 1024,
       HLSL_CURRENT_MATRIX_READ_LIMIT = 8192 };

typedef enum {
    HLSL_CURRENT_MATRIX_OK = 0,
    HLSL_CURRENT_MATRIX_NOT_APPLICABLE,
    HLSL_CURRENT_MATRIX_INVALID_ARGUMENT,
    HLSL_CURRENT_MATRIX_INVALID_PROGRAM,
    HLSL_CURRENT_MATRIX_UNSUPPORTED,
    HLSL_CURRENT_MATRIX_MISSING_AUTHORITY,
    HLSL_CURRENT_MATRIX_ALLOCATION_FAILED
} HLSLCurrentMatrixStatus;

typedef struct {
    char block_name[HLSL_CURRENT_MATRIX_NAME_LIMIT];
    char field_name[HLSL_CURRENT_MATRIX_NAME_LIMIT];
    uint32_t binding_register, declared_byte_size, reflected_byte_size;
    uint32_t field_byte_offset, field_byte_size;
    uint64_t logical_aggregate_id;
    /* Original metadata table coordinates, before projection or sorting.
     * Authority is 1=current or 2=common. No sibling/readable authority. */
    uint8_t field_authority, binding_authority, full_shell_authorities;
    uint32_t metadata_buffer_index, metadata_field_index;
    uint32_t metadata_binding_index;
    bool row_major;
} HLSLCurrentMatrixField;

typedef struct {
    uint32_t field_index;
    int instruction_index, operand_index;
    uint32_t source_instruction_index;
    uint8_t destination_lanes, logical_lane, physical_lane;
    uint32_t physical_row, byte_offset, field_relative_byte_offset;
} HLSLCurrentMatrixRead;

typedef struct {
    HLSLCurrentMatrixField *fields;
    size_t field_count;
    HLSLCurrentMatrixRead *reads;
    size_t read_count;
} HLSLCurrentMatrixReads;

void hlsl_current_matrix_reads_init(HLSLCurrentMatrixReads *inventory);
void hlsl_current_matrix_reads_dispose(HLSLCurrentMatrixReads *inventory);

/* Owned target-read observations only, never source/AST or quality authority.
 * Reuses the emitter's authority-only current/common cbuffer map and layout,
 * exact operand demands and physical projection. Fields are named column-major
 * float4x4, without arrays, loose/implicit-global scopes, relative/padding reads,
 * modified operands, ambiguous owners, or inferred builtin declarations.
 * Every retained lane has an actual instruction/operand owner. Known scalar
 * and vector fields are outside this inventory; absence grants no coverage.
 * The initialized empty destination stays empty on failure/NOT_APPLICABLE.
 * Inputs are borrowed only during this call and must be valid decoded models. */
HLSLCurrentMatrixStatus hlsl_current_matrix_reads_build(
    const USILProgram *program, const SerializedProgramParameters *current,
    const SerializedProgramParameters *common, HLSLCurrentMatrixReads *out);

#endif
