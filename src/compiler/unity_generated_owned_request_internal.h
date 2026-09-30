// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_GENERATED_OWNED_REQUEST_INTERNAL_H
#define UNITY_GENERATED_OWNED_REQUEST_INTERNAL_H

#include "compiler/unity_generated_domain_certifier.h"

/* Private normal-loop observations only. The public entry supplies NULL, and
 * this route rejects injected compiler services. All pointers are borrowed
 * within the exact current compile/reflection transaction. */
typedef struct {
    int stage_index, hardware_tier_group, subprogram_index;
    size_t generated_state_index, aliased_state_index;
    const UnityCompilerSnippetCompileRequest *request;
    const PlayerSubProgramMetadata *player;
    const SerializedProgramParameters *current, *common;
    const uint8_t *target;
    size_t target_size;
} UnityGeneratedOwnedRequest;

typedef struct {
    bool (*observe)(void *context, const UnityGeneratedOwnedRequest *request);
    void *context;
} UnityGeneratedOwnedRequestObserver;

UnityGeneratedDomainStatus unity_generated_domain_certify_owned_requests(
    const UnityGeneratedDomainCertificationInput *input,
    const UnityGeneratedOwnedRequestObserver *observer,
    UnityGeneratedDomainReport *report);

#endif
