/*
 * parser.c — visitor-API parse entry points.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * M0: the entry points exist and return JMD_ERROR_UNIMPLEMENTED so
 * consumers can link against libjmd and exercise the build system.
 * The real tokenizer and state machine land in M1–M3.
 */

#include "libjmd.h"

int jmd_parse(const char *src, size_t len,
              const jmd_visitor_t *visitor, void *ctx)
{
    (void)src; (void)len; (void)visitor; (void)ctx;
    return JMD_ERROR_UNIMPLEMENTED;
}

int jmd_parse_ex(const char *src, size_t len,
                 const jmd_visitor_t *visitor, void *ctx,
                 const jmd_allocator_t *allocator)
{
    (void)src; (void)len; (void)visitor; (void)ctx; (void)allocator;
    return JMD_ERROR_UNIMPLEMENTED;
}
