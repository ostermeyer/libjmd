/*
 * test_util.h — minimal test harness for libjmd.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Intentionally tiny: a few macros over <stdio.h> and a per-binary
 * pass/fail counter. No external dependency, no test runner. Each
 * test binary lives under tests/test_<module>.c, links against the
 * static library, runs every TEST() it declares, and exits with a
 * non-zero status if any assertion failed.
 *
 * Usage in a test file:
 *
 *     #include "test_util.h"
 *     #include "tokenizer.h"
 *
 *     TEST(tokenizer_classifies_blank_line)
 *     {
 *         jmd_line_t line;
 *         // ...exercise code...
 *         EXPECT_EQ_INT(line.heading_depth, -1);
 *     }
 *
 *     int main(void) {
 *         RUN_TEST(tokenizer_classifies_blank_line);
 *         return TEST_SUMMARY();
 *     }
 *
 * Assertions print file:line and the diverging values on failure,
 * then mark the current test as failed and return from it early
 * (so subsequent assertions in the same TEST don't spam the output).
 */

#ifndef LIBJMD_TEST_UTIL_H
#define LIBJMD_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

/* File-scope counters so multiple test files (or a single file with
 * many TEST()s) accumulate into one final summary. */
static int test_total_  = 0;
static int test_failed_ = 0;
static int test_current_failed_ = 0;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                       \
    test_current_failed_ = 0;                                     \
    test_total_++;                                                \
    test_##name();                                                \
    if (test_current_failed_) test_failed_++;                     \
} while (0)

#define TEST_SUMMARY() (                                          \
    (void)fprintf(stderr,                                         \
        "\n%d test(s) run, %d failed.\n",                         \
        test_total_, test_failed_),                               \
    test_failed_ == 0 ? 0 : 1                                     \
)

/* Internal — record a failure and bail out of the current TEST. */
#define TEST_FAIL_HERE_(fmt, ...) do {                            \
    (void)fprintf(stderr,                                         \
        "  FAIL %s:%d: " fmt "\n",                                \
        __FILE__, __LINE__, __VA_ARGS__);                         \
    test_current_failed_ = 1;                                     \
    return;                                                       \
} while (0)

#define EXPECT_EQ_INT(actual, expected) do {                      \
    long long _a = (long long)(actual);                           \
    long long _e = (long long)(expected);                         \
    if (_a != _e) {                                               \
        TEST_FAIL_HERE_("expected %lld, got %lld", _e, _a);       \
    }                                                             \
} while (0)

#define EXPECT_EQ_SIZE(actual, expected) do {                     \
    size_t _a = (size_t)(actual);                                 \
    size_t _e = (size_t)(expected);                               \
    if (_a != _e) {                                               \
        TEST_FAIL_HERE_("expected %zu, got %zu", _e, _a);         \
    }                                                             \
} while (0)

/* Compare a (ptr, len) slice against a NUL-terminated literal. */
#define EXPECT_EQ_STRN(actual_ptr, actual_len, expected_lit) do { \
    size_t _e_len = sizeof(expected_lit) - 1;                     \
    size_t _a_len = (size_t)(actual_len);                         \
    const char *_a = (actual_ptr);                                \
    if (_a_len != _e_len ||                                       \
            (_a_len > 0 && memcmp(_a, (expected_lit), _a_len))) { \
        TEST_FAIL_HERE_(                                          \
            "expected \"%s\" (len %zu), got \"%.*s\" (len %zu)",  \
            (expected_lit), _e_len,                               \
            (int)_a_len, _a, _a_len);                             \
    }                                                             \
} while (0)

#define EXPECT_TRUE(cond) do {                                    \
    if (!(cond)) {                                                \
        TEST_FAIL_HERE_("expected true: %s", #cond);              \
    }                                                             \
} while (0)

#define EXPECT_FALSE(cond) do {                                   \
    if ((cond)) {                                                 \
        TEST_FAIL_HERE_("expected false: %s", #cond);             \
    }                                                             \
} while (0)

#endif /* LIBJMD_TEST_UTIL_H */
