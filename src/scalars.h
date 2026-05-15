/*
 * scalars.h — scalar and key parsing / serialization for libjmd.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sits one level above the tokenizer: takes raw byte slices that the
 * line classifier has handed up and turns them into jmd_scalar_t
 * values (parse direction) or canonical text (serialize direction).
 *
 * Spec reference:
 *   - §2.1 scalar disambiguation (null/true/false/number, fallback to
 *     string)
 *   - §5   quoted-string rules (JSON escape semantics)
 *   - §5a  structural prefix rejection (`# ` and `- ` in bare strings)
 *   - §6.1 serializer quoting rules (when a string must be quoted to
 *     parse back as the same string)
 *
 * Allocation policy: zero. Parse returns slices that borrow from the
 * caller's source buffer; serialize writes into a caller-provided
 * fixed buffer and reports the required size on overflow so the
 * caller can grow and retry. Escape-bearing quoted strings need a
 * scratch buffer for JSON-decoding — provided by the parser layer
 * when this module is called from there, not by this module itself.
 */

#ifndef LIBJMD_SCALARS_H
#define LIBJMD_SCALARS_H

#include "libjmd.h"

#include <stddef.h>

/* ---------- Parse direction ---------- */

/*
 * jmd_scalar_parse — classify and decode a raw scalar slice.
 *
 * Input: the bytes after `: ` on a field line, or after `- ` on a
 * bullet line. Caller has already stripped the structural prefix.
 *
 * Output rules:
 *   - empty slice          ->  STRING, ptr=NULL, len=0
 *   - "null"               ->  NULL
 *   - "true" / "false"     ->  BOOL with the obvious value
 *   - parseable as integer ->  INT
 *   - parseable as float   ->  FLOAT
 *   - starts with `"`      ->  STRING; ptr/len point INSIDE the raw
 *                              slice (sans surrounding quotes) iff
 *                              the string carries no backslash; if
 *                              any `\` is present we return
 *                              JMD_ERROR_PARSE so the caller can
 *                              dispatch into its scratch-buffer
 *                              JSON-decode path. The literal `""` is
 *                              fine (empty string, no escapes).
 *   - starts with `# ` or  ->  JMD_ERROR_PARSE (§5a — structural
 *     `- ` unquoted              prefix in an unquoted string is
 *                              ambiguous; spec requires quoting)
 *   - bare `-`             ->  JMD_ERROR_PARSE (ambiguous with a
 *                              bullet line — spec requires `"-"`)
 *   - otherwise            ->  STRING with the slice as-is
 *
 * Returns JMD_OK or JMD_ERROR_PARSE. On JMD_OK the out fields are
 * populated; on JMD_ERROR_PARSE the out struct is left zeroed.
 */
int jmd_scalar_parse(const char *raw, size_t len, jmd_scalar_t *out);

/*
 * jmd_scalar_string_has_escapes — quick scan for a backslash.
 *
 * Used by the parser layer to decide whether the cheap slice path
 * is enough or whether a scratch-buffer JSON-decode is needed.
 * The argument must be the INNER bytes of a quoted string (i.e. the
 * caller has already stripped the surrounding quotes); we just scan
 * for `\\`.
 */
int jmd_scalar_string_has_escapes(const char *raw, size_t len);

/*
 * jmd_scalar_decode_string — JSON-decode a quoted-string body.
 *
 * raw / len are the bytes WITHOUT the surrounding double quotes.
 * The function decodes JSON escape sequences (`\\"`, `\\\\`, `\\n`,
 * `\\t`, `\\u%04x`, etc.) into out_buf. Returns the number of bytes
 * written (regardless of whether they fit in out_buf), so callers
 * can probe with a small buffer to discover the size, then allocate
 * and retry. On malformed escape the function returns SIZE_MAX and
 * out_buf contents are undefined.
 *
 * The decoded form is the canonical UTF-8 byte sequence (no NUL
 * terminator added). UTF-16 surrogate pairs in `\\u` escapes are
 * combined into a single UTF-8 codepoint.
 */
size_t jmd_scalar_decode_string(const char *raw, size_t len,
                                char *out_buf, size_t cap);

/* ---------- Key direction ---------- */

/*
 * jmd_key_parse — recognise a bare or quoted key at the start of a
 * line slice and report its byte range.
 *
 * On success:
 *   - *out_ptr / *out_len = the key text (sans surrounding quotes,
 *     not JSON-decoded — pure slice into raw)
 *   - *out_consumed       = bytes consumed from raw, including the
 *     surrounding quotes if any
 *
 * Returns JMD_OK or JMD_ERROR_PARSE. The function does NOT consume
 * a trailing `:`, `: `, or `[]` — those belong to the field/heading
 * layer above. A quoted key with escapes is reported as a slice;
 * the caller decodes if it cares (the parser layer does, for
 * `repeated_scalar_key` detection across the value's logical key).
 */
int jmd_key_parse(const char *raw, size_t len,
                  const char **out_ptr, size_t *out_len,
                  size_t *out_consumed);

/* ---------- Serialize direction ---------- */

/*
 * jmd_scalar_needs_quote — does this string need surrounding quotes
 * to round-trip parse back as the same string?
 *
 * True for: empty string, reserved words ("null", "true", "false"),
 * anything that would parse as a number, bare `-`, structural
 * prefixes (`# `, `- `), strings starting with `"`, and strings
 * containing newline or tab. Matches jmd-impl/jmd-js behaviour.
 */
int jmd_scalar_needs_quote(const char *s, size_t len);

/*
 * jmd_scalar_serialize — emit canonical text for a scalar value.
 *
 * Writes into out_buf up to cap bytes; returns the total number of
 * bytes the canonical form would need. If the return value exceeds
 * cap, out_buf has been truncated and the caller should grow and
 * retry. This is the same protocol as snprintf, picked deliberately
 * so callers can chain.
 *
 * No trailing NUL is written.
 */
size_t jmd_scalar_serialize(const jmd_scalar_t *v,
                            char *out_buf, size_t cap);

/*
 * jmd_key_serialize — emit canonical text for a key, quoting iff
 * the bare form would not parse back to the same key.
 *
 * Same buffer-protocol as jmd_scalar_serialize. A key passes
 * "bare-eligible" iff it matches /[A-Za-z0-9_-]+/.
 */
size_t jmd_key_serialize(const char *key, size_t len,
                         char *out_buf, size_t cap);

#endif /* LIBJMD_SCALARS_H */
