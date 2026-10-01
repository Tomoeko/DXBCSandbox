// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_COMPUTE_SOURCE_INTERNAL_H
#define HLSL_COMPUTE_SOURCE_INTERNAL_H

#include "translation/hlsl_emitter.h"
#include "translation/hlsl_ast.h"

/* Names are owned by the complete candidate. The entry emitter borrows this
 * exact per-entry declaration inventory and verifies it against USIL. No
 * declaration from a sibling can authorize a new executable resource read. */
typedef struct {
    const char *name;
    uint32_t binding_register;
    bool writable;
    bool structured; /* Explicit uint4-bit representation, original type unknown. */
    ASTScalarType scalar_type; /* Current decoded declaration, never a sibling guess. */
    /* Scalar UINT view requires the complete admitted SM5 typed atomic use;
     * repeated declaration return formats alone cannot establish its width. */
    bool scalar_atomic;
} HLSLComputeTypedResource;

typedef struct {
    const HLSLComputeTypedResource *resources;
    size_t resource_count;
    bool emit_declarations;
    /* Accepted roots transfer ownership to the callback. False retains
     * ownership with the emitter and rejects the complete emission. */
    bool (*retain_expression)(void *context, ASTExpr *expression);
    void *expression_context;
} HLSLComputeTypedSource;

bool hlsl_emit_compute_typed_stage(const USILProgram *program, StringBuilder *output,
    const HLSLEmitNames *names, const HLSLEmitOptions *options,
    const HLSLComputeTypedSource *typed_source, HLSLEmitDiagnostic *diagnostic);

#endif
