/*
 * version.c — runtime version reporter.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "libjmd.h"

const char *jmd_version(void)
{
    return LIBJMD_VERSION_STRING;
}
