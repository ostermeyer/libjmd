/*
 * test_link.c — M0 smoke test.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Validates that:
 *   1. The public header compiles as C11.
 *   2. The library's exported symbols are reachable from a consumer.
 *   3. The runtime version string matches the compile-time macro
 *      (i.e. the installed library and the installed header agree).
 *   4. Parse entry points are linkable and return the expected
 *      "not yet implemented" sentinel.
 */

#include <stdio.h>
#include <string.h>

#include "libjmd.h"

static int fail(const char *what)
{
    fprintf(stderr, "test_link FAIL: %s\n", what);
    return 1;
}

int main(void)
{
    const char *v = jmd_version();
    if (!v || !*v)
        return fail("jmd_version() returned empty string");
    if (strcmp(v, LIBJMD_VERSION_STRING) != 0) {
        fprintf(stderr,
                "test_link FAIL: runtime %s vs compile-time %s\n",
                v, LIBJMD_VERSION_STRING);
        return 1;
    }

    int rc = jmd_parse("# Hello\n", 8, NULL, NULL);
    if (rc != JMD_ERROR_UNIMPLEMENTED)
        return fail("jmd_parse should return JMD_ERROR_UNIMPLEMENTED in M0");

    rc = jmd_parse_ex("# Hello\n", 8, NULL, NULL, NULL);
    if (rc != JMD_ERROR_UNIMPLEMENTED)
        return fail("jmd_parse_ex should return JMD_ERROR_UNIMPLEMENTED in M0");

    jmd_envelope_t *env = jmd_parse_dom("# Hello\n", 8);
    if (env != NULL)
        return fail("jmd_parse_dom should return NULL in M0");

    printf("libjmd %s — M0 skeleton, link OK\n", v);
    return 0;
}
