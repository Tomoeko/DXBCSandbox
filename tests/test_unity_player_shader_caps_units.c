#include "io/unity_player_shader_caps.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    uint8_t payload[2048];
    size_t size;
    bool big_endian;
    size_t platform_count_offset;
    size_t first_platform_offset;
    size_t first_bitset_count_offset;
    size_t first_bitset_high_offset;
    size_t first_boolean_offset;
    size_t first_alignment_offset;
    TypeTreeType type;
    AssetObjectInfo objects[2];
    SerializedFile file;
} SyntheticShaderCaps;

static const uint8_t k_expected_type_hash[16] = {
    0x9bU, 0x60U, 0xe7U, 0x65U, 0xe2U, 0x1dU, 0x19U, 0x9eU,
    0x3eU, 0xaeU, 0xf5U, 0x54U, 0x00U, 0x18U, 0xe8U, 0xdaU,
};

static void append_u8(SyntheticShaderCaps* fixture, uint8_t value) {
    fixture->payload[fixture->size++] = value;
}

static void append_u32(SyntheticShaderCaps* fixture, uint32_t value) {
    if (fixture->big_endian) {
        append_u8(fixture, (uint8_t)(value >> 24U));
        append_u8(fixture, (uint8_t)(value >> 16U));
        append_u8(fixture, (uint8_t)(value >> 8U));
        append_u8(fixture, (uint8_t)value);
    } else {
        append_u8(fixture, (uint8_t)value);
        append_u8(fixture, (uint8_t)(value >> 8U));
        append_u8(fixture, (uint8_t)(value >> 16U));
        append_u8(fixture, (uint8_t)(value >> 24U));
    }
}

static void append_i32(SyntheticShaderCaps* fixture, int32_t value) {
    append_u32(fixture, (uint32_t)value);
}

static void append_i64(SyntheticShaderCaps* fixture, int64_t value) {
    const uint64_t bits = (uint64_t)value;
    if (fixture->big_endian) {
        for (unsigned shift = 56U;; shift -= 8U) {
            append_u8(fixture, (uint8_t)(bits >> shift));
            if (shift == 0U) break;
        }
    } else {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            append_u8(fixture, (uint8_t)(bits >> shift));
        }
    }
}

static void store_u32(SyntheticShaderCaps* fixture, size_t offset,
                      uint32_t value) {
    const size_t saved_size = fixture->size;
    fixture->size = offset;
    append_u32(fixture, value);
    fixture->size = saved_size;
}

static size_t append_alignment(SyntheticShaderCaps* fixture) {
    const size_t first_padding = fixture->size;
    while ((fixture->size & 3U) != 0U) append_u8(fixture, 0U);
    return first_padding;
}

static void append_pptr(SyntheticShaderCaps* fixture,
                        int32_t file_id, int64_t path_id) {
    append_i32(fixture, file_id);
    append_i64(fixture, path_id);
}

static void append_pptr_array(SyntheticShaderCaps* fixture,
                              int32_t count) {
    append_i32(fixture, count);
    for (int32_t index = 0; index < count; ++index) {
        append_pptr(fixture, 0, (int64_t)index + 1);
    }
    (void)append_alignment(fixture);
}

static void append_string(SyntheticShaderCaps* fixture,
                          const char* value) {
    const size_t size = strlen(value);
    append_i32(fixture, (int32_t)size);
    memcpy(fixture->payload + fixture->size, value, size);
    fixture->size += size;
    (void)append_alignment(fixture);
}

static void append_tier_settings(SyntheticShaderCaps* fixture,
                                 int32_t realtime_gi_usage) {
    append_i32(fixture, 1);
    append_i32(fixture, 1);
    append_i32(fixture, realtime_gi_usage);
    append_u8(fixture, 1U);
    append_u8(fixture, 0U);
    append_u8(fixture, 1U);
    append_u8(fixture, 1U);
    (void)append_alignment(fixture);
}

static void append_bitset(SyntheticShaderCaps* fixture,
                          uint32_t low, uint32_t high,
                          size_t* out_count_offset,
                          size_t* out_high_offset) {
    if (out_count_offset) *out_count_offset = fixture->size;
    append_i32(fixture, 2);
    append_u32(fixture, low);
    if (out_high_offset) *out_high_offset = fixture->size;
    append_u32(fixture, high);
    (void)append_alignment(fixture);
}

static void append_platform(SyntheticShaderCaps* fixture,
                            int32_t platform, uint32_t base,
                            size_t* out_platform_offset,
                            size_t* out_count_offset,
                            size_t* out_high_offset) {
    if (out_platform_offset) *out_platform_offset = fixture->size;
    append_i32(fixture, platform);
    append_bitset(fixture, base, 0U,
                  out_count_offset, out_high_offset);
    append_bitset(fixture, base + 1U, 1U, NULL, NULL);
    append_bitset(fixture, base + 2U, 0U, NULL, NULL);
}

static void make_fixture(SyntheticShaderCaps* fixture, bool big_endian) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->big_endian = big_endian;

    for (int index = 0;
         index < 8; ++index) {
        append_i32(fixture, index);
        append_pptr(fixture, 0, index);
    }
    append_i32(fixture, 1);       /* m_VideoShadersIncludeMode */
    append_pptr_array(fixture, 2);
    append_pptr_array(fixture, 1);
    append_i32(fixture, 17);      /* preload batch limit */
    append_pptr(fixture, 0, 101);
    append_pptr(fixture, 0, 102);
    append_i32(fixture, 3);       /* transparency sort mode */
    append_u32(fixture, UINT32_C(0x3f800000));
    append_u32(fixture, UINT32_C(0x40000000));
    append_u32(fixture, UINT32_C(0x40400000));
    append_tier_settings(fixture, 25);
    append_tier_settings(fixture, 25);
    append_tier_settings(fixture, 50);

    fixture->platform_count_offset = fixture->size;
    append_i32(fixture, 2);
    append_platform(fixture, 4, UINT32_C(0x01234567),
                    &fixture->first_platform_offset,
                    &fixture->first_bitset_count_offset,
                    &fixture->first_bitset_high_offset);
    append_platform(fixture, 15, UINT32_C(0x01020304),
                    NULL, NULL, NULL);
    (void)append_alignment(fixture);

    fixture->first_boolean_offset = fixture->size;
    append_u8(fixture, 1U);       /* m_LightsUseLinearIntensity */
    append_u8(fixture, 0U);       /* m_LightsUseColorTemperature */
    fixture->first_alignment_offset = append_alignment(fixture);
    append_u32(fixture, UINT32_C(0xffffffff));
    append_u8(fixture, 1U);       /* m_LogWhenShaderIsCompiled */
    (void)append_alignment(fixture);

    append_i32(fixture, 1);       /* m_SRPDefaultSettings */
    append_string(fixture, "pipeline");
    append_pptr(fixture, 0, 201);
    (void)append_alignment(fixture);
    append_u8(fixture, 1U);
    append_u8(fixture, 0U);

    fixture->type.type_id = UNITY_PLAYER_SHADER_CAPS_CLASS_ID;
    fixture->type.script_type_index = UINT16_MAX;
    memcpy(fixture->type.type_hash, k_expected_type_hash,
           sizeof(k_expected_type_hash));

    fixture->objects[0].path_id = 7;
    fixture->objects[0].byte_offset = 0U;
    fixture->objects[0].byte_size = (uint32_t)fixture->size;
    fixture->objects[0].type_id_or_index = 0;
    fixture->objects[0].type_id = UNITY_PLAYER_SHADER_CAPS_CLASS_ID;
    fixture->objects[0].script_type_index = UINT16_MAX;

    fixture->file.version = 22U;
    fixture->file.file_size = fixture->size;
    fixture->file.data_offset = 0U;
    fixture->file.big_endian = big_endian;
    fixture->file.unity_version = (char*)"2021.3.35f1";
    fixture->file.target_platform = 19U;
    fixture->file.type_count = 1;
    fixture->file.types = &fixture->type;
    fixture->file.object_count = 1;
    fixture->file.objects = fixture->objects;
    fixture->file.raw_data = fixture->payload;
    fixture->file.raw_size = fixture->size;
}

static int check_valid_decode(bool big_endian) {
    SyntheticShaderCaps fixture;
    make_fixture(&fixture, big_endian);
    UnityPlayerShaderCaps result;
    unity_player_shader_caps_init(&result);
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_OK);
    CHECK(result.decoded);
    CHECK(result.serialized_file_version == 22U);
    CHECK(result.target_platform == 19U);
    CHECK(result.path_id == 7);
    CHECK(result.byte_offset == 0U);
    CHECK(result.byte_size == fixture.size);
    CHECK(result.entry_count == 2U);
    CHECK(result.entries[4].present);
    CHECK(result.entries[4].shader_platform == 4U);
    CHECK(result.entries[4].tier_capabilities[0] ==
          UINT64_C(0x01234567));
    CHECK(result.entries[4].tier_capabilities[1] ==
          UINT64_C(0x101234568));
    CHECK(result.entries[4].tier_capabilities[2] ==
          UINT64_C(0x01234569));
    CHECK(result.entries[15].present);
    CHECK(!result.entries[3].present);

    uint64_t capabilities = 0U;
    CHECK(unity_player_shader_caps_get(
              &result, UNITY_PLAYER_SHADER_PLATFORM_D3D11, 3U,
              &capabilities) == UNITY_PLAYER_SHADER_CAPS_OK);
    CHECK(capabilities == UINT64_C(0x01234569));
    CHECK(unity_player_shader_caps_get(&result, 3U, 1U, &capabilities) ==
          UNITY_PLAYER_SHADER_CAPS_PLATFORM_MISSING);
    CHECK(unity_player_shader_caps_get(&result, 4U, 0U, &capabilities) ==
          UNITY_PLAYER_SHADER_CAPS_TIER_INVALID);
    CHECK(unity_player_shader_caps_get(&result, 25U, 1U,
                                       &capabilities) ==
          UNITY_PLAYER_SHADER_CAPS_PLATFORM_INVALID);
    return 0;
}

int main(void) {
    CHECK(check_valid_decode(false) == 0);
    CHECK(check_valid_decode(true) == 0);

    SyntheticShaderCaps missing_d3d11;
    make_fixture(&missing_d3d11, false);
    store_u32(&missing_d3d11, missing_d3d11.first_platform_offset, 5U);
    UnityPlayerShaderCaps missing_result;
    unity_player_shader_caps_init(&missing_result);
    CHECK(unity_player_shader_caps_decode(
              &missing_result, &missing_d3d11.file) ==
          UNITY_PLAYER_SHADER_CAPS_OK);
    uint64_t missing_value = 0U;
    CHECK(unity_player_shader_caps_get(
              &missing_result, UNITY_PLAYER_SHADER_PLATFORM_D3D11, 3U,
              &missing_value) == UNITY_PLAYER_SHADER_CAPS_PLATFORM_MISSING);

    SyntheticShaderCaps duplicate_platform;
    make_fixture(&duplicate_platform, false);
    store_u32(&duplicate_platform,
              duplicate_platform.first_platform_offset, 15U);
    UnityPlayerShaderCaps duplicate_result;
    unity_player_shader_caps_init(&duplicate_result);
    CHECK(unity_player_shader_caps_decode(
              &duplicate_result, &duplicate_platform.file) ==
          UNITY_PLAYER_SHADER_CAPS_PLATFORM_DUPLICATE);

    SyntheticShaderCaps fixture;
    make_fixture(&fixture, false);
    UnityPlayerShaderCaps result;
    unity_player_shader_caps_init(&result);
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_OK);
    CHECK(result.entries[4].tier_capabilities[0] ==
          UINT64_C(0x01234567));

    fixture.file.object_count = 0;
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_MISSING);
    CHECK(result.entries[4].tier_capabilities[0] ==
          UINT64_C(0x01234567));
    fixture.file.object_count = 1;

    fixture.objects[1] = fixture.objects[0];
    fixture.objects[1].path_id = 8;
    fixture.file.object_count = 2;
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_DUPLICATE_OBJECT);
    fixture.file.object_count = 1;

    fixture.file.version = 21U;
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_UNSUPPORTED_FILE_VERSION);
    fixture.file.version = 22U;
    fixture.file.unity_version = (char*)"2021.3.34f1";
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_UNSUPPORTED_UNITY_VERSION);
    fixture.file.unity_version = (char*)"2021.3.35f1";

    fixture.type.type_hash[0] ^= 1U;
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_TYPE_IDENTITY_UNSUPPORTED);
    fixture.type.type_hash[0] ^= 1U;

    store_u32(&fixture, fixture.platform_count_offset, UINT32_MAX);
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_COUNT_INVALID);
    store_u32(&fixture, fixture.platform_count_offset, 2U);

    store_u32(&fixture, fixture.first_platform_offset, 25U);
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_PLATFORM_INVALID);
    store_u32(&fixture, fixture.first_platform_offset, 4U);

    store_u32(&fixture, fixture.first_bitset_count_offset, 1U);
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_BITSET_WORD_COUNT_INVALID);
    store_u32(&fixture, fixture.first_bitset_count_offset, 2U);

    store_u32(&fixture, fixture.first_bitset_high_offset, 2U);
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_BITSET_HIGH_BITS_INVALID);
    store_u32(&fixture, fixture.first_bitset_high_offset, 0U);

    fixture.payload[fixture.first_boolean_offset] = 2U;
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_BOOLEAN_INVALID);
    fixture.payload[fixture.first_boolean_offset] = 1U;

    fixture.payload[fixture.first_alignment_offset] = 1U;
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_ALIGNMENT_INVALID);
    fixture.payload[fixture.first_alignment_offset] = 0U;

    fixture.payload[fixture.size] = 0U;
    ++fixture.size;
    fixture.file.file_size = fixture.size;
    fixture.file.raw_size = fixture.size;
    fixture.objects[0].byte_size = (uint32_t)fixture.size;
    CHECK(unity_player_shader_caps_decode(&result, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_OBJECT_BYTES_NOT_EXHAUSTED);

    CHECK(unity_player_shader_caps_decode(NULL, &fixture.file) ==
          UNITY_PLAYER_SHADER_CAPS_INVALID_ARGUMENT);
    CHECK(unity_player_shader_caps_decode_serialized_bytes(
              &result, fixture.payload, fixture.size) ==
          UNITY_PLAYER_SHADER_CAPS_INVALID_SERIALIZED_FILE);
    CHECK(strcmp(unity_player_shader_caps_status_name(
                     UNITY_PLAYER_SHADER_CAPS_PLATFORM_DUPLICATE),
                 "platform-duplicate") == 0);

    puts("Unity player shader-capability unit tests passed.");
    return 0;
}
