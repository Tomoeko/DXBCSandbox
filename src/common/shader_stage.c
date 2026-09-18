// SPDX-License-Identifier: GPL-3.0-only

#include "common/shader_stage.h"

#include <stddef.h>

bool shader_stage_serialized_to_compiler(
    UnitySerializedProgramStage serialized_stage,
    UnityCompilerProgramStage* out_compiler_program) {
    static const UnityCompilerProgramStage mapping[] = {
        UNITY_COMPILER_PROGRAM_VERTEX,
        UNITY_COMPILER_PROGRAM_FRAGMENT,
        UNITY_COMPILER_PROGRAM_GEOMETRY,
        UNITY_COMPILER_PROGRAM_HULL,
        UNITY_COMPILER_PROGRAM_DOMAIN,
        UNITY_COMPILER_PROGRAM_RAY_TRACING,
    };
    if (!out_compiler_program ||
        (uint32_t)serialized_stage >=
            sizeof(mapping) / sizeof(mapping[0])) {
        return false;
    }
    *out_compiler_program = mapping[serialized_stage];
    return true;
}

bool shader_stage_compiler_to_serialized(
    UnityCompilerProgramStage compiler_program,
    UnitySerializedProgramStage* out_serialized_stage) {
    if (!out_serialized_stage) return false;
    switch (compiler_program) {
        case UNITY_COMPILER_PROGRAM_VERTEX:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_VERTEX;
            return true;
        case UNITY_COMPILER_PROGRAM_FRAGMENT:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_FRAGMENT;
            return true;
        case UNITY_COMPILER_PROGRAM_HULL:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_HULL;
            return true;
        case UNITY_COMPILER_PROGRAM_DOMAIN:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_DOMAIN;
            return true;
        case UNITY_COMPILER_PROGRAM_GEOMETRY:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_GEOMETRY;
            return true;
        case UNITY_COMPILER_PROGRAM_RAY_TRACING:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_RAY_TRACING;
            return true;
        default:
            return false;
    }
}

bool shader_stage_serialized_to_dxbc(
    UnitySerializedProgramStage serialized_stage,
    DXBCProgramType* out_program_type) {
    if (!out_program_type) return false;
    switch (serialized_stage) {
        case UNITY_SERIALIZED_STAGE_VERTEX:
            *out_program_type = DXBC_PROGRAM_TYPE_VERTEX;
            return true;
        case UNITY_SERIALIZED_STAGE_FRAGMENT:
            *out_program_type = DXBC_PROGRAM_TYPE_PIXEL;
            return true;
        case UNITY_SERIALIZED_STAGE_GEOMETRY:
            *out_program_type = DXBC_PROGRAM_TYPE_GEOMETRY;
            return true;
        case UNITY_SERIALIZED_STAGE_HULL:
            *out_program_type = DXBC_PROGRAM_TYPE_HULL;
            return true;
        case UNITY_SERIALIZED_STAGE_DOMAIN:
            *out_program_type = DXBC_PROGRAM_TYPE_DOMAIN;
            return true;
        default:
            return false;
    }
}

bool shader_stage_dxbc_to_serialized(
    DXBCProgramType program_type,
    UnitySerializedProgramStage* out_serialized_stage) {
    if (!out_serialized_stage) return false;
    switch (program_type) {
        case DXBC_PROGRAM_TYPE_PIXEL:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_FRAGMENT;
            return true;
        case DXBC_PROGRAM_TYPE_VERTEX:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_VERTEX;
            return true;
        case DXBC_PROGRAM_TYPE_GEOMETRY:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_GEOMETRY;
            return true;
        case DXBC_PROGRAM_TYPE_HULL:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_HULL;
            return true;
        case DXBC_PROGRAM_TYPE_DOMAIN:
            *out_serialized_stage = UNITY_SERIALIZED_STAGE_DOMAIN;
            return true;
        default:
            return false;
    }
}

bool shader_stage_serialized_program_mask_bit(
    UnitySerializedProgramStage serialized_stage, uint32_t* out_mask_bit) {
    if (!out_mask_bit ||
        (uint32_t)serialized_stage >= UNITY_SERIALIZED_STAGE_COUNT) {
        return false;
    }
    /* SerializedPass stores stage presence at bit (slot + 1). */
    *out_mask_bit = UINT32_C(1) << ((uint32_t)serialized_stage + 1u);
    return true;
}

static bool gpu_program_matches(const ShaderStageTuple* tuple) {
    switch (tuple->gpu_program_type) {
        case UNITY_GPU_PROGRAM_D3D11_VERTEX_SM40:
            return tuple->serialized_stage == UNITY_SERIALIZED_STAGE_VERTEX &&
                   tuple->shader_model_major == 4u &&
                   tuple->shader_model_minor <= 1u;
        case UNITY_GPU_PROGRAM_D3D11_VERTEX_SM50:
            return tuple->serialized_stage == UNITY_SERIALIZED_STAGE_VERTEX &&
                   tuple->shader_model_major == 5u &&
                   tuple->shader_model_minor == 0u;
        case UNITY_GPU_PROGRAM_D3D11_PIXEL_SM40:
            return tuple->serialized_stage == UNITY_SERIALIZED_STAGE_FRAGMENT &&
                   tuple->shader_model_major == 4u &&
                   tuple->shader_model_minor <= 1u;
        case UNITY_GPU_PROGRAM_D3D11_PIXEL_SM50:
            return tuple->serialized_stage == UNITY_SERIALIZED_STAGE_FRAGMENT &&
                   tuple->shader_model_major == 5u &&
                   tuple->shader_model_minor == 0u;
        case UNITY_GPU_PROGRAM_D3D11_GEOMETRY_SM40:
            return tuple->serialized_stage == UNITY_SERIALIZED_STAGE_GEOMETRY &&
                   tuple->shader_model_major == 4u &&
                   tuple->shader_model_minor <= 1u;
        case UNITY_GPU_PROGRAM_D3D11_GEOMETRY_SM50:
            return tuple->serialized_stage == UNITY_SERIALIZED_STAGE_GEOMETRY &&
                   tuple->shader_model_major == 5u &&
                   tuple->shader_model_minor == 0u;
        case UNITY_GPU_PROGRAM_D3D11_HULL_SM50:
            return tuple->serialized_stage == UNITY_SERIALIZED_STAGE_HULL &&
                   tuple->shader_model_major == 5u &&
                   tuple->shader_model_minor == 0u;
        case UNITY_GPU_PROGRAM_D3D11_DOMAIN_SM50:
            return tuple->serialized_stage == UNITY_SERIALIZED_STAGE_DOMAIN &&
                   tuple->shader_model_major == 5u &&
                   tuple->shader_model_minor == 0u;
        default:
            return false;
    }
}

ShaderStageTupleStatus shader_stage_validate_d3d11_tuple(
    const ShaderStageTuple* tuple) {
    if (!tuple) return SHADER_STAGE_TUPLE_INVALID_ARGUMENT;

    UnityCompilerProgramStage expected_compiler;
    if (!shader_stage_serialized_to_compiler(tuple->serialized_stage,
                                              &expected_compiler)) {
        return SHADER_STAGE_TUPLE_INVALID_SERIALIZED_STAGE;
    }
    if (tuple->compiler_program != expected_compiler) {
        return SHADER_STAGE_TUPLE_COMPILER_PROGRAM_MISMATCH;
    }

    uint32_t expected_mask_bit;
    if (!shader_stage_serialized_program_mask_bit(tuple->serialized_stage,
                                                   &expected_mask_bit) ||
        (tuple->serialized_program_mask & expected_mask_bit) == 0u) {
        return SHADER_STAGE_TUPLE_PROGRAM_MASK_MISMATCH;
    }

    DXBCProgramType expected_dxbc;
    if (!shader_stage_serialized_to_dxbc(tuple->serialized_stage,
                                         &expected_dxbc) ||
        tuple->dxbc_program_type != expected_dxbc) {
        return SHADER_STAGE_TUPLE_DXBC_PROGRAM_MISMATCH;
    }

    if ((tuple->shader_model_major != 4u &&
         tuple->shader_model_major != 5u) ||
        (tuple->shader_model_major == 4u &&
         tuple->shader_model_minor > 1u) ||
        (tuple->shader_model_major == 5u &&
         tuple->shader_model_minor != 0u)) {
        return SHADER_STAGE_TUPLE_SHADER_MODEL_MISMATCH;
    }
    if (!gpu_program_matches(tuple)) {
        return SHADER_STAGE_TUPLE_GPU_PROGRAM_MISMATCH;
    }
    return SHADER_STAGE_TUPLE_OK;
}

const char* shader_stage_tuple_status_name(ShaderStageTupleStatus status) {
    switch (status) {
        case SHADER_STAGE_TUPLE_OK: return "ok";
        case SHADER_STAGE_TUPLE_INVALID_ARGUMENT: return "invalid_argument";
        case SHADER_STAGE_TUPLE_INVALID_SERIALIZED_STAGE:
            return "invalid_serialized_stage";
        case SHADER_STAGE_TUPLE_COMPILER_PROGRAM_MISMATCH:
            return "compiler_program_mismatch";
        case SHADER_STAGE_TUPLE_PROGRAM_MASK_MISMATCH:
            return "program_mask_mismatch";
        case SHADER_STAGE_TUPLE_DXBC_PROGRAM_MISMATCH:
            return "dxbc_program_mismatch";
        case SHADER_STAGE_TUPLE_GPU_PROGRAM_MISMATCH:
            return "gpu_program_mismatch";
        case SHADER_STAGE_TUPLE_SHADER_MODEL_MISMATCH:
            return "shader_model_mismatch";
        default: return "unknown";
    }
}
