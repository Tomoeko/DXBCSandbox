// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_REQUIRED_DECLARATION_QUALITY_INTERNAL_H
#define UNITY_REQUIRED_DECLARATION_QUALITY_INTERNAL_H

#include "compiler/unity_hlsl_matrix_declaration.h"
#include "translation/hlsl_emitted_matrix_uses_internal.h"

typedef bool (*UnityRequiredBlockVisitor)(void *context, size_t block_index,
    const UnityHlslCBufferBlock *block, const UnityHlslCBufferField *fields, size_t field_count);

/* Private read-only visitor over every actual retained required block/field.
 * Replay before and after binds borrowed facts to the current request/lease.
 * No expanded-source pointer, public quality grant or injectable producer. */
bool unity_hlsl_matrix_declaration_visit_required_blocks(UnityCompilerBroker *broker,
    const UnityHlslMatrixDeclarationReceipt *receipt, const UnityHlslMatrixDeclarationInput *current,
    UnityRequiredBlockVisitor visitor, void *context);

typedef struct {
    UnityHlslCBufferInventory inventory;
    uint8_t snapshot_digest[32];
    bool sealed;
} UnityRequiredDeclarationQuality;

void unity_required_declaration_quality_init(UnityRequiredDeclarationQuality *owned);
void unity_required_declaration_quality_dispose(UnityRequiredDeclarationQuality *owned);
bool unity_required_declaration_quality_block(void *context, size_t block_index,
    const UnityHlslCBufferBlock *block, const UnityHlslCBufferField *fields, size_t field_count);
bool unity_required_declaration_quality_seal(UnityRequiredDeclarationQuality *owned);
/* Unit tests exercise this private consumer with the same typed declaration
 * parser; only the live opaque receipt visitor supplies production facts. */
bool unity_required_declaration_quality_analyze(const HLSLMatrixUseCapture *entry,
    const UnityRequiredDeclarationQuality *declarations, HLSLSourceQualityResult *result,
    uint32_t *remaining_obligations, HLSLSourceQualityObserver observer, void *observer_context);

#endif
