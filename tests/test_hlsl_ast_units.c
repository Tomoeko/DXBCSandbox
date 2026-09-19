// SPDX-License-Identifier: GPL-3.0-only

#include "translation/hlsl_ast.h"
#include "translation/usil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #value);            \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static bool expect_expression(ASTExpr *expression, const char *expected) {
    CHECK(expression);
    StringBuilder text;
    sb_init(&text);
    ast_format_expr(expression, &text);
    bool matched = sb_ok(&text) && text.buf && strcmp(text.buf, expected) == 0;
    if (!matched)
        fprintf(stderr, "Expected %s, got %s\n", expected, text.buf ? text.buf : "null");
    sb_free(&text);
    ast_free_expr(expression);
    CHECK(matched);
    return true;
}

static bool check_literal_bits(void) {
    const uint32_t values[] = {0,           0x80000000u, 1,           0x007fffffu, 0x00800000u,
                               0x3f800001u, 0x3eaaaaabu, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u,
                               0xff800000u, 0x7f800001u, 0x7fc12345u, 0xffc12345u};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        ASTExpr *literal = ast_create_literal_bits(&values[i], 1, AST_SCALAR_FLOAT32);
        CHECK(literal);
        StringBuilder text;
        sb_init(&text);
        ast_format_expr(literal, &text);
        CHECK(sb_ok(&text) && text.buf);
        uint32_t roundtrip = 0;
        if (strncmp(text.buf, "asfloat(0x", 10) == 0) {
            char *end = NULL;
            unsigned long raw = strtoul(text.buf + 10, &end, 16);
            CHECK(raw <= UINT32_MAX && end && strcmp(end, "u)") == 0);
            roundtrip = (uint32_t)raw;
        } else {
            char *end = NULL;
            float numeric = strtof(text.buf, &end);
            CHECK(end && strcmp(end, "f") == 0);
            memcpy(&roundtrip, &numeric, sizeof(roundtrip));
        }
        CHECK(roundtrip == values[i]);
        sb_free(&text);
        ast_free_expr(literal);
    }
    const uint32_t vector[] = {0x80000000u, 1, 0x7fc12345u, 0x3f800000u};
    CHECK(expect_expression(ast_create_literal_bits(vector, 4, AST_SCALAR_FLOAT32),
                            "float4(-0.0f, asfloat(0x00000001u), asfloat(0x7FC12345u), 1.0f)"));
    const uint32_t integers[] = {0x80000000u, 0xffffffffu, 1};
    CHECK(expect_expression(ast_create_literal_bits(integers, 3, AST_SCALAR_SINT32),
                            "int3(asint(0x80000000u), -1, 1)"));
    CHECK(expect_expression(ast_create_literal_bits(integers, 3, AST_SCALAR_UINT32),
                            "uint3(2147483648u, 4294967295u, 1u)"));
    CHECK(!ast_create_literal_bits(vector, 0, AST_SCALAR_FLOAT32));
    CHECK(!ast_create_literal_bits(vector, 5, AST_SCALAR_FLOAT32));
    CHECK(!ast_create_literal_bits(vector, 1, (ASTScalarType)99));
    return true;
}

static bool check_operators_and_casts(void) {
    CHECK(expect_expression(ast_create_emitter_operand("input.position.wzyx"), "(input.position.wzyx)"));
    CHECK(!ast_create_emitter_operand(""));
    CHECK(!ast_create_emitter_operand("value; discard"));
    CHECK(!ast_create_emitter_operand("#define value 0"));
    CHECK(expect_expression(
        ast_create_binary(USIL_OP_MUL, ast_create_literal_int(2), ast_create_literal_int(3)),
        "(2 * 3)"));
    CHECK(expect_expression(ast_create_binary(USIL_OP_SUB, ast_create_literal_float(1.0f),
                                              ast_create_literal_float(0.5f)),
                            "(1.0f - 0.5f)"));
    CHECK(expect_expression(ast_create_unary(USIL_OP_NOT, ast_create_literal_int(1)), "~(1)"));
    CHECK(expect_expression(ast_create_unary(USIL_OP_INEG, ast_create_literal_int(2)), "-(2)"));
    CHECK(
        expect_expression(ast_create_cast("int", ast_create_literal_float(1.0f)), "((int)(1.0f))"));
    CHECK(expect_expression(ast_create_bitcast(AST_SCALAR_SINT32, ast_create_literal_float(1.0f)),
                            "asint(1.0f)"));
    int lanes[] = {1, 0};
    CHECK(expect_expression(
        ast_create_swizzle(
            ast_create_cast("uint2", ast_create_var(-1, 0, OPERAND_TYPE_TEMP, "value")), lanes, 2),
        "((uint2)(value)).yx"));
    ASTExpr *left = ast_create_literal_int(1), *right = ast_create_literal_int(2);
    CHECK(left && right);
    CHECK(!ast_create_binary(USIL_OP_IMUL, left, right));
    CHECK(!ast_create_binary(USIL_OP_EQ, left, right));
    CHECK(!ast_create_binary(999, left, right));
    CHECK(!ast_create_unary(999, left));
    CHECK(!ast_create_binary(USIL_OP_ADD, left, left));
    int bad_lanes[] = {0, 4};
    CHECK(!ast_create_swizzle(left, bad_lanes, 2));
    CHECK(!ast_create_swizzle(left, lanes, 5));
    CHECK(!ast_create_bitcast((ASTScalarType)99, left));
    /* Invalid construction never consumes either child. */
    CHECK(expect_expression(left, "1"));
    CHECK(expect_expression(right, "2"));
    return true;
}

static bool check_invalid_trees_and_ownership(void) {
    ASTStmt *block = ast_create_block();
    ASTStmt *flow = ast_create_flow(AST_STMT_RETURN);
    CHECK(block && flow && ast_block_add(block, flow));
    CHECK(!ast_block_add(block, flow) && block->u.block.stmt_count == 1);
    CHECK(!ast_block_add(block, block));
    CHECK(!ast_create_flow(AST_STMT_ASSIGN));
    CHECK(!ast_create_assign(NULL, NULL));
    StringBuilder text;
    sb_init(&text);
    ast_format_stmt(block, &text, 0);
    CHECK(sb_ok(&text) && strcmp(text.buf, "{\n  return;\n}\n") == 0);
    sb_free(&text);
    ast_free_stmt(block);

    ASTExpr invalid = {.kind = (ASTExprKind)99};
    sb_init(&text);
    ast_format_expr(&invalid, &text);
    CHECK(!sb_ok(&text));
    sb_free(&text);
    sb_init(&text);
    ast_format_expr(NULL, &text);
    CHECK(!sb_ok(&text));
    sb_free(&text);
    ASTExpr *deep = ast_create_literal_int(1);
    CHECK(deep);
    for (int i = 0; i < 258; ++i) {
        deep = ast_create_unary(USIL_OP_INEG, deep);
        CHECK(deep);
    }
    sb_init(&text);
    ast_format_expr(deep, &text);
    CHECK(!sb_ok(&text));
    sb_free(&text);
    ast_free_expr(deep);
    return true;
}

int main(void) {
    if (!check_literal_bits() || !check_operators_and_casts() ||
        !check_invalid_trees_and_ownership())
        return 1;
    puts("HLSL AST bits, operators and ownership passed");
    return 0;
}
