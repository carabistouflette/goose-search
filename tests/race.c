/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
extern int __real_openat(int, const char *, int, ...);
int __wrap_openat(int dir, const char *name, int flags, ...) {
    mode_t permissions=0;
    if(flags&O_CREAT) { va_list args; va_start(args,flags); permissions=(mode_t)va_arg(args,int); va_end(args); }
    const char *kind=getenv("SEARCH_RACE");
    if(kind && !strcmp(name,"b") && !(flags&O_PATH) && !(flags&O_DIRECTORY)) {
        if(unlinkat(dir,name,0)) _exit(90);
        if(!strcmp(kind,"fifo")) { if(mkfifoat(dir,name,0600)) _exit(91); }
        else if(!strcmp(kind,"link")) { if(symlinkat("a",dir,name)) _exit(92); }
        else if(strcmp(kind,"gone")) _exit(93);
    }
    return __real_openat(dir,name,flags,permissions);
}
