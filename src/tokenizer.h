/*
 * tokenizer.h — line-level classifier for JMD source.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * The tokenizer is the lowest layer of the parser. It scans the source
 * buffer one line at a time and classifies each line into a small set
 * of categories that the heading-stack machine above it can dispatch
 * on. It does NOT interpret content beyond what the classification
 * needs — scalar parsing, key splitting, and structural recognition
 * (bullet items, blockquotes, block scalars) live one layer up.
 *
 * Internal module — not exposed in the public libjmd.h surface. The
 * `jmd_` prefix is kept for greppability and to match the project's
 * naming ethic, but these names are not part of the ABI.
 *
 * Spec reference: jmd-spec §3 (line model), §4 (headings), §6 (blank
 * lines). The tokenizer recognises three line classes:
 *
 *   - blank          (heading_depth == JMD_LINE_BLANK)
 *   - body           (heading_depth == 0): bullet, field, blockquote
 *                    continuation, indented continuation, or thematic
 *                    break — disambiguation happens above.
 *   - heading        (heading_depth >= 1): `#`, `##`, ... followed by
 *                    a space and optional content, or a bare hash
 *                    sequence on its own (`##`).
 *
 * Line endings: LF (`\n`) only at the tokenizer surface. A trailing
 * `\r` on any line is stripped from both the content slice and the
 * raw slice so consumers see CRLF and LF inputs identically. The
 * source-buffer bytes themselves are not modified.
 */

#ifndef LIBJMD_TOKENIZER_H
#define LIBJMD_TOKENIZER_H

#include <stddef.h>

/* Heading-depth sentinel for a blank line (whitespace-only or empty).
 * Body lines carry depth 0; real headings start at 1. */
#define JMD_LINE_BLANK (-1)

/*
 * jmd_line_t — one classified line.
 *
 * All pointers are borrowed slices into the source buffer the
 * tokenizer was initialised with. They remain valid as long as that
 * buffer remains alive. None of the slices are NUL-terminated —
 * always use the paired `_len` field.
 */
typedef struct {
    /* 1-based line number. The first line of the document is 1. */
    int         line_no;

    /* JMD_LINE_BLANK for blank lines, 0 for body lines, N>=1 for
     * headings of depth N. */
    int         heading_depth;

    /* Content slice. For headings, the part after the `#... ` prefix
     * (empty if the heading is bare, e.g. `##` on its own). For body
     * lines, the line stripped of leading whitespace if and only if
     * the line is an indented continuation candidate (>= 2 spaces);
     * otherwise the slice equals raw. For blank lines, both ptr and
     * len are zeroed. */
    const char *content;
    size_t      content_len;

    /* Raw slice: the line as it appears in the source, sans the
     * terminating LF and an optional CR before it. Useful when the
     * upstream parser needs to distinguish bare from indented bodies
     * (`raw[0] == ' '` is the cheap test). */
    const char *raw;
    size_t      raw_len;
} jmd_line_t;

/*
 * jmd_tokenizer_t — opaque-ish cursor over the source buffer.
 *
 * Held by value (no allocation). Initialise with jmd_tokenizer_init,
 * then call jmd_tokenizer_next in a loop until it returns 0.
 */
typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;      /* byte index of the next unread character */
    int         line_no;  /* number to assign to the next emitted line */
} jmd_tokenizer_t;

/*
 * jmd_tokenizer_init — bind the tokenizer to a source buffer.
 *
 * The buffer must remain alive for the entire scan; the tokenizer
 * does not copy. Passing a NULL src is allowed iff len is 0 (treat
 * an empty buffer as a zero-line document).
 */
void jmd_tokenizer_init(jmd_tokenizer_t *tk,
                        const char *src, size_t len);

/*
 * jmd_tokenizer_next — emit the next classified line.
 *
 * Returns 1 if a line was written into *out, 0 if the cursor reached
 * end-of-input. On 1, the caller may inspect *out until the next call
 * to jmd_tokenizer_next; the slices remain pointers into the original
 * source buffer.
 *
 * A buffer that does not end in `\n` still produces a final line for
 * its trailing content (the LF is a separator, not a terminator —
 * compatible with editor-saved files that lack a trailing newline).
 *
 * A buffer ending in exactly one `\n` does NOT emit an extra empty
 * line for the bytes after the final newline; that LF is the line
 * terminator. A buffer ending in `\n\n` does emit a final blank line
 * for the second LF.
 */
int jmd_tokenizer_next(jmd_tokenizer_t *tk, jmd_line_t *out);

#endif /* LIBJMD_TOKENIZER_H */
