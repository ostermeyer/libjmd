/*
 * serializer.c — JMD text serializer.
 *
 * Copyright (c) 2026 Andreas Ostermeyer <andreas@ostermeyer.de>
 * SPDX-License-Identifier: Apache-2.0
 *
 * M0: stub. Real serializer lands in M4, ported from jmd-impl's
 * _cserializer.c and de-PyObjectified.
 */

#include "libjmd.h"

int jmd_serialize_dom(const jmd_envelope_t *env,
                      char **out_buf, size_t *out_len,
                      const jmd_allocator_t *allocator)
{
    (void)env; (void)out_buf; (void)out_len; (void)allocator;
    return JMD_ERROR_UNIMPLEMENTED;
}
