/*
 * tokenizer.c — line-level classifier for JMD source.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * See tokenizer.h for the surface contract. This file is the
 * implementation; it does no allocation and never modifies the source
 * buffer. All output slices borrow from the original bytes.
 *
 * Algorithm: for each call to jmd_tokenizer_next we
 *
 *   1. take the byte range [pos, next-LF-or-EOF) as the raw line,
 *   2. trim a trailing CR off the raw slice (CRLF normalisation),
 *   3. classify it: blank, heading (`#...` + ' ' + content, or bare
 *      `#...`), or body,
 *   4. write the slice pointers + classification fields into *out,
 *   5. advance pos past the LF (or to EOF if there was none).
 *
 * Empty input emits no lines; a trailing LF is consumed as the
 * separator for the line before it, not as the start of an extra
 * blank line. A trailing `\n\n` does emit a final blank line because
 * the second LF terminates a real (empty) line. This matches the
 * jmd-impl Python tokenizer byte-for-byte and the JMD spec's line
 * model (§3): line-terminated, not line-separated.
 */

#include "tokenizer.h"

#include <stddef.h>

/* Classify a raw-line slice. Writes heading_depth, content,
 * content_len into *out. raw and raw_len must already be set. */
static void classify_line(jmd_line_t *out)
{
    const char *raw = out->raw;
    size_t      n   = out->raw_len;

    /* Blank line: empty or whitespace-only. We treat any line whose
     * non-whitespace count is zero as blank — the JMD spec only cares
     * about "is this line empty for scope purposes" (§6), not about
     * preserving trailing spaces. */
    size_t i = 0;
    while (i < n && (raw[i] == ' ' || raw[i] == '\t')) {
        i++;
    }
    if (i == n) {
        out->heading_depth = JMD_LINE_BLANK;
        out->content       = NULL;
        out->content_len   = 0;
        return;
    }

    /* Heading: a run of `#` characters at column 0, optionally
     * followed by a single space and content, or standing alone.
     * Anything else falls through to "body". Indented `#` is body
     * (an indented continuation line that happens to start with
     * `#` is not a heading — §4 requires column-0 hashes).
     *
     * Special root-marker form: `#!`, `#?`, `#-` IMMEDIATELY after
     * the single `#` (no space between) classify as depth-1
     * headings whose content keeps the marker as its first byte
     * (e.g. `#! Schema` -> depth=1, content="! Schema"). The
     * parser above strips the marker when extracting label + mode.
     * Mirrors the jmd-impl Python tokenizer's _ROOT_MARKER_RE.
     */
    if (raw[0] == '#') {
        /* Root-marker check before generic hash-run: only triggers
         * on depth-1 form `#<mark> ...`. */
        if (n >= 3 && (raw[1] == '!' || raw[1] == '?' || raw[1] == '-')
                && raw[2] == ' ') {
            out->heading_depth = 1;
            out->content       = raw + 1;
            out->content_len   = n - 1;
            return;
        }
        size_t depth = 0;
        while (depth < n && raw[depth] == '#') {
            depth++;
        }
        /* Bare heading: just `#`/`##`/... with nothing after. */
        if (depth == n) {
            out->heading_depth = (int)depth;
            out->content       = NULL;
            out->content_len   = 0;
            return;
        }
        /* Heading with content: must have exactly one space after the
         * hash run. (§4: the separating whitespace is normative one
         * space; the tokenizer is strict to keep the parser above it
         * predictable. Multi-space variants belong in the tolerance
         * fixtures and are handled by relaxing this here later if a
         * conformance case demands it.) */
        if (raw[depth] == ' ') {
            out->heading_depth = (int)depth;
            out->content       = raw + depth + 1;
            out->content_len   = n - depth - 1;
            return;
        }
        /* `#foo` without space is NOT a heading — treat as body so
         * the parser above can complain coherently. */
    }

    /* Body line. content == raw for now; the parser strips leading
     * indentation itself when it needs to (it distinguishes "exactly
     * the array-item continuation indent" from "any indentation"). */
    out->heading_depth = 0;
    out->content       = raw;
    out->content_len   = n;
}

void jmd_tokenizer_init(jmd_tokenizer_t *tk,
                        const char *src, size_t len)
{
    tk->src     = src;
    tk->len     = len;
    tk->pos     = 0;
    tk->line_no = 1;
}

int jmd_tokenizer_next(jmd_tokenizer_t *tk, jmd_line_t *out)
{
    /* End-of-input: pos has reached len. We return 0 for the very
     * last LF as well — that LF is the terminator of the previous
     * line, not the start of a new blank line. Only consecutive LFs
     * (i.e. a real empty line between them) produce a blank-line
     * emit. */
    if (tk->pos >= tk->len) {
        return 0;
    }

    /* Scan to the next LF or EOF. */
    size_t start = tk->pos;
    size_t end   = start;
    while (end < tk->len && tk->src[end] != '\n') {
        end++;
    }

    /* Raw slice = [start, end). Strip a trailing CR for CRLF inputs. */
    size_t raw_len = end - start;
    if (raw_len > 0 && tk->src[end - 1] == '\r') {
        raw_len--;
    }

    out->line_no  = tk->line_no;
    out->raw      = (raw_len == 0) ? NULL : (tk->src + start);
    out->raw_len  = raw_len;
    classify_line(out);

    /* Advance past the LF if there was one. The "trailing-LF-as-
     * terminator" rule means we do NOT loop back and emit a blank
     * line for the position right after the final LF: the next call
     * sees pos == len and returns 0. But if the source has `\n\n`,
     * the first LF advances pos past it, the second iteration finds
     * an empty raw slice (start == end), emits a blank line, then
     * advances past the second LF, and the third iteration returns
     * 0. This matches Python's str.split('\n') minus the trailing
     * empty element — the rule jmd-impl's parser already uses. */
    if (end < tk->len) {
        tk->pos = end + 1;
    } else {
        tk->pos = end;
    }
    tk->line_no++;

    return 1;
}

int jmd_tokenizer_peek(const jmd_tokenizer_t *tk, jmd_line_t *out)
{
    /* Peek shares all classification logic with next; the cheapest
     * correct implementation is to operate on a private copy of the
     * cursor and discard the post-call state. classify_line writes
     * only to *out, so this is safe. */
    jmd_tokenizer_t copy = *tk;
    return jmd_tokenizer_next(&copy, out);
}
