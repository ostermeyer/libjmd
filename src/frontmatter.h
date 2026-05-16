/*
 * frontmatter.h — parse YAML-style frontmatter lines before the
 * root heading, emit Visitor on_frontmatter events.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frontmatter shape (spec §3.5):
 *
 *   key: scalar
 *   bare-key                 # value = true
 *   key:                     # multi-line value follows as blockquote
 *   > line one
 *   > line two
 *   ---                      # §3.5.1 marker tolerance (silent)
 *
 * Bracketing `---` markers on either side of the block are accepted
 * but produce no event (§3.5.1). A blank line is treated as a
 * separator and ignored; it does NOT terminate the frontmatter on
 * its own. The block ends at the first depth>=1 heading or at EOF.
 *
 * Output: one on_frontmatter callback per field. For a multi-line
 * blockquote value the lines are concatenated into the caller-
 * provided scratch buffer (LF-joined, like jmd-impl's blockquote
 * handling) and the emitted slice points into that buffer.
 */

#ifndef LIBJMD_FRONTMATTER_H
#define LIBJMD_FRONTMATTER_H

#include "libjmd.h"
#include "tokenizer.h"

#include <stddef.h>

/*
 * jmd_frontmatter_parse — walk the tokenizer cursor over the
 * frontmatter region and emit Visitor events.
 *
 * Stops without consuming the first depth>=1 heading line, so the
 * caller's heading-stack parser can pick up exactly where this one
 * left off (peek-based — the cursor is left pointing AT the heading).
 *
 * The visitor argument may be NULL or carry a NULL on_frontmatter;
 * in either case the function still parses + validates every line
 * but emits nothing. Useful for the parser layer's two-pass strategy
 * (validate first, emit second) and for unit tests that just want
 * a clean parse without subscribing.
 *
 * Scratch buffer:
 *
 *   - For each multi-line blockquote value, the joined string is
 *     materialised into [scratch, scratch+scratch_cap). The slice
 *     handed to on_frontmatter points there and remains valid only
 *     for the callback's duration — the next field re-uses the
 *     same buffer.
 *   - For quoted-string values with JSON escapes, the decoded form
 *     is also materialised into scratch. Same lifetime rule.
 *
 *   scratch == NULL is allowed iff every value fits the no-scratch
 *   path (bare scalar, no-escape quoted, simple `key` -> true).
 *   Hitting a case that needs scratch with a NULL or too-small
 *   buffer returns JMD_ERROR_MEMORY.
 *
 * Returns:
 *   JMD_OK                     normal completion (heading or EOF
 *                              reached cleanly)
 *   JMD_ERROR_PARSE            malformed field line
 *   JMD_ERROR_MEMORY           scratch exhausted
 *   any non-zero               propagated unchanged from a visitor
 *                              callback (consumer-defined abort)
 */
int jmd_frontmatter_parse(jmd_tokenizer_t *tk,
                          const jmd_visitor_t *visitor,
                          void *visitor_ctx,
                          char *scratch, size_t scratch_cap);

#endif /* LIBJMD_FRONTMATTER_H */
