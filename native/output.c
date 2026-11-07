/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
typedef struct { const uint8_t *data; int64_t len; } gs_bytes;
/* No retained pointers or signal policy; SIGPIPE retains its inherited action. */
int64_t search_write(int64_t descriptor, gs_bytes bytes) {
    if ((descriptor != 1 && descriptor != 2) || bytes.len < 0 || bytes.len > 8388608 ||
        (bytes.len && (!bytes.data || (uintptr_t)bytes.data > UINTPTR_MAX-(uint64_t)bytes.len))) return 0;
    size_t at = 0;
    while (at < (size_t)bytes.len) {
        ssize_t n = write((int)descriptor, bytes.data+at, (size_t)bytes.len-at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        at += (size_t)n;
    }
    return 1;
}
