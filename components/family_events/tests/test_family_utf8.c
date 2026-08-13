#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "family_utf8.h"

static void expect_copy(size_t capacity, const char *input,
                        const char *expected)
{
    struct {
        unsigned char before;
        char output[160];
        unsigned char after;
    } guarded;
    assert(capacity <= sizeof(guarded.output));
    guarded.before = 0x3CU;
    guarded.after = 0xC3U;
    memset(guarded.output, 0xA5, sizeof(guarded.output));
    assert(family_utf8_copy_truncate(guarded.output, capacity, input));
    assert(strcmp(guarded.output, expected) == 0);
    assert(guarded.output[capacity - 1U] == '\0' ||
           strlen(guarded.output) < capacity - 1U);
    assert(family_utf8_is_valid(guarded.output));
    for (size_t i = capacity; i < sizeof(guarded.output); ++i) {
        assert((unsigned char)guarded.output[i] == 0xA5U);
    }
    assert(guarded.before == 0x3CU && guarded.after == 0xC3U);
}

int main(void)
{
    /* ASCII behavior and exact 15-byte person-name payload boundary. */
    expect_copy(16U, "student", "student");
    expect_copy(1U, "student", "");
    expect_copy(1U, "", "");
    expect_copy(16U, "123456789012345", "123456789012345");
    expect_copy(16U, "1234567890123456", "123456789012345");

    /* Chinese: exact fit, and only one/two bytes left for the next character. */
    expect_copy(16U, "\xE5\xBC\xA0\xE4\xB8\x89\xE6\x9D\x8E\xE5\x9B\x9B\xE7\x8E\x8B",
                "\xE5\xBC\xA0\xE4\xB8\x89\xE6\x9D\x8E\xE5\x9B\x9B\xE7\x8E\x8B");
    expect_copy(14U, "12345678901\xE5\xBC\xA0", "12345678901");
    expect_copy(15U, "1234567890123\xE5\xBC\xA0", "1234567890123");
    expect_copy(4U, "\xE5\xBC\xA0", "\xE5\xBC\xA0");
    expect_copy(3U, "\xE5\xBC\xA0", "");

    /* Emoji and mixed Chinese/ASCII must remain whole and prefix-ordered. */
    expect_copy(16U, "123456789012\xF0\x9F\x99\x82", "123456789012");
    expect_copy(5U, "\xF0\x9F\x99\x82", "\xF0\x9F\x99\x82");
    expect_copy(4U, "\xF0\x9F\x99\x82", "");
    expect_copy(16U,
                "A\xE5\xBC\xA0" "B\xF0\x9F\x99\x82" "CDEFG",
                "A\xE5\xBC\xA0" "B\xF0\x9F\x99\x82" "CDEFG");

    /* detail[] permits 127 payload bytes: 123 ASCII + one 4-byte emoji. */
    char detail[140];
    memset(detail, 'd', 123U);
    memcpy(detail + 123U, "\xF0\x9F\x99\x82", 4U);
    detail[127] = '\0';
    expect_copy(128U, detail, detail);

    /* Overlong Chinese detail keeps 42 whole characters (126 bytes). */
    char long_chinese[181];
    for (size_t i = 0U; i < 60U; ++i) {
        memcpy(long_chinese + i * 3U, "\xE5\xAE\xB6", 3U);
    }
    long_chinese[180] = '\0';
    char expected_chinese[127];
    memcpy(expected_chinese, long_chinese, 126U);
    expected_chinese[126] = '\0';
    expect_copy(128U, long_chinese, expected_chinese);

    /* Invalid, overlong, surrogate, and out-of-range UTF-8 are rejected. */
    const char *invalid[] = {
        "\x80", "\xE5\xBC", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\x80",
        "\xED\xA0\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80",
        "\xF5\x80\x80\x80", "\xE5" "A\x80"
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char output[16] = "not-cleared";
        assert(!family_utf8_copy_truncate(output, sizeof(output), invalid[i]));
        assert(output[0] == '\0');
    }

    char invalid_after_truncation[18];
    memset(invalid_after_truncation, 'a', 16U);
    invalid_after_truncation[16] = (char)0x80;
    invalid_after_truncation[17] = '\0';
    char cleared[16] = "not-cleared";
    assert(!family_utf8_copy_truncate(cleared, sizeof(cleared),
                                      invalid_after_truncation));
    assert(cleared[0] == '\0');
    assert(!family_utf8_copy_truncate(NULL, 1U, "x"));
    assert(!family_utf8_copy_truncate(cleared, 0U, "x"));
    strcpy(cleared, "not-cleared");
    assert(!family_utf8_copy_truncate(cleared, sizeof(cleared), NULL));
    assert(cleared[0] == '\0');

    /* Emit actual helper outputs for the Python strict-decode/validator test. */
    char wire_name[16];
    char wire_detail[128];
    assert(family_utf8_copy_truncate(
        wire_name, sizeof(wire_name),
        "\xE5\xBC\xA0\xE4\xB8\x89\xE6\x9D\x8E\xE5\x9B\x9B\xE7\x8E\x8B\xE8\xB5\xB5"));
    assert(family_utf8_copy_truncate(wire_detail, sizeof(wire_detail),
                                     long_chinese));
    printf("WIRE_NAME=%s\n", wire_name);
    printf("WIRE_DETAIL=%s\n", wire_detail);

    puts("family UTF-8 boundary tests: PASS");
    return 0;
}
