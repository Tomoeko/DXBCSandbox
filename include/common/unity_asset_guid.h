// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_ASSET_GUID_H
#define UNITY_ASSET_GUID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNITY_ASSET_GUID_HEX_LENGTH 32U
#define UNITY_ASSET_GUID_TEXT_CAPACITY (UNITY_ASSET_GUID_HEX_LENGTH + 1U)

/* Stable domain names used by DXBCSandbox-generated Unity assets.  The
 * domain is part of the digest, so equal identities in different asset
 * namespaces cannot accidentally acquire the same GUID. */
#define UNITY_ASSET_GUID_DOMAIN_SHADER "dxbc-sandbox/shader/v1"
#define UNITY_ASSET_GUID_DOMAIN_MATERIAL "dxbc-sandbox/material/v1"
#define UNITY_ASSET_GUID_DOMAIN_TEXTURE "dxbc-sandbox/texture/v1"

/* Derive a Unity GUID from a canonical binary identity.  The construction is
 * SHA-256("DXBCSandbox.UnityAssetGUID.v1" || be64(domain-size) || domain ||
 * be64(identity-size) || identity), truncated to 128 bits and encoded as 32
 * lowercase hexadecimal characters.  Explicit lengths make concatenation
 * boundaries unambiguous.  Domain names must be nonempty printable ASCII. */
bool unity_asset_guid_derive(
    const char* domain,
    const void* identity,
    size_t identity_size,
    char out_guid[UNITY_ASSET_GUID_TEXT_CAPACITY]);

/* Convert the exact 16-byte GUID representation serialized by Unity into
 * the 32-character form used by .meta and YAML references.  Unity orders the
 * two hexadecimal nibbles of each serialized byte low-first.  For example,
 * serialized byte 0x0f is text "f0".  This is not ordinary byte-wise hex and
 * callers must not substitute a generic hex encoder.
 *
 * Authority: Unity 2021.3.35f1 build 157b46ce122a GUIDToString at
 * 0x100a76b78 emits low nibble first, StringToGUID at 0x100a76cc8 is the
 * inverse, and UnityGUID StreamedBinaryRead/Write at 0x10054c978/
 * 0x10054cea8 transfer the same 16 bytes directly.  Thus text
 * 0123456789abcdef... maps to serialized bytes 10 32 54 76 98 ba dc fe...
 * with no additional permutation. */
bool unity_asset_guid_from_serialized_bytes(
    const uint8_t serialized_guid[16],
    char out_guid[UNITY_ASSET_GUID_TEXT_CAPACITY]);

/* Unity asset GUID text accepted by emitters: exactly 32 lowercase hex
 * characters followed by NUL. */
bool unity_asset_guid_is_valid(const char* guid);

#endif /* UNITY_ASSET_GUID_H */
