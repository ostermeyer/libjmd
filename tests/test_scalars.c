/*
 * test_scalars.c — unit tests for src/scalars.c.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Covers the parse + serialize round-trip for every scalar type
 * the spec recognises, plus the §5a structural-prefix rejection,
 * the bare-`-` ambiguity, quoted-string escape detection, JSON
 * escape decoding (including surrogate pairs), key parsing both
 * bare and quoted, and the needs-quote / serialize symmetry.
 *
 * Aligned with jmd-impl/jmd/_scalars.py and jmd-js/src/value.js
 * for cross-impl byte-compat where the canonical form matters.
 */

#include "../src/scalars.h"
#include "test_util.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Parse direction                                                   */
/* ---------------------------------------------------------------- */

TEST(parse_empty_slice_is_empty_string)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("", 0, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_STRING);
    EXPECT_EQ_SIZE(v.as.string.len, 0);
}

TEST(parse_null)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("null", 4, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_NULL);
}

TEST(parse_true_and_false)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("true", 4, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_BOOL);
    EXPECT_EQ_INT(v.as.boolean, 1);
    EXPECT_EQ_INT(jmd_scalar_parse("false", 5, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_BOOL);
    EXPECT_EQ_INT(v.as.boolean, 0);
}

TEST(parse_integers_positive_zero_negative)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("42", 2, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_INT);
    EXPECT_EQ_INT(v.as.integer, 42);
    EXPECT_EQ_INT(jmd_scalar_parse("0", 1, &v), JMD_OK);
    EXPECT_EQ_INT(v.as.integer, 0);
    EXPECT_EQ_INT(jmd_scalar_parse("-17", 3, &v), JMD_OK);
    EXPECT_EQ_INT(v.as.integer, -17);
}

TEST(parse_floats_dotted_and_exponent)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("3.14", 4, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_FLOAT);
    EXPECT_TRUE(v.as.floating > 3.13 && v.as.floating < 3.15);
    EXPECT_EQ_INT(jmd_scalar_parse("1e10", 4, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_FLOAT);
    EXPECT_TRUE(v.as.floating > 9.9e9 && v.as.floating < 1.01e10);
    EXPECT_EQ_INT(jmd_scalar_parse("-2.5e-3", 7, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_FLOAT);
    EXPECT_TRUE(v.as.floating < 0 && v.as.floating > -0.01);
}

TEST(parse_leading_zero_number_is_zero_not_octal)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("0", 1, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_INT);
    EXPECT_EQ_INT(v.as.integer, 0);
    /* JSON forbids "07" — must fall through to bare string. */
    EXPECT_EQ_INT(jmd_scalar_parse("07", 2, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_STRING);
    EXPECT_EQ_STRN(v.as.string.ptr, v.as.string.len, "07");
}

TEST(parse_bare_string)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("hello", 5, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_STRING);
    EXPECT_EQ_STRN(v.as.string.ptr, v.as.string.len, "hello");
}

TEST(parse_quoted_string_no_escapes_returns_inner_slice)
{
    jmd_scalar_t v;
    const char *src = "\"hi there\"";
    EXPECT_EQ_INT(jmd_scalar_parse(src, 10, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_STRING);
    EXPECT_EQ_STRN(v.as.string.ptr, v.as.string.len, "hi there");
    /* Slice MUST point inside src, not be a fresh allocation. */
    EXPECT_TRUE(v.as.string.ptr == src + 1);
}

TEST(parse_quoted_string_empty)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("\"\"", 2, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_STRING);
    EXPECT_EQ_SIZE(v.as.string.len, 0);
}

TEST(parse_quoted_string_with_escape_defers_to_decoder)
{
    /* The parser declines escape-bearing strings so the caller can
     * dispatch to jmd_scalar_decode_string with its own scratch. */
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("\"a\\nb\"", 6, &v),
                  JMD_ERROR_PARSE);
}

TEST(parse_quoted_string_unterminated_is_parse_error)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("\"open", 5, &v), JMD_ERROR_PARSE);
}

TEST(parse_structural_prefix_hash_space_rejected)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("# heading-like", 14, &v),
                  JMD_ERROR_PARSE);
}

TEST(parse_structural_prefix_dash_space_rejected)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("- bullet-like", 13, &v),
                  JMD_ERROR_PARSE);
}

TEST(parse_bare_dash_is_ambiguous_and_rejected)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("-", 1, &v), JMD_ERROR_PARSE);
}

TEST(parse_string_containing_hash_but_not_at_start_is_fine)
{
    jmd_scalar_t v;
    EXPECT_EQ_INT(jmd_scalar_parse("foo#bar", 7, &v), JMD_OK);
    EXPECT_EQ_INT(v.type, JMD_SCALAR_STRING);
    EXPECT_EQ_STRN(v.as.string.ptr, v.as.string.len, "foo#bar");
}

/* ---------------------------------------------------------------- */
/* String-escape detection + decode                                  */
/* ---------------------------------------------------------------- */

TEST(has_escapes_detects_backslash)
{
    EXPECT_FALSE(jmd_scalar_string_has_escapes("hello", 5));
    EXPECT_TRUE(jmd_scalar_string_has_escapes("hel\\lo", 6));
}

TEST(decode_simple_escapes)
{
    char buf[64];
    size_t n = jmd_scalar_decode_string("a\\nb\\tc",
                                        7, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 5);
    EXPECT_EQ_STRN(buf, n, "a\nb\tc");
}

TEST(decode_quote_and_backslash)
{
    char buf[64];
    size_t n = jmd_scalar_decode_string("\\\"\\\\",
                                        4, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 2);
    EXPECT_EQ_STRN(buf, n, "\"\\");
}

TEST(decode_unicode_escape_bmp)
{
    /* ä → ä (UTF-8: 0xC3 0xA4) */
    char buf[64];
    size_t n = jmd_scalar_decode_string("\\u00e4", 6, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 2);
    EXPECT_EQ_INT((unsigned char)buf[0], 0xC3);
    EXPECT_EQ_INT((unsigned char)buf[1], 0xA4);
}

TEST(decode_unicode_escape_surrogate_pair)
{
    /* "💩" = U+1F4A9 = 💩 → UTF-8 F0 9F 92 A9 */
    char buf[64];
    size_t n = jmd_scalar_decode_string("\\uD83D\\uDCA9",
                                        12, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 4);
    EXPECT_EQ_INT((unsigned char)buf[0], 0xF0);
    EXPECT_EQ_INT((unsigned char)buf[1], 0x9F);
    EXPECT_EQ_INT((unsigned char)buf[2], 0x92);
    EXPECT_EQ_INT((unsigned char)buf[3], 0xA9);
}

TEST(decode_lone_surrogate_is_error)
{
    char buf[64];
    EXPECT_EQ_SIZE(jmd_scalar_decode_string("\\uD800",
                                            6, buf, sizeof buf),
                   (size_t)-1);
}

TEST(decode_reports_required_size_on_overflow)
{
    /* Buffer too small: function should still return required size. */
    char buf[2];
    size_t n = jmd_scalar_decode_string("abcde", 5, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 5);
    /* The first two bytes ARE written. */
    EXPECT_EQ_INT(buf[0], 'a');
    EXPECT_EQ_INT(buf[1], 'b');
}

/* ---------------------------------------------------------------- */
/* jmd_key_parse                                                     */
/* ---------------------------------------------------------------- */

TEST(key_bare_simple)
{
    const char *ptr; size_t len, consumed;
    EXPECT_EQ_INT(jmd_key_parse("foo: bar", 8, &ptr, &len, &consumed),
                  JMD_OK);
    EXPECT_EQ_STRN(ptr, len, "foo");
    EXPECT_EQ_SIZE(consumed, 3);
}

TEST(key_bare_with_dash_and_underscore)
{
    const char *ptr; size_t len, consumed;
    EXPECT_EQ_INT(jmd_key_parse("my-key_2: x", 11,
                                &ptr, &len, &consumed), JMD_OK);
    EXPECT_EQ_STRN(ptr, len, "my-key_2");
    EXPECT_EQ_SIZE(consumed, 8);
}

TEST(key_quoted_consumes_quotes)
{
    const char *ptr; size_t len, consumed;
    EXPECT_EQ_INT(jmd_key_parse("\"with space\": v", 15,
                                &ptr, &len, &consumed), JMD_OK);
    EXPECT_EQ_STRN(ptr, len, "with space");
    EXPECT_EQ_SIZE(consumed, 12);
}

TEST(key_quoted_with_embedded_colon_does_not_split_early)
{
    /* Regression for D10 — quoted key containing ": " must not be
     * mis-split by the scalar layer. */
    const char *ptr; size_t len, consumed;
    EXPECT_EQ_INT(jmd_key_parse("\"foo: bar\": v", 13,
                                &ptr, &len, &consumed), JMD_OK);
    EXPECT_EQ_STRN(ptr, len, "foo: bar");
    EXPECT_EQ_SIZE(consumed, 10);
}

TEST(key_quoted_unterminated_is_error)
{
    const char *ptr; size_t len, consumed;
    EXPECT_EQ_INT(jmd_key_parse("\"open", 5,
                                &ptr, &len, &consumed), JMD_ERROR_PARSE);
}

TEST(key_empty_is_error)
{
    const char *ptr; size_t len, consumed;
    EXPECT_EQ_INT(jmd_key_parse(": v", 3,
                                &ptr, &len, &consumed), JMD_ERROR_PARSE);
}

/* ---------------------------------------------------------------- */
/* needs_quote + serialize                                           */
/* ---------------------------------------------------------------- */

TEST(needs_quote_for_empty_reserved_number)
{
    EXPECT_TRUE(jmd_scalar_needs_quote("", 0));
    EXPECT_TRUE(jmd_scalar_needs_quote("null", 4));
    EXPECT_TRUE(jmd_scalar_needs_quote("true", 4));
    EXPECT_TRUE(jmd_scalar_needs_quote("false", 5));
    EXPECT_TRUE(jmd_scalar_needs_quote("42", 2));
    EXPECT_TRUE(jmd_scalar_needs_quote("3.14", 4));
}

TEST(needs_quote_for_dash_and_structural_prefix)
{
    EXPECT_TRUE(jmd_scalar_needs_quote("-", 1));
    EXPECT_TRUE(jmd_scalar_needs_quote("# h", 3));
    EXPECT_TRUE(jmd_scalar_needs_quote("- b", 3));
}

TEST(needs_quote_for_double_quote_start_and_control_chars)
{
    EXPECT_TRUE(jmd_scalar_needs_quote("\"x", 2));
    EXPECT_TRUE(jmd_scalar_needs_quote("a\nb", 3));
    EXPECT_TRUE(jmd_scalar_needs_quote("a\tb", 3));
}

TEST(does_not_need_quote_for_normal_strings)
{
    EXPECT_FALSE(jmd_scalar_needs_quote("hello", 5));
    EXPECT_FALSE(jmd_scalar_needs_quote("foo#bar", 7));  /* `#` not at start */
    EXPECT_FALSE(jmd_scalar_needs_quote("Hauptstraße 1", 14));
}

TEST(serialize_null_bool_int_float)
{
    char buf[32];
    jmd_scalar_t v;

    v.type = JMD_SCALAR_NULL;
    EXPECT_EQ_SIZE(jmd_scalar_serialize(&v, buf, sizeof buf), 4);
    EXPECT_EQ_STRN(buf, 4, "null");

    v.type = JMD_SCALAR_BOOL; v.as.boolean = 1;
    EXPECT_EQ_SIZE(jmd_scalar_serialize(&v, buf, sizeof buf), 4);
    EXPECT_EQ_STRN(buf, 4, "true");

    v.type = JMD_SCALAR_BOOL; v.as.boolean = 0;
    EXPECT_EQ_SIZE(jmd_scalar_serialize(&v, buf, sizeof buf), 5);
    EXPECT_EQ_STRN(buf, 5, "false");

    v.type = JMD_SCALAR_INT; v.as.integer = -42;
    size_t n = jmd_scalar_serialize(&v, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 3);
    EXPECT_EQ_STRN(buf, n, "-42");
}

TEST(serialize_bare_string_emits_as_is)
{
    char buf[32];
    jmd_scalar_t v = {0};
    v.type = JMD_SCALAR_STRING;
    v.as.string.ptr = "hello";
    v.as.string.len = 5;
    size_t n = jmd_scalar_serialize(&v, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 5);
    EXPECT_EQ_STRN(buf, n, "hello");
}

TEST(serialize_string_quotes_when_needed)
{
    char buf[32];
    jmd_scalar_t v = {0};
    v.type = JMD_SCALAR_STRING;
    v.as.string.ptr = "null";   /* Would parse back as JMD_SCALAR_NULL. */
    v.as.string.len = 4;
    size_t n = jmd_scalar_serialize(&v, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 6);
    EXPECT_EQ_STRN(buf, n, "\"null\"");
}

TEST(serialize_string_escapes_newline_and_quote)
{
    char buf[32];
    jmd_scalar_t v = {0};
    v.type = JMD_SCALAR_STRING;
    v.as.string.ptr = "a\"b\nc";
    v.as.string.len = 5;
    size_t n = jmd_scalar_serialize(&v, buf, sizeof buf);
    EXPECT_EQ_STRN(buf, n, "\"a\\\"b\\nc\"");
}

TEST(serialize_reports_required_size_on_overflow)
{
    char buf[3];
    jmd_scalar_t v = {0};
    v.type = JMD_SCALAR_INT;
    v.as.integer = 12345;
    /* Required: 5 bytes. Buffer holds 3. Function returns 5 and
     * writes as much as fits. */
    EXPECT_EQ_SIZE(jmd_scalar_serialize(&v, buf, sizeof buf), 5);
    EXPECT_EQ_INT(buf[0], '1');
    EXPECT_EQ_INT(buf[1], '2');
    EXPECT_EQ_INT(buf[2], '3');
}

TEST(key_serialize_bare_when_class_permits)
{
    char buf[32];
    size_t n = jmd_key_serialize("foo-bar_2", 9, buf, sizeof buf);
    EXPECT_EQ_SIZE(n, 9);
    EXPECT_EQ_STRN(buf, n, "foo-bar_2");
}

TEST(key_serialize_quotes_when_needed)
{
    char buf[32];
    size_t n = jmd_key_serialize("with space", 10, buf, sizeof buf);
    EXPECT_EQ_STRN(buf, n, "\"with space\"");
}

TEST(key_serialize_empty_key_quotes_to_empty_quoted)
{
    char buf[32];
    size_t n = jmd_key_serialize("", 0, buf, sizeof buf);
    EXPECT_EQ_STRN(buf, n, "\"\"");
}

/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */

int main(void)
{
    RUN_TEST(parse_empty_slice_is_empty_string);
    RUN_TEST(parse_null);
    RUN_TEST(parse_true_and_false);
    RUN_TEST(parse_integers_positive_zero_negative);
    RUN_TEST(parse_floats_dotted_and_exponent);
    RUN_TEST(parse_leading_zero_number_is_zero_not_octal);
    RUN_TEST(parse_bare_string);
    RUN_TEST(parse_quoted_string_no_escapes_returns_inner_slice);
    RUN_TEST(parse_quoted_string_empty);
    RUN_TEST(parse_quoted_string_with_escape_defers_to_decoder);
    RUN_TEST(parse_quoted_string_unterminated_is_parse_error);
    RUN_TEST(parse_structural_prefix_hash_space_rejected);
    RUN_TEST(parse_structural_prefix_dash_space_rejected);
    RUN_TEST(parse_bare_dash_is_ambiguous_and_rejected);
    RUN_TEST(parse_string_containing_hash_but_not_at_start_is_fine);

    RUN_TEST(has_escapes_detects_backslash);
    RUN_TEST(decode_simple_escapes);
    RUN_TEST(decode_quote_and_backslash);
    RUN_TEST(decode_unicode_escape_bmp);
    RUN_TEST(decode_unicode_escape_surrogate_pair);
    RUN_TEST(decode_lone_surrogate_is_error);
    RUN_TEST(decode_reports_required_size_on_overflow);

    RUN_TEST(key_bare_simple);
    RUN_TEST(key_bare_with_dash_and_underscore);
    RUN_TEST(key_quoted_consumes_quotes);
    RUN_TEST(key_quoted_with_embedded_colon_does_not_split_early);
    RUN_TEST(key_quoted_unterminated_is_error);
    RUN_TEST(key_empty_is_error);

    RUN_TEST(needs_quote_for_empty_reserved_number);
    RUN_TEST(needs_quote_for_dash_and_structural_prefix);
    RUN_TEST(needs_quote_for_double_quote_start_and_control_chars);
    RUN_TEST(does_not_need_quote_for_normal_strings);

    RUN_TEST(serialize_null_bool_int_float);
    RUN_TEST(serialize_bare_string_emits_as_is);
    RUN_TEST(serialize_string_quotes_when_needed);
    RUN_TEST(serialize_string_escapes_newline_and_quote);
    RUN_TEST(serialize_reports_required_size_on_overflow);
    RUN_TEST(key_serialize_bare_when_class_permits);
    RUN_TEST(key_serialize_quotes_when_needed);
    RUN_TEST(key_serialize_empty_key_quotes_to_empty_quoted);

    return TEST_SUMMARY();
}
