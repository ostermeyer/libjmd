/*
 * dom.c — DOM API: owned tree built on top of the visitor.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * M0: stubs that let consumers link and compile-check against the full
 * DOM API. Real construction and traversal land after the visitor core
 * is in place (M5).
 */

#include "libjmd.h"

jmd_envelope_t *jmd_parse_dom(const char *src, size_t len)
{
    (void)src; (void)len;
    return NULL;
}

jmd_envelope_t *jmd_parse_dom_ex(const char *src, size_t len,
                                 const jmd_allocator_t *allocator,
                                 jmd_error_t *err_out)
{
    (void)src; (void)len; (void)allocator; (void)err_out;
    return NULL;
}

void jmd_envelope_free(jmd_envelope_t *env)
{
    (void)env;
}

jmd_mode_t jmd_envelope_mode(const jmd_envelope_t *env)
{
    (void)env;
    return JMD_MODE_DATA;
}

const char *jmd_envelope_label(const jmd_envelope_t *env, size_t *out_len)
{
    (void)env;
    if (out_len) *out_len = 0;
    return NULL;
}

const jmd_value_t *jmd_envelope_value(const jmd_envelope_t *env)
{
    (void)env;
    return NULL;
}

jmd_val_kind_t jmd_value_kind(const jmd_value_t *v)
{
    (void)v;
    return JMD_VAL_NULL;
}

int jmd_value_bool(const jmd_value_t *v)
{
    (void)v;
    return 0;
}

int64_t jmd_value_int(const jmd_value_t *v)
{
    (void)v;
    return 0;
}

double jmd_value_float(const jmd_value_t *v)
{
    (void)v;
    return 0.0;
}

const char *jmd_value_string(const jmd_value_t *v, size_t *out_len)
{
    (void)v;
    if (out_len) *out_len = 0;
    return NULL;
}

size_t jmd_value_size(const jmd_value_t *v)
{
    (void)v;
    return 0;
}

const jmd_value_t *jmd_value_array_item(const jmd_value_t *v, size_t i)
{
    (void)v; (void)i;
    return NULL;
}

const jmd_value_t *jmd_value_object_get(const jmd_value_t *v,
                                        const char *key, size_t key_len)
{
    (void)v; (void)key; (void)key_len;
    return NULL;
}
