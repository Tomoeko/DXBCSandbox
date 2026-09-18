// SPDX-License-Identifier: GPL-3.0-only

#ifndef USIL_H
#define USIL_H

#include "common/common.h"
#include "dxbc/dxbc_parser.h"
#include "dxbc/dxbc_stage_contract.h"

typedef enum {
    USIL_OP_NOP,
    USIL_OP_ADD,
    USIL_OP_SUB,
    USIL_OP_MUL,
    USIL_OP_DIV,
    USIL_OP_MAD,
    USIL_OP_MOV,
    USIL_OP_MOVC,
    USIL_OP_DP2,
    USIL_OP_DP3,
    USIL_OP_DP4,
    USIL_OP_RCP,
    USIL_OP_RSQ,
    USIL_OP_SQRT,
    USIL_OP_MIN,
    USIL_OP_MAX,
    USIL_OP_LT,
    USIL_OP_GE,
    USIL_OP_EQ,
    USIL_OP_NE,
    USIL_OP_ILT,
    USIL_OP_IGE,
    USIL_OP_IEQ,
    USIL_OP_INE,
    USIL_OP_ULT,
    USIL_OP_UGE,
    USIL_OP_AND,
    USIL_OP_OR,
    USIL_OP_XOR,
    USIL_OP_NOT,
    USIL_OP_ISHL,
    USIL_OP_ISHR,
    USIL_OP_USHR,
    USIL_OP_FTOI,
    USIL_OP_FTOU,
    USIL_OP_ITOF,
    USIL_OP_UTOF,
    USIL_OP_SAMPLE,
    USIL_OP_SAMPLE_C,
    USIL_OP_SAMPLE_C_LZ,
    USIL_OP_SAMPLE_L,
    USIL_OP_SAMPLE_D,
    USIL_OP_SAMPLE_B,
    USIL_OP_LD,
    USIL_OP_LD_STRUCTURED,
    USIL_OP_LD_MS,
    USIL_OP_RESINFO,
    USIL_OP_SAMPLEINFO,
    USIL_OP_LOG,
    USIL_OP_EXP,
    USIL_OP_SIN,
    USIL_OP_COS,
    USIL_OP_FRC,
    USIL_OP_ROUND_NE,
    USIL_OP_ROUND_NI,
    USIL_OP_ROUND_PI,
    USIL_OP_ROUND_Z,
    USIL_OP_IF,
    USIL_OP_ELSE,
    USIL_OP_ENDIF,
    USIL_OP_LOOP,
    USIL_OP_ENDLOOP,
    USIL_OP_SWITCH,
    USIL_OP_CASE,
    USIL_OP_DEFAULT,
    USIL_OP_ENDSWITCH,
    USIL_OP_BREAK,
    USIL_OP_BREAKC,
    USIL_OP_CONTINUE,
    USIL_OP_CONTINUEC,
    USIL_OP_RET,
    USIL_OP_DISCARD,
    USIL_OP_DERIV_RTX,
    USIL_OP_DERIV_RTY,
    USIL_OP_DERIV_RTX_COARSE,
    USIL_OP_DERIV_RTY_COARSE,
    USIL_OP_DERIV_RTX_FINE,
    USIL_OP_DERIV_RTY_FINE,
    USIL_OP_IADD,
    USIL_OP_IMUL,
    USIL_OP_IMAD,
    USIL_OP_IMAX,
    USIL_OP_IMIN,
    USIL_OP_UMAX,
    USIL_OP_UMIN,
    USIL_OP_UDIV,
    USIL_OP_INEG,
    USIL_OP_IMM_ATOMIC_IADD,
    USIL_OP_LDMS,
    USIL_OP_SINCOS,
    USIL_OP_UBFE,
    USIL_OP_GEOMETRY_APPEND,
    USIL_OP_GEOMETRY_RESTART_STRIP
} USILOpcode;

typedef enum {
    USIL_GEOMETRY_EFFECT_NONE = 0,
    USIL_GEOMETRY_EFFECT_APPEND,
    USIL_GEOMETRY_EFFECT_RESTART_STRIP
} USILGeometryEffectKind;

typedef struct {
    USILOpcode opcode;
    bool saturate;
    uint8_t precise_mask;
    DXBCInstructionTest condition_test;
    char resource_dimension[16];
    bool has_resource_dimension;
    uint32_t resource_stride;
    bool has_texel_offset;
    int8_t texel_offsets[3];
    bool has_resource_return_types;
    uint8_t resource_return_types[4];
    uint8_t resource_info_return_type;
    uint8_t sample_info_return_type;
    uint32_t source_instruction_index;
    USILGeometryEffectKind geometry_effect;
    uint8_t geometry_stream_id;
    bool geometry_stream_explicit;
    DXBCOperand operands[DXBC_MAX_OPERANDS];
    int operand_count;
    /* Preserve the complete canonical DXBC diagnostic string.  The source
     * instruction representation has the same bound. */
    char original_asm[512];
} USILInstruction;

typedef struct {
    int reg_idx;
    int size;
    bool dynamic_indexed;
} USILConstantBuffer;

typedef struct {
    int reg_idx;
    char dimension[16];
    uint8_t return_types[4];
    uint32_t sample_count;
    int stride;
    bool globally_coherent;
    bool rasterizer_ordered;
    bool has_order_preserving_counter;
} USILUav;

typedef struct {
    int reg_idx;
    char dimension[16];
    uint8_t return_types[4];
    uint32_t sample_count;
    int stride;
} USILTexture;

typedef struct {
    int reg_idx;
    /* D3D10_SB_SAMPLER_MODE: 0=default, 1=comparison, 2=mono. */
    uint8_t mode;
} USILSampler;

typedef struct {
    int reg_idx;
    int size;
} USILIndexableTemp;

typedef struct {
    DXBCOperand operand;
    uint32_t register_count;
    uint32_t source_instruction_index;
    /* -1 outside a hull phase; otherwise an index into tessellation.phases. */
    int hull_phase_index;
} USILIndexRange;

typedef enum {
    USIL_SIGNATURE_DECL_INPUT = 95,
    USIL_SIGNATURE_DECL_INPUT_SGV = 96,
    USIL_SIGNATURE_DECL_INPUT_SIV = 97,
    USIL_SIGNATURE_DECL_INPUT_PS = 98,
    USIL_SIGNATURE_DECL_INPUT_PS_SGV = 99,
    USIL_SIGNATURE_DECL_INPUT_PS_SIV = 100,
    USIL_SIGNATURE_DECL_OUTPUT = 101,
    USIL_SIGNATURE_DECL_OUTPUT_SGV = 102,
    USIL_SIGNATURE_DECL_OUTPUT_SIV = 103
} USILSignatureDeclarationKind;

typedef struct {
    USILSignatureDeclarationKind kind;
    DXBCOperandType operand_type;
    bool has_signature_register;
    uint32_t register_id;
    uint8_t mask;
    uint8_t stream_index;
    /* Dimension zero of a two-dimensional declaration operand is the
     * compiler-authored array extent (geometry vertices or tessellation
     * control points); dimension one is the signature register. */
    bool has_array_element_count;
    uint8_t array_element_count;
    bool has_system_value;
    /* Raw D3D10_SB_NAME token (1..25), not presentation text. */
    uint32_t system_value_name;
    bool has_interpolation;
    uint8_t interpolation_mode;
    uint32_t source_instruction_index;
} USILSignatureDeclaration;

typedef struct {
    bool valid;
    DXBCInputPrimitive input_primitive;
    DXBCOutputTopology output_topology;
    uint32_t input_vertex_count;
    uint32_t max_output_vertex_count;
    bool has_instance_count;
    uint32_t instance_count;
    uint8_t declared_stream_mask;
    uint8_t referenced_stream_mask;
    size_t effect_count;
    /* DXBC output registers form one persistent tuple. Append snapshots that
     * tuple; it does not reset lanes before the next instruction. */
    bool output_tuple_state_persists;
} USILGeometryContract;

typedef struct {
    DXBCHullPhaseKind kind;
    uint32_t marker_source_instruction_index;
    uint32_t first_source_instruction_index;
    uint32_t end_source_instruction_index;
    int first_instruction_index;
    int end_instruction_index;
    bool instance_count_declared;
    uint32_t instance_count;
} USILHullPhase;

typedef struct {
    bool valid;
    uint32_t input_control_point_count;
    uint32_t output_control_point_count;
    DXBCTessellatorDomain domain;
    DXBCTessellatorPartitioning partitioning;
    DXBCTessellatorOutputPrimitive output_primitive;
    bool has_max_tessellation_factor;
    uint32_t max_tessellation_factor_bits;
    size_t phase_count;
    size_t phase_capacity;
    USILHullPhase *phases;
} USILTessellationContract;

typedef struct {
    char shader_type_model[32];
    
    DXBCSignatureElement* inputs;
    int input_count;
    int input_alloc;
    
    DXBCSignatureElement* outputs;
    int output_count;
    int output_alloc;

    DXBCSignatureElement* patch_constants;
    int patch_constant_count;
    int patch_constant_alloc;

    USILSignatureDeclaration* signature_declarations;
    int signature_declaration_count;
    int signature_declaration_alloc;
    /* True only when lowering started from dxbc_parse's byte-validated
     * signature/declaration authority rather than hand-built semantic IR. */
    bool has_parsed_signature_authority;
    
    int temp_count;
    
    USILConstantBuffer* cbuffers;
    int cbuffer_count;
    int cbuffer_alloc;
    
    USILTexture* textures;
    int texture_count;
    int texture_alloc;
    
    USILSampler* samplers;
    int sampler_count;
    int sampler_alloc;
    
    USILIndexableTemp* indexable_temps;
    int indexable_temp_count;
    int indexable_temp_alloc;

    USILIndexRange* index_ranges;
    int index_range_count;
    int index_range_alloc;
    
    USILUav* uavs;
    int uav_count;
    int uav_alloc;
    
    USILInstruction* instructions;
    int instruction_count;
    int instruction_alloc;
    
    uint32_t* icb_values;
    int icb_value_count;
    int icb_value_alloc;
    bool has_global_flags;
    uint32_t global_flags;
    bool has_stage_contract;
    DXBCProgramType program_type;
    uint8_t shader_model_major;
    uint8_t shader_model_minor;
    USILGeometryContract geometry;
    USILTessellationContract tessellation;
} USILProgram;

bool usil_translate(USILProgram* program, const DXBCContainer* container);
/* Stage-aware translation cross-checks every geometry stream effect against
 * the lossless token contract. Geometry HLSL emission accepts only programs
 * produced through this entry point. */
bool usil_translate_with_stage_contract(
    USILProgram* program, const DXBCContainer* container,
    const DXBCStageContract* stage_contract);
bool usil_signature_authority_is_valid(const USILProgram* program);
void usil_free(USILProgram* program);

#endif // USIL_H
