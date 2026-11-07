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
        if(!strcmp(kind,"grow") || !strcmp(kind,"grow-binary") || !strcmp(kind,"grow-large")) {
            int writer=__real_openat(dir,name,O_WRONLY|O_TRUNC|O_NOFOLLOW|O_NONBLOCK,0);
            if(writer<0) _exit(94);
            const char payload[]="hit longer\n";
            size_t length=!strcmp(kind,"grow-binary") ? sizeof payload : sizeof payload-1;
            if(write(writer,payload,length)!=(ssize_t)length) _exit(95);
            if(!strcmp(kind,"grow-large") && ftruncate(writer,1048577)) _exit(96);
            if(close(writer)) _exit(97);
        } else if(unlinkat(dir,name,0)) _exit(90);
        if(!strcmp(kind,"fifo")) { if(mkfifoat(dir,name,0600)) _exit(91); }
        else if(!strcmp(kind,"link")) { if(symlinkat("a",dir,name)) _exit(92); }
        else if(strcmp(kind,"gone") && strcmp(kind,"grow") && strcmp(kind,"grow-binary") && strcmp(kind,"grow-large")) _exit(93);
    }
    return __real_openat(dir,name,flags,permissions);
}
