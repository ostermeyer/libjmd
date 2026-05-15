/*
 * test_tokenizer.c — unit tests for src/tokenizer.c.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Covers the classifications the tokenizer must distinguish:
 *   - blank vs body vs heading
 *   - heading depth and content slice
 *   - bare heading (no space, no content)
 *   - CRLF normalisation (trailing \r stripped from raw + content)
 *   - empty / no-trailing-LF / trailing-LF / trailing-double-LF
 *     edge cases for the line-emission rule
 *   - indented body line keeps leading whitespace in raw and content
 *     (the parser above strips, not the tokenizer)
 */

#include "../src/tokenizer.h"
#include "test_util.h"

#include <string.h>

/* Helper: convenience to drive the tokenizer over a literal source
 * and collect the first N lines into a caller-provided array. Returns
 * the actual number of lines emitted (may be less than N if EOF hits
 * earlier). */
static int collect(const char *src, size_t len,
                   jmd_line_t *lines, int max)
{
    jmd_tokenizer_t tk;
    jmd_tokenizer_init(&tk, src, len);
    int i = 0;
    while (i < max && jmd_tokenizer_next(&tk, &lines[i])) {
        i++;
    }
    return i;
}

TEST(empty_buffer_emits_no_lines)
{
    jmd_line_t lines[4];
    int n = collect("", 0, lines, 4);
    EXPECT_EQ_INT(n, 0);
}

TEST(single_line_no_trailing_lf)
{
    const char *s = "# Order";
    jmd_line_t lines[2];
    int n = collect(s, strlen(s), lines, 2);
    EXPECT_EQ_INT(n, 1);
    EXPECT_EQ_INT(lines[0].line_no, 1);
    EXPECT_EQ_INT(lines[0].heading_depth, 1);
    EXPECT_EQ_STRN(lines[0].content, lines[0].content_len, "Order");
    EXPECT_EQ_STRN(lines[0].raw,     lines[0].raw_len,     "# Order");
}

TEST(single_line_with_trailing_lf_emits_only_one_line)
{
    const char *s = "# Order\n";
    jmd_line_t lines[4];
    int n = collect(s, strlen(s), lines, 4);
    /* The trailing LF terminates the line; no extra blank emitted. */
    EXPECT_EQ_INT(n, 1);
    EXPECT_EQ_INT(lines[0].heading_depth, 1);
}

TEST(double_trailing_lf_emits_a_blank_line)
{
    const char *s = "# Order\n\n";
    jmd_line_t lines[4];
    int n = collect(s, strlen(s), lines, 4);
    EXPECT_EQ_INT(n, 2);
    EXPECT_EQ_INT(lines[0].heading_depth, 1);
    EXPECT_EQ_INT(lines[1].heading_depth, JMD_LINE_BLANK);
    EXPECT_EQ_INT(lines[1].line_no, 2);
}

TEST(blank_line_between_two_body_lines)
{
    const char *s = "id: 1\n\nx: 2\n";
    jmd_line_t lines[4];
    int n = collect(s, strlen(s), lines, 4);
    EXPECT_EQ_INT(n, 3);
    EXPECT_EQ_INT(lines[0].heading_depth, 0);
    EXPECT_EQ_STRN(lines[0].content, lines[0].content_len, "id: 1");
    EXPECT_EQ_INT(lines[1].heading_depth, JMD_LINE_BLANK);
    EXPECT_EQ_INT(lines[2].heading_depth, 0);
    EXPECT_EQ_STRN(lines[2].content, lines[2].content_len, "x: 2");
}

TEST(heading_depths_one_two_three)
{
    const char *s = "# A\n## B\n### C\n";
    jmd_line_t lines[4];
    int n = collect(s, strlen(s), lines, 4);
    EXPECT_EQ_INT(n, 3);
    EXPECT_EQ_INT(lines[0].heading_depth, 1);
    EXPECT_EQ_STRN(lines[0].content, lines[0].content_len, "A");
    EXPECT_EQ_INT(lines[1].heading_depth, 2);
    EXPECT_EQ_STRN(lines[1].content, lines[1].content_len, "B");
    EXPECT_EQ_INT(lines[2].heading_depth, 3);
    EXPECT_EQ_STRN(lines[2].content, lines[2].content_len, "C");
}

TEST(bare_heading_no_space_no_content)
{
    const char *s = "##\n";
    jmd_line_t lines[2];
    int n = collect(s, strlen(s), lines, 2);
    EXPECT_EQ_INT(n, 1);
    EXPECT_EQ_INT(lines[0].heading_depth, 2);
    EXPECT_EQ_SIZE(lines[0].content_len, 0);
    EXPECT_TRUE(lines[0].content == NULL);
}

TEST(heading_with_brackets_content_preserved)
{
    /* The tokenizer doesn't interpret `[]` — that's the parser's job.
     * It just exposes the slice. */
    const char *s = "## items[]\n";
    jmd_line_t lines[2];
    int n = collect(s, strlen(s), lines, 2);
    EXPECT_EQ_INT(n, 1);
    EXPECT_EQ_INT(lines[0].heading_depth, 2);
    EXPECT_EQ_STRN(lines[0].content, lines[0].content_len, "items[]");
}

TEST(crlf_stripped_from_both_raw_and_content)
{
    const char *s = "# Order\r\nid: 1\r\n";
    jmd_line_t lines[4];
    int n = collect(s, strlen(s), lines, 4);
    EXPECT_EQ_INT(n, 2);
    EXPECT_EQ_STRN(lines[0].raw,     lines[0].raw_len,     "# Order");
    EXPECT_EQ_STRN(lines[0].content, lines[0].content_len, "Order");
    EXPECT_EQ_STRN(lines[1].raw,     lines[1].raw_len,     "id: 1");
    EXPECT_EQ_STRN(lines[1].content, lines[1].content_len, "id: 1");
}

TEST(indented_continuation_keeps_leading_whitespace_in_raw)
{
    /* Tokenizer convention: raw is the line as-typed; content is also
     * the full body slice. The parser strips indentation when needed.
     * We just verify here that raw[0] == ' ' is the cheap indented-
     * check the parser relies on. */
    const char *s = "  id: 1\n";
    jmd_line_t lines[2];
    int n = collect(s, strlen(s), lines, 2);
    EXPECT_EQ_INT(n, 1);
    EXPECT_EQ_INT(lines[0].heading_depth, 0);
    EXPECT_EQ_STRN(lines[0].raw, lines[0].raw_len, "  id: 1");
    EXPECT_TRUE(lines[0].raw[0] == ' ');
}

TEST(whitespace_only_line_classified_as_blank)
{
    const char *s = "# A\n   \n# B\n";
    jmd_line_t lines[4];
    int n = collect(s, strlen(s), lines, 4);
    EXPECT_EQ_INT(n, 3);
    EXPECT_EQ_INT(lines[1].heading_depth, JMD_LINE_BLANK);
}

TEST(hash_without_space_treated_as_body)
{
    /* `#foo` is not a heading per §4. Tokenizer reports body so the
     * parser above can give a coherent error if it occurs in a context
     * that requires a heading. */
    const char *s = "#foo\n";
    jmd_line_t lines[2];
    int n = collect(s, strlen(s), lines, 2);
    EXPECT_EQ_INT(n, 1);
    EXPECT_EQ_INT(lines[0].heading_depth, 0);
    EXPECT_EQ_STRN(lines[0].content, lines[0].content_len, "#foo");
}

TEST(thematic_break_is_a_body_line)
{
    /* `---` at column 0 is a body line for the tokenizer; the parser
     * decides whether it's a thematic break in the current scope. */
    const char *s = "---\n";
    jmd_line_t lines[2];
    int n = collect(s, strlen(s), lines, 2);
    EXPECT_EQ_INT(n, 1);
    EXPECT_EQ_INT(lines[0].heading_depth, 0);
    EXPECT_EQ_STRN(lines[0].content, lines[0].content_len, "---");
}

TEST(line_numbers_are_one_based_and_monotonic)
{
    const char *s = "A\nB\n\nD\n";
    jmd_line_t lines[8];
    int n = collect(s, strlen(s), lines, 8);
    EXPECT_EQ_INT(n, 4);
    EXPECT_EQ_INT(lines[0].line_no, 1);
    EXPECT_EQ_INT(lines[1].line_no, 2);
    EXPECT_EQ_INT(lines[2].line_no, 3);
    EXPECT_EQ_INT(lines[3].line_no, 4);
}

int main(void)
{
    RUN_TEST(empty_buffer_emits_no_lines);
    RUN_TEST(single_line_no_trailing_lf);
    RUN_TEST(single_line_with_trailing_lf_emits_only_one_line);
    RUN_TEST(double_trailing_lf_emits_a_blank_line);
    RUN_TEST(blank_line_between_two_body_lines);
    RUN_TEST(heading_depths_one_two_three);
    RUN_TEST(bare_heading_no_space_no_content);
    RUN_TEST(heading_with_brackets_content_preserved);
    RUN_TEST(crlf_stripped_from_both_raw_and_content);
    RUN_TEST(indented_continuation_keeps_leading_whitespace_in_raw);
    RUN_TEST(whitespace_only_line_classified_as_blank);
    RUN_TEST(hash_without_space_treated_as_body);
    RUN_TEST(thematic_break_is_a_body_line);
    RUN_TEST(line_numbers_are_one_based_and_monotonic);
    return TEST_SUMMARY();
}
