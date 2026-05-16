/*
 * frontmatter.c — implementation of jmd_frontmatter_parse.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Algorithm:
 *
 *   loop:
 *     peek next line
 *     if heading (depth >= 1): stop, leave cursor at line
 *     consume the line
 *     if blank: continue
 *     if 3+ dashes only (`---`, `----`, ...): continue        (§3.5.1)
 *     parse a key at the start of the content
 *     if content has `key:` (empty value followed by `>` blockquote
 *         lines): collect them into scratch                  (D12)
 *     emit on_frontmatter with the key + scalar value
 *
 * The scratch buffer doubles as the JSON-escape-decode destination
 * for quoted scalar values; if a value's decoded form would not
 * fit, JMD_ERROR_MEMORY is returned without partial emission.
 */

#include "frontmatter.h"

#include "scalars.h"

#include <stddef.h>
#include <string.h>

/* True iff the line slice is entirely composed of 3+ `-` bytes —
 * the §3.5.1 marker form. The tokenizer already classifies blank
 * and heading lines; this only sees body lines. */
static int is_dashes_marker(const jmd_line_t *line)
{
    if (line->heading_depth != 0) return 0;
    if (line->raw_len < 3) return 0;
    for (size_t i = 0; i < line->raw_len; i++) {
        if (line->raw[i] != '-') return 0;
    }
    return 1;
}

/* Emit one on_frontmatter event if the visitor wants it. Returns
 * whatever the visitor returned, or JMD_OK if no callback is
 * subscribed. The callback contract from libjmd.h: any non-zero
 * return aborts the parse and is propagated unchanged. */
static int emit_field(const jmd_visitor_t *visitor, void *ctx,
                      const char *key, size_t key_len,
                      const jmd_scalar_t *value)
{
    if (!visitor || !visitor->on_frontmatter) return JMD_OK;
    return visitor->on_frontmatter(ctx, key, key_len, value);
}

/* Collect subsequent `> ...` lines from the tokenizer into scratch,
 * LF-joined. Returns the number of bytes written (always <= cap),
 * or (size_t)-1 if scratch overflows.
 *
 * Stops at the first non-blockquote line; the cursor is left
 * pointing at that line. A blank line that immediately follows a
 * blockquote is NOT consumed — it belongs to the frontmatter
 * separator logic in the caller. */
static size_t collect_blockquote(jmd_tokenizer_t *tk,
                                 char *scratch, size_t scratch_cap)
{
    size_t pos = 0;
    int first = 1;
    jmd_line_t peek;
    while (jmd_tokenizer_peek(tk, &peek)) {
        if (peek.heading_depth != 0) break;
        if (peek.raw_len == 0) break;
        if (peek.raw[0] != '>') break;
        /* Two accepted forms: bare `>` (empty content line) or
         * `> ...` (one-space separator then content). Anything else
         * starting with `>` falls through as not-a-blockquote. */
        const char *content = NULL;
        size_t      content_len = 0;
        if (peek.raw_len == 1) {
            /* Bare `>` — empty content line. */
        } else if (peek.raw_len >= 2 && peek.raw[1] == ' ') {
            content     = peek.raw + 2;
            content_len = peek.raw_len - 2;
        } else {
            break;
        }
        /* Consume the line now that we've committed to it. */
        jmd_line_t consumed;
        jmd_tokenizer_next(tk, &consumed);
        /* LF separator between joined lines, no leading LF. */
        if (!first) {
            if (pos >= scratch_cap) return (size_t)-1;
            scratch[pos++] = '\n';
        }
        first = 0;
        if (content_len > 0) {
            if (pos + content_len > scratch_cap) return (size_t)-1;
            memcpy(scratch + pos, content, content_len);
            pos += content_len;
        }
    }
    return pos;
}

/* Parse one frontmatter field line. content is the line slice as
 * delivered by the tokenizer (heading_depth == 0).
 *
 * On success emits exactly one on_frontmatter event; on visitor-
 * abort propagates the visitor's return code. May advance the
 * tokenizer further to consume blockquote continuation lines. */
static int parse_field(jmd_tokenizer_t *tk,
                       const jmd_line_t *line,
                       const jmd_visitor_t *visitor, void *ctx,
                       char *scratch, size_t scratch_cap)
{
    const char *raw = line->raw;
    size_t      n   = line->raw_len;

    /* Step 1: peel off the key. */
    const char *key_ptr;
    size_t      key_len;
    size_t      key_consumed;
    if (jmd_key_parse(raw, n, &key_ptr, &key_len, &key_consumed) != JMD_OK) {
        return JMD_ERROR_PARSE;
    }

    /* Step 2: classify what follows the key. Four shapes:
     *   a) end-of-line               -> bare key, value = true
     *   b) `:` end-of-line           -> empty / multi-line
     *   c) `: ` + scalar text        -> scalar value
     *   d) anything else             -> parse error */
    if (key_consumed == n) {
        /* Bare key form. */
        jmd_scalar_t v;
        v.type = JMD_SCALAR_BOOL;
        v.as.boolean = 1;
        return emit_field(visitor, ctx, key_ptr, key_len, &v);
    }
    if (raw[key_consumed] != ':') {
        return JMD_ERROR_PARSE;
    }
    if (key_consumed + 1 == n) {
        /* `key:` at end of line. Look for blockquote continuation. */
        jmd_line_t peek;
        if (jmd_tokenizer_peek(tk, &peek)
                && peek.heading_depth == 0
                && peek.raw_len > 0
                && peek.raw[0] == '>'
                && (peek.raw_len == 1 || peek.raw[1] == ' ')) {
            if (scratch_cap == 0) return JMD_ERROR_MEMORY;
            size_t written = collect_blockquote(tk, scratch, scratch_cap);
            if (written == (size_t)-1) return JMD_ERROR_MEMORY;
            jmd_scalar_t v;
            v.type = JMD_SCALAR_STRING;
            v.as.string.ptr = (written == 0) ? NULL : scratch;
            v.as.string.len = written;
            return emit_field(visitor, ctx, key_ptr, key_len, &v);
        }
        /* No `>` follows -> empty string value. */
        jmd_scalar_t v;
        v.type = JMD_SCALAR_STRING;
        v.as.string.ptr = NULL;
        v.as.string.len = 0;
        return emit_field(visitor, ctx, key_ptr, key_len, &v);
    }
    if (raw[key_consumed + 1] != ' ') {
        return JMD_ERROR_PARSE;
    }

    /* Step 3: scalar text follows after `: `. */
    const char *val_raw = raw + key_consumed + 2;
    size_t      val_len = n - key_consumed - 2;
    jmd_scalar_t v;
    int rc = jmd_scalar_parse(val_raw, val_len, &v);
    if (rc == JMD_OK) {
        return emit_field(visitor, ctx, key_ptr, key_len, &v);
    }
    if (rc != JMD_ERROR_PARSE) return rc;

    /* JMD_ERROR_PARSE here means: it's a quoted string with
     * escapes, and the caller (us) is expected to decode. Any
     * OTHER parse error (structural-prefix, bare-dash, unterminated
     * quote) is a real failure — distinguish by peeking at the
     * first byte: only a leading `"` can mean "needs decode". */
    if (val_len < 2 || val_raw[0] != '"' || val_raw[val_len - 1] != '"') {
        return JMD_ERROR_PARSE;
    }
    if (scratch_cap == 0) return JMD_ERROR_MEMORY;
    size_t decoded = jmd_scalar_decode_string(val_raw + 1, val_len - 2,
                                              scratch, scratch_cap);
    if (decoded == (size_t)-1) return JMD_ERROR_PARSE;
    if (decoded > scratch_cap) return JMD_ERROR_MEMORY;
    v.type = JMD_SCALAR_STRING;
    v.as.string.ptr = (decoded == 0) ? NULL : scratch;
    v.as.string.len = decoded;
    return emit_field(visitor, ctx, key_ptr, key_len, &v);
}

int jmd_frontmatter_parse(jmd_tokenizer_t *tk,
                          const jmd_visitor_t *visitor,
                          void *visitor_ctx,
                          char *scratch, size_t scratch_cap)
{
    for (;;) {
        jmd_line_t peek;
        if (!jmd_tokenizer_peek(tk, &peek)) {
            return JMD_OK;  /* EOF before any heading — empty doc. */
        }
        if (peek.heading_depth >= 1) {
            return JMD_OK;  /* Caller takes over. */
        }
        /* Commit to consuming. */
        jmd_line_t line;
        jmd_tokenizer_next(tk, &line);

        if (line.heading_depth < 0) continue;     /* blank */
        if (is_dashes_marker(&line)) continue;    /* §3.5.1 */

        int rc = parse_field(tk, &line, visitor, visitor_ctx,
                             scratch, scratch_cap);
        if (rc != JMD_OK) return rc;
    }
}
