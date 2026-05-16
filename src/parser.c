/*
 * parser.c — Visitor parse entry points.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * M1 slice 4b: object + array body parse. Handles
 *
 *   - root heading classification (all 4 modes, label, root-array
 *     postfix `[]` for any mode)
 *   - frontmatter pre-pass via frontmatter.c
 *   - nested object scopes opened by `## key`, `### key`, ...
 *   - array scopes opened by `## key[]`, `### key[]`, ...
 *   - scalar field lines at column 0 (`key: value`)
 *   - scalar heading form (`## key: value`) — assigns to parent
 *     object, does NOT open a sub-scope
 *   - empty-value blockquote field (`key:` + `> ...` lines), both
 *     bare and heading positions
 *   - bullet lines `- value` (scalar item via on_item_value) and
 *     `- key: value` (dict item start; on_item_start + first field)
 *   - 2-space indented continuation `  key: value` adding scalar
 *     fields to the current dict item
 *   - thematic break `---` between dict-with-nested items (note:
 *     dict items with NESTED sub-objects are deferred to slice 4c;
 *     slice 4b items only carry scalar fields)
 *   - scope return on shallower heading (pops + emits object_end /
 *     array_end + item_end as appropriate)
 *
 * Not yet (slice 4c+):
 *   - depth-qualified item form `##N -` (tolerance fixtures)
 *   - depth+1 item form `###N -` under a depth-N array heading
 *   - sub-objects inside array items (`- sku: A1\n### nested\n...`)
 *   - heterogeneous arrays mixing scalar / dict / sub-array items
 *   - §7.4 repeated-heading promotion + structured errors
 *
 * Memory: a fixed-size internal scratch buffer (2 KiB) backs both
 * the frontmatter pre-pass and any escape-decoded / multi-line
 * accumulated strings the body emits. The buffer is single-use per
 * field — emitted slices in callbacks remain valid only for the
 * callback's duration, matching the libjmd.h surface contract.
 * jmd_parse_ex with a non-NULL allocator currently delegates to
 * jmd_parse (allocator hook lands once the buffer outgrows the
 * fixed stack form).
 */

#include "libjmd.h"

#include "frontmatter.h"
#include "scalars.h"
#include "tokenizer.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Internal types                                                    */
/* ---------------------------------------------------------------- */

#define JMD_PARSER_MAX_DEPTH   64
#define JMD_PARSER_SCRATCH_CAP 2048

typedef struct {
    /* The heading depth that opened this scope. Root = 1. */
    int depth;
    /* 0 = object, 1 = array. */
    int is_array;
    /* Array scopes only: 1 iff a dict item is currently open
     * (item_start emitted, item_end pending). Scalar items leave
     * this 0 because they wrap as item_value with no end-event. */
    int item_active;
} parser_scope_t;

typedef struct {
    jmd_tokenizer_t      *tk;
    const jmd_visitor_t  *vis;
    void                 *vctx;
    char                 *scratch;
    size_t                scratch_cap;
    parser_scope_t        stack[JMD_PARSER_MAX_DEPTH];
    int                   top;  /* -1 when no scopes are open */
} parser_t;

/* ---------------------------------------------------------------- */
/* Event emission                                                    */
/* ---------------------------------------------------------------- */

/* All emit helpers return the visitor's return code (or JMD_OK if
 * the callback was NULL). The caller propagates non-zero unchanged
 * — that's the abort contract documented on jmd_visitor_t. */

static int emit_doc_start(parser_t *p, jmd_mode_t mode,
                          const char *label, size_t label_len)
{
    if (!p->vis || !p->vis->on_document_start) return JMD_OK;
    return p->vis->on_document_start(p->vctx, mode, label, label_len);
}

static int emit_doc_end(parser_t *p)
{
    if (!p->vis || !p->vis->on_document_end) return JMD_OK;
    return p->vis->on_document_end(p->vctx);
}

static int emit_object_start(parser_t *p, const char *key, size_t key_len)
{
    if (!p->vis || !p->vis->on_object_start) return JMD_OK;
    return p->vis->on_object_start(p->vctx, key, key_len);
}

static int emit_object_end(parser_t *p)
{
    if (!p->vis || !p->vis->on_object_end) return JMD_OK;
    return p->vis->on_object_end(p->vctx);
}

static int emit_array_start(parser_t *p, const char *key, size_t key_len)
{
    if (!p->vis || !p->vis->on_array_start) return JMD_OK;
    return p->vis->on_array_start(p->vctx, key, key_len);
}

static int emit_array_end(parser_t *p)
{
    if (!p->vis || !p->vis->on_array_end) return JMD_OK;
    return p->vis->on_array_end(p->vctx);
}

static int emit_item_start(parser_t *p)
{
    if (!p->vis || !p->vis->on_item_start) return JMD_OK;
    return p->vis->on_item_start(p->vctx);
}

static int emit_item_end(parser_t *p)
{
    if (!p->vis || !p->vis->on_item_end) return JMD_OK;
    return p->vis->on_item_end(p->vctx);
}

static int emit_item_value(parser_t *p, const jmd_scalar_t *v)
{
    if (!p->vis || !p->vis->on_item_value) return JMD_OK;
    return p->vis->on_item_value(p->vctx, v);
}

static int emit_field(parser_t *p,
                      const char *key, size_t key_len,
                      const jmd_scalar_t *value)
{
    if (!p->vis || !p->vis->on_field) return JMD_OK;
    return p->vis->on_field(p->vctx, key, key_len, value);
}

static int emit_ml_start(parser_t *p, const char *key, size_t key_len)
{
    if (!p->vis || !p->vis->on_multiline_field_start) return JMD_OK;
    return p->vis->on_multiline_field_start(p->vctx, key, key_len);
}

static int emit_ml_content(parser_t *p, const char *content, size_t len,
                           int is_paragraph_break)
{
    if (!p->vis || !p->vis->on_multiline_content) return JMD_OK;
    return p->vis->on_multiline_content(p->vctx, content, len,
                                        is_paragraph_break);
}

static int emit_ml_end(parser_t *p)
{
    if (!p->vis || !p->vis->on_multiline_field_end) return JMD_OK;
    return p->vis->on_multiline_field_end(p->vctx);
}

static int emit_error(parser_t *p, int line, const char *msg)
{
    if (!p->vis || !p->vis->on_parse_error) return JMD_OK;
    jmd_error_t err = { line, 1, msg };
    return p->vis->on_parse_error(p->vctx, &err);
}

/* ---------------------------------------------------------------- */
/* Scope stack                                                       */
/* ---------------------------------------------------------------- */

/* Close any in-flight dict item on an array scope, emitting
 * item_end. No-op for object scopes or for arrays with no item
 * currently active. */
static int close_current_item(parser_t *p, parser_scope_t *s)
{
    if (!s->is_array || !s->item_active) return JMD_OK;
    int rc = emit_item_end(p);
    if (rc) return rc;
    s->item_active = 0;
    return JMD_OK;
}

/* Pop scopes whose depth is >= target_depth, emitting the right
 * end event for each: array_end for array scopes (with item_end
 * first if a dict item was still open), object_end otherwise.
 * After this call, either the stack is empty or the top scope's
 * depth is strictly less than target_depth. */
static int pop_to(parser_t *p, int target_depth)
{
    while (p->top >= 0 && p->stack[p->top].depth >= target_depth) {
        parser_scope_t *s = &p->stack[p->top];
        if (s->is_array) {
            int rc = close_current_item(p, s);
            if (rc) return rc;
            rc = emit_array_end(p);
            if (rc) return rc;
        } else {
            int rc = emit_object_end(p);
            if (rc) return rc;
        }
        p->top--;
    }
    return JMD_OK;
}

/* Push an object scope at the given heading depth. */
static int push_object(parser_t *p, int depth,
                       const char *key, size_t key_len)
{
    if (p->top + 1 >= JMD_PARSER_MAX_DEPTH) {
        return JMD_ERROR_PARSE;  /* Too deep — pathological input. */
    }
    int rc = emit_object_start(p, key, key_len);
    if (rc) return rc;
    p->top++;
    p->stack[p->top].depth       = depth;
    p->stack[p->top].is_array    = 0;
    p->stack[p->top].item_active = 0;
    return JMD_OK;
}

/* Push an array scope at the given heading depth. */
static int push_array(parser_t *p, int depth,
                      const char *key, size_t key_len)
{
    if (p->top + 1 >= JMD_PARSER_MAX_DEPTH) {
        return JMD_ERROR_PARSE;
    }
    int rc = emit_array_start(p, key, key_len);
    if (rc) return rc;
    p->top++;
    p->stack[p->top].depth       = depth;
    p->stack[p->top].is_array    = 1;
    p->stack[p->top].item_active = 0;
    return JMD_OK;
}

/* ---------------------------------------------------------------- */
/* Mode marker on the root heading                                   */
/* ---------------------------------------------------------------- */

/* The tokenizer surfaces `#! Label` / `#? Label` / `#- Label` as a
 * depth-1 heading whose content starts with the mode mark + space.
 * Strip the mark here and return the residual label slice + mode.
 * For plain `# Label`, mode == JMD_MODE_DATA and the input slice is
 * the label as-is. */
static void classify_root_mode(const char *content, size_t len,
                               jmd_mode_t *out_mode,
                               const char **out_label,
                               size_t *out_label_len)
{
    if (len >= 2 && content[1] == ' ') {
        switch (content[0]) {
        case '!': *out_mode = JMD_MODE_SCHEMA;
                  *out_label = content + 2;
                  *out_label_len = len - 2;
                  return;
        case '?': *out_mode = JMD_MODE_QUERY;
                  *out_label = content + 2;
                  *out_label_len = len - 2;
                  return;
        case '-': *out_mode = JMD_MODE_DELETE;
                  *out_label = content + 2;
                  *out_label_len = len - 2;
                  return;
        default:  break;
        }
    }
    *out_mode      = JMD_MODE_DATA;
    *out_label     = content;
    *out_label_len = len;
}

/* ---------------------------------------------------------------- */
/* Body-line dispatch                                                */
/* ---------------------------------------------------------------- */

/* Collect a body-blockquote into the streaming visitor. The cursor
 * is positioned at the first `> ...` or `>` line on entry. Returns
 * JMD_OK on success, the visitor's return code on abort, or
 * JMD_ERROR_PARSE on a malformed continuation line. */
static int collect_body_blockquote(parser_t *p,
                                   const char *key, size_t key_len)
{
    int rc = emit_ml_start(p, key, key_len);
    if (rc) return rc;
    for (;;) {
        jmd_line_t peek;
        if (!jmd_tokenizer_peek(p->tk, &peek)) break;
        if (peek.heading_depth != 0) break;
        if (peek.raw_len == 0) break;
        if (peek.raw[0] != '>') break;
        if (peek.raw_len != 1 && !(peek.raw_len >= 2 && peek.raw[1] == ' ')) {
            break;
        }
        jmd_line_t line;
        jmd_tokenizer_next(p->tk, &line);
        if (line.raw_len == 1) {
            /* Bare `>` — paragraph break / empty line in the value. */
            rc = emit_ml_content(p, NULL, 0, 1);
        } else {
            rc = emit_ml_content(p, line.raw + 2, line.raw_len - 2, 0);
        }
        if (rc) return rc;
    }
    return emit_ml_end(p);
}

/* Bare body line at column 0: must be a `key: value` field on the
 * current object scope (slice 4a). */
static int handle_bare_field(parser_t *p, const jmd_line_t *line)
{
    if (p->top < 0 || p->stack[p->top].is_array) {
        emit_error(p, line->line_no,
                   "field line outside an object scope");
        return JMD_ERROR_PARSE;
    }
    /* Peel the key. */
    const char *key_ptr;
    size_t      key_len;
    size_t      consumed;
    if (jmd_key_parse(line->content, line->content_len,
                      &key_ptr, &key_len, &consumed) != JMD_OK) {
        emit_error(p, line->line_no, "malformed key");
        return JMD_ERROR_PARSE;
    }
    if (consumed >= line->content_len || line->content[consumed] != ':') {
        emit_error(p, line->line_no, "field line missing ':'");
        return JMD_ERROR_PARSE;
    }
    /* Two cases: `key:` (empty value, possibly blockquote) or
     * `key: value` (scalar). */
    if (consumed + 1 == line->content_len) {
        /* Empty after `:` — check for blockquote continuation. */
        jmd_line_t peek;
        if (jmd_tokenizer_peek(p->tk, &peek)
                && peek.heading_depth == 0
                && peek.raw_len > 0
                && peek.raw[0] == '>'
                && (peek.raw_len == 1 || peek.raw[1] == ' ')) {
            return collect_body_blockquote(p, key_ptr, key_len);
        }
        /* No continuation: empty-string field. */
        jmd_scalar_t v = {0};
        v.type = JMD_SCALAR_STRING;
        v.as.string.ptr = NULL;
        v.as.string.len = 0;
        return emit_field(p, key_ptr, key_len, &v);
    }
    if (line->content[consumed + 1] != ' ') {
        emit_error(p, line->line_no, "field needs ': ' separator");
        return JMD_ERROR_PARSE;
    }
    const char *val_raw = line->content + consumed + 2;
    size_t      val_len = line->content_len - consumed - 2;
    jmd_scalar_t v;
    int rc = jmd_scalar_parse(val_raw, val_len, &v);
    if (rc == JMD_OK) {
        return emit_field(p, key_ptr, key_len, &v);
    }
    if (rc != JMD_ERROR_PARSE) return rc;
    /* JMD_ERROR_PARSE => either a real failure or a quoted string
     * with escapes. Differentiate by re-checking the slice. */
    if (val_len < 2 || val_raw[0] != '"' || val_raw[val_len - 1] != '"') {
        emit_error(p, line->line_no, "malformed scalar value");
        return JMD_ERROR_PARSE;
    }
    size_t dec = jmd_scalar_decode_string(val_raw + 1, val_len - 2,
                                          p->scratch, p->scratch_cap);
    if (dec == (size_t)-1) {
        emit_error(p, line->line_no, "bad JSON escape in string");
        return JMD_ERROR_PARSE;
    }
    if (dec > p->scratch_cap) return JMD_ERROR_MEMORY;
    v.type = JMD_SCALAR_STRING;
    v.as.string.ptr = (dec == 0) ? NULL : p->scratch;
    v.as.string.len = dec;
    return emit_field(p, key_ptr, key_len, &v);
}

/* Body heading line at depth >= 2: dispatch into scope-open or
 * scalar-field-on-parent. */
static int handle_body_heading(parser_t *p, const jmd_line_t *line)
{
    /* Array-shaped heading: `key[]` opens an array scope under the
     * parent object; anonymous `[]` on its own (sub-array inside an
     * array scope) is deferred to slice 4c. */
    if (line->content_len >= 2
            && line->content[line->content_len - 2] == '['
            && line->content[line->content_len - 1] == ']') {
        if (line->content_len == 2) {
            emit_error(p, line->line_no,
                       "anonymous sub-array headings not yet implemented");
            return JMD_ERROR_PARSE;
        }
        size_t key_text_len = line->content_len - 2;
        const char *kp;
        size_t kl, kc;
        if (jmd_key_parse(line->content, key_text_len,
                          &kp, &kl, &kc) != JMD_OK
                || kc != key_text_len) {
            emit_error(p, line->line_no, "malformed array heading key");
            return JMD_ERROR_PARSE;
        }
        int rc = pop_to(p, line->heading_depth);
        if (rc) return rc;
        return push_array(p, line->heading_depth, kp, kl);
    }
    if (line->content_len == 0) {
        /* Anonymous object heading — open a nameless scope. */
        int rc = pop_to(p, line->heading_depth);
        if (rc) return rc;
        return push_object(p, line->heading_depth, NULL, 0);
    }

    /* Peel a key. */
    const char *key_ptr;
    size_t      key_len;
    size_t      consumed;
    if (jmd_key_parse(line->content, line->content_len,
                      &key_ptr, &key_len, &consumed) != JMD_OK) {
        emit_error(p, line->line_no, "malformed heading key");
        return JMD_ERROR_PARSE;
    }

    /* Three shapes after the key:
     *   a) end of line               -> open object scope
     *   b) `:` end of line           -> empty / blockquote field on parent
     *   c) `: value`                 -> scalar field on parent
     */
    if (consumed == line->content_len) {
        /* (a) Open object scope at this depth. Close anything at
         * the same or deeper depth first. */
        int rc = pop_to(p, line->heading_depth);
        if (rc) return rc;
        return push_object(p, line->heading_depth, key_ptr, key_len);
    }
    if (line->content[consumed] != ':') {
        emit_error(p, line->line_no, "malformed heading");
        return JMD_ERROR_PARSE;
    }

    /* (b)/(c) — scalar heading: assigns to the parent scope, does
     * NOT open a sub-scope. The parent is whatever scope sits at
     * depth < heading_depth (close intermediates). */
    int rc = pop_to(p, line->heading_depth);
    if (rc) return rc;
    if (p->top < 0) {
        emit_error(p, line->line_no, "scalar heading without parent");
        return JMD_ERROR_PARSE;
    }

    if (consumed + 1 == line->content_len) {
        /* `## key:` — peek for blockquote. */
        jmd_line_t peek;
        if (jmd_tokenizer_peek(p->tk, &peek)
                && peek.heading_depth == 0
                && peek.raw_len > 0
                && peek.raw[0] == '>'
                && (peek.raw_len == 1 || peek.raw[1] == ' ')) {
            return collect_body_blockquote(p, key_ptr, key_len);
        }
        jmd_scalar_t v = {0};
        v.type = JMD_SCALAR_STRING;
        v.as.string.ptr = NULL;
        v.as.string.len = 0;
        return emit_field(p, key_ptr, key_len, &v);
    }
    if (line->content[consumed + 1] != ' ') {
        emit_error(p, line->line_no, "heading field needs ': '");
        return JMD_ERROR_PARSE;
    }
    const char *val_raw = line->content + consumed + 2;
    size_t      val_len = line->content_len - consumed - 2;
    jmd_scalar_t v;
    int prc = jmd_scalar_parse(val_raw, val_len, &v);
    if (prc == JMD_OK) {
        return emit_field(p, key_ptr, key_len, &v);
    }
    if (prc != JMD_ERROR_PARSE) return prc;
    if (val_len < 2 || val_raw[0] != '"' || val_raw[val_len - 1] != '"') {
        emit_error(p, line->line_no, "malformed scalar value");
        return JMD_ERROR_PARSE;
    }
    size_t dec = jmd_scalar_decode_string(val_raw + 1, val_len - 2,
                                          p->scratch, p->scratch_cap);
    if (dec == (size_t)-1) {
        emit_error(p, line->line_no, "bad JSON escape in string");
        return JMD_ERROR_PARSE;
    }
    if (dec > p->scratch_cap) return JMD_ERROR_MEMORY;
    v.type = JMD_SCALAR_STRING;
    v.as.string.ptr = (dec == 0) ? NULL : p->scratch;
    v.as.string.len = dec;
    return emit_field(p, key_ptr, key_len, &v);
}

/* ---------------------------------------------------------------- */
/* Top-level body loop                                               */
/* ---------------------------------------------------------------- */

/* Test whether a line is a thematic break (`---` or more, no other
 * content). The tokenizer delivers it as a depth=0 body line. */
static int is_thematic_break(const jmd_line_t *line)
{
    if (line->heading_depth != 0) return 0;
    if (line->raw_len < 3) return 0;
    for (size_t i = 0; i < line->raw_len; i++) {
        if (line->raw[i] != '-') return 0;
    }
    return 1;
}

/* Common helper: emit a scalar field with the given key+value into
 * the top scope, picking the right target (object container vs
 * dict-item under an array). On success returns the visitor's
 * return code; on a scalar-parse failure, returns JMD_ERROR_PARSE
 * after emitting an on_parse_error event. */
static int emit_scalar_field(parser_t *p, int line_no,
                             const char *key, size_t key_len,
                             const char *val_raw, size_t val_len)
{
    jmd_scalar_t v;
    int rc = jmd_scalar_parse(val_raw, val_len, &v);
    if (rc == JMD_OK) {
        return emit_field(p, key, key_len, &v);
    }
    if (rc != JMD_ERROR_PARSE) return rc;
    /* Could be a quoted-string with JSON escapes. */
    if (val_len < 2 || val_raw[0] != '"' || val_raw[val_len - 1] != '"') {
        emit_error(p, line_no, "malformed scalar value");
        return JMD_ERROR_PARSE;
    }
    size_t dec = jmd_scalar_decode_string(val_raw + 1, val_len - 2,
                                          p->scratch, p->scratch_cap);
    if (dec == (size_t)-1) {
        emit_error(p, line_no, "bad JSON escape in string");
        return JMD_ERROR_PARSE;
    }
    if (dec > p->scratch_cap) return JMD_ERROR_MEMORY;
    v.type = JMD_SCALAR_STRING;
    v.as.string.ptr = (dec == 0) ? NULL : p->scratch;
    v.as.string.len = dec;
    return emit_field(p, key, key_len, &v);
}

/* Parse a `key: value` field text and emit it (no blockquote
 * handling — caller deals with empty-value continuation). Used by
 * bullet items + indented continuations. */
static int parse_and_emit_field(parser_t *p, int line_no,
                                const char *text, size_t len)
{
    const char *kp;
    size_t kl, kc;
    if (jmd_key_parse(text, len, &kp, &kl, &kc) != JMD_OK) {
        emit_error(p, line_no, "malformed key");
        return JMD_ERROR_PARSE;
    }
    if (kc + 2 > len
            || text[kc] != ':'
            || text[kc + 1] != ' ') {
        emit_error(p, line_no, "field needs ': ' separator");
        return JMD_ERROR_PARSE;
    }
    return emit_scalar_field(p, line_no, kp, kl,
                             text + kc + 2, len - kc - 2);
}

/* Handle a bullet line `- value` or `- key: value` at column 0.
 * Top scope must be an array (or its current item). */
static int handle_bullet(parser_t *p, const jmd_line_t *line)
{
    if (p->top < 0 || !p->stack[p->top].is_array) {
        emit_error(p, line->line_no, "bullet outside array scope");
        return JMD_ERROR_PARSE;
    }
    parser_scope_t *arr = &p->stack[p->top];

    /* `- ` or just `-`. */
    const char *rest;
    size_t rest_len;
    if (line->raw_len == 1) {
        /* Bare `-` — opens an empty dict item. */
        rest = NULL;
        rest_len = 0;
    } else if (line->raw_len >= 2 && line->raw[1] == ' ') {
        rest = line->raw + 2;
        rest_len = line->raw_len - 2;
    } else {
        emit_error(p, line->line_no, "bullet needs '- ' separator");
        return JMD_ERROR_PARSE;
    }

    /* Close the previous item if there was one. */
    int rc = close_current_item(p, arr);
    if (rc) return rc;

    if (rest_len == 0) {
        /* Bare `-` -> empty dict item. */
        rc = emit_item_start(p);
        if (rc) return rc;
        arr->item_active = 1;
        return JMD_OK;
    }

    /* Try as `key: value` (dict item start). If the first token
     * doesn't parse as a key + ': ' pair, treat as a scalar item. */
    const char *kp;
    size_t kl, kc;
    if (jmd_key_parse(rest, rest_len, &kp, &kl, &kc) == JMD_OK
            && kc + 1 < rest_len
            && rest[kc] == ':'
            && rest[kc + 1] == ' ') {
        rc = emit_item_start(p);
        if (rc) return rc;
        arr->item_active = 1;
        return emit_scalar_field(p, line->line_no, kp, kl,
                                 rest + kc + 2, rest_len - kc - 2);
    }
    if (jmd_key_parse(rest, rest_len, &kp, &kl, &kc) == JMD_OK
            && kc + 1 == rest_len
            && rest[kc] == ':') {
        /* `- key:` — empty-value field on a fresh item. */
        rc = emit_item_start(p);
        if (rc) return rc;
        arr->item_active = 1;
        jmd_scalar_t v = {0};
        v.type = JMD_SCALAR_STRING;
        v.as.string.ptr = NULL;
        v.as.string.len = 0;
        return emit_field(p, kp, kl, &v);
    }

    /* Scalar item. */
    jmd_scalar_t v;
    rc = jmd_scalar_parse(rest, rest_len, &v);
    if (rc == JMD_ERROR_PARSE
            && rest_len >= 2
            && rest[0] == '"'
            && rest[rest_len - 1] == '"') {
        size_t dec = jmd_scalar_decode_string(rest + 1, rest_len - 2,
                                              p->scratch, p->scratch_cap);
        if (dec == (size_t)-1) {
            emit_error(p, line->line_no, "bad JSON escape in string");
            return JMD_ERROR_PARSE;
        }
        if (dec > p->scratch_cap) return JMD_ERROR_MEMORY;
        v.type = JMD_SCALAR_STRING;
        v.as.string.ptr = (dec == 0) ? NULL : p->scratch;
        v.as.string.len = dec;
        rc = JMD_OK;
    }
    if (rc) {
        emit_error(p, line->line_no, "malformed bullet value");
        return JMD_ERROR_PARSE;
    }
    /* Scalar items wrap as item_value — no item_start/item_end. */
    return emit_item_value(p, &v);
}

/* Handle an indented continuation line `  key: value` (>= 2 leading
 * spaces). The current top must be an array scope with an active
 * dict item. */
static int handle_indented(parser_t *p, const jmd_line_t *line)
{
    if (p->top < 0
            || !p->stack[p->top].is_array
            || !p->stack[p->top].item_active) {
        emit_error(p, line->line_no,
                   "indented continuation without active array item");
        return JMD_ERROR_PARSE;
    }
    /* Strip leading whitespace. */
    const char *raw = line->raw;
    size_t n = line->raw_len;
    size_t i = 0;
    while (i < n && (raw[i] == ' ' || raw[i] == '\t')) i++;
    return parse_and_emit_field(p, line->line_no, raw + i, n - i);
}

/* Thematic break inside an array scope: closes the current item.
 * Tolerated as decoration elsewhere — the spec only ascribes
 * semantic effect when it sits between dict items in an array
 * whose last item carries nested structure (§8.6). Slice 4b
 * accepts the simpler form: close the current item; the next
 * bullet starts fresh. */
static int handle_thematic_break(parser_t *p, const jmd_line_t *line)
{
    if (p->top >= 0 && p->stack[p->top].is_array) {
        return close_current_item(p, &p->stack[p->top]);
    }
    (void)line;
    return JMD_OK;  /* Decoration outside an array scope. */
}

static int parse_body(parser_t *p)
{
    for (;;) {
        jmd_line_t line;
        if (!jmd_tokenizer_next(p->tk, &line)) break;

        if (line.heading_depth < 0) continue;     /* blank */

        if (line.heading_depth >= 1) {
            int rc = handle_body_heading(p, &line);
            if (rc) return rc;
            continue;
        }

        /* heading_depth == 0 — body line. Dispatch by shape:
         *   - leading whitespace -> indented continuation
         *   - `---` (3+ dashes)  -> thematic break
         *   - `-` / `- ...`      -> bullet item
         *   - everything else    -> bare field line
         */
        if (line.raw_len > 0 && line.raw[0] == ' ') {
            int rc = handle_indented(p, &line);
            if (rc) return rc;
            continue;
        }
        if (is_thematic_break(&line)) {
            int rc = handle_thematic_break(p, &line);
            if (rc) return rc;
            continue;
        }
        if (line.raw_len >= 1 && line.raw[0] == '-'
                && (line.raw_len == 1 || line.raw[1] == ' ')) {
            int rc = handle_bullet(p, &line);
            if (rc) return rc;
            continue;
        }
        int rc = handle_bare_field(p, &line);
        if (rc) return rc;
    }
    /* EOF: close any still-open scopes. */
    return pop_to(p, 1);  /* depth 1 = root; closes everything */
}

/* ---------------------------------------------------------------- */
/* Public entry points                                               */
/* ---------------------------------------------------------------- */

int jmd_parse(const char *src, size_t len,
              const jmd_visitor_t *visitor, void *ctx)
{
    jmd_tokenizer_t tk;
    jmd_tokenizer_init(&tk, src, len);

    char scratch[JMD_PARSER_SCRATCH_CAP];

    parser_t p = {
        .tk = &tk,
        .vis = visitor,
        .vctx = ctx,
        .scratch = scratch,
        .scratch_cap = sizeof scratch,
        .top = -1,
    };

    /* Frontmatter pre-pass. */
    int rc = jmd_frontmatter_parse(&tk, visitor, ctx,
                                   scratch, sizeof scratch);
    if (rc) return rc;

    /* Root heading is required (unless the document is empty —
     * spec leaves that case as "no document"; we reject). */
    jmd_line_t root;
    if (!jmd_tokenizer_next(&tk, &root)) {
        if (visitor && visitor->on_parse_error) {
            jmd_error_t err = { 0, 1, "no root heading" };
            visitor->on_parse_error(ctx, &err);
        }
        return JMD_ERROR_PARSE;
    }
    if (root.heading_depth != 1) {
        if (visitor && visitor->on_parse_error) {
            jmd_error_t err = { root.line_no, 1,
                                "first non-frontmatter line must be a depth-1 heading" };
            visitor->on_parse_error(ctx, &err);
        }
        return JMD_ERROR_PARSE;
    }

    jmd_mode_t   mode;
    const char  *label;
    size_t       label_len;
    classify_root_mode(root.content, root.content_len,
                       &mode, &label, &label_len);

    /* Root-array form: trailing `[]` on the label opens an array
     * as the root container. `# []` (empty label + brackets)
     * yields label "" + array. */
    int root_is_array = 0;
    if (label_len >= 2
            && label[label_len - 2] == '['
            && label[label_len - 1] == ']') {
        root_is_array = 1;
        label_len -= 2;
    }

    rc = emit_doc_start(&p, mode, label, label_len);
    if (rc) return rc;

    if (root_is_array) {
        rc = push_array(&p, 1, NULL, 0);
    } else {
        rc = push_object(&p, 1, NULL, 0);
    }
    if (rc) return rc;

    rc = parse_body(&p);
    if (rc) return rc;

    return emit_doc_end(&p);
}

int jmd_parse_ex(const char *src, size_t len,
                 const jmd_visitor_t *visitor, void *ctx,
                 const jmd_allocator_t *allocator)
{
    /* Allocator hook is documented but not yet honoured — the
     * current parser body uses a fixed stack-resident scratch. The
     * hook becomes load-bearing once slices grow past it. */
    (void)allocator;
    return jmd_parse(src, len, visitor, ctx);
}
