/*
 * test_parser.c — Visitor-event tests for jmd_parse (slice 4a).
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Slice 4a focuses on object-only documents. The tests subscribe a
 * capture visitor that records every event into a stringified
 * trace, then assert the exact sequence against an expected literal.
 * That gives the parser a single source of truth per test without
 * needing a full DOM builder.
 *
 * Trace alphabet (one entry per event):
 *
 *   DS<mode> <label>           document_start
 *   DE                         document_end
 *   OS <key>                   object_start
 *   OE                         object_end
 *   AS <key>                   array_start
 *   AE                         array_end
 *   IS                         item_start (dict item)
 *   IE                         item_end
 *   IV <value>                 item_value (scalar item)
 *   F <key> = <value>          field (scalar)
 *   MS <key>                   multiline_field_start
 *   MC <text>                  multiline_content (text or "<break>")
 *   ME                         multiline_field_end
 *   FM <key> = <value>         frontmatter
 *   PE <line>: <msg>           on_parse_error
 *
 * <value> is a compact textual repr: null / true / false / <int> /
 * <float> / s"<text>" for strings. Mode mark: d/s/q/x (data/schema/
 * query/delete).
 */

#include "libjmd.h"
#include "test_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Capture visitor                                                   */
/* ---------------------------------------------------------------- */

typedef struct {
    char buf[4096];
    size_t pos;
} trace_t;

static void t_append(trace_t *t, const char *s)
{
    size_t n = strlen(s);
    if (t->pos + n + 1 >= sizeof t->buf) return;
    memcpy(t->buf + t->pos, s, n);
    t->pos += n;
    t->buf[t->pos++] = '\n';
    t->buf[t->pos]   = '\0';
}

static void t_appendf(trace_t *t, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    t_append(t, line);
}

static void format_scalar(char *buf, size_t cap, const jmd_scalar_t *v)
{
    switch (v->type) {
    case JMD_SCALAR_NULL:
        snprintf(buf, cap, "null"); break;
    case JMD_SCALAR_BOOL:
        snprintf(buf, cap, v->as.boolean ? "true" : "false"); break;
    case JMD_SCALAR_INT:
        snprintf(buf, cap, "%lld", (long long)v->as.integer); break;
    case JMD_SCALAR_FLOAT:
        snprintf(buf, cap, "%g", v->as.floating); break;
    case JMD_SCALAR_STRING:
        snprintf(buf, cap, "s\"%.*s\"",
                 (int)v->as.string.len,
                 v->as.string.ptr ? v->as.string.ptr : ""); break;
    }
}

static int v_document_start(void *ctx, jmd_mode_t mode,
                            const char *label, size_t label_len)
{
    trace_t *t = (trace_t *)ctx;
    char mark = (mode == JMD_MODE_SCHEMA) ? 's'
              : (mode == JMD_MODE_QUERY)  ? 'q'
              : (mode == JMD_MODE_DELETE) ? 'x'
              :                             'd';
    t_appendf(t, "DS%c %.*s", mark, (int)label_len, label);
    return JMD_OK;
}

static int v_document_end(void *ctx)
{
    t_append((trace_t *)ctx, "DE");
    return JMD_OK;
}

static int v_object_start(void *ctx, const char *key, size_t key_len)
{
    trace_t *t = (trace_t *)ctx;
    if (key == NULL) {
        t_append(t, "OS");
    } else {
        t_appendf(t, "OS %.*s", (int)key_len, key);
    }
    return JMD_OK;
}

static int v_object_end(void *ctx)
{
    t_append((trace_t *)ctx, "OE");
    return JMD_OK;
}

static int v_array_start(void *ctx, const char *key, size_t key_len)
{
    trace_t *t = (trace_t *)ctx;
    if (key == NULL) {
        t_append(t, "AS");
    } else {
        t_appendf(t, "AS %.*s", (int)key_len, key);
    }
    return JMD_OK;
}

static int v_array_end(void *ctx)
{
    t_append((trace_t *)ctx, "AE");
    return JMD_OK;
}

static int v_item_start(void *ctx)
{
    t_append((trace_t *)ctx, "IS");
    return JMD_OK;
}

static int v_item_end(void *ctx)
{
    t_append((trace_t *)ctx, "IE");
    return JMD_OK;
}

static int v_item_value(void *ctx, const jmd_scalar_t *value)
{
    trace_t *t = (trace_t *)ctx;
    char val[256];
    format_scalar(val, sizeof val, value);
    t_appendf(t, "IV %s", val);
    return JMD_OK;
}

static int v_field(void *ctx,
                   const char *key, size_t key_len,
                   const jmd_scalar_t *value)
{
    trace_t *t = (trace_t *)ctx;
    char val[256];
    format_scalar(val, sizeof val, value);
    t_appendf(t, "F %.*s = %s", (int)key_len, key, val);
    return JMD_OK;
}

static int v_ml_start(void *ctx, const char *key, size_t key_len)
{
    trace_t *t = (trace_t *)ctx;
    t_appendf(t, "MS %.*s", (int)key_len, key);
    return JMD_OK;
}

static int v_ml_content(void *ctx, const char *content, size_t len,
                        int is_paragraph_break)
{
    trace_t *t = (trace_t *)ctx;
    if (is_paragraph_break) {
        t_append(t, "MC <break>");
    } else {
        t_appendf(t, "MC %.*s", (int)len, content);
    }
    return JMD_OK;
}

static int v_ml_end(void *ctx)
{
    t_append((trace_t *)ctx, "ME");
    return JMD_OK;
}

static int v_frontmatter(void *ctx,
                         const char *key, size_t key_len,
                         const jmd_scalar_t *value)
{
    trace_t *t = (trace_t *)ctx;
    char val[256];
    format_scalar(val, sizeof val, value);
    t_appendf(t, "FM %.*s = %s", (int)key_len, key, val);
    return JMD_OK;
}

static int v_parse_error(void *ctx, const jmd_error_t *err)
{
    trace_t *t = (trace_t *)ctx;
    t_appendf(t, "PE %d: %s", err->line, err->message);
    return JMD_OK;
}

static const jmd_visitor_t CAPTURE_VISITOR = {
    .on_document_start       = v_document_start,
    .on_document_end         = v_document_end,
    .on_frontmatter          = v_frontmatter,
    .on_object_start         = v_object_start,
    .on_object_end           = v_object_end,
    .on_array_start          = v_array_start,
    .on_array_end            = v_array_end,
    .on_item_start           = v_item_start,
    .on_item_end             = v_item_end,
    .on_item_value           = v_item_value,
    .on_field                = v_field,
    .on_multiline_field_start = v_ml_start,
    .on_multiline_content    = v_ml_content,
    .on_multiline_field_end  = v_ml_end,
    .on_scope_reset          = NULL,
    .on_parse_error          = v_parse_error,
};

/* Helper: parse src into a trace_t, return the rc. */
static int parse_into(const char *src, trace_t *t, int *rc_out)
{
    t->pos = 0;
    t->buf[0] = '\0';
    int rc = jmd_parse(src, strlen(src), &CAPTURE_VISITOR, t);
    if (rc_out) *rc_out = rc;
    return rc;
}

/* Assertion: trace equals expected literal (LF-separated lines,
 * trailing \n at the end). */
#define EXPECT_TRACE(actual, expected) do {                         \
    if (strcmp((actual), (expected)) != 0) {                        \
        fprintf(stderr,                                             \
                "  FAIL %s:%d:\n"                                   \
                "    expected:\n%s"                                 \
                "    got:\n%s",                                     \
                __FILE__, __LINE__, (expected), (actual));          \
        test_current_failed_ = 1;                                   \
        return;                                                     \
    }                                                               \
} while (0)

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

TEST(minimal_object)
{
    trace_t t = {0};
    int rc;
    parse_into("# Order\nid: 42\nstatus: pending\n", &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Order\n"
        "OS\n"
        "F id = 42\n"
        "F status = s\"pending\"\n"
        "OE\n"
        "DE\n");
}

TEST(scalar_zoo)
{
    /* All scalar kinds + quoted-disambiguation. */
    trace_t t = {0};
    int rc;
    parse_into(
        "# Types\n"
        "n: 42\n"
        "f: 3.14\n"
        "neg: -7\n"
        "b: true\n"
        "z: null\n"
        "s: hello\n"
        "qn: \"42\"\n"
        "empty: \"\"\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Types\n"
        "OS\n"
        "F n = 42\n"
        "F f = 3.14\n"
        "F neg = -7\n"
        "F b = true\n"
        "F z = null\n"
        "F s = s\"hello\"\n"
        "F qn = s\"42\"\n"
        "F empty = s\"\"\n"
        "OE\n"
        "DE\n");
}

TEST(nested_objects_close_in_lifo_order)
{
    trace_t t = {0};
    int rc;
    parse_into(
        "# Order\n"
        "id: 42\n"
        "## address\n"
        "city: Berlin\n"
        "### geo\n"
        "lat: 52.52\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Order\n"
        "OS\n"
        "F id = 42\n"
        "OS address\n"
        "F city = s\"Berlin\"\n"
        "OS geo\n"
        "F lat = 52.52\n"
        "OE\n"   /* geo */
        "OE\n"   /* address */
        "OE\n"   /* root */
        "DE\n");
}

TEST(scalar_heading_assigns_to_parent_no_sub_scope)
{
    /* `## key: value` is a scalar field on the PARENT, doesn't open
     * a sub-scope. Subsequent `## other` headings close any peer
     * scope first. */
    trace_t t = {0};
    int rc;
    parse_into(
        "# Doc\n"
        "## sub\n"
        "x: 1\n"
        "## tag: ready\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Doc\n"
        "OS\n"
        "OS sub\n"
        "F x = 1\n"
        "OE\n"
        "F tag = s\"ready\"\n"
        "OE\n"
        "DE\n");
}

TEST(empty_value_field_emits_empty_string)
{
    trace_t t = {0};
    int rc;
    parse_into("# Doc\nkey:\n", &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Doc\n"
        "OS\n"
        "F key = s\"\"\n"
        "OE\n"
        "DE\n");
}

TEST(blockquote_multiline_field)
{
    trace_t t = {0};
    int rc;
    parse_into(
        "# Doc\n"
        "body:\n"
        "> line one\n"
        "> line two\n"
        ">\n"
        "> line four\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Doc\n"
        "OS\n"
        "MS body\n"
        "MC line one\n"
        "MC line two\n"
        "MC <break>\n"
        "MC line four\n"
        "ME\n"
        "OE\n"
        "DE\n");
}

TEST(blockquote_in_heading_position)
{
    /* `## body:` followed by `> ...` — same semantics, just the
     * field is opened via a heading rather than a bare line. */
    trace_t t = {0};
    int rc;
    parse_into(
        "# Doc\n"
        "## body:\n"
        "> a\n"
        "> b\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Doc\n"
        "OS\n"
        "MS body\n"
        "MC a\n"
        "MC b\n"
        "ME\n"
        "OE\n"
        "DE\n");
}

TEST(frontmatter_with_body)
{
    trace_t t = {0};
    int rc;
    parse_into(
        "page: 2\n"
        "page-size: 50\n"
        "\n"
        "# Doc\n"
        "status: active\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "FM page = 2\n"
        "FM page-size = 50\n"
        "DSd Doc\n"
        "OS\n"
        "F status = s\"active\"\n"
        "OE\n"
        "DE\n");
}

TEST(mode_markers_schema_query_delete)
{
    trace_t t = {0};
    int rc;
    parse_into("#! Schema\nfield: x\n", &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSs Schema\n"
        "OS\n"
        "F field = s\"x\"\n"
        "OE\n"
        "DE\n");

    parse_into("#? Find\nx: 1\n", &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSq Find\n"
        "OS\n"
        "F x = 1\n"
        "OE\n"
        "DE\n");

    parse_into("#- Order\nid: 9\n", &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSx Order\n"
        "OS\n"
        "F id = 9\n"
        "OE\n"
        "DE\n");
}

TEST(quoted_string_with_escapes_is_decoded)
{
    trace_t t = {0};
    int rc;
    parse_into("# Doc\nbody: \"a\\nb\"\n", &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    /* The literal \n in the value becomes a real LF. The trace
     * format prints the slice verbatim, so the assertion includes
     * an LF inside the string literal. */
    EXPECT_TRACE(t.buf,
        "DSd Doc\n"
        "OS\n"
        "F body = s\"a\nb\"\n"
        "OE\n"
        "DE\n");
}

TEST(missing_root_heading_is_parse_error)
{
    trace_t t = {0};
    int rc;
    parse_into("", &t, &rc);
    EXPECT_EQ_INT(rc, JMD_ERROR_PARSE);
}

TEST(array_of_scalars)
{
    trace_t t = {0};
    int rc;
    parse_into(
        "# Collection\n"
        "## tags[]\n"
        "- express\n"
        "- fragile\n"
        "## numbers[]\n"
        "- 1\n"
        "- 2\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Collection\n"
        "OS\n"
        "AS tags\n"
        "IV s\"express\"\n"
        "IV s\"fragile\"\n"
        "AE\n"
        "AS numbers\n"
        "IV 1\n"
        "IV 2\n"
        "AE\n"
        "OE\n"
        "DE\n");
}

TEST(array_of_object_items_with_indented_continuation)
{
    trace_t t = {0};
    int rc;
    parse_into(
        "# Order\n"
        "## items[]\n"
        "- sku: A1\n"
        "  qty: 2\n"
        "- sku: B3\n"
        "  qty: 1\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Order\n"
        "OS\n"
        "AS items\n"
        "IS\n"
        "F sku = s\"A1\"\n"
        "F qty = 2\n"
        "IE\n"
        "IS\n"
        "F sku = s\"B3\"\n"
        "F qty = 1\n"
        "IE\n"
        "AE\n"
        "OE\n"
        "DE\n");
}

TEST(root_array_with_dict_items)
{
    trace_t t = {0};
    int rc;
    parse_into(
        "# []\n"
        "- name: Alice\n"
        "  age: 30\n"
        "- name: Bob\n"
        "  age: 25\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd \n"
        "AS\n"
        "IS\n"
        "F name = s\"Alice\"\n"
        "F age = 30\n"
        "IE\n"
        "IS\n"
        "F name = s\"Bob\"\n"
        "F age = 25\n"
        "IE\n"
        "AE\n"
        "DE\n");
}

TEST(root_array_scalar_items_delete_mode)
{
    /* `#- Order[]` + scalar bullets: delete-mode root array. */
    trace_t t = {0};
    int rc;
    parse_into("#- Order[]\n- 42\n- 43\n", &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSx Order\n"
        "AS\n"
        "IV 42\n"
        "IV 43\n"
        "AE\n"
        "DE\n");
}

TEST(thematic_break_closes_current_item)
{
    /* Inside an array, a `---` on its own line ends the current
     * dict item. The next bullet starts a fresh one. */
    trace_t t = {0};
    int rc;
    parse_into(
        "# Doc\n"
        "## items[]\n"
        "- a: 1\n"
        "---\n"
        "- b: 2\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Doc\n"
        "OS\n"
        "AS items\n"
        "IS\n"
        "F a = 1\n"
        "IE\n"
        "IS\n"
        "F b = 2\n"
        "IE\n"
        "AE\n"
        "OE\n"
        "DE\n");
}

TEST(array_closes_on_shallower_heading)
{
    /* When a heading at the array's depth (or shallower) appears,
     * the array — and any open item — close before the new scope
     * opens. */
    trace_t t = {0};
    int rc;
    parse_into(
        "# Doc\n"
        "## items[]\n"
        "- a: 1\n"
        "## tag: ready\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Doc\n"
        "OS\n"
        "AS items\n"
        "IS\n"
        "F a = 1\n"
        "IE\n"
        "AE\n"
        "F tag = s\"ready\"\n"
        "OE\n"
        "DE\n");
}

TEST(bare_dash_starts_empty_dict_item)
{
    /* A line containing just `-` opens a fresh dict item with no
     * fields yet. Continuation lines may follow. */
    trace_t t = {0};
    int rc;
    parse_into(
        "# Doc\n"
        "## items[]\n"
        "-\n"
        "  k: 1\n",
        &t, &rc);
    EXPECT_EQ_INT(rc, JMD_OK);
    EXPECT_TRACE(t.buf,
        "DSd Doc\n"
        "OS\n"
        "AS items\n"
        "IS\n"
        "F k = 1\n"
        "IE\n"
        "AE\n"
        "OE\n"
        "DE\n");
}

TEST(null_visitor_parses_cleanly)
{
    /* Useful for fixture-walk runners that don't need events — just
     * the syntactic OK/error answer. */
    int rc = jmd_parse(
        "# Doc\nx: 1\n## y: 2\n", strlen("# Doc\nx: 1\n## y: 2\n"),
        NULL, NULL);
    EXPECT_EQ_INT(rc, JMD_OK);
}

int main(void)
{
    RUN_TEST(minimal_object);
    RUN_TEST(scalar_zoo);
    RUN_TEST(nested_objects_close_in_lifo_order);
    RUN_TEST(scalar_heading_assigns_to_parent_no_sub_scope);
    RUN_TEST(empty_value_field_emits_empty_string);
    RUN_TEST(blockquote_multiline_field);
    RUN_TEST(blockquote_in_heading_position);
    RUN_TEST(frontmatter_with_body);
    RUN_TEST(mode_markers_schema_query_delete);
    RUN_TEST(quoted_string_with_escapes_is_decoded);
    RUN_TEST(missing_root_heading_is_parse_error);
    RUN_TEST(array_of_scalars);
    RUN_TEST(array_of_object_items_with_indented_continuation);
    RUN_TEST(root_array_with_dict_items);
    RUN_TEST(root_array_scalar_items_delete_mode);
    RUN_TEST(thematic_break_closes_current_item);
    RUN_TEST(array_closes_on_shallower_heading);
    RUN_TEST(bare_dash_starts_empty_dict_item);
    RUN_TEST(null_visitor_parses_cleanly);
    return TEST_SUMMARY();
}
