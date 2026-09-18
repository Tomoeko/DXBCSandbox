// SPDX-License-Identifier: GPL-3.0-only

#include "common/unity_asset_guid.h"

#include "common/sha256.h"

#include <stdint.h>
#include <string.h>

static void store_be64(uint8_t output[8], uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        output[7U - index] = (uint8_t)(value >> (index * 8U));
    }
}

static bool domain_is_valid(const char* domain, size_t* out_size) {
    if (!domain || domain[0] == '\0') return false;
    size_t size = 0U;
    while (domain[size] != '\0') {
        unsigned char byte = (unsigned char)domain[size];
        if (byte < 0x21U || byte > 0x7eU) return false;
        ++size;
    }
    if (out_size) *out_size = size;
    return true;
}

bool unity_asset_guid_derive(
    const char* domain,
    const void* identity,
    size_t identity_size,
    char out_guid[UNITY_ASSET_GUID_TEXT_CAPACITY]) {
    static const uint8_t prefix[] =
        "DXBCSandbox.UnityAssetGUID.v1";
    static const char hex_digits[] = "0123456789abcdef";
    size_t domain_size = 0U;
    if (!out_guid || !domain_is_valid(domain, &domain_size) ||
        (!identity && identity_size != 0U)) {
        return false;
    }
#if SIZE_MAX > UINT64_MAX
    if (domain_size > UINT64_MAX || identity_size > UINT64_MAX) return false;
#endif

    uint8_t encoded_size[8];
    CommonSha256Context context;
    common_sha256_init(&context);
    common_sha256_update(&context, prefix, sizeof(prefix) - 1U);
    store_be64(encoded_size, (uint64_t)domain_size);
    common_sha256_update(&context, encoded_size, sizeof(encoded_size));
    common_sha256_update(&context, domain, domain_size);
    store_be64(encoded_size, (uint64_t)identity_size);
    common_sha256_update(&context, encoded_size, sizeof(encoded_size));
    if (identity_size != 0U) {
        common_sha256_update(&context, identity, identity_size);
    }

    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256_final(&context, digest);
    for (size_t index = 0U; index < 16U; ++index) {
        out_guid[index * 2U] = hex_digits[digest[index] >> 4U];
        out_guid[index * 2U + 1U] = hex_digits[digest[index] & 0x0fU];
    }
    out_guid[UNITY_ASSET_GUID_HEX_LENGTH] = '\0';
    return true;
}

bool unity_asset_guid_from_serialized_bytes(
    const uint8_t serialized_guid[16],
    char out_guid[UNITY_ASSET_GUID_TEXT_CAPACITY]) {
    static const char hex_digits[] = "0123456789abcdef";
    if (!serialized_guid || !out_guid) return false;
    for (size_t index = 0U; index < 16U; ++index) {
        out_guid[index * 2U] = hex_digits[serialized_guid[index] & 0x0fU];
        out_guid[index * 2U + 1U] =
            hex_digits[serialized_guid[index] >> 4U];
    }
    out_guid[UNITY_ASSET_GUID_HEX_LENGTH] = '\0';
    return true;
}

bool unity_asset_guid_is_valid(const char* guid) {
    if (!guid) return false;
    for (size_t index = 0U; index < UNITY_ASSET_GUID_HEX_LENGTH; ++index) {
        unsigned char byte = (unsigned char)guid[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return guid[UNITY_ASSET_GUID_HEX_LENGTH] == '\0';
}
