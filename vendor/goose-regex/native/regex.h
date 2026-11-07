/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GOOSE_REGEX_H
#define GOOSE_REGEX_H
#include <stdint.h>
typedef struct { const uint8_t *data; int64_t len; } gr_bytes;
typedef struct { int64_t *data; int64_t len; } gr_ints;
enum { GR_OK, GR_INPUT, GR_HANDLE, GR_PATTERN, GR_LIMIT, GR_MEMORY, GR_BACKEND, GR_BUSY };
/* config: subject bytes, allocation bytes, work, depth.
 * compile meta: token, pairs, backend code, error offset.
 * find meta: matched, pairs, backend code. offsets: start/end pairs. */
int64_t gr_compile(gr_bytes pattern, gr_ints config, gr_ints meta);
int64_t gr_find(int64_t token, gr_bytes subject, int64_t start, gr_ints offsets, gr_ints meta);
int64_t gr_close(int64_t token);
#endif
