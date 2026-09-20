// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_NATIVE_RUNTIME_INTERNAL_H
#define UNITY_NATIVE_RUNTIME_INTERNAL_H

#include "compiler/unity_native_runtime.h"

/* Bounded protocol inspection shared by production and mutation tests. This
 * cannot construct an opaque runtime owner or evidence. Digests in summary are
 * only observed data until capture binds the authenticated invocation. */
UnityNativeRuntimeStatus unity_native_runtime_inspect(const uint8_t *bytes, size_t size,
                                                       const UnityPlayerPackageAuthority *player,
                                                       UnityNativeRuntimeSummary *summary);

#endif
