/*
 * test_frontmatter.c — unit tests for src/frontmatter.c.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Covers:
 *   - bare key      -> value=true
 *   - key: scalar   -> typed scalar (each scalar kind)
 *   - key:          -> empty string
 *   - key: + > ...  -> multi-line blockquote (D12)
 *   - quoted keys   (incl. embedded ":" — D10 regression)
 *   - quoted scalar values with escapes -> decoded into scratch
 *   - §3.5.1 marker tolerance (`---`, `----`, both sides)
 *   - blank lines between entries
 *   - cursor stops AT the first heading, not past it
 *   - empty / no-frontmatter inputs are clean no-ops
 *   - visitor abort propagates
 *   - scratch overflow returns JMD_ERROR_MEMORY without partial
 *     emission
 *
 * Captures emitted on_frontmatter events into a fixed-size array
 * via a small "capture" visitor so each test can assert exactly
 * which fields were emitted in what order with what values.
 */

#include "../src/frontmatter.h"
#include "../src/tokenizer.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Capture visitor                                                   */
/* ---------------------------------------------------------------- */

#define CAPTURE_MAX_FIELDS 16
#define CAPTURE_BUF_SIZE   256

typedef struct {
    /* Captured key — copied here because the callback slice is
     * only valid for the call's duration. */
    char        key[64];
    size_t      key_len;
    /* Captured value — type + payload. For string values we copy
     * up to CAPTURE_BUF_SIZE bytes into `string_buf`. */
    jmd_scalar_type_t type;
    int         boolean;
    int64_t     integer;
    double      floating;
    char        string_buf[CAPTURE_BUF_SIZE];
    size_t      string_len;
} capture_field_t;

typedef struct {
    capture_field_t fields[CAPTURE_MAX_FIELDS];
    int             count;
    int             abort_after;   /* -1 = never */
} capture_ctx_t;

static int capture_on_frontmatter(void *ctx,
                                  const char *key, size_t key_len,
                                  const jmd_scalar_t *value)
{
    capture_ctx_t *cc = (capture_ctx_t *)ctx;
    if (cc->count >= CAPTURE_MAX_FIELDS) return JMD_ERROR_INTERNAL;
    /* Abort gate: if the caller asked us to stop after N emissions,
     * refuse the (N+1)-th call before recording it — the visitor
     * contract is "non-zero aborts the parse", so the parser will
     * not invoke us again. cc->count then reflects the number of
     * emissions actually accepted. */
    if (cc->abort_after >= 0 && cc->count >= cc->abort_after) {
        return JMD_ABORT;
    }
    capture_field_t *f = &cc->fields[cc->count];
    size_t kn = (key_len < sizeof f->key) ? key_len : sizeof f->key - 1;
    memcpy(f->key, key, kn);
    f->key[kn] = '\0';
    f->key_len = key_len;
    f->type = value->type;
    switch (value->type) {
    case JMD_SCALAR_BOOL:   f->boolean  = value->as.boolean;       break;
    case JMD_SCALAR_INT:    f->integer  = value->as.integer;       break;
    case JMD_SCALAR_FLOAT:  f->floating = value->as.floating;      break;
    case JMD_SCALAR_STRING: {
        size_t n = value->as.string.len;
        if (n > sizeof f->string_buf) n = sizeof f->string_buf;
        if (n > 0 && value->as.string.ptr) {
            memcpy(f->string_buf, value->as.string.ptr, n);
        }
        f->string_len = n;
        break;
    }
    case JMD_SCALAR_NULL:
        break;
    }
    cc->count++;
    return JMD_OK;
}

static const jmd_visitor_t CAPTURE_VISITOR = {
    .on_frontmatter = capture_on_frontmatter,
};

static void capture_init(capture_ctx_t *cc)
{
    memset(cc, 0, sizeof *cc);
    cc->abort_after = -1;
}

/* Helper: drive parse on a literal source with default scratch. */
static int run(const char *src, capture_ctx_t *cc,
               jmd_tokenizer_t *out_tk)
{
    jmd_tokenizer_init(out_tk, src, strlen(src));
    static char scratch[1024];
    return jmd_frontmatter_parse(out_tk, &CAPTURE_VISITOR, cc,
                                 scratch, sizeof scratch);
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

TEST(empty_input_is_clean_noop)
{
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run("", &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 0);
}

TEST(only_heading_no_frontmatter)
{
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run("# Doc\nx: 1\n", &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 0);
    /* Cursor must be positioned AT the heading, not past it. */
    jmd_line_t peek;
    EXPECT_TRUE(jmd_tokenizer_peek(&tk, &peek));
    EXPECT_EQ_INT(peek.heading_depth, 1);
}

TEST(single_scalar_field)
{
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run("confidence: high\n\n# Doc\n", &cc, &tk),
                  JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
    EXPECT_EQ_STRN(cc.fields[0].key, cc.fields[0].key_len, "confidence");
    EXPECT_EQ_INT(cc.fields[0].type, JMD_SCALAR_STRING);
    EXPECT_EQ_STRN(cc.fields[0].string_buf, cc.fields[0].string_len,
                   "high");
}

TEST(typed_scalars)
{
    const char *src =
        "n: 42\n"
        "f: 3.14\n"
        "b: true\n"
        "n2: null\n"
        "\n"
        "# Doc\n";
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run(src, &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 4);
    EXPECT_EQ_INT(cc.fields[0].type, JMD_SCALAR_INT);
    EXPECT_EQ_INT(cc.fields[0].integer, 42);
    EXPECT_EQ_INT(cc.fields[1].type, JMD_SCALAR_FLOAT);
    EXPECT_TRUE(cc.fields[1].floating > 3.13 && cc.fields[1].floating < 3.15);
    EXPECT_EQ_INT(cc.fields[2].type, JMD_SCALAR_BOOL);
    EXPECT_EQ_INT(cc.fields[2].boolean, 1);
    EXPECT_EQ_INT(cc.fields[3].type, JMD_SCALAR_NULL);
}

TEST(bare_key_is_true)
{
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run("draft\n# Doc\n", &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
    EXPECT_EQ_INT(cc.fields[0].type, JMD_SCALAR_BOOL);
    EXPECT_EQ_INT(cc.fields[0].boolean, 1);
}

TEST(empty_value_is_empty_string)
{
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run("k:\n# Doc\n", &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
    EXPECT_EQ_INT(cc.fields[0].type, JMD_SCALAR_STRING);
    EXPECT_EQ_SIZE(cc.fields[0].string_len, 0);
}

TEST(quoted_key_with_embedded_colon)
{
    /* D10 regression: split must not mis-fire on `: ` inside the
     * quoted key. */
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run("\"foo: bar\": v\n# Doc\n", &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
    EXPECT_EQ_STRN(cc.fields[0].key, cc.fields[0].key_len, "foo: bar");
    EXPECT_EQ_STRN(cc.fields[0].string_buf, cc.fields[0].string_len,
                   "v");
}

TEST(multi_line_blockquote_value_d12)
{
    const char *src =
        "summary:\n"
        "> line one\n"
        "> line two\n"
        "\n"
        "# Doc\n";
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run(src, &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
    EXPECT_EQ_INT(cc.fields[0].type, JMD_SCALAR_STRING);
    EXPECT_EQ_STRN(cc.fields[0].string_buf, cc.fields[0].string_len,
                   "line one\nline two");
}

TEST(blockquote_with_embedded_blank_line)
{
    /* Bare `>` is a blockquote-internal empty line — joins as a
     * literal `\n` in the value (since LF is the separator we add
     * between every joined line). */
    const char *src =
        "note:\n"
        "> a\n"
        ">\n"
        "> b\n"
        "# Doc\n";
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run(src, &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
    EXPECT_EQ_STRN(cc.fields[0].string_buf, cc.fields[0].string_len,
                   "a\n\nb");
}

TEST(quoted_scalar_value_with_escape_decoded)
{
    /* `\n` JSON escape in a quoted value must come out as a
     * literal LF byte in the emitted slice. */
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run("note: \"a\\nb\"\n# Doc\n", &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
    EXPECT_EQ_STRN(cc.fields[0].string_buf, cc.fields[0].string_len,
                   "a\nb");
}

TEST(marker_dashes_consumed_silently_before)
{
    const char *src =
        "---\n"
        "k: v\n"
        "\n"
        "# Doc\n";
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run(src, &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
    EXPECT_EQ_STRN(cc.fields[0].key, cc.fields[0].key_len, "k");
}

TEST(marker_dashes_consumed_silently_after)
{
    const char *src =
        "k: v\n"
        "---\n"
        "# Doc\n";
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run(src, &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
}

TEST(markers_on_both_sides_match_plain_form)
{
    const char *src =
        "---\n"
        "a: 1\n"
        "b: 2\n"
        "---\n"
        "\n"
        "# Doc\n";
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run(src, &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 2);
    EXPECT_EQ_STRN(cc.fields[0].key, cc.fields[0].key_len, "a");
    EXPECT_EQ_INT(cc.fields[0].integer, 1);
    EXPECT_EQ_STRN(cc.fields[1].key, cc.fields[1].key_len, "b");
    EXPECT_EQ_INT(cc.fields[1].integer, 2);
}

TEST(four_and_five_dashes_are_tolerated_markers)
{
    const char *src =
        "----\n"
        "k: v\n"
        "-----\n"
        "# Doc\n";
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run(src, &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 1);
}

TEST(blank_lines_do_not_terminate_frontmatter)
{
    const char *src =
        "a: 1\n"
        "\n"
        "b: 2\n"
        "\n"
        "# Doc\n";
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run(src, &cc, &tk), JMD_OK);
    EXPECT_EQ_INT(cc.count, 2);
}

TEST(cursor_stops_at_heading_with_no_extra_consumption)
{
    capture_ctx_t cc; capture_init(&cc);
    jmd_tokenizer_t tk;
    EXPECT_EQ_INT(run("k: v\n# Doc\nx: 1\n", &cc, &tk), JMD_OK);
    jmd_line_t peek;
    EXPECT_TRUE(jmd_tokenizer_peek(&tk, &peek));
    EXPECT_EQ_INT(peek.heading_depth, 1);
    EXPECT_EQ_STRN(peek.content, peek.content_len, "Doc");
}

TEST(visitor_abort_propagates_unchanged)
{
    capture_ctx_t cc; capture_init(&cc);
    cc.abort_after = 1;  /* Abort after the 1st emission. */
    jmd_tokenizer_t tk;
    int rc = run("a: 1\nb: 2\nc: 3\n# Doc\n", &cc, &tk);
    EXPECT_EQ_INT(rc, JMD_ABORT);
    EXPECT_EQ_INT(cc.count, 1);
}

TEST(scratch_overflow_returns_memory_error)
{
    /* Tiny scratch (8 bytes) — a 20-byte blockquote value won't fit. */
    const char *src =
        "note:\n"
        "> aaaaaaaaaaaaaaaaaaa\n"
        "# Doc\n";
    jmd_tokenizer_t tk;
    jmd_tokenizer_init(&tk, src, strlen(src));
    capture_ctx_t cc; capture_init(&cc);
    char tiny[8];
    int rc = jmd_frontmatter_parse(&tk, &CAPTURE_VISITOR, &cc,
                                   tiny, sizeof tiny);
    EXPECT_EQ_INT(rc, JMD_ERROR_MEMORY);
    EXPECT_EQ_INT(cc.count, 0);  /* No partial emission. */
}

TEST(null_visitor_is_silent_but_still_validates)
{
    /* Useful for a two-pass strategy: validate cleanly, then a
     * second pass with a real visitor for emission. */
    const char *src = "k: v\n# Doc\n";
    jmd_tokenizer_t tk;
    jmd_tokenizer_init(&tk, src, strlen(src));
    char scratch[64];
    EXPECT_EQ_INT(jmd_frontmatter_parse(&tk, NULL, NULL,
                                        scratch, sizeof scratch),
                  JMD_OK);
}

int main(void)
{
    RUN_TEST(empty_input_is_clean_noop);
    RUN_TEST(only_heading_no_frontmatter);
    RUN_TEST(single_scalar_field);
    RUN_TEST(typed_scalars);
    RUN_TEST(bare_key_is_true);
    RUN_TEST(empty_value_is_empty_string);
    RUN_TEST(quoted_key_with_embedded_colon);
    RUN_TEST(multi_line_blockquote_value_d12);
    RUN_TEST(blockquote_with_embedded_blank_line);
    RUN_TEST(quoted_scalar_value_with_escape_decoded);
    RUN_TEST(marker_dashes_consumed_silently_before);
    RUN_TEST(marker_dashes_consumed_silently_after);
    RUN_TEST(markers_on_both_sides_match_plain_form);
    RUN_TEST(four_and_five_dashes_are_tolerated_markers);
    RUN_TEST(blank_lines_do_not_terminate_frontmatter);
    RUN_TEST(cursor_stops_at_heading_with_no_extra_consumption);
    RUN_TEST(visitor_abort_propagates_unchanged);
    RUN_TEST(scratch_overflow_returns_memory_error);
    RUN_TEST(null_visitor_is_silent_but_still_validates);
    return TEST_SUMMARY();
}
