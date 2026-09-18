// SPDX-License-Identifier: GPL-3.0-only

#ifndef DXBC_PARSER_H
#define DXBC_PARSER_H

#include "common/common.h"
#include "common/shader_stage.h"
#include "common/stream.h"

/* D3D11.3 tiled-resource forms add an optional scalar minimum-LOD clamp and
 * scalar feedback destination to the six-operand SAMPLE_D form.  Eight is
 * therefore the widest published SM4/5 opcode grammar, not a semantic
 * support claim by the HLSL inverse. */
#define DXBC_MAX_OPERANDS 8

/* Ordinary SM4/5 instructions encode a seven-bit DWORD count including the
 * opcode token.  A relative-address tree can therefore contain at most 126
 * nested operand tokens in any representable instruction.  Downstream tree
 * walkers should use this format-derived bound instead of a local heuristic. */
#define DXBC_MAX_NESTED_OPERAND_TOKENS 126u

typedef struct {
    /* Signature semantics are chunk-relative NUL-terminated strings with no
     * token-format length cap.  Keep common names inline, but spill longer
     * names to owned storage instead of rejecting otherwise valid DXBC. */
    char semantic_name[64];
    char* semantic_name_extended;
    size_t semantic_name_length;
    uint32_t stream_index;
    uint32_t semantic_index;
    uint32_t system_value;
    uint32_t component_type;
    uint32_t register_id;
    uint32_t min_precision;
    uint8_t interpolation_mode;
    uint8_t mask;
    uint8_t rw_mask;
} DXBCSignatureElement;

typedef enum {
    DXBC_SIGNATURE_ROLE_INPUT = 0,
    DXBC_SIGNATURE_ROLE_OUTPUT = 1,
    DXBC_SIGNATURE_ROLE_PATCH_CONSTANT = 2
} DXBCSignatureRole;

/* Returns the complete semantic name (inline or dynamically owned). */
const char* dxbc_signature_semantic_name(
    const DXBCSignatureElement* element);

/* Deep-copy/free helpers used when signature tables cross ownership
 * boundaries (DXBCContainer -> USILProgram). */
bool dxbc_signature_element_clone(DXBCSignatureElement* destination,
                                  const DXBCSignatureElement* source);
void dxbc_signature_element_free(DXBCSignatureElement* element);

/* Validates the reflection-level signature authority encoded by ISGN/ISG1,
 * OSGN/OSG1/OSG5, and PCSG/PSG1. ReadWriteMask has role-dependent meaning:
 * an input mask is an always-read subset, while an output mask marks lanes
 * that are never written. */
bool dxbc_signature_element_is_valid(
    const DXBCSignatureElement* element, DXBCSignatureRole role);

typedef struct {
    char name[5];
    uint32_t size;
    uint32_t offset;
} DXBCChunkInfo;

typedef enum {
    DXBC_INSTRUCTION_TEST_NONE = 0,
    DXBC_INSTRUCTION_TEST_ZERO = 1,
    DXBC_INSTRUCTION_TEST_NONZERO = 2
} DXBCInstructionTest;

typedef enum {
    OPERAND_TYPE_TEMP = 0,
    OPERAND_TYPE_INPUT = 1,
    OPERAND_TYPE_OUTPUT = 2,
    OPERAND_TYPE_INDEXABLE_TEMP = 3,
    OPERAND_TYPE_IMMEDIATE32 = 4,
    OPERAND_TYPE_IMMEDIATE64 = 5,
    OPERAND_TYPE_SAMPLER = 6,
    OPERAND_TYPE_RESOURCE = 7,
    OPERAND_TYPE_CONSTANT_BUFFER = 8,
    OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER = 9,
    OPERAND_TYPE_LABEL = 10,
    OPERAND_TYPE_INPUT_PRIMITIVE_ID = 11,
    OPERAND_TYPE_OUTPUT_DEPTH = 12,
    OPERAND_TYPE_NULL = 13,
    OPERAND_TYPE_RASTERIZER = 14,
    OPERAND_TYPE_OUTPUT_COVERAGE_MASK = 15,
    OPERAND_TYPE_STREAM = 16,
    OPERAND_TYPE_FUNCTION_BODY = 17,
    OPERAND_TYPE_FUNCTION_TABLE = 18,
    OPERAND_TYPE_INTERFACE = 19,
    OPERAND_TYPE_FUNCTION_INPUT = 20,
    OPERAND_TYPE_FUNCTION_OUTPUT = 21,
    OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID = 22,
    OPERAND_TYPE_FORK_INSTANCE_ID = 23,
    OPERAND_TYPE_JOIN_INSTANCE_ID = 24,
    OPERAND_TYPE_INPUT_CONTROL_POINT = 25,
    OPERAND_TYPE_OUTPUT_CONTROL_POINT = 26,
    OPERAND_TYPE_INPUT_PATCH_CONSTANT = 27,
    OPERAND_TYPE_DOMAIN_LOCATION = 28,
    OPERAND_TYPE_INPUT_DOMAIN_POINT = 28,
    OPERAND_TYPE_THIS_POINTER = 29,
    OPERAND_TYPE_UAV = 30,
    OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY = 31,
    OPERAND_TYPE_INPUT_THREAD_ID = 32,
    OPERAND_TYPE_INPUT_THREAD_GROUP_ID = 33,
    OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP = 34,
    OPERAND_TYPE_INPUT_COVERAGE_MASK = 35,
    OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED = 36,
    OPERAND_TYPE_INPUT_GS_INSTANCE_ID = 37,
    OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL = 38,
    OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL = 39,
    OPERAND_TYPE_CYCLE_COUNTER = 40,
    OPERAND_TYPE_OUTPUT_STENCIL_REF = 41,
    OPERAND_TYPE_INNER_COVERAGE = 42,
    OPERAND_TYPE_UNKNOWN = 0xff
} DXBCOperandType;

typedef struct DXBCOperand {
    DXBCOperandType type;
    uint32_t raw_token;
    uint32_t* extended_tokens;
    size_t extended_token_count;
    char text[128];
    int register_index;
    uint8_t swizzle[4]; // 0=x, 1=y, 2=z, 3=w
    uint8_t swizzle_mode; // 0=mask, 1=swizzle, 2=scalar select
    uint8_t destination_mask; // bits 4-7: x,y,z,w
    bool has_neg;
    bool has_abs;
    
    // Immediate literals
    uint32_t imm_values[4];
    int imm_value_count;
    /* Exact immediate payload.  Immediate64 has two DWORDs per component;
     * keeping the words as well as the combined values avoids losing NaN
     * payloads or depending on host floating-point formatting. */
    uint32_t immediate_words[8];
    int immediate_word_count;
    uint64_t imm64_values[4];
    uint8_t min_precision;
    
    // Relative addressing operands (e.g. r0[r1.x + 2])
    struct DXBCOperand* rel_op0;
    struct DXBCOperand* rel_op1;
    struct DXBCOperand* rel_op2;
    int rel_offset0;
    int rel_offset1;
    int rel_offset2;
    int register_index_dim;
    /* Lossless form of all three legal index dimensions.  The legacy
     * register_index/rel_offset fields above remain populated for existing
     * emitters, while these fields retain 64-bit indices and representation
     * kinds 0..4 exactly. */
    uint64_t index_values[3];
    uint8_t index_representations[3];
    bool index_has_immediate[3];
    bool index_value_exceeds_int[3];
} DXBCOperand;

typedef struct {
    int id; // instruction sequence ID
    bool is_decl;
    char opcode_str[128];
    uint32_t opcode; // raw opcode index
    uint32_t token;
    uint32_t file_offset;
    uint32_t byte_length;
    int structured_stride;
    bool has_texel_offset;
    int8_t texel_offsets[3];
    bool has_resource_dimension;
    uint32_t resource_dimension;
    bool has_resource_return_types;
    uint8_t resource_return_types[4];
    /* D3D10_SB_RESINFO_RETURN_TYPE: 0=float, 1=rcpFloat, 2=uint. */
    uint8_t resource_info_return_type;
    /* D3D11_SB_INSTRUCTION_RETURN_TYPE for SAMPLEINFO: 0=float, 1=uint. */
    uint8_t sample_info_return_type;
    bool saturate;
    /* Per-component precise control from opcode-token bits 19:22. */
    uint8_t precise_mask;
    /* Raw opcode-token test control for IF/BREAKC/CONTINUEC/DISCARD. This is
     * semantic authority; opcode_str/formatted_asm are diagnostics only. */
    DXBCInstructionTest condition_test;
    /* Typed authority carried by SGV/SIV and pixel-input declarations.
     * formatted_asm/operand text are diagnostics and must not be parsed to
     * recover these values. */
    bool has_declaration_system_value;
    uint32_t declaration_system_value;
    bool has_declaration_interpolation;
    uint8_t declaration_interpolation;
    
    DXBCOperand operands[DXBC_MAX_OPERANDS];
    int operand_count;
    
    char formatted_asm[512];
} DXBCInstruction;

typedef struct {
    int register_index;
    char dim_name[32];
    char ret_types[64];
    /* Preserve the declaration token verbatim through semantic lowering.
     * Human-readable strings are presentation only and must never be used as
     * the exact authority for resource typing. */
    uint32_t dimension;
    uint8_t return_types[4];
    uint32_t sample_count;
    uint32_t stride;
    bool is_structured;
    bool globally_coherent;
    bool rasterizer_ordered;
    bool has_order_preserving_counter;
    bool declared;
} DXBCResourceDecl;

typedef struct {
    char shader_type_model[32]; // e.g. "ps_5_0"
    /* Raw version-token authority. shader_type_model is presentation only. */
    DXBCProgramType program_type;
    uint8_t major_version;
    uint8_t minor_version;
    bool has_executable_program;
    DXBCResourceDecl* resources;
    int resource_count;
    int resource_alloc;
    DXBCResourceDecl* uavs;
    int uav_count;
    int uav_alloc;
    
    DXBCSignatureElement* input_signature;
    int input_signature_count;
    int input_signature_alloc;
    
    DXBCSignatureElement* output_signature;
    int output_signature_count;
    int output_signature_alloc;
    
    DXBCSignatureElement* patch_constant_signature;
    int patch_constant_signature_count;
    int patch_constant_signature_alloc;
    
    DXBCChunkInfo* chunks;
    int chunk_count;
    int chunk_alloc;
    
    DXBCInstruction* instructions;
    int instruction_count;
    int instruction_alloc;
    
    uint32_t* icb_values;
    int icb_value_count;
    int icb_value_alloc;

    /* Set only by dxbc_parse after the raw container and its signature chunks
     * have passed validation. Hand-built semantic fixtures remain explicitly
     * distinguishable from parsed bytecode authority. */
    bool parsed_signature_authority;
} DXBCContainer;

typedef struct {
    const uint8_t* data;
    size_t size;
} DXBCContainerView;

// Resolves the first complete container from a raw DXBC blob or a supported
// Unity wrapper. Recognition is structural and never scans for magic bytes.
bool dxbc_container_view_first(const uint8_t* data, size_t size,
                               DXBCContainerView* out_view);

// Parses a raw DXBC container, a Unity 2021 compiled-program record, or a USBD
// record table. Wrapper recognition is structural; arbitrary byte scanning is
// intentionally not supported.
bool dxbc_parse(DXBCContainer* container, const uint8_t* data, size_t size);
const DXBCResourceDecl* dxbc_find_resource_declaration(
    const DXBCContainer* container, int register_index, bool uav);
void dxbc_free(DXBCContainer* container);

#endif // DXBC_PARSER_H
