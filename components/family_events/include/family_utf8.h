/*
 * Small, allocation-free UTF-8 boundary helper for fixed-size wire fields.
 *
 * Policy:
 * - Valid input is copied up to the largest complete code point that fits.
 * - Output is always NUL-terminated when dst_size is non-zero.
 * - Invalid UTF-8 is rejected and the destination is cleared.
 *
 * The complete source is validated even after the destination becomes full,
 * so malformed bytes cannot be hidden beyond the truncation boundary.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool family_utf8_is_continuation(uint8_t value)
{
    return value >= 0x80U && value <= 0xBFU;
}

/* Return one valid code point's byte length, or zero for malformed input. */
static inline size_t family_utf8_code_point_size(const uint8_t *input)
{
    uint8_t first = input[0];
    if (first <= 0x7FU) {
        return first == 0U ? 0U : 1U;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        return family_utf8_is_continuation(input[1]) ? 2U : 0U;
    }
    if (first == 0xE0U) {
        return input[1] >= 0xA0U && input[1] <= 0xBFU &&
                       family_utf8_is_continuation(input[2])
                   ? 3U
                   : 0U;
    }
    if ((first >= 0xE1U && first <= 0xECU) ||
        (first >= 0xEEU && first <= 0xEFU)) {
        return family_utf8_is_continuation(input[1]) &&
                       family_utf8_is_continuation(input[2])
                   ? 3U
                   : 0U;
    }
    if (first == 0xEDU) {
        /* Exclude UTF-16 surrogate code points U+D800..U+DFFF. */
        return input[1] >= 0x80U && input[1] <= 0x9FU &&
                       family_utf8_is_continuation(input[2])
                   ? 3U
                   : 0U;
    }
    if (first == 0xF0U) {
        return input[1] >= 0x90U && input[1] <= 0xBFU &&
                       family_utf8_is_continuation(input[2]) &&
                       family_utf8_is_continuation(input[3])
                   ? 4U
                   : 0U;
    }
    if (first >= 0xF1U && first <= 0xF3U) {
        return family_utf8_is_continuation(input[1]) &&
                       family_utf8_is_continuation(input[2]) &&
                       family_utf8_is_continuation(input[3])
                   ? 4U
                   : 0U;
    }
    if (first == 0xF4U) {
        /* U+10FFFF is the largest Unicode scalar value. */
        return input[1] >= 0x80U && input[1] <= 0x8FU &&
                       family_utf8_is_continuation(input[2]) &&
                       family_utf8_is_continuation(input[3])
                   ? 4U
                   : 0U;
    }
    return 0U;
}

/* src must be a NUL-terminated C string; fixed buffers use the bounded
 * family_utf8_field_is_valid() validator below. */
static inline bool family_utf8_copy_truncate(char *dst, size_t dst_size,
                                              const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return false;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return false;
    }

    const uint8_t *input = (const uint8_t *)src;
    size_t written = 0U;
    bool truncated = false;

    while (*input != 0U) {
        size_t code_point_size = family_utf8_code_point_size(input);
        if (code_point_size == 0U) {
            dst[0] = '\0';
            return false;
        }
        if (!truncated && written + code_point_size < dst_size) {
            for (size_t i = 0U; i < code_point_size; ++i) {
                dst[written + i] = (char)input[i];
            }
            written += code_point_size;
        } else {
            /* Truncation preserves a prefix; never skip a large code point. */
            truncated = true;
        }
        input += code_point_size;
    }

    dst[written] = '\0';
    return true;
}

static inline bool family_utf8_is_valid(const char *value)
{
    char sink[1];
    return family_utf8_copy_truncate(sink, sizeof(sink), value);
}

static inline bool family_utf8_field_is_valid(const char *value,
                                               size_t capacity)
{
    if (value == NULL || capacity == 0U) {
        return false;
    }
    size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length < capacity && family_utf8_is_valid(value);
}

#ifdef __cplusplus
}
#endif
