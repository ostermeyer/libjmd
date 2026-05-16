/*
 * parser.c — Visitor parse entry points.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * M1 slice 4a: object-only body parse. Handles
 *
 *   - root heading classification (all 4 modes, label, root-array
 *     postfix `[]`)
 *   - frontmatter pre-pass via frontmatter.c
 *   - nested object scopes opened by `## key`, `### key`, ...
 *   - scalar field lines at column 0 (`key: value`)
 *   - scalar heading form (`## key: value`) — assigns to parent
 *     object, does NOT open a sub-scope
 *   - empty-value blockquote field (`key:` + `> ...` lines), both
 *     bare and heading positions
 *   - scope return on shallower heading (pops + emits object_end)
 *   - blank lines as cosmetic separators
 *
 * Not yet (slice 4b):
 *   - arrays of any kind (`key[]`, `### []`, bullet items `- ...`)
 *   - depth-qualified item form `##N -`
 *   - indented continuation `  key: val`
 *   - thematic break inside an array of nested-dict items
 *   - root array `# label[]`
 *
 * Hitting any of those today produces JMD_ERROR_PARSE — the parser
 * is strict-by-construction so a regression in earlier slices can't
 * mask itself as silent acceptance.
 *
 * Memory: a fixed-size internal scratch buffer (2 KiB) backs both
 * the frontmatter pre-pass and any escape-decoded / multi-line
 * accumulated strings the body emits. The buffer is single-use per
 * field — emitted slices in callbacks remain valid only for the
 * callback's duration, matching the libjmd.h surface contract.
 * jmd_parse_ex with a non-NULL allocator currently delegates to
 * jmd_parse (allocator hook lands once the buffer outgrows the
 * fixed stack form, expected in slice 4b or later).
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
    /* Reserved for slice 4b: 0 = object, 1 = array. Slice 4a only
     * opens object scopes, so this is always 0 here. */
    int is_array;
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

/* Pop scopes whose depth is >= target_depth, emitting object_end
 * for each. After this call, either the stack is empty or the top
 * scope's depth is strictly less than target_depth. */
static int pop_to(parser_t *p, int target_depth)
{
    while (p->top >= 0 && p->stack[p->top].depth >= target_depth) {
        int rc = emit_object_end(p);
        if (rc) return rc;
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
    p->stack[p->top].depth    = depth;
    p->stack[p->top].is_array = 0;
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
    /* Slice 4a rejects array-shaped headings. Future slices will
     * pick them up here. */
    if (line->content_len >= 2
            && line->content[line->content_len - 2] == '['
            && line->content[line->content_len - 1] == ']') {
        emit_error(p, line->line_no,
                   "array headings not yet implemented");
        return JMD_ERROR_PARSE;
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

        /* heading_depth == 0 — body line. Slice 4a accepts:
         *   - bare field lines
         * and rejects:
         *   - bullet lines (`- ...`)
         *   - indented continuations (` *` at start)
         *   - thematic breaks (`---`)
         */
        if (line.raw_len > 0 && line.raw[0] == ' ') {
            emit_error(p, line.line_no,
                       "indented continuation not yet implemented");
            return JMD_ERROR_PARSE;
        }
        if (line.raw_len >= 1 && line.raw[0] == '-') {
            emit_error(p, line.line_no,
                       "bullet items not yet implemented");
            return JMD_ERROR_PARSE;
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

    /* Slice 4a: root must be an object form. The `[]` suffix is
     * reserved for slice 4b. */
    if (label_len >= 2
            && label[label_len - 2] == '['
            && label[label_len - 1] == ']') {
        if (visitor && visitor->on_parse_error) {
            jmd_error_t err = { root.line_no, 1,
                                "root arrays not yet implemented" };
            visitor->on_parse_error(ctx, &err);
        }
        return JMD_ERROR_PARSE;
    }

    rc = emit_doc_start(&p, mode, label, label_len);
    if (rc) return rc;

    rc = push_object(&p, 1, NULL, 0);
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
