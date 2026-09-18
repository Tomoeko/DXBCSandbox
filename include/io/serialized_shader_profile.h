// SPDX-License-Identifier: GPL-3.0-only

#ifndef SERIALIZED_SHADER_PROFILE_H
#define SERIALIZED_SHADER_PROFILE_H

#include "io/serialized_shader.h"

/*
 * Validates the complete, ordered value shape consumed by the pinned Shader
 * projection.  This is intentionally separate from semantic projection: a
 * missing field is never converted into a plausible default.
 */
bool serialized_shader_profile_validate_value(
    const TypeTreeValue* value,
    SerializedShaderSchemaProfile profile);

#endif /* SERIALIZED_SHADER_PROFILE_H */
