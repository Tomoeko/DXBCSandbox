// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_ARTIFACT_H
#define SHADER_ARTIFACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Produces a bounded, Windows/POSIX-safe ShaderLab filename from serialized
// object identity. Shader names are not unique within a SerializedFile;
// PathIDs are. Callers scope files by SerializedFile content digest.
bool shader_artifact_filename(char* out, size_t out_size,
                              const char* shader_name, int64_t path_id);

/* Produces a flat graphics-Shader filename directly from the serialized
 * Shader name. Path separators and cross-platform-invalid filename bytes are
 * replaced with underscores, while ordinary spaces are preserved. When
 * identity_tag is non-NULL, its lowercase hexadecimal digest and path_id are
 * appended to disambiguate names that collide in one flat directory. */
bool shader_flat_artifact_filename(
    char* out, size_t out_size, const char* shader_name,
    const char* identity_tag, int64_t path_id);

/* Generic Unity text-asset filename using the same portable slug policy.
 * identity_tag, when non-NULL, must contain only lowercase hexadecimal bytes
 * and separates equal PathIDs originating in different SerializedFiles.
 * Extension letters may retain Unity's class-specific casing (for example,
 * .texture2D and .renderTexture). */
bool unity_asset_artifact_filename(
    char* out, size_t out_size, const char* prefix, const char* asset_name,
    const char* identity_tag, int64_t path_id, const char* extension);

#endif // SHADER_ARTIFACT_H
