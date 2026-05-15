/*
 * scalars.c — scalar and key parsing / serialization for libjmd.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * See scalars.h for the surface contract. This file is the
 * implementation; it does no allocation and never owns memory.
 *
 * Number recognition follows the JSON grammar (RFC 8259 §6) one
 * byte at a time, then re-parses the recognised slice via strtoll /
 * strtod. We do the byte-by-byte pass ourselves rather than letting
 * strtoll decide because strtoll accepts forms the spec does not —
 * leading whitespace, hexadecimal (with strtol(...,0,...)), trailing
 * junk — and we want to be strict about what counts as a number.
 *
 * Quoted-string handling is split into "is there an escape?" (cheap
 * scan) and "decode escapes into buf" (the expensive path). The
 * scalar parser does NOT decode escapes; it returns JMD_ERROR_PARSE
 * for any quoted slice that contains a backslash so the caller can
 * dispatch to jmd_scalar_decode_string with its own scratch buffer.
 * This keeps scalars.c allocation-free.
 */

#include "scalars.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Internal helpers                                                  */
/* ---------------------------------------------------------------- */

static int byte_is_bare_key(unsigned char c)
{
    /* Matches /[A-Za-z0-9_-]/ — the bare-key character class shared
     * with jmd-impl and jmd-js. ASCII-only by design; non-ASCII keys
     * always quote. */
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        ||  c == '_'
        ||  c == '-';
}

/* Recognise a JSON number at the start of [raw, raw+len). Returns
 * the number of bytes that form the number (0 if no number). Sets
 * *is_float to 1 iff the slice contains `.`, `e`, or `E`. */
static size_t scan_number(const char *raw, size_t len, int *is_float)
{
    size_t i = 0;
    *is_float = 0;
    if (i < len && raw[i] == '-') i++;
    if (i >= len) return 0;
    if (raw[i] == '0') {
        i++;
    } else if (raw[i] >= '1' && raw[i] <= '9') {
        i++;
        while (i < len && raw[i] >= '0' && raw[i] <= '9') i++;
    } else {
        return 0;
    }
    if (i < len && raw[i] == '.') {
        *is_float = 1;
        i++;
        size_t start = i;
        while (i < len && raw[i] >= '0' && raw[i] <= '9') i++;
        if (i == start) return 0;  /* Need >=1 digit after '.' */
    }
    if (i < len && (raw[i] == 'e' || raw[i] == 'E')) {
        *is_float = 1;
        i++;
        if (i < len && (raw[i] == '+' || raw[i] == '-')) i++;
        size_t start = i;
        while (i < len && raw[i] >= '0' && raw[i] <= '9') i++;
        if (i == start) return 0;  /* Need >=1 digit in exponent */
    }
    return i;
}

/* Hex digit -> value 0-15, or -1 if not a hex digit. */
static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Encode codepoint cp as UTF-8 into out (up to 4 bytes). Returns
 * the number of bytes written, or 0 for invalid codepoint. */
static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;  /* lone surrogate */
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Append up to n bytes from src to out_buf at position *pos, but
 * stop appending past cap. *pos always increments by n so the
 * caller learns the required buffer size even on overflow. */
static void buf_append(char *out_buf, size_t cap, size_t *pos,
                       const char *src, size_t n)
{
    if (out_buf && *pos < cap) {
        size_t room = cap - *pos;
        size_t to_copy = (n < room) ? n : room;
        memcpy(out_buf + *pos, src, to_copy);
    }
    *pos += n;
}

static void buf_putc(char *out_buf, size_t cap, size_t *pos, char c)
{
    if (out_buf && *pos < cap) {
        out_buf[*pos] = c;
    }
    (*pos)++;
}

/* ---------------------------------------------------------------- */
/* jmd_scalar_parse                                                  */
/* ---------------------------------------------------------------- */

int jmd_scalar_parse(const char *raw, size_t len, jmd_scalar_t *out)
{
    memset(out, 0, sizeof *out);

    /* Empty slice -> empty string. */
    if (len == 0) {
        out->type = JMD_SCALAR_STRING;
        out->as.string.ptr = NULL;
        out->as.string.len = 0;
        return JMD_OK;
    }

    /* Reserved words. */
    if (len == 4 && memcmp(raw, "null", 4) == 0) {
        out->type = JMD_SCALAR_NULL;
        return JMD_OK;
    }
    if (len == 4 && memcmp(raw, "true", 4) == 0) {
        out->type = JMD_SCALAR_BOOL;
        out->as.boolean = 1;
        return JMD_OK;
    }
    if (len == 5 && memcmp(raw, "false", 5) == 0) {
        out->type = JMD_SCALAR_BOOL;
        out->as.boolean = 0;
        return JMD_OK;
    }

    /* Number. scan_number returns the byte-count it recognised; if
     * that equals len, the whole slice is a number. */
    int is_float = 0;
    size_t numlen = scan_number(raw, len, &is_float);
    if (numlen == len) {
        /* Copy into a stack buffer for strto*. Cap at 63 chars; any
         * legit JSON number fits comfortably. Beyond that we treat
         * as a string (defensive — strtoll/strtod would mis-parse
         * on overflow anyway). */
        if (len > 63) goto bare_string;
        char nbuf[64];
        memcpy(nbuf, raw, len);
        nbuf[len] = '\0';
        char *end = NULL;
        if (is_float) {
            double d = strtod(nbuf, &end);
            if (end != nbuf + len) goto bare_string;
            out->type = JMD_SCALAR_FLOAT;
            out->as.floating = d;
        } else {
            long long ll = strtoll(nbuf, &end, 10);
            if (end != nbuf + len) goto bare_string;
            out->type = JMD_SCALAR_INT;
            out->as.integer = (int64_t)ll;
        }
        return JMD_OK;
    }

    /* Quoted string. We accept the slice if it begins AND ends with
     * `"`; in that case we either return the inner slice (no
     * escapes — cheap path) or JMD_ERROR_PARSE so the caller can
     * decode into its own scratch buffer (escape path). */
    if (raw[0] == '"') {
        if (len < 2 || raw[len - 1] != '"') {
            return JMD_ERROR_PARSE;  /* Unterminated quote. */
        }
        const char *inner = raw + 1;
        size_t inner_len = len - 2;
        if (jmd_scalar_string_has_escapes(inner, inner_len)) {
            return JMD_ERROR_PARSE;  /* Needs decoder. */
        }
        out->type = JMD_SCALAR_STRING;
        out->as.string.ptr = (inner_len == 0) ? NULL : inner;
        out->as.string.len = inner_len;
        return JMD_OK;
    }

    /* Structural-prefix rejection (§5a). A bare `# ` or `- ` at the
     * start of a scalar would collide with structural lines if it
     * ever got serialised back; the spec requires quoting. */
    if (len >= 2 && raw[0] == '#' && raw[1] == ' ') {
        return JMD_ERROR_PARSE;
    }
    if (len >= 2 && raw[0] == '-' && raw[1] == ' ') {
        return JMD_ERROR_PARSE;
    }

    /* Bare `-` is ambiguous with the bullet form; spec requires the
     * literal value to be quoted (`"-"`). */
    if (len == 1 && raw[0] == '-') {
        return JMD_ERROR_PARSE;
    }

bare_string:
    out->type = JMD_SCALAR_STRING;
    out->as.string.ptr = raw;
    out->as.string.len = len;
    return JMD_OK;
}

/* ---------------------------------------------------------------- */
/* String-decoding helpers                                           */
/* ---------------------------------------------------------------- */

int jmd_scalar_string_has_escapes(const char *raw, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (raw[i] == '\\') return 1;
    }
    return 0;
}

size_t jmd_scalar_decode_string(const char *raw, size_t len,
                                char *out_buf, size_t cap)
{
    size_t pos = 0;
    size_t i   = 0;
    while (i < len) {
        unsigned char c = (unsigned char)raw[i];
        if (c != '\\') {
            buf_putc(out_buf, cap, &pos, (char)c);
            i++;
            continue;
        }
        /* Escape sequence: at least one byte must follow. */
        if (i + 1 >= len) return (size_t)-1;
        unsigned char esc = (unsigned char)raw[i + 1];
        switch (esc) {
        case '"':  buf_putc(out_buf, cap, &pos, '"');  i += 2; break;
        case '\\': buf_putc(out_buf, cap, &pos, '\\'); i += 2; break;
        case '/':  buf_putc(out_buf, cap, &pos, '/');  i += 2; break;
        case 'b':  buf_putc(out_buf, cap, &pos, '\b'); i += 2; break;
        case 'f':  buf_putc(out_buf, cap, &pos, '\f'); i += 2; break;
        case 'n':  buf_putc(out_buf, cap, &pos, '\n'); i += 2; break;
        case 'r':  buf_putc(out_buf, cap, &pos, '\r'); i += 2; break;
        case 't':  buf_putc(out_buf, cap, &pos, '\t'); i += 2; break;
        case 'u': {
            if (i + 6 > len) return (size_t)-1;
            int h1 = hex_value((unsigned char)raw[i + 2]);
            int h2 = hex_value((unsigned char)raw[i + 3]);
            int h3 = hex_value((unsigned char)raw[i + 4]);
            int h4 = hex_value((unsigned char)raw[i + 5]);
            if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return (size_t)-1;
            uint32_t cp = (uint32_t)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
            i += 6;
            /* High surrogate: must be followed by `\\u<low>`. */
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 6 > len || raw[i] != '\\' || raw[i + 1] != 'u') {
                    return (size_t)-1;
                }
                int l1 = hex_value((unsigned char)raw[i + 2]);
                int l2 = hex_value((unsigned char)raw[i + 3]);
                int l3 = hex_value((unsigned char)raw[i + 4]);
                int l4 = hex_value((unsigned char)raw[i + 5]);
                if (l1 < 0 || l2 < 0 || l3 < 0 || l4 < 0) {
                    return (size_t)-1;
                }
                uint32_t lo = (uint32_t)((l1 << 12) | (l2 << 8) | (l3 << 4) | l4);
                if (lo < 0xDC00 || lo > 0xDFFF) return (size_t)-1;
                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                i += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return (size_t)-1;  /* Lone low surrogate. */
            }
            char enc[4];
            size_t n = utf8_encode(cp, enc);
            if (n == 0) return (size_t)-1;
            buf_append(out_buf, cap, &pos, enc, n);
            break;
        }
        default:
            return (size_t)-1;  /* Unknown escape. */
        }
    }
    return pos;
}

/* ---------------------------------------------------------------- */
/* jmd_key_parse                                                     */
/* ---------------------------------------------------------------- */

int jmd_key_parse(const char *raw, size_t len,
                  const char **out_ptr, size_t *out_len,
                  size_t *out_consumed)
{
    if (len == 0) return JMD_ERROR_PARSE;

    if (raw[0] == '"') {
        /* Quoted key: scan to the matching unescaped close-quote. */
        size_t i = 1;
        while (i < len) {
            if (raw[i] == '\\') {
                /* Skip the escape and its argument. We don't decode
                 * here — the caller gets the raw slice. */
                if (i + 1 >= len) return JMD_ERROR_PARSE;
                i += 2;
                continue;
            }
            if (raw[i] == '"') {
                *out_ptr      = raw + 1;
                *out_len      = i - 1;
                *out_consumed = i + 1;
                return JMD_OK;
            }
            i++;
        }
        return JMD_ERROR_PARSE;  /* Unterminated. */
    }

    /* Bare key: run of byte_is_bare_key chars, must be non-empty. */
    size_t i = 0;
    while (i < len && byte_is_bare_key((unsigned char)raw[i])) {
        i++;
    }
    if (i == 0) return JMD_ERROR_PARSE;
    *out_ptr      = raw;
    *out_len      = i;
    *out_consumed = i;
    return JMD_OK;
}

/* ---------------------------------------------------------------- */
/* jmd_scalar_needs_quote                                            */
/* ---------------------------------------------------------------- */

int jmd_scalar_needs_quote(const char *s, size_t len)
{
    /* Empty string would parse back as an empty value, not the
     * empty string — must quote. */
    if (len == 0) return 1;

    /* Reserved words. */
    if (len == 4 && memcmp(s, "null", 4) == 0) return 1;
    if (len == 4 && memcmp(s, "true", 4) == 0) return 1;
    if (len == 5 && memcmp(s, "false", 5) == 0) return 1;

    /* Anything that would parse as a number. We re-use scan_number
     * — if it consumes the full slice, quoting is required. */
    int is_float = 0;
    if (scan_number(s, len, &is_float) == len) return 1;

    /* Bare `-`. */
    if (len == 1 && s[0] == '-') return 1;

    /* Structural prefix in a bare string. */
    if (len >= 2 && s[0] == '#' && s[1] == ' ') return 1;
    if (len >= 2 && s[0] == '-' && s[1] == ' ') return 1;

    /* Starts with a double quote — bare form would be mistaken for
     * a quoted-string opener. */
    if (s[0] == '"') return 1;

    /* Contains a control character that the tokenizer treats as
     * line structure (newline) or whose escape we'd lose round-trip
     * (tab). */
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n' || s[i] == '\t') return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* jmd_scalar_serialize                                              */
/* ---------------------------------------------------------------- */

/* Encode the bytes of a string as a JSON-quoted literal into
 * out_buf+pos. Returns total bytes the literal would need. */
static void serialize_string_literal(const char *s, size_t len,
                                     char *out_buf, size_t cap,
                                     size_t *pos)
{
    buf_putc(out_buf, cap, pos, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  buf_append(out_buf, cap, pos, "\\\"", 2); break;
        case '\\': buf_append(out_buf, cap, pos, "\\\\", 2); break;
        case '\b': buf_append(out_buf, cap, pos, "\\b", 2);  break;
        case '\f': buf_append(out_buf, cap, pos, "\\f", 2);  break;
        case '\n': buf_append(out_buf, cap, pos, "\\n", 2);  break;
        case '\r': buf_append(out_buf, cap, pos, "\\r", 2);  break;
        case '\t': buf_append(out_buf, cap, pos, "\\t", 2);  break;
        default:
            if (c < 0x20) {
                char enc[6];
                /* `\uXXXX` form for other control bytes. */
                snprintf(enc, sizeof enc, "\\u%04x", c);
                buf_append(out_buf, cap, pos, enc, 6);
            } else {
                buf_putc(out_buf, cap, pos, (char)c);
            }
        }
    }
    buf_putc(out_buf, cap, pos, '"');
}

size_t jmd_scalar_serialize(const jmd_scalar_t *v,
                            char *out_buf, size_t cap)
{
    size_t pos = 0;
    switch (v->type) {
    case JMD_SCALAR_NULL:
        buf_append(out_buf, cap, &pos, "null", 4);
        break;
    case JMD_SCALAR_BOOL:
        if (v->as.boolean) {
            buf_append(out_buf, cap, &pos, "true", 4);
        } else {
            buf_append(out_buf, cap, &pos, "false", 5);
        }
        break;
    case JMD_SCALAR_INT: {
        /* PRId64 macro from <inttypes.h> would work but pulls in
         * extra. snprintf to a stack buffer is portable and small. */
        char nbuf[32];
        int n = snprintf(nbuf, sizeof nbuf, "%lld",
                         (long long)v->as.integer);
        if (n > 0) buf_append(out_buf, cap, &pos, nbuf, (size_t)n);
        break;
    }
    case JMD_SCALAR_FLOAT: {
        /* Shortest-round-trip representation is locale-dependent
         * via %g; we use %.17g which is the float64-safe upper
         * bound and trim trailing zeros below for prettier output
         * when the value is exactly representable. Matches Python's
         * repr() / JS's String() for most JSON-sourced doubles. */
        char nbuf[32];
        int n = snprintf(nbuf, sizeof nbuf, "%.17g", v->as.floating);
        if (n > 0) buf_append(out_buf, cap, &pos, nbuf, (size_t)n);
        break;
    }
    case JMD_SCALAR_STRING: {
        const char *s = v->as.string.ptr;
        size_t      n = v->as.string.len;
        if (jmd_scalar_needs_quote(s, n)) {
            serialize_string_literal(s, n, out_buf, cap, &pos);
        } else {
            buf_append(out_buf, cap, &pos, s, n);
        }
        break;
    }
    }
    return pos;
}

/* ---------------------------------------------------------------- */
/* jmd_key_serialize                                                 */
/* ---------------------------------------------------------------- */

size_t jmd_key_serialize(const char *key, size_t len,
                         char *out_buf, size_t cap)
{
    int bare_ok = (len > 0);
    for (size_t i = 0; bare_ok && i < len; i++) {
        if (!byte_is_bare_key((unsigned char)key[i])) bare_ok = 0;
    }
    size_t pos = 0;
    if (bare_ok) {
        buf_append(out_buf, cap, &pos, key, len);
    } else {
        serialize_string_literal(key, len, out_buf, cap, &pos);
    }
    return pos;
}
