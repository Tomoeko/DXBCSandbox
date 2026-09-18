#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "app/native_texture_batch.h"
#include "common/file_io.h"
#include "common/sha256.h"
#include "common/string_builder.h"
#include "io/unity_texture_object.h"
#include "translation/native_texture_yaml_emitter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define TEST_PROCESS_ID() ((unsigned long)_getpid())
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_GETCWD(buffer, size) _getcwd((buffer), (int)(size))
#define TEST_CHDIR(path) _chdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_PROCESS_ID() ((unsigned long)getpid())
#define TEST_RMDIR(path) rmdir(path)
#define TEST_MKDIR(path) mkdir((path), 0755)
#define TEST_GETCWD(buffer, size) getcwd((buffer), (size))
#define TEST_CHDIR(path) chdir(path)
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static const uint8_t k_texture2d_hash[16] = {
    0x0dU, 0x08U, 0x41U, 0x4cU, 0xfdU, 0x5bU, 0xdbU, 0x0dU,
    0x22U, 0x79U, 0x20U, 0x11U, 0xbdU, 0xa9U, 0xabU, 0x26U,
};

static const uint8_t k_render_texture_hash[16] = {
    0xaeU, 0x59U, 0x18U, 0xd7U, 0x86U, 0x68U, 0x29U, 0x7fU,
    0x97U, 0xb9U, 0x8eU, 0x4bU, 0x8bU, 0x72U, 0xcfU, 0xc2U,
};

typedef struct {
    uint8_t bytes[1024];
    size_t size;
    size_t first_boolean_offset;
    TypeTreeType type;
    AssetObjectInfo info;
    SerializedFile file;
} TextureFixture;

static bool append_bytes(TextureFixture* fixture, const void* bytes,
                         size_t size) {
    if (!fixture || (!bytes && size != 0U) ||
        size > sizeof(fixture->bytes) - fixture->size) {
        return false;
    }
    if (size != 0U) memcpy(fixture->bytes + fixture->size, bytes, size);
    fixture->size += size;
    return true;
}

static bool append_u8(TextureFixture* fixture, uint8_t value) {
    return append_bytes(fixture, &value, 1U);
}

static bool append_u32(TextureFixture* fixture, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U), (uint8_t)(value >> 24U),
    };
    return append_bytes(fixture, bytes, sizeof(bytes));
}

static bool append_u16(TextureFixture* fixture, uint16_t value) {
    uint8_t bytes[2] = {
        (uint8_t)value, (uint8_t)(value >> 8U),
    };
    return append_bytes(fixture, bytes, sizeof(bytes));
}

static bool append_i32(TextureFixture* fixture, int32_t value) {
    return append_u32(fixture, (uint32_t)value);
}

static bool append_u64(TextureFixture* fixture, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
    return append_bytes(fixture, bytes, sizeof(bytes));
}

static bool append_align4(TextureFixture* fixture) {
    while ((fixture->size & 3U) != 0U) {
        if (!append_u8(fixture, 0U)) return false;
    }
    return true;
}

static bool append_align(TextureFixture* fixture, size_t alignment) {
    if (!fixture || alignment == 0U) return false;
    while ((fixture->size % alignment) != 0U) {
        if (!append_u8(fixture, 0U)) return false;
    }
    return true;
}

static void patch_be32(TextureFixture* fixture, size_t offset,
                       uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        fixture->bytes[offset + index] =
            (uint8_t)(value >> (24U - index * 8U));
    }
}

static void patch_be64(TextureFixture* fixture, size_t offset,
                       uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        fixture->bytes[offset + index] =
            (uint8_t)(value >> (56U - index * 8U));
    }
}

static bool append_string(TextureFixture* fixture, const char* value) {
    size_t size = strlen(value);
    return size <= UINT32_MAX && append_u32(fixture, (uint32_t)size) &&
        append_bytes(fixture, value, size) && append_align4(fixture);
}

static bool append_array(TextureFixture* fixture, const uint8_t* bytes,
                         size_t size) {
    return size <= UINT32_MAX && append_u32(fixture, (uint32_t)size) &&
        append_bytes(fixture, bytes, size) && append_align4(fixture);
}

static bool append_settings(TextureFixture* fixture, int32_t filter,
                            int32_t aniso, int32_t wrap) {
    return append_i32(fixture, filter) && append_i32(fixture, aniso) &&
        append_u32(fixture, 0U) && append_i32(fixture, wrap) &&
        append_i32(fixture, wrap) && append_i32(fixture, wrap);
}

static void finish_fixture(TextureFixture* fixture, int32_t class_id,
                           const uint8_t hash[16]) {
    fixture->type.type_id = class_id;
    fixture->type.script_type_index = UINT16_MAX;
    memcpy(fixture->type.type_hash, hash, 16U);
    fixture->info.path_id = INT64_C(77);
    fixture->info.byte_size = (uint32_t)fixture->size;
    fixture->info.type_id_or_index = 0;
    fixture->info.type_id = class_id;
    fixture->info.script_type_index = UINT16_MAX;
    fixture->file.file_size = fixture->size;
    fixture->file.version = 22U;
    fixture->file.unity_version = (char*)"2021.3.35f1";
    fixture->file.target_platform = 19U;
    fixture->file.type_count = 1;
    fixture->file.types = &fixture->type;
    fixture->file.object_count = 1;
    fixture->file.objects = &fixture->info;
    fixture->file.raw_data = fixture->bytes;
    fixture->file.raw_size = fixture->size;
}

static bool build_texture2d_with_stream(
    TextureFixture* fixture, bool empty, bool incoherent_empty,
    bool platform_blob, const char* stream_path) {
    memset(fixture, 0, sizeof(*fixture));
    static const uint8_t platform[] = {0x44U};
    const char* name = empty ? "Font Texture" : "Probe \"RGBA\"";
    if (!empty && !stream_path) return false;
    if (!append_string(fixture, name) || !append_i32(fixture, 4)) return false;
    fixture->first_boolean_offset = fixture->size;
    if (!append_u8(fixture, 0U) || !append_u8(fixture, 1U) ||
        !append_align4(fixture) ||
        !append_i32(fixture, empty ? 0 : 1) ||
        !append_i32(fixture, empty ? 0 : 1) ||
        !append_u32(fixture, empty ? 0U : 4U) ||
        !append_i32(fixture, 0) || !append_i32(fixture, 4) ||
        !append_i32(fixture, 1) ||
        !append_u8(fixture, 0U) || !append_u8(fixture, 0U) ||
        !append_u8(fixture, 0U) || !append_u8(fixture, 0U) ||
        !append_align4(fixture) || !append_i32(fixture, 0) ||
        !append_i32(fixture, empty ? (incoherent_empty ? 1 : 0) : 1) ||
        !append_i32(fixture, 2) ||
        !append_settings(fixture, 1, 1, empty ? 0 : 1) ||
        !append_i32(fixture, empty ? 0 : 3) ||
        !append_i32(fixture, empty ? 0 : 1) ||
        !append_array(fixture, platform_blob ? platform : NULL,
                      platform_blob ? sizeof(platform) : 0U) ||
        !append_array(fixture, NULL, 0U) ||
        !append_u64(fixture, empty ? 0U : 9U) ||
        !append_u32(fixture, empty ? 0U : 4U) ||
        !append_string(fixture, empty ? "" : stream_path)) {
        return false;
    }
    finish_fixture(fixture, UNITY_TEXTURE2D_CLASS_ID, k_texture2d_hash);
    return true;
}

static bool build_texture2d(TextureFixture* fixture, bool empty,
                            bool incoherent_empty, bool platform_blob) {
    return build_texture2d_with_stream(
        fixture, empty, incoherent_empty, platform_blob,
        "sharedassets0.assets.resS");
}

static bool build_serialized_texture_file(
    TextureFixture* file, const TextureFixture* payload) {
    static const uint8_t zeros[48] = {0};
    static const char unity_version[] = "2021.3.35f1";
    if (!file || !payload || payload->size > UINT32_MAX) return false;
    memset(file, 0, sizeof(*file));
    if (!append_bytes(file, zeros, sizeof(zeros))) return false;
    const size_t metadata_start = file->size;
    if (!append_bytes(file, unity_version, sizeof(unity_version)) ||
        !append_u32(file, 19U) || !append_u8(file, 0U) ||
        !append_u32(file, 1U) ||
        !append_i32(file, UNITY_TEXTURE2D_CLASS_ID) ||
        !append_u8(file, 0U) || !append_u16(file, UINT16_MAX) ||
        !append_bytes(file, k_texture2d_hash, sizeof(k_texture2d_hash)) ||
        !append_u32(file, 1U) || !append_align4(file) ||
        !append_u64(file, UINT64_C(77)) || !append_u64(file, 0U) ||
        !append_u32(file, (uint32_t)payload->size) ||
        !append_u32(file, 0U) || !append_u32(file, 0U) ||
        !append_u32(file, 0U) || !append_u32(file, 0U) ||
        !append_u8(file, 0U)) {
        return false;
    }
    const size_t metadata_size = file->size - metadata_start;
    if (metadata_size > UINT32_MAX || !append_align(file, 16U)) {
        return false;
    }
    const size_t data_offset = file->size;
    if (!append_bytes(file, payload->bytes, payload->size)) return false;
    patch_be32(file, 8U, 22U);
    patch_be32(file, 20U, (uint32_t)metadata_size);
    patch_be64(file, 24U, file->size);
    patch_be64(file, 32U, data_offset);
    return true;
}

static bool build_render_texture(TextureFixture* fixture) {
    memset(fixture, 0, sizeof(*fixture));
    if (!append_string(fixture, "FixtureRenderTarget") ||
        !append_i32(fixture, 4)) return false;
    fixture->first_boolean_offset = fixture->size;
    if (!append_u8(fixture, 0U) || !append_u8(fixture, 0U) ||
        !append_align4(fixture) || !append_i32(fixture, 256) ||
        !append_i32(fixture, 256) || !append_i32(fixture, 1) ||
        !append_i32(fixture, -1) || !append_i32(fixture, 94) ||
        !append_i32(fixture, 8) || !append_u8(fixture, 0U) ||
        !append_u8(fixture, 1U) || !append_u8(fixture, 0U) ||
        !append_u8(fixture, 0U) || !append_u8(fixture, 0U) ||
        !append_u8(fixture, 1U) || !append_u8(fixture, 0U) ||
        !append_align4(fixture) || !append_settings(fixture, 1, 0, 1) ||
        !append_i32(fixture, 2) || !append_i32(fixture, 1) ||
        !append_i32(fixture, 2)) {
        return false;
    }
    finish_fixture(fixture, UNITY_RENDER_TEXTURE_CLASS_ID,
                   k_render_texture_hash);
    return true;
}

static bool contains(const StringBuilder* builder, const char* text) {
    return builder && builder->buf && strstr(builder->buf, text) != NULL;
}

static bool texture_reference_is_empty(
    const MaterialBatchTextureReference* reference) {
    return reference && reference->file_id == 0 &&
        reference->guid[0] == '\0' && reference->type == 0 &&
        !reference->asset_exported;
}

static bool close_texture_reference_lease(
    MaterialBatchTextureReference* reference) {
    if (!reference || !reference->publication_lease) return false;
    void* lease = reference->publication_lease;
    reference->publication_lease = NULL;
    return native_texture_batch_close_publication_lease(lease);
}

static bool write_new_bytes(const char* path, const uint8_t* bytes,
                            size_t size) {
    (void)remove(path);
    return common_file_write_new_atomic(path, bytes, size) ==
        COMMON_FILE_OK;
}

static char* duplicate_text(const char* text) {
    if (!text) return NULL;
    size_t size = strlen(text);
    char* copy = (char*)malloc(size + 1U);
    if (copy) memcpy(copy, text, size + 1U);
    return copy;
}

static char* duplicate_parent_path(const char* path) {
    char* copy = duplicate_text(path);
    if (!copy) return NULL;
    char* separator = NULL;
    for (char* cursor = copy; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') separator = cursor;
    }
    if (!separator) {
        free(copy);
        return NULL;
    }
    *separator = '\0';
    return copy;
}

static int test_texture2d_decode_and_emit(void) {
    TextureFixture fixture;
    CHECK(build_texture2d(&fixture, false, false, false));
    UnityTextureObject object;
    unity_texture_object_init(&object);
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_OK);
    CHECK(object.class_id == UNITY_TEXTURE2D_CLASS_ID);
    CHECK(object.payload.texture2d.stream_offset == 9U);
    CHECK(object.payload.texture2d.stream_size == 4U);
    CHECK(unity_native_texture_yaml_export_shape_is_supported(&object));
#ifndef _WIN32
    static const uint8_t literal_backslash_sidecar[] =
        "sidecar\\literal.resS";
    UnityTextureObject literal_backslash = object;
    literal_backslash.payload.texture2d.stream_path.bytes =
        literal_backslash_sidecar;
    literal_backslash.payload.texture2d.stream_path.size =
        sizeof(literal_backslash_sidecar) - 1U;
    CHECK(unity_native_texture_yaml_export_shape_is_supported(
        &literal_backslash));
#endif

    const uint8_t pixels[] = {0xabU, 0xcdU, 0x01U, 0xefU};
    StringBuilder yaml;
    sb_init(&yaml);
    CHECK(unity_native_texture_yaml_emit(
              &object, pixels, sizeof(pixels), &yaml) ==
          UNITY_NATIVE_TEXTURE_YAML_OK);
    CHECK(contains(&yaml, "--- !u!28 &2800000"));
    CHECK(contains(&yaml, "m_Name: \"Probe \\\"RGBA\\\"\""));
    CHECK(contains(&yaml, "image data: 4\n  _typelessdata: abcd01ef"));
    CHECK(contains(&yaml, "m_StreamData:\n    serializedVersion: 2\n"
                          "    offset: 0\n    size: 0\n    path: \n"));
    sb_free(&yaml);

    UnityTextureObject unsupported = object;
    unsupported.payload.texture2d.texture_format = 99;
    sb_init(&yaml);
    CHECK(unity_native_texture_yaml_emit(
              &unsupported, pixels, sizeof(pixels), &yaml) ==
          UNITY_NATIVE_TEXTURE_YAML_EXPORT_SHAPE_UNSUPPORTED);
    sb_free(&yaml);
    unsupported = object;
    unsupported.payload.texture2d.streaming_mipmaps = true;
    CHECK(!unity_native_texture_yaml_export_shape_is_supported(&unsupported));
    unsupported = object;
    unsupported.payload.texture2d.complete_image_size = 3U;
    unsupported.payload.texture2d.stream_size = 3U;
    sb_init(&yaml);
    CHECK(unity_native_texture_yaml_emit(
              &unsupported, pixels, sizeof(pixels), &yaml) ==
          UNITY_NATIVE_TEXTURE_YAML_EXPORT_SHAPE_UNSUPPORTED);
    sb_free(&yaml);

    unsupported = object;
    unsupported.payload.texture2d.width = INT32_MAX;
    unsupported.payload.texture2d.height = INT32_MAX;
    unsupported.payload.texture2d.complete_image_size = 1U;
    unsupported.payload.texture2d.stream_size = 1U;
    CHECK(!unity_native_texture_yaml_export_shape_is_supported(&unsupported));
    unsupported = object;
    unsupported.payload.texture2d.mip_count = 2;
    unsupported.payload.texture2d.complete_image_size = 8U;
    unsupported.payload.texture2d.stream_size = 8U;
    CHECK(!unity_native_texture_yaml_export_shape_is_supported(&unsupported));
    unsupported = object;
    unsupported.payload.texture2d.texture_format = 10;
    unsupported.payload.texture2d.width = 5;
    unsupported.payload.texture2d.height = 5;
    unsupported.payload.texture2d.complete_image_size = 32U;
    unsupported.payload.texture2d.stream_size = 32U;
    CHECK(unity_native_texture_yaml_export_shape_is_supported(&unsupported));
    unsupported.payload.texture2d.complete_image_size = 31U;
    unsupported.payload.texture2d.stream_size = 31U;
    CHECK(!unity_native_texture_yaml_export_shape_is_supported(&unsupported));
    return 0;
}

static int test_zero_and_fail_closed_shapes(void) {
    TextureFixture fixture;
    UnityTextureObject object;
    CHECK(build_texture2d(&fixture, true, false, false));
    unity_texture_object_init(&object);
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_OK);
    CHECK(unity_native_texture_yaml_export_shape_is_supported(&object));
    StringBuilder yaml;
    sb_init(&yaml);
    CHECK(unity_native_texture_yaml_emit(&object, NULL, 0U, &yaml) ==
          UNITY_NATIVE_TEXTURE_YAML_OK);
    CHECK(contains(&yaml, "m_Width: 0\n  m_Height: 0"));
    CHECK(contains(&yaml, "image data: 0\n  _typelessdata: \n"));
    sb_free(&yaml);

    CHECK(build_texture2d(&fixture, true, true, false));
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_MODEL_INVALID);

    CHECK(build_texture2d(&fixture, false, false, true));
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_OK);
    CHECK(!unity_native_texture_yaml_export_shape_is_supported(&object));
    return 0;
}

static int test_identity_and_wire_failures(void) {
    TextureFixture fixture;
    UnityTextureObject object;
    CHECK(build_texture2d(&fixture, false, false, false));
    fixture.file.target_platform = 13U;
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_UNSUPPORTED_TARGET_PLATFORM);

    CHECK(build_texture2d(&fixture, false, false, false));
    fixture.file.big_endian = true;
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_UNSUPPORTED_BYTE_ORDER);
    CHECK(strcmp(unity_texture_object_status_name(
                     UNITY_TEXTURE_OBJECT_UNSUPPORTED_BYTE_ORDER),
                 "unsupported-byte-order") == 0);

    CHECK(build_texture2d(&fixture, false, false, false));
    fixture.type.type_hash[0] ^= 1U;
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_TYPE_IDENTITY_UNSUPPORTED);

    CHECK(build_texture2d(&fixture, false, false, false));
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_OK);
    object.target_platform = 13U;
    CHECK(!unity_texture_object_has_layout_authority(&object));
    object.target_platform = 19U;
    object.serialized_big_endian = true;
    CHECK(!unity_texture_object_has_layout_authority(&object));

    CHECK(build_texture2d(&fixture, false, false, false));
    fixture.bytes[fixture.first_boolean_offset] = 2U;
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_BOOLEAN_INVALID);

    CHECK(build_texture2d(&fixture, false, false, false));
    CHECK(append_u8(&fixture, 0U));
    fixture.info.byte_size = (uint32_t)fixture.size;
    fixture.file.file_size = fixture.size;
    fixture.file.raw_size = fixture.size;
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_OBJECT_BYTES_NOT_EXHAUSTED);
    return 0;
}

static int test_render_texture_and_meta(void) {
    TextureFixture fixture;
    UnityTextureObject object;
    CHECK(build_render_texture(&fixture));
    CHECK(unity_texture_object_decode_borrowed(
              &object, &fixture.file, &fixture.info) ==
          UNITY_TEXTURE_OBJECT_OK);
    CHECK(unity_native_texture_yaml_export_shape_is_supported(&object));
    StringBuilder yaml;
    sb_init(&yaml);
    CHECK(unity_native_texture_yaml_emit(&object, NULL, 0U, &yaml) ==
          UNITY_NATIVE_TEXTURE_YAML_OK);
    CHECK(contains(&yaml, "--- !u!84 &8400000"));
    CHECK(contains(&yaml, "m_DepthStencilFormat: 94"));
    sb_free(&yaml);

    UnityTextureObject variant = object;
    variant.payload.render_texture.width = 257;
    variant.payload.render_texture.height = 193;
    CHECK(unity_native_texture_yaml_export_shape_is_supported(&variant));
    sb_init(&yaml);
    CHECK(unity_native_texture_yaml_emit(&variant, NULL, 0U, &yaml) ==
          UNITY_NATIVE_TEXTURE_YAML_OK);
    CHECK(contains(&yaml, "m_Width: 257"));
    CHECK(contains(&yaml, "m_Height: 193"));
    sb_free(&yaml);

    UnityTextureObject unsupported = object;
    unsupported.payload.render_texture.width = 0;
    CHECK(!unity_native_texture_yaml_export_shape_is_supported(&unsupported));
    unsupported = object;
    unsupported.payload.render_texture.height = -1;
    CHECK(!unity_native_texture_yaml_export_shape_is_supported(&unsupported));
    unsupported = object;
    unsupported.payload.render_texture.depth_stencil_format = 1;
    CHECK(!unity_native_texture_yaml_export_shape_is_supported(&unsupported));

    sb_init(&yaml);
    CHECK(unity_native_texture_meta_emit(
              UNITY_RENDER_TEXTURE_CLASS_ID,
              "00112233445566778899aabbccddeeff", &yaml) ==
          UNITY_NATIVE_TEXTURE_YAML_OK);
    CHECK(contains(&yaml, "mainObjectFileID: 8400000"));
    sb_free(&yaml);
    return 0;
}

static int test_resource_path_and_identity(void) {
    ShaderCatalogSource source;
    memset(&source, 0, sizeof(source));
#ifdef _WIN32
    source.outer_path = (char*)"C:\\game\\Data\\sharedassets0.assets";
    const char* expected =
        "C:\\game\\Data\\sharedassets0.assets.resS";
#else
    source.outer_path = (char*)"/game/Data/sharedassets0.assets";
    const char* expected = "/game/Data/sharedassets0.assets.resS";
#endif
    source.scope_root = (char*)"/game";
    const uint8_t name[] = "sharedassets0.assets.resS";
    UnityTextureByteView path = {name, sizeof(name) - 1U};
    char* resolved = native_texture_batch_resolve_loose_resource_path(
        &source, path);
    CHECK(resolved != NULL);
    CHECK(strcmp(resolved, expected) == 0);
    CHECK(strcmp(resolved, source.scope_root) != 0);
    ShaderCatalogSource same_sidecar_source = source;
#ifdef _WIN32
    same_sidecar_source.outer_path =
        (char*)"C:\\game\\Data\\another.assets";
#else
    same_sidecar_source.outer_path = (char*)"/game/Data/another.assets";
#endif
    char* same_sidecar = native_texture_batch_resolve_loose_resource_path(
        &same_sidecar_source, path);
    CHECK(same_sidecar != NULL);
    CHECK(strcmp(same_sidecar, resolved) == 0);
    free(same_sidecar);
    free(resolved);
#ifndef _WIN32
    const uint8_t backslash_name[] = "sidecar\\literal.resS";
    path.bytes = backslash_name;
    path.size = sizeof(backslash_name) - 1U;
    resolved = native_texture_batch_resolve_loose_resource_path(
        &source, path);
    CHECK(resolved != NULL);
    CHECK(strcmp(resolved, "/game/Data/sidecar\\literal.resS") == 0);
    free(resolved);
#endif
    const uint8_t unsafe[] = "../sharedassets0.assets.resS";
    path.bytes = unsafe;
    path.size = sizeof(unsafe) - 1U;
    CHECK(native_texture_batch_resolve_loose_resource_path(
              &source, path) == NULL);
    source.is_bundle_member = true;
    path.bytes = name;
    path.size = sizeof(name) - 1U;
    CHECK(native_texture_batch_resolve_loose_resource_path(
              &source, path) == NULL);

    uint8_t serialized_a[COMMON_SHA256_DIGEST_SIZE] = {0};
    uint8_t serialized_b[COMMON_SHA256_DIGEST_SIZE] = {0};
    uint8_t resource_a[COMMON_SHA256_DIGEST_SIZE] = {0};
    uint8_t resource_b[COMMON_SHA256_DIGEST_SIZE] = {0};
    serialized_b[0] = 1U;
    resource_b[0] = 1U;
    char identity_a[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char identity_b[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char guid_a[UNITY_ASSET_GUID_TEXT_CAPACITY];
    char guid_b[UNITY_ASSET_GUID_TEXT_CAPACITY];
    CHECK(native_texture_batch_derive_asset_identity(
        serialized_a, 7, UNITY_TEXTURE2D_CLASS_ID, resource_a,
        identity_a, guid_a));
    CHECK(native_texture_batch_derive_asset_identity(
        serialized_b, 7, UNITY_TEXTURE2D_CLASS_ID, resource_a,
        identity_b, guid_b));
    CHECK(strcmp(identity_a, identity_b) != 0);
    CHECK(strcmp(guid_a, guid_b) != 0);
    CHECK(native_texture_batch_derive_asset_identity(
        serialized_a, 7, UNITY_TEXTURE2D_CLASS_ID, resource_b,
        identity_b, guid_b));
    CHECK(strcmp(identity_a, identity_b) != 0);
    CHECK(strcmp(guid_a, guid_b) != 0);

    char residue_path[192];
    int written = snprintf(
        residue_path, sizeof(residue_path),
        "dxbc_native_texture_residue_%lu.bin", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(residue_path));
    (void)remove(residue_path);
    static const uint8_t exact_bytes[] = {0x31U, 0x72U, 0xa4U};
    CHECK(common_file_write_new_atomic(
              residue_path, exact_bytes, sizeof(exact_bytes)) ==
          COMMON_FILE_OK);
    CHECK(!native_texture_batch_publication_residue_possible(
        false, COMMON_OUTPUT_PREFLIGHT_MISSING,
        COMMON_OUTPUT_PUBLISH_IO_ERROR, residue_path,
        exact_bytes, sizeof(exact_bytes)));
    CHECK(native_texture_batch_publication_residue_possible(
        true, COMMON_OUTPUT_PREFLIGHT_MISSING,
        COMMON_OUTPUT_PUBLISH_IO_ERROR, residue_path,
        exact_bytes, sizeof(exact_bytes)));
    CHECK(remove(residue_path) == 0);
    return 0;
}

static int test_resolver_authority_and_partial_publication(void) {
    ShaderCatalog catalog;
    ShaderCatalog other_catalog;
    shader_catalog_init(&catalog);
    shader_catalog_init(&other_catalog);
    char published_asset_path[192];
    char published_meta_path[192];
    int written = snprintf(
        published_asset_path, sizeof(published_asset_path),
        "dxbc_resolver_asset_%lu.texture2D", TEST_PROCESS_ID());
    CHECK(written > 0 &&
          (size_t)written < sizeof(published_asset_path));
    written = snprintf(
        published_meta_path, sizeof(published_meta_path),
        "dxbc_resolver_asset_%lu.texture2D.meta", TEST_PROCESS_ID());
    CHECK(written > 0 &&
          (size_t)written < sizeof(published_meta_path));
    (void)remove(published_asset_path);
    (void)remove(published_meta_path);
    static const uint8_t published_asset[] = "published texture\n";
    uint8_t resource_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(published_asset, sizeof(published_asset) - 1U,
                  resource_digest);
    uint8_t serialized_digest[COMMON_SHA256_DIGEST_SIZE] = {0};
    serialized_digest[0] = 9U;
    char artifact_identity[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    char asset_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    CHECK(native_texture_batch_derive_asset_identity(
        serialized_digest, 77, UNITY_TEXTURE2D_CLASS_ID,
        resource_digest, artifact_identity, asset_guid));
    StringBuilder published_meta;
    sb_init(&published_meta);
    CHECK(unity_native_texture_meta_emit(
              UNITY_TEXTURE2D_CLASS_ID, asset_guid, &published_meta) ==
          UNITY_NATIVE_TEXTURE_YAML_OK);
    CHECK(common_file_write_new_atomic(
              published_asset_path, published_asset,
              sizeof(published_asset) - 1U) == COMMON_FILE_OK);
    CHECK(common_file_write_new_atomic(
              published_meta_path, published_meta.buf,
              published_meta.len) == COMMON_FILE_OK);
    ShaderCatalogSource source;
    memset(&source, 0, sizeof(source));
    memcpy(source.occurrence_id, "f:source-a", sizeof("f:source-a"));
    memcpy(source.serialized_digest, serialized_digest,
           sizeof(source.serialized_digest));
    source.outer_path = (char*)"source-a.assets";
    source.scope_root = (char*)".";
    AssetObjectInfo target;
    memset(&target, 0, sizeof(target));
    target.type_id = UNITY_TEXTURE2D_CLASS_ID;
    target.path_id = 77;
    source.objects = &target;
    source.object_reference_count = 1U;
    catalog.sources = &source;
    catalog.source_count = 1U;
    MaterialTextureProperty property;
    memset(&property, 0, sizeof(property));
    property.texture.file_id = 0;
    property.texture.path_id = target.path_id;
    ShaderCatalogMaterialRecord material;
    memset(&material, 0, sizeof(material));
    material.status = SHADER_CATALOG_MATERIAL_READY;
    material.source_index = 0U;
    material.object.decoded = true;
    material.object.texture_properties = &property;
    material.object.texture_property_count = 1U;
    catalog.materials_included = true;
    catalog.materials = &material;
    catalog.material_count = 1U;
    UnityPPtrResolveResult resolution;
    unity_pptr_resolve_result_init(&resolution);
    resolution.status = UNITY_PPTR_RESOLVE_LOCAL;
    resolution.source_index = 0U;
    resolution.target_index = 0U;
    resolution.object = &target;
    MaterialBatchTextureReferenceRequest request;
    memset(&request, 0, sizeof(request));
    request.catalog = &catalog;
    request.material_index = 0U;
    request.texture_property_index = 0U;
    request.source_index = 0U;
    request.serialized_pointer = &property.texture;
    request.target_source = &source;
    request.target_object = &target;
    request.resolution = &resolution;

    NativeTextureBatchRecordResult record;
    memset(&record, 0, sizeof(record));
    record.source_index = 0U;
    memcpy(record.source_serialized_digest, source.serialized_digest,
           sizeof(record.source_serialized_digest));
    memcpy(record.source_occurrence_id, source.occurrence_id,
           sizeof(record.source_occurrence_id));
    record.class_id = UNITY_TEXTURE2D_CLASS_ID;
    record.path_id = 77;
    record.status = NATIVE_TEXTURE_BATCH_EMITTED;
    record.asset_publish_attempted = true;
    record.meta_publish_attempted = true;
    record.asset_publish_status = COMMON_OUTPUT_PUBLISH_EMITTED;
    record.meta_publish_status = COMMON_OUTPUT_PUBLISH_EMITTED;
    record.output_path = published_asset_path;
    record.output_meta_path = published_meta_path;
    record.published_asset_size = sizeof(published_asset) - 1U;
    record.published_meta_size = published_meta.len;
    common_sha256(published_asset, sizeof(published_asset) - 1U,
                  record.published_asset_digest);
    common_sha256(published_meta.buf, published_meta.len,
                  record.published_meta_digest);
    record.published_content_recorded = true;
    memcpy(record.resource_digest, resource_digest,
           sizeof(record.resource_digest));
    common_sha256_digest_to_hex(record.resource_digest,
                                record.resource_digest_hex);
    memcpy(record.artifact_identity_hex, artifact_identity,
           sizeof(record.artifact_identity_hex));
    memcpy(record.asset_guid, asset_guid, sizeof(record.asset_guid));
    NativeTextureBatchResult result;
    native_texture_batch_result_init(&result);
    result.catalog_authority = &catalog;
    result.source_transaction_complete = true;
    result.staging_cleanup_complete = true;
    result.records = &record;
    result.record_count = 1U;
    MaterialBatchTextureReference reference;
    material_batch_texture_reference_init(&reference);
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(reference.file_id == UNITY_TEXTURE2D_LOCAL_FILE_ID);
    CHECK(reference.type == 2 && reference.asset_exported);
    CHECK(close_texture_reference_lease(&reference));

    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(remove(published_asset_path) == 0);
    CHECK(!close_texture_reference_lease(&reference));
    CHECK(common_file_write_new_atomic(
              published_asset_path, published_asset,
              sizeof(published_asset) - 1U) == COMMON_FILE_OK);

    CHECK(remove(published_asset_path) == 0);
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(texture_reference_is_empty(&reference));
    CHECK(common_file_write_new_atomic(
              published_asset_path, published_asset,
              sizeof(published_asset) - 1U) == COMMON_FILE_OK);

    CHECK(remove(published_meta_path) == 0);
    static const uint8_t replaced_meta[] = "replaced texture meta\n";
    CHECK(common_file_write_new_atomic(
              published_meta_path, replaced_meta,
              sizeof(replaced_meta) - 1U) == COMMON_FILE_OK);
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(texture_reference_is_empty(&reference));
    CHECK(remove(published_meta_path) == 0);
    CHECK(common_file_write_new_atomic(
              published_meta_path, published_meta.buf,
              published_meta.len) == COMMON_FILE_OK);

    char saved_guid[UNITY_ASSET_GUID_TEXT_CAPACITY];
    memcpy(saved_guid, record.asset_guid, sizeof(saved_guid));
    record.asset_guid[0] = record.asset_guid[0] == '0' ? '1' : '0';
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(texture_reference_is_empty(&reference));
    memcpy(record.asset_guid, saved_guid, sizeof(record.asset_guid));

    char saved_identity[COMMON_SHA256_DIGEST_SIZE * 2U + 1U];
    memcpy(saved_identity, record.artifact_identity_hex,
           sizeof(saved_identity));
    record.artifact_identity_hex[0] =
        record.artifact_identity_hex[0] == '0' ? '1' : '0';
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(texture_reference_is_empty(&reference));
    memcpy(record.artifact_identity_hex, saved_identity,
           sizeof(record.artifact_identity_hex));

    request.material_index = 1U;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    request.material_index = 0U;

    request.texture_property_index = 1U;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    request.texture_property_index = 0U;

    AssetPPtr fabricated_pointer = property.texture;
    request.serialized_pointer = &fabricated_pointer;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    request.serialized_pointer = &property.texture;

    request.source_index = 1U;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    request.source_index = 0U;

    UnityPPtrResolveResult fabricated_resolution = resolution;
    fabricated_resolution.external_index = 0U;
    request.resolution = &fabricated_resolution;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    request.resolution = &resolution;

    result.source_transaction_complete = false;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(texture_reference_is_empty(&reference));
    result.source_transaction_complete = true;

    result.staging_cleanup_complete = false;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(texture_reference_is_empty(&reference));
    result.staging_cleanup_complete = true;

    record.meta_publish_attempted = false;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(texture_reference_is_empty(&reference));
    record.meta_publish_attempted = true;

    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(close_texture_reference_lease(&reference));
    resolution.status = UNITY_PPTR_RESOLVE_OUT_OF_SCOPE;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    resolution.status = UNITY_PPTR_RESOLVE_LOCAL;

    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(close_texture_reference_lease(&reference));
    resolution.source_index = 1U;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    resolution.source_index = 0U;

    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(close_texture_reference_lease(&reference));
    resolution.target_index = 1U;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    resolution.target_index = 0U;

    AssetObjectInfo unrelated_target = target;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(close_texture_reference_lease(&reference));
    resolution.object = &unrelated_target;
    request.target_object = &unrelated_target;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    resolution.object = &target;
    request.target_object = &target;

    ShaderCatalogSource unrelated_source = source;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(close_texture_reference_lease(&reference));
    request.target_source = &unrelated_source;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    request.target_source = &source;

    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(close_texture_reference_lease(&reference));
    request.catalog = &other_catalog;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    request.catalog = &catalog;

    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(close_texture_reference_lease(&reference));
    source.occurrence_id[2] = 'x';
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(texture_reference_is_empty(&reference));

    NativeTextureBatchRecordResult* saved_records = result.records;
    result.records = NULL;
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_REJECTED);
    CHECK(texture_reference_is_empty(&reference));
    CHECK(!native_texture_batch_is_complete(&result));
    result.records = saved_records;

    NativeTextureBatchResult empty;
    native_texture_batch_result_init(&empty);
    CHECK(!native_texture_batch_is_complete(&empty));
    empty.catalog_authority = &catalog;
    empty.source_transaction_complete = true;
    CHECK(!native_texture_batch_is_complete(&empty));
    empty.staging_cleanup_complete = true;
    CHECK(native_texture_batch_is_complete(&empty));
    NativeTextureBatchRecordResult dummy_record;
    memset(&dummy_record, 0, sizeof(dummy_record));
    empty.records = &dummy_record;
    empty.record_count = 1U;
    CHECK(!native_texture_batch_is_complete(&empty));
    empty.stats.selected = 1U;
    empty.stats.texture2d = 1U;
    empty.stats.emitted = 1U;
    dummy_record.class_id = UNITY_TEXTURE2D_CLASS_ID;
    dummy_record.status = NATIVE_TEXTURE_BATCH_FAILED;
    CHECK(!native_texture_batch_is_complete(&empty));
    CHECK(remove(published_meta_path) == 0);
    CHECK(remove(published_asset_path) == 0);
    sb_free(&published_meta);
    return 0;
}

static int test_native_batch_success_and_collision(void) {
    TextureFixture payload;
    TextureFixture serialized;
    char source_path[192];
    char resource_path[192];
    char output_root[192];
    char cwd_switch_directory[192];
    char original_cwd[1024];
    char occurrence_id[96];
    int written = snprintf(
        source_path, sizeof(source_path),
        "dxbc_native_texture_source_%lu.assets", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(source_path));
    written = snprintf(
        resource_path, sizeof(resource_path),
        "dxbc_native_texture_resource_%lu.resS", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(resource_path));
    written = snprintf(
        output_root, sizeof(output_root),
        "dxbc_native_texture_output_%lu", TEST_PROCESS_ID());
    CHECK(written > 0 && (size_t)written < sizeof(output_root));
    written = snprintf(
        cwd_switch_directory, sizeof(cwd_switch_directory),
        "dxbc_native_texture_cwd_%lu", TEST_PROCESS_ID());
    CHECK(written > 0 &&
          (size_t)written < sizeof(cwd_switch_directory));
    written = snprintf(
        occurrence_id, sizeof(occurrence_id), "f:%s", source_path);
    CHECK(written > 0 && (size_t)written < sizeof(occurrence_id));

    CHECK(build_texture2d_with_stream(
        &payload, false, false, false, resource_path));
    CHECK(build_serialized_texture_file(&serialized, &payload));
    static const uint8_t resource_bytes[] = {
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0xabU, 0xcdU, 0x01U, 0xefU,
    };
    CHECK(write_new_bytes(source_path, serialized.bytes, serialized.size));
    CHECK(write_new_bytes(
        resource_path, resource_bytes, sizeof(resource_bytes)));

    AssetObjectInfo source_object;
    memset(&source_object, 0, sizeof(source_object));
    source_object.path_id = 77;
    source_object.type_id = UNITY_TEXTURE2D_CLASS_ID;

    ShaderCatalogSource source;
    memset(&source, 0, sizeof(source));
    memcpy(source.occurrence_id, occurrence_id, strlen(occurrence_id) + 1U);
    common_sha256(
        serialized.bytes, serialized.size, source.serialized_digest);
    source.outer_path = source_path;
    source.scope_root = (char*)".";
    source.objects = &source_object;
    source.object_reference_count = 1U;

    MaterialTextureProperty texture_property;
    memset(&texture_property, 0, sizeof(texture_property));
    texture_property.texture.path_id = 77;

    ShaderCatalogMaterialRecord material;
    memset(&material, 0, sizeof(material));
    material.status = SHADER_CATALOG_MATERIAL_READY;
    material.source_index = 0U;
    material.shader_record_index = 0U;
    material.object.decoded = true;
    material.object.texture_properties = &texture_property;
    material.object.texture_property_count = 1U;

    ShaderCatalogRecord shader;
    memset(&shader, 0, sizeof(shader));
    UnityInputSnapshot snapshot;
    unity_input_snapshot_init(&snapshot);
    CHECK(unity_input_snapshot_open(source_path, &snapshot) ==
          UNITY_INPUT_OK);
    char* canonical_source_path = duplicate_text(
        unity_input_snapshot_path(&snapshot));
    CHECK(canonical_source_path != NULL);
    source.outer_path = canonical_source_path;
    CHECK(TEST_GETCWD(original_cwd, sizeof(original_cwd)) != NULL);
    (void)TEST_RMDIR(cwd_switch_directory);
    CHECK(TEST_MKDIR(cwd_switch_directory) == 0);
    char* absolute_output_root = common_output_join_path(
        original_cwd, output_root);
    CHECK(absolute_output_root != NULL);

    ShaderCatalog catalog;
    shader_catalog_init(&catalog);
    catalog.materials_included = true;
    catalog.sources = &source;
    catalog.source_count = 1U;
    catalog.records = &shader;
    catalog.record_count = 1U;
    catalog.materials = &material;
    catalog.material_count = 1U;
    catalog.retained_source_snapshots = &snapshot;
    catalog.retained_source_snapshot_count = 1U;
    bool selected[] = {true};

    NativeTextureBatchResult result;
    native_texture_batch_result_init(&result);
    ShaderCatalog missing_materials = catalog;
    missing_materials.materials = NULL;
    CHECK(native_texture_batch_export(
              &missing_materials, selected, output_root, &result) ==
          NATIVE_TEXTURE_BATCH_INVALID_ARGUMENT);
    ShaderCatalog missing_snapshots = catalog;
    missing_snapshots.retained_source_snapshots = NULL;
    CHECK(native_texture_batch_export(
              &missing_snapshots, selected, output_root, &result) ==
          NATIVE_TEXTURE_BATCH_INVALID_ARGUMENT);
    ShaderCatalog missing_records = catalog;
    missing_records.records = NULL;
    CHECK(native_texture_batch_export(
              &missing_records, selected, output_root, &result) ==
          NATIVE_TEXTURE_BATCH_INVALID_ARGUMENT);
    ShaderCatalogMaterialRecord malformed_material = material;
    malformed_material.object.texture_properties = NULL;
    ShaderCatalog malformed_material_catalog = catalog;
    malformed_material_catalog.materials = &malformed_material;
    CHECK(native_texture_batch_export(
              &malformed_material_catalog, selected, output_root, &result) ==
          NATIVE_TEXTURE_BATCH_INVALID_ARGUMENT);
    CHECK(TEST_CHDIR(cwd_switch_directory) == 0);
    NativeTextureBatchStatus cwd_export_status =
        native_texture_batch_export(
            &catalog, selected, absolute_output_root, &result);
    int cwd_restore_status = TEST_CHDIR(original_cwd);
    CHECK(cwd_restore_status == 0);
    CHECK(cwd_export_status == NATIVE_TEXTURE_BATCH_OK);
    CHECK(native_texture_batch_is_complete(&result));

    /* Completeness rebuilds the exact unique target population from the
     * borrowed current selection.  Flipping the bit after export turns this
     * record into an unauthorized extra target and must fail closed. */
    selected[0] = false;
    CHECK(!native_texture_batch_is_complete(&result));
    selected[0] = true;
    CHECK(native_texture_batch_is_complete(&result));

    CHECK(!unity_input_snapshot_is_open(&snapshot));
    CHECK(result.record_count == 1U);
    CHECK(result.stats.selected == 1U && result.stats.emitted == 1U &&
          result.stats.unchanged == 0U && result.stats.failed == 0U);
    CHECK(result.records[0].status == NATIVE_TEXTURE_BATCH_EMITTED);
    CHECK(native_texture_batch_record_is_published(&result.records[0]));
    CHECK(result.records[0].output_path != NULL);
    CHECK(result.records[0].output_meta_path != NULL);
    CHECK(result.records[0].owned_staged_bytes == NULL);
    CHECK(result.records[0].owned_staged_size == 0U);
    CHECK(result.records[0].staging_path == NULL);
    CHECK(result.staging_directory == NULL);
    CHECK(result.staging_cleanup_complete);

    /* Counter-consistent empty records cannot omit a selected native target. */
    NativeTextureBatchRecordResult* saved_target_records = result.records;
    size_t saved_target_record_count = result.record_count;
    NativeTextureBatchStats saved_target_stats = result.stats;
    result.records = NULL;
    result.record_count = 0U;
    memset(&result.stats, 0, sizeof(result.stats));
    CHECK(!native_texture_batch_is_complete(&result));
    result.records = saved_target_records;
    result.record_count = saved_target_record_count;
    result.stats = saved_target_stats;
    CHECK(native_texture_batch_is_complete(&result));

    char* asset_path = duplicate_text(result.records[0].output_path);
    char* meta_path = duplicate_text(result.records[0].output_meta_path);
    char* artifact_directory = duplicate_parent_path(
        result.records[0].output_path);
    char* dependency_directory = common_output_join_path(
        output_root, "texture-dependencies");
    CHECK(asset_path && meta_path && artifact_directory &&
          dependency_directory);

    UnityPPtrResolveResult resolution;
    unity_pptr_resolve_result_init(&resolution);
    resolution.status = UNITY_PPTR_RESOLVE_LOCAL;
    resolution.source_index = 0U;
    resolution.target_index = 0U;
    resolution.object = &source_object;
    MaterialBatchTextureReferenceRequest request;
    memset(&request, 0, sizeof(request));
    request.catalog = &catalog;
    request.material_index = 0U;
    request.texture_property_index = 0U;
    request.source_index = 0U;
    request.serialized_pointer = &texture_property.texture;
    request.target_source = &source;
    request.target_object = &source_object;
    request.resolution = &resolution;
    MaterialBatchTextureReference reference;
    material_batch_texture_reference_init(&reference);
    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_OK);
    CHECK(reference.file_id == UNITY_TEXTURE2D_LOCAL_FILE_ID);
    CHECK(reference.guid[0] != '\0' && reference.type == 2 &&
          reference.asset_exported);
    CHECK(close_texture_reference_lease(&reference));

    /* The completeness result and per-record publication bit come from the
     * same identity census.  Removing one companion invalidates both even
     * though the aggregate counters are unchanged. */
    bool publication_snapshot[1] = {false};
    CHECK(native_texture_batch_validate_publication_snapshot(
        &result, publication_snapshot, 1U));
    CHECK(publication_snapshot[0]);
    StringBuilder restored_meta;
    sb_init(&restored_meta);
    CHECK(unity_native_texture_meta_emit(
              result.records[0].class_id, result.records[0].asset_guid,
              &restored_meta) == UNITY_NATIVE_TEXTURE_YAML_OK);
    CHECK(remove(meta_path) == 0);
    publication_snapshot[0] = true;
    CHECK(!native_texture_batch_validate_publication_snapshot(
        &result, publication_snapshot, 1U));
    CHECK(!publication_snapshot[0]);
    CHECK(!native_texture_batch_is_complete(&result));
    CHECK(common_file_write_new_atomic(
              meta_path, restored_meta.buf, restored_meta.len) ==
          COMMON_FILE_OK);
    CHECK(native_texture_batch_is_complete(&result));
    sb_free(&restored_meta);

    /* Record/summary status is a derived view of the asset/meta publication
     * ledger.  Byte-exact live files cannot authorize a contradictory view. */
    CHECK(result.records[0].asset_publish_status ==
          COMMON_OUTPUT_PUBLISH_EMITTED);
    CHECK(result.records[0].meta_publish_status ==
          COMMON_OUTPUT_PUBLISH_EMITTED);
    result.records[0].asset_publish_status =
        COMMON_OUTPUT_PUBLISH_UNCHANGED;
    result.records[0].meta_publish_status =
        COMMON_OUTPUT_PUBLISH_UNCHANGED;
    publication_snapshot[0] = true;
    CHECK(!native_texture_batch_validate_publication_snapshot(
        &result, publication_snapshot, 1U));
    CHECK(!publication_snapshot[0]);
    result.records[0].asset_publish_status = COMMON_OUTPUT_PUBLISH_EMITTED;
    result.records[0].meta_publish_status = COMMON_OUTPUT_PUBLISH_EMITTED;
    result.records[0].status = NATIVE_TEXTURE_BATCH_UNCHANGED;
    result.stats.emitted = 0U;
    result.stats.unchanged = 1U;
    CHECK(!native_texture_batch_is_complete(&result));
    result.records[0].status = NATIVE_TEXTURE_BATCH_EMITTED;
    result.stats.emitted = 1U;
    result.stats.unchanged = 0U;
    CHECK(native_texture_batch_is_complete(&result));

    /* Report-v7 completeness is also bound to the originating catalog and to
     * the successful terminal decoder/YAML/resource/owned-staging state.  A
     * live byte-exact asset pair alone cannot authorize contradictory record
     * provenance or staging evidence. */
    const ShaderCatalog* saved_catalog_authority = result.catalog_authority;
    result.catalog_authority = NULL;
    CHECK(!native_texture_batch_is_complete(&result));
    result.catalog_authority = saved_catalog_authority;

    size_t saved_source_index = result.records[0].source_index;
    result.records[0].source_index = SIZE_MAX;
    publication_snapshot[0] = true;
    CHECK(!native_texture_batch_validate_publication_snapshot(
        &result, publication_snapshot, 1U));
    CHECK(!publication_snapshot[0]);
    result.records[0].source_index = saved_source_index;

    UnityTextureObjectStatus saved_object_status =
        result.records[0].object_status;
    result.records[0].object_status =
        UNITY_TEXTURE_OBJECT_OBJECT_RANGE_INVALID;
    CHECK(!native_texture_batch_is_complete(&result));
    result.records[0].object_status = saved_object_status;

    result.records[0].yaml_attempted = false;
    CHECK(!native_texture_batch_is_complete(&result));
    result.records[0].yaml_attempted = true;

    CommonFileStatus saved_resource_status =
        result.records[0].resource_status;
    result.records[0].resource_status = COMMON_FILE_IO_ERROR;
    CHECK(!native_texture_batch_is_complete(&result));
    result.records[0].resource_status = saved_resource_status;

    result.records[0].owned_staged_size = 1U;
    CHECK(!native_texture_batch_is_complete(&result));
    result.records[0].owned_staged_size = 0U;

    result.staging_directory = (char*)"synthetic-staging-residue";
    CHECK(!native_texture_batch_is_complete(&result));
    result.staging_directory = NULL;
    CHECK(native_texture_batch_is_complete(&result));

    native_texture_batch_result_dispose(&result);
    static const uint8_t collision[] = "collision";
    CHECK(remove(asset_path) == 0);
    CHECK(common_file_write_new_atomic(
              asset_path, collision, sizeof(collision) - 1U) ==
          COMMON_FILE_OK);
    unity_input_snapshot_init(&snapshot);
    CHECK(unity_input_snapshot_open(source_path, &snapshot) ==
          UNITY_INPUT_OK);
    CHECK(native_texture_batch_export(
              &catalog, selected, output_root, &result) ==
          NATIVE_TEXTURE_BATCH_OK);
    CHECK(!native_texture_batch_is_complete(&result));
    CHECK(result.source_transaction_complete);
    CHECK(result.staging_cleanup_complete);
    CHECK(!unity_input_snapshot_is_open(&snapshot));
    CHECK(result.record_count == 1U);
    CHECK(result.stats.selected == 1U && result.stats.emitted == 0U &&
          result.stats.unchanged == 0U && result.stats.failed == 1U);
    CHECK(result.records[0].status == NATIVE_TEXTURE_BATCH_FAILED);
    CHECK(result.records[0].failure ==
          NATIVE_TEXTURE_BATCH_FAILURE_OUTPUT_COLLISION);
    CHECK(!result.records[0].publication_residue);
    CHECK(!native_texture_batch_record_is_published(&result.records[0]));

    CHECK(native_texture_batch_resolve_reference(
              &request, &result, &reference) ==
          MATERIAL_BATCH_TEXTURE_REFERENCE_TARGET_ASSET_UNEXPORTED);
    CHECK(reference.file_id == 0 && reference.guid[0] == '\0' &&
          reference.type == 0 && !reference.asset_exported);

    native_texture_batch_result_dispose(&result);
    CHECK(remove(meta_path) == 0);
    CHECK(remove(asset_path) == 0);
    CHECK(TEST_RMDIR(artifact_directory) == 0);
    CHECK(TEST_RMDIR(dependency_directory) == 0);
    CHECK(TEST_RMDIR(output_root) == 0);
    CHECK(remove(resource_path) == 0);
    CHECK(remove(source_path) == 0);
    CHECK(TEST_RMDIR(cwd_switch_directory) == 0);
    free(dependency_directory);
    free(artifact_directory);
    free(meta_path);
    free(asset_path);
    free(absolute_output_root);
    free(canonical_source_path);
    return 0;
}

int main(void) {
    if (test_texture2d_decode_and_emit() != 0) return 1;
    if (test_zero_and_fail_closed_shapes() != 0) return 1;
    if (test_identity_and_wire_failures() != 0) return 1;
    if (test_render_texture_and_meta() != 0) return 1;
    if (test_resource_path_and_identity() != 0) return 1;
    if (test_resolver_authority_and_partial_publication() != 0) return 1;
    if (test_native_batch_success_and_collision() != 0) return 1;
    puts("unity texture object unit tests passed");
    return 0;
}
