// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_STAGE_H
#define SHADER_STAGE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Unity uses different integer domains for a serialized pass slot and for a
 * ShaderCompilerProgram.  DXBC's version token uses a third ordering.  These
 * enums must not be cast between domains: geometry and hull are deliberately
 * in different positions.
 */
typedef enum {
    UNITY_SERIALIZED_STAGE_VERTEX = 0,
    UNITY_SERIALIZED_STAGE_FRAGMENT = 1,
    UNITY_SERIALIZED_STAGE_GEOMETRY = 2,
    UNITY_SERIALIZED_STAGE_HULL = 3,
    UNITY_SERIALIZED_STAGE_DOMAIN = 4,
    UNITY_SERIALIZED_STAGE_RAY_TRACING = 5,
    UNITY_SERIALIZED_STAGE_COUNT = 6,
    UNITY_SERIALIZED_STAGE_INVALID = 0xff
} UnitySerializedProgramStage;

typedef enum {
    UNITY_COMPILER_PROGRAM_VERTEX = 0,
    UNITY_COMPILER_PROGRAM_FRAGMENT = 1,
    UNITY_COMPILER_PROGRAM_HULL = 2,
    UNITY_COMPILER_PROGRAM_DOMAIN = 3,
    UNITY_COMPILER_PROGRAM_GEOMETRY = 4,
    UNITY_COMPILER_PROGRAM_COMPUTE = 5,
    UNITY_COMPILER_PROGRAM_RAY_TRACING = 6,
    UNITY_COMPILER_PROGRAM_COUNT = 7,
    UNITY_COMPILER_PROGRAM_INVALID = 0xff
} UnityCompilerProgramStage;

typedef enum {
    DXBC_PROGRAM_TYPE_PIXEL = 0,
    DXBC_PROGRAM_TYPE_VERTEX = 1,
    DXBC_PROGRAM_TYPE_GEOMETRY = 2,
    DXBC_PROGRAM_TYPE_HULL = 3,
    DXBC_PROGRAM_TYPE_DOMAIN = 4,
    DXBC_PROGRAM_TYPE_COMPUTE = 5,
    DXBC_PROGRAM_TYPE_COUNT = 6,
    DXBC_PROGRAM_TYPE_INVALID = 0xffff
} DXBCProgramType;

typedef enum {
    UNITY_GPU_PROGRAM_D3D11_VERTEX_SM40 = 15,
    UNITY_GPU_PROGRAM_D3D11_VERTEX_SM50 = 16,
    UNITY_GPU_PROGRAM_D3D11_PIXEL_SM40 = 17,
    UNITY_GPU_PROGRAM_D3D11_PIXEL_SM50 = 18,
    UNITY_GPU_PROGRAM_D3D11_GEOMETRY_SM40 = 19,
    UNITY_GPU_PROGRAM_D3D11_GEOMETRY_SM50 = 20,
    UNITY_GPU_PROGRAM_D3D11_HULL_SM50 = 21,
    UNITY_GPU_PROGRAM_D3D11_DOMAIN_SM50 = 22,
    UNITY_GPU_PROGRAM_RAY_TRACING = 31
} UnityGPUProgramType;

typedef enum {
    SHADER_STAGE_TUPLE_OK = 0,
    SHADER_STAGE_TUPLE_INVALID_ARGUMENT,
    SHADER_STAGE_TUPLE_INVALID_SERIALIZED_STAGE,
    SHADER_STAGE_TUPLE_COMPILER_PROGRAM_MISMATCH,
    SHADER_STAGE_TUPLE_PROGRAM_MASK_MISMATCH,
    SHADER_STAGE_TUPLE_DXBC_PROGRAM_MISMATCH,
    SHADER_STAGE_TUPLE_GPU_PROGRAM_MISMATCH,
    SHADER_STAGE_TUPLE_SHADER_MODEL_MISMATCH
} ShaderStageTupleStatus;

typedef struct {
    UnitySerializedProgramStage serialized_stage;
    UnityCompilerProgramStage compiler_program;
    uint32_t serialized_program_mask;
    UnityGPUProgramType gpu_program_type;
    DXBCProgramType dxbc_program_type;
    uint8_t shader_model_major;
    uint8_t shader_model_minor;
} ShaderStageTuple;

bool shader_stage_serialized_to_compiler(
    UnitySerializedProgramStage serialized_stage,
    UnityCompilerProgramStage* out_compiler_program);
bool shader_stage_compiler_to_serialized(
    UnityCompilerProgramStage compiler_program,
    UnitySerializedProgramStage* out_serialized_stage);
bool shader_stage_serialized_to_dxbc(
    UnitySerializedProgramStage serialized_stage,
    DXBCProgramType* out_program_type);
bool shader_stage_dxbc_to_serialized(
    DXBCProgramType program_type,
    UnitySerializedProgramStage* out_serialized_stage);
bool shader_stage_serialized_program_mask_bit(
    UnitySerializedProgramStage serialized_stage, uint32_t* out_mask_bit);

ShaderStageTupleStatus shader_stage_validate_d3d11_tuple(
    const ShaderStageTuple* tuple);
const char* shader_stage_tuple_status_name(ShaderStageTupleStatus status);

#endif /* SHADER_STAGE_H */
