// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_ast.h"
#include "translation/hlsl_literal.h"
#include "translation/usil.h"
#include "common/string_builder.h"
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *ast_duplicate_string(const char *value) {
    if (!value)
        return NULL;
    size_t length = strlen(value);
    if (length == SIZE_MAX)
        return NULL;
    char *copy = malloc(length + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, value, length + 1u);
    return copy;
}

ASTExpr *ast_create_var(int ssa_var, int reg, int type, const char *name) {
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr)
        return NULL;
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

static bool scalar_type_valid(ASTScalarType type) {
    return type == AST_SCALAR_FLOAT32 || type == AST_SCALAR_SINT32 || type == AST_SCALAR_UINT32;
}

ASTExpr *ast_create_literal_bits(const uint32_t *bits, int components, ASTScalarType scalar_type) {
    if (!bits || components < 1 || components > 4 || !scalar_type_valid(scalar_type))
        return NULL;
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr)
        return NULL;
    expr->kind = AST_EXPR_LITERAL;
    memcpy(expr->u.literal.val, bits, (size_t)components * sizeof(*bits));
    expr->u.literal.components = components;
    expr->u.literal.scalar_type = scalar_type;
    return expr;
}

ASTExpr *ast_create_literal_float(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return ast_create_literal_bits(&bits, 1, AST_SCALAR_FLOAT32);
}

ASTExpr *ast_create_literal_int(int i) {
    uint32_t bits = (uint32_t)i;
    return ast_create_literal_bits(&bits, 1, AST_SCALAR_SINT32);
}

static const char *unary_operator(int opcode) {
    switch (opcode) {
    case USIL_OP_INEG:
        return "-";
    case USIL_OP_NOT:
        return "~";
    default:
        return NULL;
    }
}

static const char *binary_operator(int opcode) {
    switch (opcode) {
    case USIL_OP_ADD:
    case USIL_OP_IADD:
        return " + ";
    case USIL_OP_SUB:
        return " - ";
    case USIL_OP_MUL:
        return " * ";
    case USIL_OP_DIV:
        return " / ";
    case USIL_OP_AND:
        return " & ";
    case USIL_OP_OR:
        return " | ";
    case USIL_OP_XOR:
        return " ^ ";
    case USIL_OP_ISHL:
        return " << ";
    case USIL_OP_ISHR:
    case USIL_OP_USHR:
        return " >> ";
    default:
        return NULL;
    }
}

ASTExpr *ast_create_unary(int op, ASTExpr *sub) {
    if (!sub || !unary_operator(op))
        return NULL;
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr)
        return NULL;
    expr->kind = AST_EXPR_UNARY;
    expr->u.unary.op = op;
    expr->u.unary.sub = sub;
    return expr;
}

ASTExpr *ast_create_binary(int op, ASTExpr *left, ASTExpr *right) {
    if (!left || !right || left == right || !binary_operator(op))
        return NULL;
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr)
        return NULL;
    expr->kind = AST_EXPR_BINARY;
    expr->u.binary.op = op;
    expr->u.binary.left = left;
    expr->u.binary.right = right;
    return expr;
}

ASTExpr *ast_create_ternary(ASTExpr *cond, ASTExpr *true_expr, ASTExpr *false_expr) {
    if (!cond || !true_expr || !false_expr || cond == true_expr || cond == false_expr ||
        true_expr == false_expr)
        return NULL;
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr)
        return NULL;
    expr->kind = AST_EXPR_TERNARY;
    expr->u.ternary.cond = cond;
    expr->u.ternary.true_expr = true_expr;
    expr->u.ternary.false_expr = false_expr;
    return expr;
}

ASTExpr *ast_create_swizzle(ASTExpr *sub, const int *swizzle, int count) {
    if (!sub || !swizzle || count < 1 || count > 4)
        return NULL;
    for (int i = 0; i < count; ++i)
        if (swizzle[i] < 0 || swizzle[i] > 3)
            return NULL;
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr)
        return NULL;
    expr->kind = AST_EXPR_SWIZZLE;
    expr->u.swizzle.sub = sub;
    expr->u.swizzle.swizzle_count = count;
    for (int i = 0; i < expr->u.swizzle.swizzle_count; i++) {
        expr->u.swizzle.swizzle[i] = swizzle[i];
    }
    return expr;
}

ASTExpr *ast_create_call(const char *name, ASTExpr **args, int count) {
    if (!name || !name[0] || count < 0 || (count > 0 && !args) ||
        (size_t)count > SIZE_MAX / sizeof(ASTExpr *))
        return NULL;
    for (int i = 0; i < count; ++i) {
        if (!args[i])
            return NULL;
        for (int j = 0; j < i; ++j)
            if (args[i] == args[j])
                return NULL;
    }
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr)
        return NULL;
    expr->kind = AST_EXPR_CALL;
    expr->u.call.name = ast_duplicate_string(name);
    if (!expr->u.call.name) {
        free(expr);
        return NULL;
    }
    expr->u.call.arg_count = count;
    expr->u.call.args = count > 0 ? malloc((size_t)count * sizeof(ASTExpr *)) : NULL;
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

ASTExpr *ast_create_cast(const char *type_name, ASTExpr *sub) {
    if (!type_name || !type_name[0] || !sub)
        return NULL;
    ASTExpr *expr = malloc(sizeof(ASTExpr));
    if (!expr)
        return NULL;
    expr->kind = AST_EXPR_CAST;
    expr->u.cast.type_name = ast_duplicate_string(type_name);
    if (!expr->u.cast.type_name) {
        free(expr);
        return NULL;
    }
    expr->u.cast.sub = sub;
    return expr;
}

ASTExpr *ast_create_bitcast(ASTScalarType scalar_type, ASTExpr *sub) {
    if (!sub || !scalar_type_valid(scalar_type))
        return NULL;
    ASTExpr *expr = malloc(sizeof(*expr));
    if (!expr)
        return NULL;
    expr->kind = AST_EXPR_BITCAST;
    expr->u.bitcast.scalar_type = scalar_type;
    expr->u.bitcast.sub = sub;
    return expr;
}

void ast_free_expr(ASTExpr *expr) {
    if (!expr)
        return;
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
    case AST_EXPR_BITCAST:
        ast_free_expr(expr->u.bitcast.sub);
        break;
    default:
        break;
    }
    free(expr);
}

ASTStmt *ast_create_block(void) {
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt)
        return NULL;
    stmt->kind = AST_STMT_BLOCK;
    stmt->u.block.stmts = NULL;
    stmt->u.block.stmt_count = 0;
    return stmt;
}

bool ast_block_add(ASTStmt *block, ASTStmt *stmt) {
    if (!block || !stmt || block == stmt || block->kind != AST_STMT_BLOCK ||
        block->u.block.stmt_count < 0 || block->u.block.stmt_count == INT_MAX ||
        (block->u.block.stmt_count && !block->u.block.stmts) ||
        (size_t)block->u.block.stmt_count >= SIZE_MAX / sizeof(ASTStmt *))
        return false;
    for (int i = 0; i < block->u.block.stmt_count; ++i)
        if (block->u.block.stmts[i] == stmt)
            return false;
    size_t count = (size_t)block->u.block.stmt_count + 1u;
    ASTStmt **replacement = realloc(block->u.block.stmts, count * sizeof(*replacement));
    if (!replacement)
        return false;
    block->u.block.stmts = replacement;
    block->u.block.stmts[block->u.block.stmt_count++] = stmt;
    return true;
}

ASTStmt *ast_create_assign(ASTExpr *dest, ASTExpr *src) {
    if (!dest || !src || dest == src)
        return NULL;
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt)
        return NULL;
    stmt->kind = AST_STMT_ASSIGN;
    stmt->u.assign.dest = dest;
    stmt->u.assign.src = src;
    return stmt;
}

ASTStmt *ast_create_if(ASTExpr *cond, ASTStmt *true_body, ASTStmt *false_body) {
    if (!cond || !true_body || true_body == false_body)
        return NULL;
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt)
        return NULL;
    stmt->kind = AST_STMT_IF;
    stmt->u.if_stmt.cond = cond;
    stmt->u.if_stmt.true_body = true_body;
    stmt->u.if_stmt.false_body = false_body;
    return stmt;
}

ASTStmt *ast_create_loop(ASTExpr *cond, ASTStmt *body) {
    if (!body)
        return NULL;
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt)
        return NULL;
    stmt->kind = AST_STMT_LOOP;
    stmt->u.loop.cond = cond;
    stmt->u.loop.body = body;
    return stmt;
}

ASTStmt *ast_create_flow(ASTStmtKind kind) {
    if (kind != AST_STMT_BREAK && kind != AST_STMT_CONTINUE && kind != AST_STMT_RETURN &&
        kind != AST_STMT_DISCARD)
        return NULL;
    ASTStmt *stmt = malloc(sizeof(ASTStmt));
    if (!stmt)
        return NULL;
    stmt->kind = kind;
    return stmt;
}

void ast_free_stmt(ASTStmt *stmt) {
    if (!stmt)
        return;
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

static void format_literal(const ASTLiteral *literal, StringBuilder *sb) {
    if (literal->components < 1 || literal->components > 4 ||
        !scalar_type_valid(literal->scalar_type)) {
        sb->failed = true;
        return;
    }
    if (literal->components > 1) {
        const char *type = literal->scalar_type == AST_SCALAR_FLOAT32  ? "float"
                           : literal->scalar_type == AST_SCALAR_UINT32 ? "uint"
                                                                       : "int";
        sb_appendf(sb, "%s%d(", type, literal->components);
    }
    for (int i = 0; i < literal->components; ++i) {
        if (i)
            sb_append(sb, ", ");
        if (literal->scalar_type == AST_SCALAR_FLOAT32) {
            char text[64];
            if (!format_float_bits_hlsl(literal->val[i], text, sizeof(text))) {
                sb->failed = true;
                return;
            }
            sb_append(sb, text);
        } else if (literal->scalar_type == AST_SCALAR_UINT32) {
            sb_appendf(sb, "%" PRIu32 "u", literal->val[i]);
        } else if (literal->val[i] == UINT32_C(0x80000000)) {
            /* Avoid an out-of-range signed decimal token in HLSL. */
            sb_append(sb, "asint(0x80000000u)");
        } else {
            int32_t value;
            memcpy(&value, &literal->val[i], sizeof(value));
            sb_appendf(sb, "%" PRId32, value);
        }
    }
    if (literal->components > 1)
        sb_append(sb, ")");
}

static void format_expr(const ASTExpr *expr, StringBuilder *sb, unsigned depth) {
    if (!sb || !sb_ok(sb))
        return;
    if (!expr || depth > 256u) {
        sb->failed = true;
        return;
    }
    switch (expr->kind) {
    case AST_EXPR_VAR:
        if (expr->u.var.name && expr->u.var.name[0] != '\0') {
            sb_append(sb, expr->u.var.name);
        } else {
            if (expr->u.var.ssa_var >= 0) {
                sb_appendf(sb, "r%d_%d", expr->u.var.register_index, expr->u.var.ssa_var);
            } else {
                sb_appendf(sb, "r%d", expr->u.var.register_index);
            }
        }
        break;
    case AST_EXPR_LITERAL: {
        format_literal(&expr->u.literal, sb);
        break;
    }
    case AST_EXPR_UNARY:
        if (!unary_operator(expr->u.unary.op)) {
            sb->failed = true;
            break;
        }
        sb_append(sb, unary_operator(expr->u.unary.op));
        sb_append(sb, "(");
        format_expr(expr->u.unary.sub, sb, depth + 1u);
        sb_append(sb, ")");
        break;
    case AST_EXPR_BINARY:
        if (!binary_operator(expr->u.binary.op)) {
            sb->failed = true;
            break;
        }
        sb_append(sb, "(");
        format_expr(expr->u.binary.left, sb, depth + 1u);
        sb_append(sb, binary_operator(expr->u.binary.op));
        format_expr(expr->u.binary.right, sb, depth + 1u);
        sb_append(sb, ")");
        break;
    case AST_EXPR_TERNARY:
        sb_append(sb, "(");
        format_expr(expr->u.ternary.cond, sb, depth + 1u);
        sb_append(sb, " ? ");
        format_expr(expr->u.ternary.true_expr, sb, depth + 1u);
        sb_append(sb, " : ");
        format_expr(expr->u.ternary.false_expr, sb, depth + 1u);
        sb_append(sb, ")");
        break;
    case AST_EXPR_SWIZZLE: {
        if (expr->u.swizzle.swizzle_count < 1 || expr->u.swizzle.swizzle_count > 4) {
            sb->failed = true;
            break;
        }
        /* Parenthesize scalar literals before a member selector. All
         * compound expression forms already preserve their precedence. */
        bool literal = expr->u.swizzle.sub && expr->u.swizzle.sub->kind == AST_EXPR_LITERAL;
        if (literal)
            sb_append(sb, "(");
        format_expr(expr->u.swizzle.sub, sb, depth + 1u);
        if (literal)
            sb_append(sb, ")");
        sb_append(sb, ".");
        const char comps[] = "xyzw";
        for (int i = 0; i < expr->u.swizzle.swizzle_count; i++) {
            int c = expr->u.swizzle.swizzle[i];
            if (c >= 0 && c < 4) {
                char ch[2] = {comps[c], '\0'};
                sb_append(sb, ch);
            } else
                sb->failed = true;
        }
        break;
    }
    case AST_EXPR_CALL:
        if (!expr->u.call.name || expr->u.call.arg_count < 0 ||
            (expr->u.call.arg_count && !expr->u.call.args)) {
            sb->failed = true;
            break;
        }
        sb_append(sb, expr->u.call.name);
        sb_append(sb, "(");
        for (int i = 0; i < expr->u.call.arg_count; i++) {
            if (i > 0)
                sb_append(sb, ", ");
            format_expr(expr->u.call.args[i], sb, depth + 1u);
        }
        sb_append(sb, ")");
        break;
    case AST_EXPR_CAST:
        if (!expr->u.cast.type_name) {
            sb->failed = true;
            break;
        }
        sb_append(sb, "((");
        sb_append(sb, expr->u.cast.type_name);
        sb_append(sb, ")(");
        format_expr(expr->u.cast.sub, sb, depth + 1u);
        sb_append(sb, "))");
        break;
    case AST_EXPR_BITCAST:
        if (!scalar_type_valid(expr->u.bitcast.scalar_type)) {
            sb->failed = true;
            break;
        }
        sb_append(sb, expr->u.bitcast.scalar_type == AST_SCALAR_FLOAT32  ? "asfloat("
                      : expr->u.bitcast.scalar_type == AST_SCALAR_UINT32 ? "asuint("
                                                                         : "asint(");
        format_expr(expr->u.bitcast.sub, sb, depth + 1u);
        sb_append(sb, ")");
        break;
    default:
        sb->failed = true;
        break;
    }
}

void ast_format_expr(const ASTExpr *expr, StringBuilder *sb) { format_expr(expr, sb, 0u); }

static void append_indent(StringBuilder *sb, int indent) {
    for (int i = 0; i < indent; i++) {
        sb_append(sb, "  ");
    }
}

static void format_stmt(const ASTStmt *stmt, StringBuilder *sb, int indent, unsigned depth) {
    if (!sb || !sb_ok(sb))
        return;
    if (!stmt || indent < 0 || indent > 256 || depth > 256u) {
        sb->failed = true;
        return;
    }
    switch (stmt->kind) {
    case AST_STMT_BLOCK:
        if (stmt->u.block.stmt_count < 0 || (stmt->u.block.stmt_count && !stmt->u.block.stmts)) {
            sb->failed = true;
            break;
        }
        append_indent(sb, indent);
        sb_append(sb, "{\n");
        for (int i = 0; i < stmt->u.block.stmt_count; i++) {
            format_stmt(stmt->u.block.stmts[i], sb, indent + 1, depth + 1u);
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
        format_stmt(stmt->u.if_stmt.true_body, sb, indent, depth + 1u);
        if (stmt->u.if_stmt.false_body) {
            append_indent(sb, indent);
            sb_append(sb, "else\n");
            format_stmt(stmt->u.if_stmt.false_body, sb, indent, depth + 1u);
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
        format_stmt(stmt->u.loop.body, sb, indent, depth + 1u);
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
    default:
        sb->failed = true;
        break;
    }
}

void ast_format_stmt(const ASTStmt *stmt, StringBuilder *sb, int indent) {
    format_stmt(stmt, sb, indent, 0u);
}
