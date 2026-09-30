// SPDX-License-Identifier: GPL-3.0-only
#ifndef UNITY_COMPUTE_BINARY_H
#define UNITY_COMPUTE_BINARY_H

#include "io/compute_shader_object.h"

/* The selected macOS Unity 2021.3.35f1 ComputeShaderBinary writer uses u64
 * counts, u32 byte-length strings, and no alignment. This grammar is separate
 * from the player ClassID 72 layout. Decoding does not authenticate a compiler
 * process, infer source, or grant ClassID 72/source/semantic authority.
 * All strings and code borrow the supplied payload; arrays are owned.
 * Counts (including code bytes) are bounded to 1,048,576 and by remaining
 * payload bytes. These are conservative parser limits, not native ABI limits. */
typedef struct {
    ComputeShaderStringView name;
    ComputeShaderStringView value;
} UnityComputeMacro;

typedef struct {
    ComputeShaderStringView name;
    UnityComputeMacro* macros;
    size_t macro_count;
} UnityComputeKernelDirective;

typedef struct {
    ComputeShaderConstantBuffer* buffers;
    size_t buffer_count;
} UnityComputeBufferVariant;

typedef struct {
    ComputeShaderStringView name;
    /* Shared resource/code tuple. keyword_key, CB variant indices and
     * requirements are absent from this native wire format and remain zero;
     * zero is not evidence about those ClassID 72 fields. */
    ComputeShaderKernelVariant data;
} UnityComputeKernelBinary;

typedef struct {
    const uint8_t* payload;
    size_t payload_size;
    UnityComputeKernelDirective* directives;
    size_t directive_count;
    int32_t target_level;
    UnityComputeBufferVariant* buffer_variants;
    size_t buffer_variant_count;
    UnityComputeKernelBinary* kernels;
    size_t kernel_count;
    bool resources_resolved;
    bool decoded;
} UnityComputeBinary;

void unity_compute_binary_init(UnityComputeBinary* binary);
void unity_compute_binary_dispose(UnityComputeBinary* binary);

/* Complete exhaustion and strong initialized-output guarantee. Failure never
 * exposes a partially decoded success. Reuses the player compute reader's
 * string/resource/parameter and allocation guards, with native wire mode. */
ComputeShaderObjectStatus unity_compute_binary_decode(UnityComputeBinary* destination,
                                                      const uint8_t* payload, size_t size);

#endif
