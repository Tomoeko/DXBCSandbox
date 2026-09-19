// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_EMITTER_INTERNAL_H
#define HLSL_EMITTER_INTERNAL_H

#include "translation/hlsl_emitter.h"
#include "translation/hlsl_unity_uv_lift.h"
#include "translation/hlsl_compiler_model.h"
#include "translation/hlsl_cfg.h"
#include "translation/hlsl_semantic.h"
#include "translation/hlsl_storage_plan.h"
#include "translation/hlsl_use_def.h"
#include "translation/hlsl_ssa.h"
#include "translation/hlsl_ast.h"
#include "translation/hlsl_literal.h"
#include "translation/hlsl_value_analysis.h"
#include "translation/dxbc_cbuffer_projection.h"
#include "io/parameter_layout.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char* name;
    uint32_t type;
    uint32_t rows;
    uint32_t dim;
    uint32_t is_matrix;
    uint32_t matrix_array_size;
    uint32_t reg_offset;
    uint32_t byte_offset;
    uint32_t byte_size;
    bool row_major;
    /* 1 = stage-specific compiled parameters, 2 = common TypeTree
     * parameters, 3 = readable-only builtin fallback. */
    uint8_t authority;
} TempVariable;

/* Shader Model 5 / Direct3D 11 architectural register limits.  Keeping these
 * distinct is important: the old implementation used 128 for nearly every
 * register class, which both rejected valid r128+/v16+ programs and accepted
 * invalid s16+/cb15+ programs. */
enum {
    HLSL_SM5_TEMP_REGISTER_COUNT = 4096,
    HLSL_SM5_IO_REGISTER_COUNT = 32,
    HLSL_SM5_PIXEL_OUTPUT_REGISTER_COUNT = 8,
    HLSL_SM5_RESOURCE_REGISTER_COUNT = 128,
    HLSL_SM5_SAMPLER_REGISTER_COUNT = 16,
    HLSL_SM5_CBUFFER_REGISTER_COUNT = 15,
    HLSL_SM5_UAV_REGISTER_COUNT = 8,
    HLSL_SM5_FLOW_CONTROL_NESTING_LIMIT = 64,
    HLSL_D3D11_MAX_STRUCTURED_STRIDE = 2048,
    HLSL_D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT = 32
};

#define HLSL_MAX_CBUFFER_LAYOUTS HLSL_SM5_CBUFFER_REGISTER_COUNT

typedef struct {
    int reg;
    /* Rows retained by the stripped DXBC declaration. They bound every
     * executable access and must not be inflated to reproduce reflection. */
    int row_count;
    /* Full source declaration shell retained by Unity serialization. Unused
     * trailing declarations may extend this beyond row_count * 16 without
     * changing the optimized DXBC declaration or instruction stream. */
    uint32_t reflection_size_bytes;
    bool has_reflection_size_authority;
    const char* serialized_name;
    char* declaration_name;
    bool is_globals;
    bool is_unity_builtin;
    bool omit_declaration;
    bool raw_storage;
    /* Exact inverse for an HLSL array-of-row-struct declaration.  DXBC only
     * retains the flattened row address, so this mode is enabled solely when
     * the instruction stream proves that one scalar relative base is the
     * low result of an exact constant row-stride scale and every use of that
     * result selects a row in [0, row_struct_stride), including the exact
     * D3DCompiler-trimmed tail of the final element.  Recompile emission
     * replaces the scale with a capture of its unscaled source; spelling the
     * serialized array access then makes D3DCompiler regenerate the scale
     * instead of adding a lossy shift/divide inverse. */
    bool row_struct_storage;
    int row_struct_stride;
    int row_struct_elements;
    /* -1 denotes the serialized one-row identity stride, which needs no
     * scale/capture and uses the operand's relative scalar directly. */
    int row_struct_scale_instruction;
    const DXBCOperand* row_struct_index_source;
    /* When non-NULL, the array-of-struct topology is serialized reflection
     * authority rather than an anonymous executable-only inverse.  The
     * parameter record owns both borrowed pointers for the lifetime of one
     * emission. */
    const SerializedProgramParameters* row_struct_parameters;
    const SerializedStructParam* row_struct_parameter;
    bool has_serialized_authority;
    TempVariable* variables;
    DXBCCBufferVariableUse* uses;
    int variable_count;
    int variable_alloc;
    int use_alloc;
    DXBCCBufferProjection projection;
    DXBCCBufferProjectionStatus projection_status;
} HLSLCBufferLayout;

typedef struct {
    const char* cb_name;
    const char* var_name;
    uint32_t offset;
    uint32_t type; // 0 for float, 1 for int
    uint32_t rows;
    uint32_t dim;
    uint32_t is_matrix;
    uint32_t matrix_array_size;
} BuiltinVariable;

typedef struct {
    int dxbc_reg;           // DXBC register index (b0, b1, ...)
    const char* cb_name;    // Borrowed resolved metadata cbuffer name
    int metadata_cb_idx;    // Index into params->constant_buffers (-1 if unresolved)
    bool resolved;          // True if deterministically resolved
    uint8_t authority;      // 1 = stage-specific, 2 = common TypeTree
} CBufferRegMapEntry;

typedef struct {
    int DestRegIdx;
    int SrcRegIdx;
    int IntDestComp;
    int FloatDestComps[2];
    char IntVarName[64];
    char Float2VarName[64];
    int Permutation[3];
} SwizzleDecomposition;

typedef enum {
    HLSL_TYPED_LOOP_BOUND_NONE = 0,
    HLSL_TYPED_LOOP_BOUND_FTOI,
    HLSL_TYPED_LOOP_BOUND_IMAX,
    HLSL_TYPED_LOOP_BOUND_IMIN,
    HLSL_TYPED_LOOP_BOUND_ITOF
} HLSLTypedLoopBoundPhase;

#define HLSL_TYPED_LOOP_BOUND_ALIAS_SIZE 96

typedef struct {
    bool is_optimized;
    int comparison_inst_idx;
    int breakc_inst_idx;
    int inc_inst_idx;
    /* Exact compiler inverse for a scalar FTOI/IMAX/IMIN/ITOF loop bound.
     * The indices remain -1 unless the decoded stream also proves that the
     * loop-prefix comparison result is overwritten before its first read.
     * This keeps the typed source spelling confined to one certified
     * straight-line definition and one certified loop consumer. */
    int typed_bound_ftoi_idx;
    int typed_bound_imax_idx;
    int typed_bound_imin_idx;
    int typed_bound_itof_idx;
    /* Reserved once during analysis and reused verbatim by every emission
     * phase.  An empty name disables the inverse even when the instruction
     * shape otherwise matches. */
    char typed_bound_alias[HLSL_TYPED_LOOP_BOUND_ALIAS_SIZE];
} LoopOptimizationInfo;

typedef struct {
    /* -1 means unclaimed; -2 permanently records a conflicting association. */
    int loop_instruction;
    HLSLTypedLoopBoundPhase phase;
} HLSLTypedLoopBoundInstruction;

typedef struct {
    bool is_cross_mul;
    bool is_cross_mad;
    int source_instruction;
} CrossProductInfo;

typedef struct {
    bool valid;
    int transform_start;
    int tangent_start;
    int tangent_end;
    int position_input_reg;
    int normal_input_reg;
    int tangent_input_reg;
    int clip_output_reg;
    int clip_temp_reg;
    int tangent_output_regs[3];
} SurfaceTangentFrameInfo;

typedef struct {
    bool valid;
    int start;
    int end;
    int uv_input_reg;
    int output_reg;
} IndexedFaceBasisInfo;

/* The declaration that physically backs a formatted operand lane.  This is
 * deliberately independent of identifier spelling and of the value-domain
 * requested by an opcode (for example, reading raw uint storage as float).
 */
typedef enum HLSLBackingStorage {
    HLSL_BACKING_STORAGE_FLOAT = 0,
    HLSL_BACKING_STORAGE_SINT,
    HLSL_BACKING_STORAGE_UINT,
    HLSL_BACKING_STORAGE_MIXED,
    HLSL_BACKING_STORAGE_INVALID
} HLSLBackingStorage;

/* Representation produced by the opcode emitter before assignment. */
typedef enum HLSLExpressionKind {
    HLSL_EXPRESSION_FLOAT_VALUE = 0,
    HLSL_EXPRESSION_NATIVE_INTEGER,
    HLSL_EXPRESSION_INTEGER_BITS_AS_FLOAT
} HLSLExpressionKind;

typedef struct {
    int stride;
    int field_count;
    int field_alloc;
    int* offsets;
    int* widths;
} StructuredResourceLayout;

typedef char HLSLTempLaneNames[4][64];
typedef bool HLSLTempLaneFlags[4];
typedef uint8_t HLSLTempLaneStorage[4];
typedef int HLSLRegisterPermutation[4];

/* At most two pure two-MUL signatures: the product precedes or follows the
 * scale in the outer operation. A group is emitted only for repeated uses. */
typedef struct {
    int group[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT];
    int scale_operand[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT];
    unsigned use_count[2];
    size_t definition_begin[2][2];
    size_t definition_end[2][2];
} HLSLFloat4FunctionPlan;

typedef struct HLSLEmitterContext {
    const USILProgram* program;
    HLSLEmitDiagnostic* diagnostic;
    HLSLEmitMode emit_mode;
    bool omit_unity_builtin_declarations;
    bool is_vertex;
    bool is_geometry;
    const SerializedProgramParameters* params;
    const SerializedProgramParameters* common_params; // Shared/common parameters from TypeTree (m_CommonParameters)
    StringBuilder* sb;
    bool use_uint_temps;
    bool emit_unity_builtins;
    /* Borrowed from HLSLEmitOptions for this emission only. */
    const char* const* reserved_preprocessor_identifiers;
    size_t reserved_preprocessor_identifier_count;
    HLSLExpressionSourceMap *expression_source_map;
    bool unity_uv_helper;
    HLSLFloat4FunctionPlan float4_functions;
    int current_instruction_index;
    bool is_formatting_dest;
    int indent;
    bool* skip_instruction;

    // Deterministic cbuffer register → metadata name mapping
    CBufferRegMapEntry* cb_reg_map;
    int cb_reg_map_count;
    int cb_reg_map_alloc;
    HLSLCBufferLayout cbuffer_layouts[HLSL_MAX_CBUFFER_LAYOUTS];
    int cbuffer_layout_count;
    bool cbuffer_layouts_built;

    // Sampler names proven by serialized metadata or unique sample t#/s# use.
    char* sampler_names[HLSL_SM5_SAMPLER_REGISTER_COUNT];

    // Generation / Liveness Tracking. Sized to the validated program rather
    // than embedding a worst-case 16 MiB register matrix in every context.
    int* inst_src_gen;
    int* inst_dest_gen;
    int* reg_max_gen;
    int generation_instruction_count;
    int generation_register_count;

    // Swizzle/Temp Tracking
    int temp_state_count;
    SwizzleDecomposition* decompositions;
    bool* has_decomposition;
    HLSLTempLaneNames* ftoi_temps;
    HLSLTempLaneFlags* has_ftoi_temp;
    HLSLTempLaneNames* int_temps;
    HLSLTempLaneFlags* has_int_temp;
    HLSLTempLaneNames* write_redirects;
    HLSLTempLaneFlags* has_write_redirect;
    HLSLTempLaneStorage* write_redirect_storage;
    HLSLTempLaneNames* deferred_floats;
    HLSLTempLaneFlags* has_deferred_float;

    // ScreenPos Pattern Tracking
    int readable_screen_pos_mul_y_idx;
    int readable_screen_pos_mul_xzw_idx;
    int readable_screen_pos_add_idx;
    int readable_screen_pos_mov_idx;

    // Scrambled Register Tracking (dynamic simulation per instruction index)
    HLSLRegisterPermutation* inst_reg_permutation;
    bool* inst_reg_is_scrambled;
    int* modulo_divisor;
    LoopOptimizationInfo* loop_info;
    HLSLTypedLoopBoundInstruction* typed_loop_bound_instructions;
    CrossProductInfo* cross_info;
    int* saved_mul_id;
    bool* saved_mul_is_definition;
    bool* saved_mul_reverse_definition;
    int saved_mul_count;
    SurfaceTangentFrameInfo surface_tangent_frame;
    IndexedFaceBasisInfo indexed_face_basis;
    HLSLComponentProvenance *component_provenance;
    int provenance_register_count;
    HLSLControlFlowGraph cfg;
    HLSLValueAnalysis value_analysis;
    HLSLUseDefGraph use_def;
    HLSLSSAGraph ssa;
    HLSLStoragePlan storage_plan;
    HLSLSemanticProgram semantic_program;
    HLSLCompilerModelProgram compiler_model;
    const char* readable_screen_pos_helper;
} HLSLEmitterContext;

/* First-failure-wins diagnostic helpers.  They never allocate and are safe to
 * call while unwinding an allocation failure. */
void hlsl_emit_fail(HLSLEmitterContext* ctx, HLSLEmitStatus status,
                    HLSLEmitPhase phase, HLSLEmitReason reason);
void hlsl_emit_fail_instruction(HLSLEmitterContext* ctx,
                                HLSLEmitStatus status,
                                HLSLEmitReason reason,
                                int instruction_index,
                                int operand_index);
void hlsl_emit_fail_metadata(HLSLEmitterContext* ctx,
                             HLSLEmitStatus status,
                             HLSLEmitPhase phase,
                             HLSLEmitReason reason,
                             HLSLEmitMetadataSource source,
                             HLSLEmitMetadataKind kind,
                             int record_index, int member_index,
                             int register_index);
bool hlsl_emit_check_output(HLSLEmitterContext* ctx,
                            HLSLEmitPhase phase);

/* Single policy boundary for non-bijective, readability-only transforms
 * inferred from instruction fragments.  The separately validated full-
 * program geometry inverse does not use this permission. */
static inline bool hlsl_readability_transforms_enabled(
    const HLSLEmitterContext* ctx) {
    return ctx && ctx->emit_mode == HLSL_EMIT_MODE_READABLE;
}

static inline HLSLRegisterPermutation* hlsl_register_permutation_at(
    HLSLEmitterContext* ctx, int instruction, int reg) {
    return &ctx->inst_reg_permutation[
        (size_t)instruction * (size_t)ctx->temp_state_count + (size_t)reg];
}

static inline const HLSLRegisterPermutation*
hlsl_register_permutation_at_const(const HLSLEmitterContext* ctx,
                                   int instruction, int reg) {
    return &ctx->inst_reg_permutation[
        (size_t)instruction * (size_t)ctx->temp_state_count + (size_t)reg];
}

static inline bool* hlsl_register_scrambled_at(HLSLEmitterContext* ctx,
                                               int instruction, int reg) {
    return &ctx->inst_reg_is_scrambled[
        (size_t)instruction * (size_t)ctx->temp_state_count + (size_t)reg];
}

static inline bool hlsl_register_is_scrambled(
    const HLSLEmitterContext* ctx, int instruction, int reg) {
    return ctx->inst_reg_is_scrambled[
        (size_t)instruction * (size_t)ctx->temp_state_count + (size_t)reg];
}

extern const BuiltinVariable g_builtins[];
extern const size_t g_builtins_count;
#define G_BUILTINS_COUNT g_builtins_count

// Top-level emission phases. Each phase owns one coherent HLSL section.
void emit_comments_and_icb(HLSLEmitterContext* ctx);
void emit_cbuffers(HLSLEmitterContext* ctx);
void emit_cbuffer_helpers(HLSLEmitterContext* ctx);
bool build_cbuffer_emission_layouts(HLSLEmitterContext* ctx);
void free_cbuffer_emission_layouts(HLSLEmitterContext* ctx);
const HLSLCBufferLayout* get_cbuffer_emission_layout(
    const HLSLEmitterContext* ctx, int reg);
void emit_resources(HLSLEmitterContext* ctx);
bool hlsl_texture_declaration_supported(const USILTexture* texture);
bool hlsl_uav_declaration_supported(const USILUav* uav);
bool hlsl_structured_atomic_address(const USILProgram* program,
                                    const USILInstruction* instruction,
                                    int* uav_register, int* byte_offset);
bool get_structured_resource_layout(const USILProgram* program, int reg,
                                    StructuredResourceLayout* layout);
void free_structured_resource_layout(StructuredResourceLayout* layout);
void emit_io_structs(HLSLEmitterContext* ctx, const char* input_struct,
                     const char* output_struct);
void emit_entry_point_declarations(HLSLEmitterContext* ctx,
                                   const char* entry_point,
                                   const char* input_struct,
                                   const char* output_struct,
                                   const bool inputs_used[HLSL_SM5_IO_REGISTER_COUNT],
                                   const bool outputs_used[HLSL_SM5_IO_REGISTER_COUNT]);
/* Exact compiler-inverse lift for the canonical triangle edge-distance DAG.
 * The predicate checks the complete opcode/operand graph; it is independent
 * of shader names, paths, and bytecode hashes. */
bool hlsl_triangle_edge_distance_lift_matches(const USILProgram* program);
/* Exact compiler-inverse lift for the canonical six-vertex extrusion
 * geometry DAG produced by two fixed trip-count loops, object-to-clip
 * transforms, a normalized face cross product, and two explicit stream
 * cuts.  Admission checks the complete opcode/operand graph rather than an
 * asset name or bytecode digest. */
bool hlsl_extruded_triangle_lift_matches(const USILProgram* program);
/* Exact compiler inverse for the ray/box slab-intersection DAG.  The matcher
 * proves the reciprocal, paired near/far products, min/max reduction, and
 * packed three-intersection discard chain before any source lift is emitted. */
bool hlsl_ray_box_intersection_lift_matches(const USILProgram* program);
/* Exact continuation of the ray/box inverse through a three-filter volume
 * sample diamond and its normal-map unpack/saturate join.  This predicate
 * validates the complete decoded operand graph; names and asset identity are
 * deliberately outside the contract. */
bool hlsl_volume_slice_sampling_lift_matches(const USILProgram* program);
void emit_exact_structural_helpers(HLSLEmitterContext* ctx);
void emit_instructions(HLSLEmitterContext* ctx);
bool emit_high_level_expressions(HLSLEmitterContext* ctx);
bool emit_high_level_functions(HLSLEmitterContext *ctx);
ASTExpr *hlsl_float4_function_call(HLSLEmitterContext *ctx, int instruction);
bool hlsl_float4_validate_expressions(HLSLEmitterContext *ctx,
                                     unsigned uses[HLSL_HIGH_LEVEL_INSTRUCTION_LIMIT]);
bool emit_high_level_structured(HLSLEmitterContext *ctx);
void hlsl_expression_source_map_begin(HLSLEmitterContext *ctx);
/* Shared closed float4 contracts and compiler inverse AST construction. */
bool hlsl_lift_operand_is_plain(const DXBCOperand *value);
HLSLEmitReason hlsl_float4_program_contract(const USILProgram *program);
bool hlsl_float4_program_supported(HLSLEmitterContext *ctx);
bool emit_unity_uv_lift(HLSLEmitterContext *ctx);
bool hlsl_float4_instruction_supported(HLSLEmitterContext *ctx, int instruction);
bool hlsl_float4_append_output(HLSLEmitterContext *ctx, const DXBCOperand *destination);
ASTExpr *hlsl_float4_source_atom(HLSLEmitterContext *ctx, const DXBCOperand *source);
/* Consumes both children on success and failure. MOV expects a NULL right. */
ASTExpr *hlsl_float4_operation(HLSLEmitterContext *ctx, int instruction, ASTExpr *left,
                              ASTExpr *right);

bool hlsl_expression_identifiers_available(HLSLEmitterContext* ctx, size_t source_start);
void emit_return_block(HLSLEmitterContext* ctx);

/* Complete, source-backed tessellation compiler inverses. Hull/domain stages
 * outside these exact structural contracts remain fail-closed. */
bool hlsl_exact_tessellation_lift_matches(const USILProgram* program);
bool hlsl_emit_exact_tessellation_stage(HLSLEmitterContext* ctx,
                                        const char* entry_point);
bool hlsl_output_field_name(HLSLEmitterContext* ctx,
                            const DXBCSignatureElement* element,
                            int element_index, char* name,
                            size_t name_size);

// Deterministic dataflow and pattern analysis (hlsl_emitter_analysis.c)
bool RunAnalysisPasses(HLSLEmitterContext* ctx);
bool build_cbuffer_register_map(HLSLEmitterContext* ctx);
bool build_sampler_name_map(HLSLEmitterContext* ctx);
void free_sampler_name_map(HLSLEmitterContext* ctx);
void get_sampler_usage(const USILProgram *program, int sampler_reg,
                       bool *uses_regular, bool *uses_comparison);
typedef void* (*HLSLCallocFunction)(size_t count, size_t element_size);
bool allocate_hlsl_generation_state(HLSLEmitterContext* ctx,
                                    HLSLCallocFunction allocate_zeroed);
void free_hlsl_generation_state(HLSLEmitterContext* ctx);
bool analyze_live_ranges(HLSLEmitterContext* ctx);
int hlsl_instruction_generation(HLSLEmitterContext* ctx, bool destination,
                                int instruction, int reg);
int hlsl_register_max_generation(HLSLEmitterContext* ctx, int reg);
void detect_readable_screen_pos_pattern(HLSLEmitterContext* ctx);
void PreScanSwizzleDecomposition(HLSLEmitterContext* ctx);
void PreScanScrambledRegisters(HLSLEmitterContext* ctx);
void detect_signed_modulo_pattern(HLSLEmitterContext* ctx);
void detect_loop_patterns(HLSLEmitterContext* ctx);
void detect_cross_product_patterns(HLSLEmitterContext* ctx);
void detect_reused_multiply_expressions(HLSLEmitterContext* ctx);
void detect_surface_tangent_frame(HLSLEmitterContext* ctx);
void detect_indexed_face_basis(HLSLEmitterContext* ctx);
bool build_component_provenance(HLSLEmitterContext* ctx);
void free_component_provenance(HLSLEmitterContext* ctx);
const HLSLComponentProvenance *get_component_provenance(
    const HLSLEmitterContext *ctx, int state, int reg, int component);
bool component_value_unchanged(const HLSLEmitterContext *ctx, int reg,
                               int component, int first_state,
                               int second_state);
bool analyze_semantic_lifts(HLSLEmitterContext* ctx);
void free_semantic_lifts(HLSLEmitterContext* ctx);
bool emit_semantic_lift_before(HLSLEmitterContext* ctx, int instruction);
void emit_semantic_prelude(HLSLEmitterContext* ctx);
void emit_semantic_lift_after(HLSLEmitterContext* ctx, int instruction);
int semantic_alpha_clip_first_operand(const HLSLEmitterContext *ctx,
                                      int multiply_instruction);
bool semantic_instruction_has_flag(const HLSLEmitterContext *ctx,
                                   int instruction, unsigned int flag);
int semantic_binary_operand_order(const HLSLEmitterContext *ctx,
                                  int instruction);
bool semantic_truthiness_condition_alias(HLSLEmitterContext *ctx,
                                   int instruction, int operand,
                                   int component, char *alias,
                                   size_t alias_size);
bool is_register_decomposed(HLSLEmitterContext* ctx, int regIdx);
bool is_increment_of(const USILInstruction *inst, const DXBCOperand *loop_counter);

// Instruction Translation (hlsl_emitter_ops.c)
void hlsl_emit_instruction(HLSLEmitterContext* ctx, const USILInstruction* inst);

// Helper Query functions (hlsl_emitter_metadata.c)
uint32_t get_var_occupied_regs(uint32_t is_matrix, uint32_t rows, uint32_t matrix_array_size);
bool resolve_variable_info(const SerializedProgramParameters* params, const char* var_name, uint32_t* out_dim, uint32_t* out_byte_offset, bool* out_is_matrix_or_array);
bool resolve_variable_layout_ctx(const HLSLEmitterContext* ctx,
                                 const char* var_name,
                                 DecodedVariableLayout* out_layout);
int resolve_variable_type(const HLSLEmitterContext* ctx, const char* var_name);
bool resolve_variable_is_matrix(const HLSLEmitterContext* ctx, const char* var_name);
bool resolve_variable_is_row_major(const HLSLEmitterContext* ctx, const char* var_name);
const char* get_cbuffer_name_from_map(const HLSLEmitterContext* ctx, int cb_reg);
const char* get_cbuffer_name(const SerializedProgramParameters* params, int cb_reg);
const char* resolve_cb_variable_ctx(const HLSLEmitterContext* ctx, int cb_reg, int float4_offset, int component_hint, int* out_var_offset);
const char* resolve_cb_variable(const SerializedProgramParameters* params, int cb_reg, int float4_offset, int component_hint, int* out_var_offset);
/* Resolves exactly one serialized SRV binding of the requested Unity kind.
 * TEXTURE and BUFFER share D3D's t-register namespace but are distinct
 * reflection records.  A missing binding is successful with *out_name NULL;
 * malformed or duplicate authority fails. */
bool resolve_srv_name(const SerializedProgramParameters* params, int reg_idx,
                      SerializedResourceType bind_type,
                      const char** out_name);
/* Applies the stage-parameters-first authority rule without allowing an
 * invalid stage record to fall through to common parameters. */
bool resolve_srv_name_ctx(const HLSLEmitterContext* ctx, int reg_idx,
                          SerializedResourceType bind_type,
                          const char** out_name);
/* Texture-only compatibility query for sampling paths and focused tests.
 * Ambiguous or malformed texture authority returns NULL. */
const char* resolve_texture_name(const SerializedProgramParameters* params,
                                 int reg_idx);
/* Resolves an exact, malloc-owned HLSL sampler identifier. Serialized inline
 * state outranks texture pairing. A successful unresolved/ambiguous texture
 * lookup returns true with *out_name == NULL; malformed inline authority
 * returns false. */
bool resolve_sampler_name(const SerializedProgramParameters* params,
                          int reg_idx, char** out_name);
const char* resolve_uav_name(const SerializedProgramParameters* params, int reg_idx);
bool is_unity_builtin_cbuffer(const char* name);
bool is_unity_builtin_variable_ex(const char* var_name,
                                  const SerializedProgramParameters* common_params);
const char* resolve_builtin_cb_name_for_reg(const SerializedProgramParameters* params, int cb_reg);

// Formatting / Printing Helpers (hlsl_emitter_format.c)
void sb_append_spaces(StringBuilder* sb, int count);
bool format_float(float f, char* buf, size_t buf_sz);
bool format_immediate_hlsl_masked(const DXBCOperand* op, bool isInt,
                                  bool isUint, int write_mask, char* buf,
                                  size_t buf_sz);
bool format_immediate_hlsl(const DXBCOperand* op, bool isInt, bool isUint,
                           char* buf, size_t buf_sz);
bool format_swizzle_hlsl(const DXBCOperand* op, int write_mask,
                         bool preserve_vector, char* buf, size_t buf_sz);
const char* get_packoffset_suffix(uint32_t byte_offset);
bool format_cb_swizzle(const DXBCOperand* op, uint32_t dim,
                       uint32_t byte_offset, int write_mask,
                       bool preserve_vector, char* buf, size_t buf_sz);
bool format_operand_hlsl_sb(HLSLEmitterContext* ctx, const DXBCOperand* op,
                            bool isInt, bool isUint, int write_mask,
                            bool preserve_vector, StringBuilder* output);
void format_operand_hlsl(HLSLEmitterContext* ctx, const DXBCOperand* op, bool isInt, bool isUint, int write_mask, bool preserve_vector, char* buf, size_t buf_sz);
bool format_dest_operand_hlsl_sb(HLSLEmitterContext* ctx,
                                 const DXBCOperand* op, bool isInt,
                                 bool isUint, int write_mask,
                                 bool preserve_vector,
                                 StringBuilder* output);
void format_dest_operand_hlsl(HLSLEmitterContext* ctx, const DXBCOperand* op, bool isInt, bool isUint, int write_mask, bool preserve_vector, char* buf, size_t buf_sz);

// Common utilities/predicates (hlsl_emitter_utils.c)
#if defined(__GNUC__) || defined(__clang__)
#define DXBC_PRINTF_FORMAT(format_index, first_argument) \
  __attribute__((format(printf, format_index, first_argument)))
#else
#define DXBC_PRINTF_FORMAT(format_index, first_argument)
#endif
bool hlsl_format_checked(HLSLEmitterContext* ctx, char* buf, size_t buf_sz,
                         const char* format, ...)
    DXBC_PRINTF_FORMAT(4, 5);
bool hlsl_copy_checked(HLSLEmitterContext* ctx, char* buf, size_t buf_sz,
                       const char* value);
const char* get_type_str(uint32_t comp_type, int components);
uint32_t hlsl_output_register_component_type(const USILProgram* program,
                                             uint32_t register_index);
int get_mask_component_count(int mask);
bool is_mask_non_contiguous(int mask);
bool has_non_contiguous_swizzle(const DXBCOperand* op, int write_mask);
bool is_signed_int_op(USILOpcode opcode);
bool is_unsigned_int_op(USILOpcode opcode);
const char* get_cast_type_str(const char* base_type, int comps);
bool is_replicate_swizzle(const DXBCOperand* op);
bool is_operand_scalar(const DXBCOperand* op, const SerializedProgramParameters* params);
int get_operand_priority(const DXBCOperand* op);
bool is_non_canonical_add(const DXBCOperand* op0, const DXBCOperand* op1);
bool is_register_program_output(HLSLEmitterContext* ctx, int reg_idx);
bool is_scalar_integer_op(USILOpcode op);
bool is_componentwise_op(USILOpcode op);
bool is_cross_product_pattern(const USILProgram *program, int idx, int *out_op1_reg, int *out_op2_reg);
bool inst_writes_to_dest(const USILInstruction* inst);
HLSLBackingStorage hlsl_operand_backing_storage(
    const HLSLEmitterContext* ctx, const DXBCOperand* operand,
    int write_mask, bool destination);
void format_native_operand_hlsl(HLSLEmitterContext* ctx,
                                const DXBCOperand* operand, int write_mask,
                                bool preserve_vector, char* buffer,
                                size_t buffer_size);
bool format_native_operand_hlsl_sb(HLSLEmitterContext* ctx,
                                   const DXBCOperand* operand,
                                   int write_mask, bool preserve_vector,
                                   StringBuilder* output);
void format_native_dest_operand_hlsl(HLSLEmitterContext* ctx,
                                     const DXBCOperand* operand,
                                     int write_mask, bool preserve_vector,
                                     char* buffer, size_t buffer_size);
bool format_native_dest_operand_hlsl_sb(HLSLEmitterContext* ctx,
                                        const DXBCOperand* operand,
                                        int write_mask,
                                        bool preserve_vector,
                                        StringBuilder* output);
HLSLExpressionKind hlsl_instruction_expression_kind(
    const HLSLEmitterContext* ctx, const USILInstruction* instruction);
bool hlsl_retarget_expression_for_storage(
    HLSLEmitterContext* ctx, HLSLExpressionKind expression_kind,
    HLSLBackingStorage destination_storage, char* expression);
bool build_control_flow_graph(HLSLEmitterContext* ctx);
bool instructions_have_unambiguous_path(const HLSLEmitterContext* ctx,
                                        int first, int second);
void free_control_flow_graph(HLSLEmitterContext* ctx);
bool analyze_lane_value_types(HLSLEmitterContext* ctx);
bool build_hlsl_use_def_graph(HLSLEmitterContext* ctx);
int hlsl_operand_definition(const HLSLEmitterContext* ctx, int instruction,
                            int operand, int component);
unsigned int hlsl_definition_use_count(const HLSLEmitterContext* ctx,
                                       int instruction, int component);
void free_hlsl_use_def_graph(HLSLEmitterContext* ctx);
unsigned int get_lane_value_facts(const HLSLEmitterContext* ctx, int state,
                                  int reg, int component);
unsigned int get_operand_value_facts(const HLSLEmitterContext* ctx,
                                     int instruction, int operand,
                                     int component);
void free_lane_value_types(HLSLEmitterContext* ctx);
bool build_hlsl_storage_plan(HLSLEmitterContext* ctx);
bool temp_register_requires_raw_storage(const HLSLEmitterContext* ctx,
                                        int reg);
void free_hlsl_storage_plan(HLSLEmitterContext* ctx);
bool analyze_d3dcompiler_model(HLSLEmitterContext* ctx);
bool build_d3dcompiler_model(HLSLEmitterContext* ctx);
const LoopOptimizationInfo* hlsl_typed_loop_bound_for_instruction(
    const HLSLEmitterContext* ctx, int instruction,
    HLSLTypedLoopBoundPhase* out_phase);
bool compiler_add_uses_mad(const USILInstruction* instruction);
bool compiler_model_swaps_binary_operands(const HLSLEmitterContext* ctx,
                                          int instruction);
bool compiler_model_preserves_vector_output(const HLSLEmitterContext* ctx,
                                            int instruction);
bool hlsl_compiler_tangent_frame_matches(const USILProgram* program,
                                         int instruction);
bool hlsl_compiler_screen_position_matches(const USILProgram* program,
                                           int clip_instruction,
                                           int* out_screen_instruction);
bool hlsl_compiler_split_matrix_transform_matches(
    const USILProgram* program, int clip_instruction,
    int* out_world_instruction);
bool hlsl_compiler_cross_product_mul_reverses_source(
    const USILProgram* program, int instruction);
bool emit_compiler_split_matrix_transform(HLSLEmitterContext* ctx,
                                          int instruction);
bool emit_compiler_tangent_frame_lowering(HLSLEmitterContext* ctx,
                                          int instruction);
bool emit_compiler_screen_position_lowering(HLSLEmitterContext* ctx,
                                            int instruction);
void free_d3dcompiler_model(HLSLEmitterContext* ctx);
void get_signature_swizzle(const DXBCSignatureElement* el, char* out_swizzle, size_t max_sz);
uint8_t hlsl_output_written_mask(const USILProgram* program,
                                 const DXBCSignatureElement* element);
void emit_copy_back_outputs(HLSLEmitterContext* ctx, bool first_line_already_indented);
const DXBCSignatureElement* find_matching_output_signature(const USILProgram* program, uint32_t register_id, int mask);
#endif // HLSL_EMITTER_INTERNAL_H
