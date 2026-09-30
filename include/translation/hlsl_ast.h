// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_AST_H
#define HLSL_AST_H

#include <stdbool.h>
#include <stdint.h>
#include "common/string_builder.h"

typedef enum {
    AST_EXPR_VAR,
    AST_EXPR_LITERAL,
    AST_EXPR_UNARY,
    AST_EXPR_BINARY,
    AST_EXPR_TERNARY,
    AST_EXPR_SWIZZLE,
    AST_EXPR_CALL,
    AST_EXPR_CAST,
    AST_EXPR_BITCAST,
    AST_EXPR_EMITTER_OPERAND,
    AST_EXPR_COMPARISON
} ASTExprKind;

typedef enum { AST_SCALAR_FLOAT32, AST_SCALAR_SINT32, AST_SCALAR_UINT32, AST_SCALAR_BOOL } ASTScalarType;

typedef enum {
    AST_OPERAND_VALUE_UNKNOWN = 0,
    AST_OPERAND_VALUE_LOGICAL,
    AST_OPERAND_VALUE_REGISTER
} ASTOperandValueRole;

typedef enum {
    AST_COMPONENT_SELECTION_NONE = 0,
    AST_COMPONENT_SELECTION_SEMANTIC,
    AST_COMPONENT_SELECTION_TRANSPORT
} ASTComponentSelectionRole;

typedef enum {
    AST_OPERAND_BITCAST_NONE = 0,
    AST_OPERAND_BITCAST_PROGRAM,
    AST_OPERAND_BITCAST_STORAGE
} ASTOperandBitcastRole;

/* By-value authority for the validated operand formatter boundary. The producer
 * must account for the entire spelling, including hidden helper/bitcast/lane
 * operations. UNKNOWN never supplies source-quality authority. Natural/result
 * widths are 1..4 for scalar/vector values, or both zero for aggregates. A
 * semantic selection names components of a logical value; it is not permission
 * to relabel a register shuffle. No pointers to transient operands are retained. */
typedef struct {
    bool complete;
    ASTOperandValueRole value_role;
    uint64_t logical_value_id;
    uint8_t natural_components;
    uint8_t result_components;
    ASTComponentSelectionRole selection_role;
    uint8_t selected_components[4];
    ASTOperandBitcastRole bitcast_role;
    bool raw_buffer_reconstruction;
    bool synthetic_interface;
    int instruction_index;
    uint32_t source_instruction_index;
    int operand_index;
    uint8_t destination_lanes;
} ASTOperandProvenance;

void ast_operand_provenance_init(ASTOperandProvenance *provenance);

/* Owned result facts for a recovered logical expression. The producer must
 * derive type, width and instruction ownership from the logical-value plan;
 * a source spelling, call name or register index is not authority. Children
 * retain their own origins, including for inlined operations. The defaults
 * carry no authority. Scalar/vector widths are limited to 1..4 here. */
typedef struct {
    bool complete;
    ASTScalarType scalar_type;
    uint8_t components;
    uint64_t logical_value_id;
    int instruction_index;
    uint32_t source_instruction_index;
    uint8_t destination_lanes;
    bool semantic_projection;
    bool program_bitcast;
} ASTLogicalValueOrigin;

void ast_logical_value_origin_init(ASTLogicalValueOrigin *origin);

typedef enum {
    AST_STMT_BLOCK,
    AST_STMT_ASSIGN,
    AST_STMT_IF,
    AST_STMT_LOOP,
    AST_STMT_BREAK,
    AST_STMT_CONTINUE,
    AST_STMT_RETURN,
    AST_STMT_DISCARD
} ASTStmtKind;

struct ASTExpr;

typedef struct {
    int ssa_var;
    int register_index;
    int operand_type;
    /* Owned, exact spelling.  Serialized identifiers are not bounded by the
     * DXBC register model and must never be silently truncated. */
    char *name;
} ASTVar;

typedef struct {
    uint32_t val[4];
    int components;
    ASTScalarType scalar_type;
} ASTLiteral;

typedef struct {
    int op;
    struct ASTExpr *sub;
} ASTUnaryExpr;

typedef struct {
    int op;
    struct ASTExpr *left;
    struct ASTExpr *right;
} ASTBinaryExpr;

typedef struct {
    struct ASTExpr *cond;
    struct ASTExpr *true_expr;
    struct ASTExpr *false_expr;
} ASTTernaryExpr;

typedef struct {
    struct ASTExpr *sub;
    int swizzle[4];
    int swizzle_count;
} ASTSwizzleExpr;

typedef struct {
    char *name;
    struct ASTExpr **args;
    int arg_count;
} ASTCallExpr;

typedef struct {
    char *type_name;
    struct ASTExpr *sub;
} ASTCastExpr;

typedef struct {
    ASTScalarType scalar_type;
    struct ASTExpr *sub;
} ASTBitcastExpr;

typedef struct ASTExpr {
    ASTExprKind kind;
    /* Meaningful only for EMITTER_OPERAND; copied and owned with this node. */
    ASTOperandProvenance operand_provenance;
    ASTLogicalValueOrigin logical_origin;
    union {
        ASTVar var;
        ASTLiteral literal;
        ASTUnaryExpr unary;
        ASTBinaryExpr binary;
        ASTTernaryExpr ternary;
        ASTSwizzleExpr swizzle;
        ASTCallExpr call;
        ASTCastExpr cast;
        ASTBitcastExpr bitcast;
        char *emitter_operand;
    } u;
} ASTExpr;

struct ASTStmt;

typedef struct {
    struct ASTStmt **stmts;
    int stmt_count;
} ASTBlockStmt;

typedef struct {
    ASTExpr *dest;
    ASTExpr *src;
} ASTAssignStmt;

typedef struct {
    ASTExpr *cond;
    struct ASTStmt *true_body;
    struct ASTStmt *false_body;
} ASTIfStmt;

typedef struct {
    ASTExpr *cond;
    struct ASTStmt *body;
} ASTLoopStmt;

typedef struct ASTStmt {
    ASTStmtKind kind;
    union {
        ASTBlockStmt block;
        ASTAssignStmt assign;
        ASTIfStmt if_stmt;
        ASTLoopStmt loop;
    } u;
} ASTStmt;

/* Successful constructors own their children; a failed constructor leaves
 * ownership with the caller. Children form a tree and must not be shared.
 * These nodes preserve source operations; they do not prove a DXBC lift. */
ASTExpr *ast_create_var(int ssa_var, int reg, int type, const char *name);
ASTExpr *ast_create_literal_bits(const uint32_t *bits, int components, ASTScalarType scalar_type);
ASTExpr *ast_create_literal_float(float f);
ASTExpr *ast_create_literal_int(int i);
/* Operators are an explicitly admitted subset of USILOpcode: unary INEG/NOT;
 * binary ADD/SUB/MUL/DIV/IADD/AND/OR/XOR/ISHL/ISHR/USHR. Comparisons that produce
 * DXBC masks and multi-result integer operations require separate lowering.
 * Callers must supply the correct HLSL operand types, including unsigned USHR
 * inputs; this syntax layer is not a DXBC type/provenance analysis. */
ASTExpr *ast_create_unary(int op, ASTExpr *sub);
ASTExpr *ast_create_binary(int op, ASTExpr *left, ASTExpr *right);
/* Scalar logical predicate only. DXBC mask values need separate lowering. */
ASTExpr *ast_create_comparison(int op, ASTExpr *left, ASTExpr *right);
ASTExpr *ast_create_ternary(ASTExpr *cond, ASTExpr *true_expr, ASTExpr *false_expr);
ASTExpr *ast_create_swizzle(ASTExpr *sub, const int *swizzle, int count);
ASTExpr *ast_create_call(const char *name, ASTExpr **args, int count);
ASTExpr *ast_create_cast(const char *type_name, ASTExpr *sub);
/* Numeric conversion uses CAST; bit reinterpretation uses BITCAST. */
ASTExpr *ast_create_bitcast(ASTScalarType scalar_type, ASTExpr *sub);
/* Trusted expression text from the existing validated operand formatter.
 * This is an explicit boundary for interface/ABI syntax not modeled by this
 * AST. It owns a copy and always prints parentheses. Never pass source loaded
 * from a file or user text: this constructor is not an HLSL parser. */
ASTExpr *ast_create_emitter_operand(const char *expression);
ASTExpr *ast_create_emitter_operand_with_provenance(
    const char *expression, const ASTOperandProvenance *provenance);
/* Copies validated facts. Failure leaves the previous origin unchanged.
 * EMITTER_OPERAND uses the fuller operand-provenance constructor instead. */
bool ast_set_logical_value_origin(ASTExpr *expression,
                                  const ASTLogicalValueOrigin *origin);
void ast_free_expr(ASTExpr *expr);

// AST Statement Constructors
ASTStmt *ast_create_block(void);
/* On failure the block is unchanged and the caller still owns stmt. */
bool ast_block_add(ASTStmt *block, ASTStmt *stmt);
ASTStmt *ast_create_assign(ASTExpr *dest, ASTExpr *src);
ASTStmt *ast_create_if(ASTExpr *cond, ASTStmt *true_body, ASTStmt *false_body);
ASTStmt *ast_create_loop(ASTExpr *cond, ASTStmt *body);
ASTStmt *ast_create_flow(ASTStmtKind kind);
void ast_free_stmt(ASTStmt *stmt);

/* Visits each emitted node after its children with byte offsets in sb.
 * A false observer result fails the builder. Callbacks must not mutate the
 * tree or builder; offsets are valid only if the entire formatting succeeds. */
typedef bool (*ASTExprSpanObserver)(void *context, const ASTExpr *expression, size_t begin,
                                    size_t end);
void ast_format_expr_traced(const ASTExpr *expr, StringBuilder *sb, ASTExprSpanObserver observer,
                            void *context);
void ast_format_expr(const ASTExpr *expr, StringBuilder *sb);
void ast_format_stmt(const ASTStmt *stmt, StringBuilder *sb, int indent);

#endif // HLSL_AST_H
