// SPDX-License-Identifier: GPL-3.0-only

#include "common/shader_artifact.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool lowercase_hex_string(const char* value) {
    if (!value || !value[0]) return false;
    for (const unsigned char* cursor = (const unsigned char*)value;
         *cursor; ++cursor) {
        if (!((*cursor >= '0' && *cursor <= '9') ||
              (*cursor >= 'a' && *cursor <= 'f'))) {
            return false;
        }
    }
    return true;
}

static unsigned char ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z'
        ? (unsigned char)(value + ('a' - 'A')) : value;
}

static bool ascii_word_equal_ignore_case(
    const char* value, size_t value_size, const char* expected) {
    size_t expected_size = strlen(expected);
    if (value_size != expected_size) return false;
    for (size_t index = 0U; index < value_size; ++index) {
        if (ascii_lower((unsigned char)value[index]) !=
            ascii_lower((unsigned char)expected[index])) return false;
    }
    return true;
}

static bool windows_device_basename(const char* value, size_t value_size) {
    size_t base_size = 0U;
    while (base_size < value_size && value[base_size] != '.') ++base_size;
    if (ascii_word_equal_ignore_case(value, base_size, "con") ||
        ascii_word_equal_ignore_case(value, base_size, "prn") ||
        ascii_word_equal_ignore_case(value, base_size, "aux") ||
        ascii_word_equal_ignore_case(value, base_size, "nul") ||
        ascii_word_equal_ignore_case(value, base_size, "clock$")) {
        return true;
    }
    return base_size == 4U &&
        (ascii_word_equal_ignore_case(value, 3U, "com") ||
         ascii_word_equal_ignore_case(value, 3U, "lpt")) &&
        value[3] >= '1' && value[3] <= '9';
}

static size_t valid_utf8_width(const unsigned char* cursor) {
    unsigned char value = cursor[0];
    size_t width = value >= 0xf0U ? 4U :
        (value >= 0xe0U ? 3U : (value >= 0xc2U ? 2U : 0U));
    if (width == 0U) return 0U;
    for (size_t index = 1U; index < width; ++index) {
        if (cursor[index] == 0U || (cursor[index] & 0xc0U) != 0x80U) {
            return 0U;
        }
    }
    return width;
}

bool unity_asset_artifact_filename(
    char* out, size_t out_size, const char* prefix, const char* asset_name,
    const char* identity_tag, int64_t path_id, const char* extension) {
    enum { MAX_SLUG_BYTES = 96 };
    char safe_name[MAX_SLUG_BYTES + 1U];

    if (!out || out_size == 0 || !prefix || !prefix[0] || !asset_name ||
        !asset_name[0] || !extension || !extension[0]) {
        return false;
    }
    for (const unsigned char* cursor = (const unsigned char*)prefix;
         *cursor; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_')) {
            return false;
        }
    }
    for (const unsigned char* cursor = (const unsigned char*)extension;
         *cursor; ++cursor) {
        if (!((*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9'))) {
            return false;
        }
    }
    if (identity_tag) {
        if (!identity_tag[0]) return false;
        for (const unsigned char* cursor =
                 (const unsigned char*)identity_tag; *cursor; ++cursor) {
            if (!((*cursor >= '0' && *cursor <= '9') ||
                  (*cursor >= 'a' && *cursor <= 'f'))) {
                return false;
            }
        }
    }
    size_t slug_size = 0U;
    for (const unsigned char* cursor = (const unsigned char*)asset_name;
         *cursor && slug_size < MAX_SLUG_BYTES; ++cursor) {
        unsigned char value = *cursor;
        bool portable = (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '-' ||
            value == '_' || value == '.';
        safe_name[slug_size++] = portable ? (char)value : '_';
    }
    if (slug_size == 0U) return false;
    while (slug_size > 0U &&
           (safe_name[slug_size - 1U] == '.' ||
            safe_name[slug_size - 1U] == ' ')) {
        safe_name[--slug_size] = '\0';
    }
    if (slug_size == 0U) safe_name[slug_size++] = '_';
    safe_name[slug_size] = '\0';

    /* The fixed prefix prevents Windows device-name aliases (CON, PRN,
     * COM1, and similar), while the bounded slug stays below NAME_MAX. */
    int written = identity_tag
        ? snprintf(out, out_size, "%s_%s__%s_%" PRId64 ".%s", prefix,
                   safe_name, identity_tag, path_id, extension)
        : snprintf(out, out_size, "%s_%s__%" PRId64 ".%s", prefix,
                   safe_name, path_id, extension);
    return written >= 0 && (size_t)written < out_size;
}

bool shader_artifact_filename(char* out, size_t out_size,
                              const char* shader_name, int64_t path_id) {
    return unity_asset_artifact_filename(
        out, out_size, "shader", shader_name, NULL, path_id, "shader");
}

bool shader_flat_artifact_filename(
    char* out, size_t out_size, const char* shader_name,
    const char* identity_tag, int64_t path_id) {
    enum { MAX_FLAT_NAME_BYTES = 144 };
    char safe_name[MAX_FLAT_NAME_BYTES + 2U];
    if (!out || out_size == 0U || !shader_name || !shader_name[0] ||
        (identity_tag && !lowercase_hex_string(identity_tag))) {
        return false;
    }

    size_t safe_size = 0U;
    const unsigned char* cursor = (const unsigned char*)shader_name;
    while (*cursor && safe_size < MAX_FLAT_NAME_BYTES) {
        unsigned char value = *cursor;
        if (value >= 0x80U) {
            size_t width = valid_utf8_width(cursor);
            if (width == 0U) {
                safe_name[safe_size++] = '_';
                ++cursor;
            } else {
                if (safe_size + width > MAX_FLAT_NAME_BYTES) break;
                memcpy(safe_name + safe_size, cursor, width);
                safe_size += width;
                cursor += width;
            }
            continue;
        }
        bool invalid = value < 0x20U || value == '<' || value == '>' ||
            value == ':' || value == '"' || value == '/' || value == '\\' ||
            value == '|' || value == '?' || value == '*';
        safe_name[safe_size++] = invalid ? '_' : (char)value;
        ++cursor;
    }
    while (safe_size > 0U &&
           (safe_name[safe_size - 1U] == '.' ||
            safe_name[safe_size - 1U] == ' ')) {
        --safe_size;
    }
    if (safe_size == 0U) safe_name[safe_size++] = '_';
    safe_name[safe_size] = '\0';
    if ((safe_size == 1U && safe_name[0] == '.') ||
        (safe_size == 2U && safe_name[0] == '.' && safe_name[1] == '.') ||
        windows_device_basename(safe_name, safe_size)) {
        memmove(safe_name + 1U, safe_name, safe_size + 1U);
        safe_name[0] = '_';
        ++safe_size;
    }

    int written = identity_tag
        ? snprintf(out, out_size, "%s__%s_%" PRId64 ".shader", safe_name,
                   identity_tag, path_id)
        : snprintf(out, out_size, "%s.shader", safe_name);
    return written >= 0 && (size_t)written < out_size;
}
