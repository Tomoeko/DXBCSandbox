// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_ast.h"
#include "common/string_builder.h"
#include <stdlib.h>
#include <string.h>

static char *ast_duplicate_string(const char *value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1u);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1u);
    return copy;
}

ASTExpr* ast_create_var(int ssa_var, int reg, int type, const char *name) {
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_VAR;
    expr->u.var.ssa_var = ssa_var;
    expr->u.var.register_index = reg;
    expr->u.var.operand_type = type;
    expr->u.var.name = name ? ast_duplicate_string(name) : NULL;
    if (name && !expr->u.var.name) {
        free(expr);
        return NULL;
    }
    return expr;
}

ASTExpr* ast_create_literal_float(float f) {
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_LITERAL;
    memcpy(&expr->u.literal.val[0], &f, sizeof(float));
    expr->u.literal.components = 1;
    expr->u.literal.is_float = true;
    expr->u.literal.is_uint = false;
    return expr;
}

ASTExpr* ast_create_literal_int(int i) {
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_LITERAL;
    expr->u.literal.val[0] = (uint32_t)i;
    expr->u.literal.components = 1;
    expr->u.literal.is_float = false;
    expr->u.literal.is_uint = false;
    return expr;
}

ASTExpr* ast_create_unary(int op, ASTExpr *sub) {
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_UNARY;
    expr->u.unary.op = op;
    expr->u.unary.sub = sub;
    return expr;
}

ASTExpr* ast_create_binary(int op, ASTExpr *left, ASTExpr *right) {
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_BINARY;
    expr->u.binary.op = op;
    expr->u.binary.left = left;
    expr->u.binary.right = right;
    return expr;
}

ASTExpr* ast_create_ternary(ASTExpr *cond, ASTExpr *true_expr, ASTExpr *false_expr) {
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_TERNARY;
    expr->u.ternary.cond = cond;
    expr->u.ternary.true_expr = true_expr;
    expr->u.ternary.false_expr = false_expr;
    return expr;
}

ASTExpr* ast_create_swizzle(ASTExpr *sub, const int *swizzle, int count) {
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_SWIZZLE;
    expr->u.swizzle.sub = sub;
    expr->u.swizzle.swizzle_count = count > 4 ? 4 : count;
    for (int i = 0; i < expr->u.swizzle.swizzle_count; i++) {
        expr->u.swizzle.swizzle[i] = swizzle[i];
    }
    return expr;
}

ASTExpr* ast_create_call(const char *name, ASTExpr **args, int count) {
    if (!name || count < 0 || (count > 0 && !args) ||
        (size_t)count > SIZE_MAX / sizeof(ASTExpr*)) return NULL;
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_CALL;
    expr->u.call.name = ast_duplicate_string(name);
    if (!expr->u.call.name) {
        free(expr);
        return NULL;
    }
    expr->u.call.arg_count = count;
    expr->u.call.args = count > 0 ? malloc((size_t)count * sizeof(ASTExpr*)) : NULL;
    if (count > 0 && !expr->u.call.args) {
        free(expr->u.call.name);
        free(expr);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        expr->u.call.args[i] = args[i];
    }
    return expr;
}

ASTExpr* ast_create_cast(const char *type_name, ASTExpr *sub) {
    if (!type_name) return NULL;
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr) return NULL;
    expr->kind = AST_EXPR_CAST;
    expr->u.cast.type_name = ast_duplicate_string(type_name);
    if (!expr->u.cast.type_name) {
        free(expr);
        return NULL;
    }
    expr->u.cast.sub = sub;
    return expr;
}

void ast_free_expr(ASTExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_EXPR_VAR:
            free(expr->u.var.name);
            break;
        case AST_EXPR_UNARY:
            ast_free_expr(expr->u.unary.sub);
            break;
        case AST_EXPR_BINARY:
            ast_free_expr(expr->u.binary.left);
            ast_free_expr(expr->u.binary.right);
            break;
        case AST_EXPR_TERNARY:
            ast_free_expr(expr->u.ternary.cond);
            ast_free_expr(expr->u.ternary.true_expr);
            ast_free_expr(expr->u.ternary.false_expr);
            break;
        case AST_EXPR_SWIZZLE:
            ast_free_expr(expr->u.swizzle.sub);
            break;
        case AST_EXPR_CALL:
            for (int i = 0; i < expr->u.call.arg_count; i++) {
                ast_free_expr(expr->u.call.args[i]);
            }
            free(expr->u.call.args);
            free(expr->u.call.name);
            break;
        case AST_EXPR_CAST:
            ast_free_expr(expr->u.cast.sub);
            free(expr->u.cast.type_name);
            break;
        default:
            break;
    }
    free(expr);
}

ASTStmt* ast_create_block(void) {
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt) return NULL;
    stmt->kind = AST_STMT_BLOCK;
    stmt->u.block.stmts = NULL;
    stmt->u.block.stmt_count = 0;
    return stmt;
}

void ast_block_add(ASTStmt *block, ASTStmt *stmt) {
    if (!block || !stmt || block->kind != AST_STMT_BLOCK) return;
    block->u.block.stmts = realloc(block->u.block.stmts, (size_t)(block->u.block.stmt_count + 1) * sizeof(ASTStmt*));
    block->u.block.stmts[block->u.block.stmt_count++] = stmt;
}

ASTStmt* ast_create_assign(ASTExpr *dest, ASTExpr *src) {
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt) return NULL;
    stmt->kind = AST_STMT_ASSIGN;
    stmt->u.assign.dest = dest;
    stmt->u.assign.src = src;
    return stmt;
}

ASTStmt* ast_create_if(ASTExpr *cond, ASTStmt *true_body, ASTStmt *false_body) {
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt) return NULL;
    stmt->kind = AST_STMT_IF;
    stmt->u.if_stmt.cond = cond;
    stmt->u.if_stmt.true_body = true_body;
    stmt->u.if_stmt.false_body = false_body;
    return stmt;
}

ASTStmt* ast_create_loop(ASTExpr *cond, ASTStmt *body) {
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt) return NULL;
    stmt->kind = AST_STMT_LOOP;
    stmt->u.loop.cond = cond;
    stmt->u.loop.body = body;
    return stmt;
}

ASTStmt* ast_create_flow(ASTStmtKind kind) {
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt) return NULL;
    stmt->kind = kind;
    return stmt;
}

void ast_free_stmt(ASTStmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case AST_STMT_BLOCK:
            for (int i = 0; i < stmt->u.block.stmt_count; i++) {
                ast_free_stmt(stmt->u.block.stmts[i]);
            }
            free(stmt->u.block.stmts);
            break;
        case AST_STMT_ASSIGN:
            ast_free_expr(stmt->u.assign.dest);
            ast_free_expr(stmt->u.assign.src);
            break;
        case AST_STMT_IF:
            ast_free_expr(stmt->u.if_stmt.cond);
            ast_free_stmt(stmt->u.if_stmt.true_body);
            ast_free_stmt(stmt->u.if_stmt.false_body);
            break;
        case AST_STMT_LOOP:
            ast_free_expr(stmt->u.loop.cond);
            ast_free_stmt(stmt->u.loop.body);
            break;
        default:
            break;
    }
    free(stmt);
}

void ast_format_expr(const ASTExpr *expr, StringBuilder *sb) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_EXPR_VAR:
            if (expr->u.var.name && expr->u.var.name[0] != '\0') {
                sb_append(sb, expr->u.var.name);
            } else {
                if (expr->u.var.ssa_var >= 0) {
                    sb_appendf(sb, "r%d_%d", expr->u.var.register_index,
                               expr->u.var.ssa_var);
                } else {
                    sb_appendf(sb, "r%d", expr->u.var.register_index);
                }
            }
            break;
        case AST_EXPR_LITERAL: {
            if (expr->u.literal.is_float) {
                float f;
                memcpy(&f, &expr->u.literal.val[0], sizeof(float));
                sb_appendf(sb, "%f", f);
            } else if (expr->u.literal.is_uint) {
                sb_appendf(sb, "%u", expr->u.literal.val[0]);
            } else {
                sb_appendf(sb, "%d", (int)expr->u.literal.val[0]);
            }
            break;
        }
        case AST_EXPR_UNARY:
            sb_append(sb, "-(");
            ast_format_expr(expr->u.unary.sub, sb);
            sb_append(sb, ")");
            break;
        case AST_EXPR_BINARY:
            sb_append(sb, "(");
            ast_format_expr(expr->u.binary.left, sb);
            sb_append(sb, " + ");
            ast_format_expr(expr->u.binary.right, sb);
            sb_append(sb, ")");
            break;
        case AST_EXPR_TERNARY:
            sb_append(sb, "(");
            ast_format_expr(expr->u.ternary.cond, sb);
            sb_append(sb, " ? ");
            ast_format_expr(expr->u.ternary.true_expr, sb);
            sb_append(sb, " : ");
            ast_format_expr(expr->u.ternary.false_expr, sb);
            sb_append(sb, ")");
            break;
        case AST_EXPR_SWIZZLE: {
            ast_format_expr(expr->u.swizzle.sub, sb);
            sb_append(sb, ".");
            const char comps[] = "xyzw";
            for (int i = 0; i < expr->u.swizzle.swizzle_count; i++) {
                int c = expr->u.swizzle.swizzle[i];
                if (c >= 0 && c < 4) {
                    char ch[2] = {comps[c], '\0'};
                    sb_append(sb, ch);
                }
            }
            break;
        }
        case AST_EXPR_CALL:
            sb_append(sb, expr->u.call.name);
            sb_append(sb, "(");
            for (int i = 0; i < expr->u.call.arg_count; i++) {
                if (i > 0) sb_append(sb, ", ");
                ast_format_expr(expr->u.call.args[i], sb);
            }
            sb_append(sb, ")");
            break;
        case AST_EXPR_CAST:
            sb_append(sb, "(");
            sb_append(sb, expr->u.cast.type_name);
            sb_append(sb, ")");
            ast_format_expr(expr->u.cast.sub, sb);
            break;
    }
}

static void append_indent(StringBuilder *sb, int indent) {
    for (int i = 0; i < indent; i++) {
        sb_append(sb, "  ");
    }
}

void ast_format_stmt(const ASTStmt *stmt, StringBuilder *sb, int indent) {
    if (!stmt) return;
    switch (stmt->kind) {
        case AST_STMT_BLOCK:
            append_indent(sb, indent);
            sb_append(sb, "{\n");
            for (int i = 0; i < stmt->u.block.stmt_count; i++) {
                ast_format_stmt(stmt->u.block.stmts[i], sb, indent + 1);
            }
            append_indent(sb, indent);
            sb_append(sb, "}\n");
            break;
        case AST_STMT_ASSIGN:
            append_indent(sb, indent);
            ast_format_expr(stmt->u.assign.dest, sb);
            sb_append(sb, " = ");
            ast_format_expr(stmt->u.assign.src, sb);
            sb_append(sb, ";\n");
            break;
        case AST_STMT_IF:
            append_indent(sb, indent);
            sb_append(sb, "if (");
            ast_format_expr(stmt->u.if_stmt.cond, sb);
            sb_append(sb, ")\n");
            ast_format_stmt(stmt->u.if_stmt.true_body, sb, indent);
            if (stmt->u.if_stmt.false_body) {
                append_indent(sb, indent);
                sb_append(sb, "else\n");
                ast_format_stmt(stmt->u.if_stmt.false_body, sb, indent);
            }
            break;
        case AST_STMT_LOOP:
            append_indent(sb, indent);
            if (stmt->u.loop.cond) {
                sb_append(sb, "while (");
                ast_format_expr(stmt->u.loop.cond, sb);
                sb_append(sb, ")\n");
            } else {
                sb_append(sb, "while (true)\n");
            }
            ast_format_stmt(stmt->u.loop.body, sb, indent);
            break;
        case AST_STMT_BREAK:
            append_indent(sb, indent);
            sb_append(sb, "break;\n");
            break;
        case AST_STMT_CONTINUE:
            append_indent(sb, indent);
            sb_append(sb, "continue;\n");
            break;
        case AST_STMT_RETURN:
            append_indent(sb, indent);
            sb_append(sb, "return;\n");
            break;
        case AST_STMT_DISCARD:
            append_indent(sb, indent);
            sb_append(sb, "discard;\n");
            break;
    }
}
