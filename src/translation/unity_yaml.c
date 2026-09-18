// SPDX-License-Identifier: GPL-3.0-only

#include "translation/unity_yaml.h"

#include <stdio.h>
#include <string.h>

#define BIG_DECIMAL_BASE 1000000000U
#define BIG_DECIMAL_LIMBS 24U

typedef struct {
    uint32_t limb[BIG_DECIMAL_LIMBS];
    size_t count;
} BigDecimalInteger;

const char* unity_yaml_status_name(UnityYamlStatus status) {
    switch (status) {
        case UNITY_YAML_OK: return "ok";
        case UNITY_YAML_INVALID_ARGUMENT: return "invalid_argument";
        case UNITY_YAML_INVALID_UTF8: return "invalid_utf8";
        case UNITY_YAML_OUTPUT_FAILED: return "output_failed";
    }
    return "unknown";
}

bool unity_yaml_utf8_is_valid(const void* input, size_t size) {
    if (!input && size != 0U) return false;
    const uint8_t* bytes = (const uint8_t*)input;
    size_t index = 0U;
    while (index < size) {
        uint8_t first = bytes[index++];
        if (first <= 0x7fU) continue;
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index >= size || bytes[index] < 0x80U ||
                bytes[index] > 0xbfU) {
                return false;
            }
            ++index;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (size - index < 2U) return false;
            uint8_t second = bytes[index];
            uint8_t third = bytes[index + 1U];
            if (third < 0x80U || third > 0xbfU ||
                (first == 0xe0U && (second < 0xa0U || second > 0xbfU)) ||
                (first == 0xedU && (second < 0x80U || second > 0x9fU)) ||
                (first != 0xe0U && first != 0xedU &&
                 (second < 0x80U || second > 0xbfU))) {
                return false;
            }
            index += 2U;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (size - index < 3U) return false;
            uint8_t second = bytes[index];
            uint8_t third = bytes[index + 1U];
            uint8_t fourth = bytes[index + 2U];
            if (third < 0x80U || third > 0xbfU ||
                fourth < 0x80U || fourth > 0xbfU ||
                (first == 0xf0U && (second < 0x90U || second > 0xbfU)) ||
                (first == 0xf4U && (second < 0x80U || second > 0x8fU)) ||
                (first != 0xf0U && first != 0xf4U &&
                 (second < 0x80U || second > 0xbfU))) {
                return false;
            }
            index += 3U;
            continue;
        }
        return false;
    }
    return true;
}

static size_t utf8_sequence_size(uint8_t first) {
    if (first <= 0x7fU) return 1U;
    if (first <= 0xdfU) return 2U;
    if (first <= 0xefU) return 3U;
    return 4U;
}

static uint32_t utf8_decode(const uint8_t* bytes, size_t count) {
    if (count == 1U) return bytes[0];
    if (count == 2U) {
        return ((uint32_t)(bytes[0] & 0x1fU) << 6U) |
               (uint32_t)(bytes[1] & 0x3fU);
    }
    if (count == 3U) {
        return ((uint32_t)(bytes[0] & 0x0fU) << 12U) |
               ((uint32_t)(bytes[1] & 0x3fU) << 6U) |
               (uint32_t)(bytes[2] & 0x3fU);
    }
    return ((uint32_t)(bytes[0] & 0x07U) << 18U) |
           ((uint32_t)(bytes[1] & 0x3fU) << 12U) |
           ((uint32_t)(bytes[2] & 0x3fU) << 6U) |
           (uint32_t)(bytes[3] & 0x3fU);
}

static void append_hex_escape(StringBuilder* output, uint32_t codepoint) {
    static const char digits[] = "0123456789abcdef";
    if (codepoint <= 0xffffU) {
        sb_append(output, "\\u");
        for (int shift = 12; shift >= 0; shift -= 4) {
            sb_append_char(output, digits[(codepoint >> (unsigned)shift) & 0xfU]);
        }
        return;
    }
    sb_append(output, "\\U");
    for (int shift = 28; shift >= 0; shift -= 4) {
        sb_append_char(output, digits[(codepoint >> (unsigned)shift) & 0xfU]);
    }
}

UnityYamlStatus unity_yaml_append_quoted_n(
    StringBuilder* output, const void* input, size_t size) {
    if (!output || (!input && size != 0U)) {
        return UNITY_YAML_INVALID_ARGUMENT;
    }
    if (!unity_yaml_utf8_is_valid(input, size)) {
        return UNITY_YAML_INVALID_UTF8;
    }
    const uint8_t* bytes = (const uint8_t*)input;
    sb_append_char(output, '"');
    size_t index = 0U;
    while (index < size) {
        size_t count = utf8_sequence_size(bytes[index]);
        uint32_t codepoint = utf8_decode(bytes + index, count);
        switch (codepoint) {
            case 0x00U: sb_append(output, "\\0"); break;
            case 0x07U: sb_append(output, "\\a"); break;
            case 0x08U: sb_append(output, "\\b"); break;
            case 0x09U: sb_append(output, "\\t"); break;
            case 0x0aU: sb_append(output, "\\n"); break;
            case 0x0bU: sb_append(output, "\\v"); break;
            case 0x0cU: sb_append(output, "\\f"); break;
            case 0x0dU: sb_append(output, "\\r"); break;
            case 0x1bU: sb_append(output, "\\e"); break;
            case 0x22U: sb_append(output, "\\\""); break;
            case 0x5cU: sb_append(output, "\\\\"); break;
            default:
                if (codepoint < 0x20U ||
                    (codepoint >= 0x7fU && codepoint <= 0x9fU) ||
                    codepoint == 0x2028U || codepoint == 0x2029U) {
                    append_hex_escape(output, codepoint);
                } else {
                    sb_append_len(output, (const char*)bytes + index, count);
                }
                break;
        }
        index += count;
    }
    sb_append_char(output, '"');
    return sb_ok(output) ? UNITY_YAML_OK : UNITY_YAML_OUTPUT_FAILED;
}

UnityYamlStatus unity_yaml_append_quoted(
    StringBuilder* output, const char* text) {
    if (!text) return UNITY_YAML_INVALID_ARGUMENT;
    return unity_yaml_append_quoted_n(output, text, strlen(text));
}

static bool big_decimal_multiply(BigDecimalInteger* value, uint32_t factor) {
    uint64_t carry = 0U;
    for (size_t index = 0U; index < value->count; ++index) {
        uint64_t product = (uint64_t)value->limb[index] * factor + carry;
        value->limb[index] = (uint32_t)(product % BIG_DECIMAL_BASE);
        carry = product / BIG_DECIMAL_BASE;
    }
    if (carry != 0U) {
        if (value->count == BIG_DECIMAL_LIMBS) return false;
        value->limb[value->count++] = (uint32_t)carry;
    }
    return true;
}

static bool append_decimal_limb(char* output, size_t capacity,
                                size_t* inout_size, uint32_t value,
                                unsigned minimum_digits) {
    char reverse[10];
    unsigned count = 0U;
    do {
        reverse[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count < minimum_digits) reverse[count++] = '0';
    if (count > capacity - *inout_size - 1U) return false;
    while (count > 0U) output[(*inout_size)++] = reverse[--count];
    output[*inout_size] = '\0';
    return true;
}

static bool big_decimal_to_digits(const BigDecimalInteger* value,
                                  char* output, size_t capacity,
                                  size_t* out_size) {
    if (!value || value->count == 0U || !output || capacity == 0U ||
        !out_size) {
        return false;
    }
    size_t size = 0U;
    if (!append_decimal_limb(output, capacity, &size,
                             value->limb[value->count - 1U], 1U)) {
        return false;
    }
    for (size_t index = value->count - 1U; index > 0U; --index) {
        if (!append_decimal_limb(output, capacity, &size,
                                 value->limb[index - 1U], 9U)) {
            return false;
        }
    }
    *out_size = size;
    return true;
}

static bool copy_float_text(const char* text,
                            char output[UNITY_YAML_FLOAT32_TEXT_CAPACITY]) {
    size_t size = strlen(text);
    if (size >= UNITY_YAML_FLOAT32_TEXT_CAPACITY) return false;
    memcpy(output, text, size + 1U);
    return true;
}

bool unity_yaml_format_float32(
    uint32_t bits,
    char output[UNITY_YAML_FLOAT32_TEXT_CAPACITY]) {
    if (!output) return false;
    bool negative = (bits & 0x80000000U) != 0U;
    uint32_t exponent = (bits >> 23U) & 0xffU;
    uint32_t fraction = bits & 0x7fffffU;
    if (exponent == 0xffU) {
        if (fraction != 0U) return copy_float_text("NaN", output);
        return copy_float_text(negative ? "-Infinity" : "Infinity", output);
    }
    if (exponent == 0U && fraction == 0U) {
        return copy_float_text(negative ? "-0" : "0", output);
    }

    uint32_t significand = exponent == 0U
        ? fraction
        : 0x800000U | fraction;
    int exponent_two = exponent == 0U
        ? -149
        : (int)exponent - 127 - 23;
    BigDecimalInteger integer;
    memset(&integer, 0, sizeof(integer));
    integer.limb[0] = significand;
    integer.count = 1U;
    int decimal_places = 0;
    if (exponent_two >= 0) {
        for (int index = 0; index < exponent_two; ++index) {
            if (!big_decimal_multiply(&integer, 2U)) return false;
        }
    } else {
        decimal_places = -exponent_two;
        for (int index = 0; index < decimal_places; ++index) {
            if (!big_decimal_multiply(&integer, 5U)) return false;
        }
    }

    char digits[UNITY_YAML_FLOAT32_TEXT_CAPACITY];
    size_t digit_count = 0U;
    if (!big_decimal_to_digits(&integer, digits, sizeof(digits),
                               &digit_count)) {
        return false;
    }
    while (decimal_places > 0 && digit_count > 0U &&
           digits[digit_count - 1U] == '0') {
        --digit_count;
        --decimal_places;
    }

    size_t needed = negative ? 1U : 0U;
    if (decimal_places == 0) {
        needed += digit_count;
    } else if (digit_count <= (size_t)decimal_places) {
        needed += 2U + (size_t)decimal_places;
    } else {
        needed += digit_count + 1U;
    }
    if (needed >= UNITY_YAML_FLOAT32_TEXT_CAPACITY) return false;

    size_t position = 0U;
    if (negative) output[position++] = '-';
    if (decimal_places == 0) {
        memcpy(output + position, digits, digit_count);
        position += digit_count;
    } else if (digit_count <= (size_t)decimal_places) {
        output[position++] = '0';
        output[position++] = '.';
        size_t zero_count = (size_t)decimal_places - digit_count;
        memset(output + position, '0', zero_count);
        position += zero_count;
        memcpy(output + position, digits, digit_count);
        position += digit_count;
    } else {
        size_t integer_digits = digit_count - (size_t)decimal_places;
        memcpy(output + position, digits, integer_digits);
        position += integer_digits;
        output[position++] = '.';
        memcpy(output + position, digits + integer_digits,
               (size_t)decimal_places);
        position += (size_t)decimal_places;
    }
    output[position] = '\0';
    return true;
}

UnityYamlStatus unity_yaml_append_float32(
    StringBuilder* output, uint32_t bits) {
    if (!output) return UNITY_YAML_INVALID_ARGUMENT;
    char text[UNITY_YAML_FLOAT32_TEXT_CAPACITY];
    if (!unity_yaml_format_float32(bits, text)) {
        return UNITY_YAML_OUTPUT_FAILED;
    }
    sb_append(output, text);
    return sb_ok(output) ? UNITY_YAML_OK : UNITY_YAML_OUTPUT_FAILED;
}
