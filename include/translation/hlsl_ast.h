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
    AST_EXPR_CAST
} ASTExprKind;

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
    bool is_float;
    bool is_uint;
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

typedef struct ASTExpr {
    ASTExprKind kind;
    union {
        ASTVar var;
        ASTLiteral literal;
        ASTUnaryExpr unary;
        ASTBinaryExpr binary;
        ASTTernaryExpr ternary;
        ASTSwizzleExpr swizzle;
        ASTCallExpr call;
        ASTCastExpr cast;
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

// AST Expression Constructors
ASTExpr* ast_create_var(int ssa_var, int reg, int type, const char *name);
ASTExpr* ast_create_literal_float(float f);
ASTExpr* ast_create_literal_int(int i);
ASTExpr* ast_create_unary(int op, ASTExpr *sub);
ASTExpr* ast_create_binary(int op, ASTExpr *left, ASTExpr *right);
ASTExpr* ast_create_ternary(ASTExpr *cond, ASTExpr *true_expr, ASTExpr *false_expr);
ASTExpr* ast_create_swizzle(ASTExpr *sub, const int *swizzle, int count);
ASTExpr* ast_create_call(const char *name, ASTExpr **args, int count);
ASTExpr* ast_create_cast(const char *type_name, ASTExpr *sub);
void ast_free_expr(ASTExpr *expr);

// AST Statement Constructors
ASTStmt* ast_create_block(void);
void ast_block_add(ASTStmt *block, ASTStmt *stmt);
ASTStmt* ast_create_assign(ASTExpr *dest, ASTExpr *src);
ASTStmt* ast_create_if(ASTExpr *cond, ASTStmt *true_body, ASTStmt *false_body);
ASTStmt* ast_create_loop(ASTExpr *cond, ASTStmt *body);
ASTStmt* ast_create_flow(ASTStmtKind kind);
void ast_free_stmt(ASTStmt *stmt);

void ast_format_expr(const ASTExpr *expr, StringBuilder *sb);
void ast_format_stmt(const ASTStmt *stmt, StringBuilder *sb, int indent);

#endif // HLSL_AST_H
