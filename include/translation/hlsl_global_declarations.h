// SPDX-License-Identifier: GPL-3.0-only
#ifndef HLSL_GLOBAL_DECLARATIONS_H
#define HLSL_GLOBAL_DECLARATIONS_H

#include "io/serialized_shader.h"
#include "io/parameter_layout.h"
#include "translation/usil.h"

/* A declaration-only union of one pass/stage's D3D11 keyword siblings. It
 * never changes runtime common/residual parameters or grants read authority.
 * The union owns copied layouts, names and witness IDs. Inputs are borrowed only
 * during construction; target reads are rechecked against their own metadata. */
typedef struct HLSLGlobalDeclarationUnion HLSLGlobalDeclarationUnion;
typedef struct {
    int subprogram_index;
    const PlayerSubProgramMetadata *player;
    const SerializedProgramParameters *residual;
} HLSLGlobalDeclarationWitness;
typedef struct {
    const char *name; /* Owned by the union. */
    DecodedVariableLayout layout;
    uint32_t byte_size;
    bool current_authority;
    size_t witness_count;
    const uint32_t *witness_subprogram_indices; /* Owned, sorted, duplicate-free. */
} HLSLGlobalDeclarationField;
typedef enum {
    HLSL_GLOBAL_DECLARATIONS_OK = 0,
    HLSL_GLOBAL_DECLARATIONS_NOT_APPLICABLE,
    HLSL_GLOBAL_DECLARATIONS_INVALID,
    HLSL_GLOBAL_DECLARATIONS_SCOPE_CONFLICT,
    HLSL_GLOBAL_DECLARATIONS_SHELL_CONFLICT,
    HLSL_GLOBAL_DECLARATIONS_FIELD_CONFLICT,
    HLSL_GLOBAL_DECLARATIONS_CURRENT_READ_UNAUTHORIZED,
    HLSL_GLOBAL_DECLARATIONS_ALLOCATION_FAILED
} HLSLGlobalDeclarationStatus;
typedef struct {
    HLSLGlobalDeclarationStatus status;
    int subprogram_index;
    int conflicting_subprogram_index;
    int field_index;
} HLSLGlobalDeclarationDiagnostic;

/* Fast authority-only scope check before loading sibling metadata. */
HLSLGlobalDeclarationStatus
hlsl_global_declarations_scope_status(const SerializedProgramParameters *residual,
                                      const SerializedProgramParameters *common);
/* Exact request family, before parsing or recording sibling witnesses. Keywords
 * may vary; platform, program type, tier identity and requirements may not. */
bool hlsl_global_declarations_same_family(const SerializedPass *pass, int serialized_stage,
                                          int current_subprogram_index,
                                          int candidate_subprogram_index);

/* Every provided witness must be a complete same-platform/type/tier/requirements
 * sibling. Missing/duplicate/current-less witnesses and mismatched player
 * keyword authority reject. Fields are metadata-proven float or integer scalar/
 * vector values within one 16-byte row; packed four-byte offsets are admitted.
 * Integer metadata does not retain signedness. Declarations use the existing
 * integer spelling; no unsigned execution or reflection authority is inferred.
 * Boolean, arrays/matrices/structs and type/name-based inference are unsupported. */
HLSLGlobalDeclarationStatus hlsl_global_declarations_build(
    const SerializedPass *pass, int serialized_stage, int current_subprogram_index,
    const HLSLGlobalDeclarationWitness *witnesses, size_t witness_count,
    HLSLGlobalDeclarationUnion **output, HLSLGlobalDeclarationDiagnostic *diagnostic);
void hlsl_global_declarations_free(HLSLGlobalDeclarationUnion *declarations);
const HLSLGlobalDeclarationField *
hlsl_global_declarations_fields(const HLSLGlobalDeclarationUnion *declarations, size_t *count);
uint32_t hlsl_global_declarations_shell_size(const HLSLGlobalDeclarationUnion *declarations);
int hlsl_global_declarations_current_variant(const HLSLGlobalDeclarationUnion *declarations);
/* Re-checks the target's original metadata and projects actual DXBC reads onto
 * only those original fields. Imported fields cannot fill an executable hole. */
HLSLGlobalDeclarationStatus hlsl_global_declarations_validate_target(
    const HLSLGlobalDeclarationUnion *declarations, const USILProgram *program,
    const SerializedProgramParameters *residual, const SerializedProgramParameters *common);
#endif
