/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
typedef struct { const uint8_t *data; int64_t len; } bytes;
extern int64_t search_write(int64_t, bytes);
static int mode, calls;
static size_t used;
static uint8_t copied[32];
ssize_t __wrap_write(int fd, const void *data, size_t length) {
    assert(fd==1 || fd==2);
    ++calls;
    if(mode==1) return 0;
    if(mode==2) { errno=EIO; return -1; }
    if(calls==1) { errno=EINTR; return -1; }
    size_t n=length>2 ? 2 : length;
    assert(used+n<=sizeof copied);
    memcpy(copied+used,data,n); used+=n;
    return (ssize_t)n;
}
int main(void) {
    bytes input={(const uint8_t *)"abcdefg",7};
    assert(search_write(1,input)==1);
    assert(calls==5 && used==7 && !memcmp(copied,"abcdefg",7));
    mode=1; calls=0; assert(search_write(1,input)==0 && calls==1);
    mode=2; calls=0; assert(search_write(2,input)==0 && calls==1);
    calls=0;
    assert(search_write(1,(bytes){NULL,0})==1 && calls==0);
    assert(search_write(3,input)==0 && calls==0);
    assert(search_write(1,(bytes){input.data,-1})==0 && calls==0);
    assert(search_write(1,(bytes){input.data,8388609})==0 && calls==0);
    assert(search_write(1,(bytes){(const uint8_t *)(uintptr_t)(UINTPTR_MAX-1),4})==0 && calls==0);
}
