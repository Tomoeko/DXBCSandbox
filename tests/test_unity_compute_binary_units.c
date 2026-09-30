// SPDX-License-Identifier: GPL-3.0-only
#include "io/unity_compute_binary.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__, #x);                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
typedef struct {
    uint8_t bytes[1024];
    size_t size;
} Fixture;
static void u32(Fixture* f, uint32_t x) {
    for (unsigned i = 0; i < 4U; ++i)
        f->bytes[f->size++] = (uint8_t)(x >> (8U * i));
}
static void u64(Fixture* f, uint64_t x) {
    for (unsigned i = 0; i < 8U; ++i)
        f->bytes[f->size++] = (uint8_t)(x >> (8U * i));
}
static void string(Fixture* f, const char* s) {
    size_t n = strlen(s);
    u32(f, (uint32_t)n);
    memcpy(f->bytes + f->size, s, n);
    f->size += n;
}
static void resource(Fixture* f, const char* name, uint32_t bind, uint32_t dimension) {
    u64(f, 1U);
    string(f, name);
    string(f, "");
    u32(f, bind);
    u32(f, UINT32_MAX);
    u32(f, dimension);
}
static Fixture fixture(void) {
    Fixture f = {{0}, 0U};
    u64(&f, 1U);
    string(&f, "Kernel");
    u64(&f, 1U);
    string(&f, "MACRO");
    string(&f, "odd");
    u32(&f, 7U);
    u64(&f, 1U);
    u64(&f, 1U);
    string(&f, "$Globals");
    u32(&f, 16U);
    u64(&f, 1U);
    string(&f, "_Factor");
    u32(&f, 0U);
    u32(&f, 0U);
    u32(&f, 0U);
    u32(&f, 1U);
    u32(&f, 1U);
    u64(&f, 1U);
    string(&f, "Kernel");
    resource(&f, "$Globals", 0U, 0U);
    resource(&f, "Texture", 1U, 2U);
    resource(&f, "Input", 2U, 0U);
    resource(&f, "Output", 3U, 2U);
    u64(&f, 1U);
    u32(&f, 5U);
    u32(&f, 4U);
    u64(&f, 4U);
    memcpy(f.bytes + f.size, "DXBC", 4U);
    f.size += 4U;
    u32(&f, 8U);
    u32(&f, 4U);
    u32(&f, 1U);
    f.bytes[f.size++] = 1U;
    return f;
}
static bool equal_string(ComputeShaderStringView s, const char* t) {
    return s.size == strlen(t) && memcmp(s.bytes, t, s.size) == 0;
}
int main(void) {
    Fixture f = fixture();
    UnityComputeBinary b;
    unity_compute_binary_init(&b);
    CHECK(unity_compute_binary_decode(&b, f.bytes, f.size) == COMPUTE_SHADER_OBJECT_OK);
    CHECK(b.decoded && b.payload == f.bytes && b.payload_size == f.size && b.target_level == 7 &&
          b.resources_resolved);
    CHECK(b.directive_count == 1U && equal_string(b.directives[0].name, "Kernel") &&
          b.directives[0].macro_count == 1U &&
          equal_string(b.directives[0].macros[0].value, "odd"));
    CHECK(b.buffer_variant_count == 1U && b.buffer_variants[0].buffer_count == 1U);
    const ComputeShaderConstantBuffer* cb = &b.buffer_variants[0].buffers[0];
    CHECK(cb->byte_size == 16 && equal_string(cb->name, "$Globals") && cb->parameter_count == 1U &&
          equal_string(cb->parameters[0].name, "_Factor") && cb->parameters[0].row_count == 1U);
    CHECK(b.kernel_count == 1U && equal_string(b.kernels[0].name, "Kernel"));
    const ComputeShaderKernelVariant* k = &b.kernels[0].data;
    CHECK(k->constant_buffer_count == 1U && k->texture_count == 1U && k->input_buffer_count == 1U &&
          k->output_buffer_count == 1U && k->builtin_sampler_count == 1U);
    CHECK(k->output_buffers[0].bind_point == 3 && k->output_buffers[0].sampler_bind_point == -1 &&
          k->output_buffers[0].texture_dimension == 2);
    CHECK(k->code_size == 4U && memcmp(k->code, "DXBC", 4U) == 0 &&
          k->thread_group_size_count == 3U && k->thread_group_size[0] == 8U &&
          k->thread_group_size[1] == 4U && k->thread_group_size[2] == 1U);
    CHECK(!k->keyword_key.bytes && !k->constant_buffer_variant_indices && k->requirements == 0);
    const void* retained = b.kernels;
    for (size_t length = 0U; length < f.size; ++length) {
        CHECK(unity_compute_binary_decode(&b, f.bytes, length) != COMPUTE_SHADER_OBJECT_OK);
        CHECK(b.kernels == retained && b.payload == f.bytes && b.decoded && b.kernel_count == 1U);
    }
    Fixture bad = f;
    memset(bad.bytes, 0xff, 8U);
    CHECK(unity_compute_binary_decode(&b, bad.bytes, bad.size) ==
          COMPUTE_SHADER_OBJECT_COUNT_INVALID);
    CHECK(b.kernels == retained);
    bad = f;
    bad.bytes[12U] = 0U;
    CHECK(unity_compute_binary_decode(&b, bad.bytes, bad.size) ==
          COMPUTE_SHADER_OBJECT_STRING_CONTAINS_NUL);
    bad = f;
    bad.bytes[bad.size - 1U] = 2U;
    CHECK(unity_compute_binary_decode(&b, bad.bytes, bad.size) ==
          COMPUTE_SHADER_OBJECT_MODEL_INVALID);
    bad = f;
    bad.bytes[bad.size++] = 0U;
    CHECK(unity_compute_binary_decode(&b, bad.bytes, bad.size) ==
          COMPUTE_SHADER_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED);
    CHECK(b.kernels == retained);
    unity_compute_binary_dispose(&b);
    CHECK(!b.decoded && !b.kernels && !b.payload);
    unity_compute_binary_dispose(&b);
    puts("native compute binary unit tests passed");
    return 0;
}
