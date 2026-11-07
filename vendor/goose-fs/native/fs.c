/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "fs_internal.h"
#if !defined(__linux__) || UINTPTR_MAX != UINT64_MAX
#error "goose-fs currently requires 64-bit Linux"
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <linux/stat.h>

/* The optional libc entry point need not exist on older Linux runtimes. */
extern int statx(int, const char *, int, unsigned int, struct statx *) __attribute__((weak));
#include <unistd.h>

enum { READ, LIST, CREATE, MKDIR, UNLINK, RMDIR };

bool gf_overlap(const void *a, int64_t an, const void *b, int64_t bn) {
    if (an <= 0 || bn <= 0) return false;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < (uint64_t)an : x - y < (uint64_t)bn;
}

/* Validate before copying; never normalize a path into a different meaning. */
static bool copy_path(gf_input path, char text[GF_MAX_PATH + 1], bool relative) {
    if (path.len < 1 || path.len > GF_MAX_PATH || !path.data ||
        (relative ? path.data[0] == '/' : path.data[0] != '/') ||
        memchr(path.data,0,(size_t)path.len)) return false;
    bool self = path.len == 1 && (relative ? path.data[0] == '.' : path.data[0] == '/');
    if (!self) {
        size_t start = relative ? 0 : 1;
        for (size_t i=start; i<=(size_t)path.len; ++i) {
            if (i != (size_t)path.len && path.data[i] != '/') continue;
            size_t n = i-start;
            if (!n || (n == 1 && path.data[start] == '.') ||
                (n == 2 && path.data[start] == '.' && path.data[start+1] == '.')) return false;
            start = i+1;
        }
    }
    memcpy(text,path.data,(size_t)path.len); text[path.len] = 0;
    return true;
}
bool gf_path_copy(gf_input path, char text[GF_MAX_PATH + 1]) { return copy_path(path,text,false); }
bool gf_relative_copy(gf_input path, char text[GF_MAX_PATH + 1]) { return copy_path(path,text,true); }

static int close_walk_fd(int fd, int64_t *cleanup) {
    if (close(fd) == 0) return 0;
    int error = errno;
    if (cleanup && !*cleanup) *cleanup = error;
    return error;
}
/* Consume old exactly once. On failure consume any opened next too; callers
 * own no descriptor after -1. Linux close is never retried, including EINTR. */
static int walk_child(int old, const char *part, int64_t *cleanup) {
    int next = openat(old,part,O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int error = next < 0 ? errno : 0;
    int closed = close_walk_fd(old,cleanup);
    if (!error) error = closed;
    if (error) {
        if (next >= 0) close_walk_fd(next,cleanup);
        errno = error;
        return -1;
    }
    return next;
}

/* Return parent descriptor and a leaf in text; every ancestor rejects symlinks. */
static int parent_walk(char *text, char **leaf, int64_t *cleanup) {
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *part = text + 1, *slash;
    while ((slash = strchr(part, '/'))) {
        *slash = 0;
        fd = walk_child(fd,part,cleanup);
        if (fd < 0) return -1;
        part = slash + 1;
    }
    *leaf = *part ? part : ".";
    return fd;
}

/* A single kernel path walk preserves the same all-component no-symlink policy.
 * No cached feature state: older kernels and seccomp-denied syscalls use the
 * descriptor walker. Other lookup failures are returned unchanged. */
int gf_parent_checked(char *text, char **leaf, int64_t *cleanup) {
    char *separator = strrchr(text, '/'); /* path_copy guarantees an absolute path. */
    *separator = 0;
    const char *directory = separator == text ? "/" : text;
    struct open_how how = { .flags = O_PATH | O_DIRECTORY | O_CLOEXEC,
                            .resolve = RESOLVE_NO_SYMLINKS };
    int fd = (int)syscall(SYS_openat2, AT_FDCWD, directory, &how, sizeof how);
    int saved = errno;
    *separator = '/';
    if (fd >= 0) { *leaf = separator[1] ? separator + 1 : "."; return fd; }
    if (saved == ENOSYS || saved == EPERM) return parent_walk(text, leaf,cleanup);
    errno = saved;
    return -1;
}

int gf_parent(char *text, char **leaf) { return gf_parent_checked(text,leaf,NULL); }

/* Private staging may be smaller than limit+1; a full buffer is never success. */
bool gf_read_link_arguments(gf_input path, int64_t limit, gf_output out, gf_meta meta) {
    if (meta.len != 3 || !meta.data || gf_overlap(meta.data,24,path.data,path.len) ||
        gf_overlap(meta.data,24,out.data,out.len)) return false;
    memset(meta.data,0,24);
    return limit >= 0 && limit <= GF_MAX_BYTES && out.len > 0 &&
        out.len <= limit + 1 && out.data && !gf_overlap(out.data,out.len,path.data,path.len);
}
/* Consume the owned parent exactly once. Never retry close, including EINTR. */
int gf_read_link_owned_parent(int dir, const char *leaf, int64_t limit, gf_output out, gf_meta meta) {
    ssize_t n;
    do { n = readlinkat(dir,leaf,(char *)out.data,(size_t)out.len); } while (n < 0 && errno == EINTR);
    int code = n < 0 ? GF_SYSTEM : n > limit ? GF_LIMIT : n == out.len ? GF_NEED_SPACE : GF_OK;
    int error = n < 0 ? errno : 0;
    if (close(dir) < 0 && (code == GF_OK || code == GF_NEED_SPACE)) { code = GF_SYSTEM; error = errno; }
    meta.data[0] = error;
    meta.data[1] = code == GF_OK ? n : 0;
    return code;
}
int64_t gf_read_link(gf_input path, int64_t limit, gf_output out, gf_meta meta) {
    if (!gf_read_link_arguments(path,limit,out,meta)) return GF_INVALID;
    char text[GF_MAX_PATH+1];
    if (!gf_path_copy(path,text)) return GF_INVALID;
    char *leaf;
    int dir = gf_parent(text,&leaf);
    if (dir < 0) { meta.data[0] = errno; return GF_SYSTEM; }
    return gf_read_link_owned_parent(dir,leaf,limit,out,meta);
}

int gf_read_regular(int dir, const char *leaf, gf_output out, int64_t *length) {
    int fd = openat(dir, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return GF_SYSTEM;
    struct stat st;
    int result = GF_OK, saved = 0;
    if (fstat(fd, &st) < 0) { result = GF_SYSTEM; saved = errno; }
    else if (!S_ISREG(st.st_mode)) result = GF_NOT_REGULAR;
    else {
        while (*length < out.len) {
            ssize_t n = read(fd, out.data + *length, (size_t)(out.len - *length));
            if (n < 0) { if (errno == EINTR) continue; result = GF_SYSTEM; saved = errno; break; }
            if (!n) break;
            *length += n;
        }
        if (result == GF_OK && *length == out.len) {
            uint8_t extra;
            ssize_t n;
            do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
            if (n > 0) result = GF_LIMIT;
            else if (n < 0) { result = GF_SYSTEM; saved = errno; }
        }
    }
    if (close(fd) < 0 && result == GF_OK) { result = GF_SYSTEM; saved = errno; }
    errno = saved;
    return result;
}

/* Sizing passes never retain a DIR or pointers into Goose storage. After a
 * spill, stop writing permanently but charge every remaining record to limits. */
int gf_list_dir_sized(int dir, const char *leaf, gf_output out, int64_t bytes,
                      int64_t cap, int64_t *length, int64_t *count, int64_t *needed) {
    *length = *count = *needed = 0;
    int fd = openat(dir, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return GF_SYSTEM;
    DIR *stream = fdopendir(fd);
    if (!stream) { int saved = errno; close(fd); errno = saved; return GF_SYSTEM; }
    int result = GF_OK, saved = 0;
    int64_t seen_bytes = 0, seen_entries = 0;
    bool spilled = false;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) { if (errno) { result = GF_SYSTEM; saved = errno; } break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t n = strlen(entry->d_name);
        if (seen_entries == cap || n > (size_t)(bytes - seen_bytes) ||
            (int64_t)n + 2 > bytes - seen_bytes) { result = GF_LIMIT; break; }
        int64_t record = (int64_t)n + 2;
        if (record > out.len - seen_bytes) spilled = true;
        if (!spilled) {
            uint8_t kind = entry->d_type == DT_REG ? 1 : entry->d_type == DT_DIR ? 2 :
                           entry->d_type == DT_LNK ? 3 : entry->d_type == DT_UNKNOWN ? 0 : 4;
            out.data[seen_bytes] = kind;
            memcpy(out.data + seen_bytes + 1, entry->d_name, n + 1);
        }
        seen_bytes += record;
        ++seen_entries;
    }
    if (closedir(stream) < 0 && result == GF_OK) { result = GF_SYSTEM; saved = errno; }
    if (result == GF_OK) {
        if (spilled) { *needed = seen_bytes; result = GF_NEED_SPACE; }
        else { *length = seen_bytes; *count = seen_entries; }
    }
    errno = saved;
    return result;
}
int gf_list_dir(int dir, const char *leaf, gf_output out, int64_t cap, int64_t *length, int64_t *count) {
    int64_t needed;
    return gf_list_dir_sized(dir,leaf,out,out.len,cap,length,count,&needed);
}

bool gf_list_arguments(gf_input path, gf_output out, int64_t bytes, int64_t entries, gf_meta meta) {
    if (meta.len != 4 || !meta.data || gf_overlap(meta.data,32,path.data,path.len) ||
        gf_overlap(meta.data,32,out.data,out.len)) return false;
    memset(meta.data,0,32);
    return bytes >= 0 && bytes <= GF_MAX_BYTES && out.len >= 0 && out.len <= bytes &&
        (!out.len || out.data) && entries >= 0 && entries <= GF_MAX_ENTRIES &&
        !gf_overlap(out.data,out.len,path.data,path.len);
}

int gf_create_file(int dir, const char *leaf, gf_input data) {
    int fd = openat(dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return GF_SYSTEM;
    int result = GF_OK, saved = 0;
    int64_t done = 0;
    while (done < data.len) {
        ssize_t n = write(fd, data.data + done, (size_t)(data.len - done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { result = GF_SYSTEM; saved = n < 0 ? errno : EIO; break; }
        done += n;
    }
    if (close(fd) < 0 && result == GF_OK) { result = GF_SYSTEM; saved = errno; }
    errno = saved;
    return result;
}

int64_t gf_call(int64_t op, gf_input path, gf_input data, int64_t entries,
                gf_output out, gf_meta meta) {
    if (meta.len != 3 || !meta.data) return GF_INVALID;
    /* Guard the private ABI before touching writable slices, including metadata. */
    if (gf_overlap(meta.data, 24, path.data, path.len) || gf_overlap(meta.data, 24, data.data, data.len) ||
        gf_overlap(meta.data, 24, out.data, out.len)) return GF_INVALID;
    meta.data[0] = meta.data[1] = meta.data[2] = 0;
    char text[GF_MAX_PATH + 1];
    if (op < READ || op > RMDIR || !gf_path_copy(path, text) ||
        data.len < 0 || data.len > GF_MAX_BYTES || (data.len && !data.data) ||
        out.len < 0 || out.len > GF_MAX_BYTES || (out.len && !out.data) ||
        entries < 0 || entries > GF_MAX_ENTRIES ||
        gf_overlap(out.data, out.len, path.data, path.len) || gf_overlap(out.data, out.len, data.data, data.len) ||
        (op >= CREATE && path.len == 1)) return GF_INVALID;
    char *leaf;
    int dir = gf_parent(text, &leaf);
    if (dir < 0) { meta.data[0] = errno; return GF_SYSTEM; }
    int result;
    if (op == READ) result = gf_read_regular(dir, leaf, out, &meta.data[1]);
    else if (op == LIST) result = gf_list_dir(dir, leaf, out, entries, &meta.data[1], &meta.data[2]);
    else if (op == CREATE) result = gf_create_file(dir, leaf, data);
    else {
        int rc = op == MKDIR ? mkdirat(dir, leaf, 0700) : unlinkat(dir, leaf, op == RMDIR ? AT_REMOVEDIR : 0);
        result = rc < 0 ? GF_SYSTEM : GF_OK;
    }
    int saved = result == GF_SYSTEM ? errno : 0;
    close(dir);
    if (result != GF_OK) meta.data[1] = meta.data[2] = 0;
    meta.data[0] = saved;
    return result;
}

/* Separate staging ABI: only clean, complete sizing passes return NEED_SPACE. */
int64_t gf_list(gf_input path, int64_t bytes, gf_output out, int64_t entries, gf_meta meta) {
    if (!gf_list_arguments(path,out,bytes,entries,meta)) return GF_INVALID;
    char text[GF_MAX_PATH+1];
    if (!gf_path_copy(path,text)) return GF_INVALID;
    char *leaf;
    int dir = gf_parent(text,&leaf);
    if (dir < 0) { meta.data[0] = errno; return GF_SYSTEM; }
    int code = gf_list_dir_sized(dir,leaf,out,bytes,entries,&meta.data[1],&meta.data[2],&meta.data[3]);
    int saved = code == GF_SYSTEM ? errno : 0;
    if (close(dir) < 0 && (code == GF_OK || code == GF_NEED_SPACE)) { code = GF_SYSTEM; saved = errno; }
    if (code != GF_OK) meta.data[1] = meta.data[2] = 0;
    if (code != GF_NEED_SPACE) meta.data[3] = 0;
    meta.data[0] = saved;
    return code;
}

/* Rename outcomes describe the syscall result, not a source-name absence or
 * persistence guarantee. In particular, an NFS error can follow an applied move. */
bool gf_rename_arguments(gf_input from, gf_input to, int64_t flags, gf_meta meta) {
    if (meta.len != 4 || !meta.data || gf_overlap(meta.data,32,from.data,from.len) ||
        gf_overlap(meta.data,32,to.data,to.len)) return false;
    memset(meta.data,0,32);
    return flags >= 0 && flags <= 3;
}
static void rename_close(int fd, int *code, int64_t *meta) {
    if (fd >= 0 && close(fd) < 0) {
        int error = errno;
        if (!meta[3]) meta[3] = error;
        if (*code == GF_OK) { *code = GF_SYSTEM; meta[0] = error; }
    }
}
int gf_rename_paths(int root, bool relative, char *from, char *to, int64_t flags, int64_t *meta) {
    int parents[2] = {-1,-1}, syncs[2] = {-1,-1}, code = GF_OK;
    char *paths[2] = {from,to}, *leaves[2];
    for (int i = 0; i < 2; ++i) {
        parents[i] = relative ? gf_parent_relative_checked(root,paths[i],&leaves[i],&meta[3]) : gf_parent_checked(paths[i],&leaves[i],&meta[3]);
        if (parents[i] < 0) { code = GF_SYSTEM; meta[0] = errno; goto cleanup; }
    }
    if (flags & 2) {
        /* Opening before the move prevents a post-commit permission/open failure
         * from being mistaken for an unattempted operation. O_PATH cannot fsync. */
        for (int i = 0; i < 2; ++i) {
            syncs[i] = openat(parents[i],".",O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (syncs[i] < 0) { code = GF_SYSTEM; meta[0] = errno; goto cleanup; }
        }
    }
    meta[1] = 2; /* Unknown once attempted; never retry a failed rename. */
    int rc = flags & 1 ? (int)syscall(SYS_renameat2,parents[0],leaves[0],parents[1],leaves[1],(unsigned int)RENAME_NOREPLACE) :
                         renameat(parents[0],leaves[0],parents[1],leaves[1]);
    if (rc < 0) { code = GF_SYSTEM; meta[0] = errno; goto cleanup; }
    meta[1] = 1; /* Applied includes successful same-path/same-inode no-ops. */
    if (flags & 2) {
        bool synced = true;
        for (int i = 0; i < 2; ++i) {
            do { rc = fsync(syncs[i]); } while (rc < 0 && errno == EINTR);
            if (rc < 0) {
                synced = false;
                if (code == GF_OK) { code = GF_SYSTEM; meta[0] = errno; }
            }
        }
        meta[2] = synced;
    }
cleanup:
    for (int i = 0; i < 2; ++i) rename_close(syncs[i],&code,meta);
    for (int i = 0; i < 2; ++i) rename_close(parents[i],&code,meta);
    return code;
}
int64_t gf_rename(gf_input from, gf_input to, int64_t flags, gf_meta meta) {
    if (!gf_rename_arguments(from,to,flags,meta)) return GF_INVALID;
    char source[GF_MAX_PATH+1], target[GF_MAX_PATH+1];
    if (!gf_path_copy(from,source) || !gf_path_copy(to,target) || from.len == 1 || to.len == 1) return GF_INVALID;
    return gf_rename_paths(-1,false,source,target,flags,meta.data);
}

/* Metadata has a separate ABI; legacy operation tuples remain unchanged. */
_Bool gf_name_before(gf_input a, gf_input b) {
    if (a.len < 0 || b.len < 0 || (a.len && !a.data) || (b.len && !b.data)) return false;
    size_t common = (size_t)(a.len < b.len ? a.len : b.len);
    int order = common ? memcmp(a.data,b.data,common) : 0;
    return order ? order < 0 : a.len < b.len;
}
_Bool gf_missing(int64_t os_error) { return os_error == ENOENT; }

static int64_t kind_of(mode_t mode) {
    return S_ISREG(mode) ? 1 : S_ISDIR(mode) ? 2 : S_ISLNK(mode) ? 3 :
           S_ISFIFO(mode) ? 5 : S_ISSOCK(mode) ? 6 : S_ISCHR(mode) ? 7 : S_ISBLK(mode) ? 8 : 4;
}
static void store_unsigned(int64_t *target, uint64_t value) {
    memcpy(target, &value, sizeof value);
}
int64_t gf_metadata(gf_input path, gf_meta fields) {
    if (fields.len != 18 || !fields.data ||
        gf_overlap(fields.data, 18 * (int64_t)sizeof(int64_t), path.data, path.len)) return GF_INVALID;
    memset(fields.data, 0, 18 * sizeof(int64_t));
    char text[GF_MAX_PATH + 1];
    if (!gf_path_copy(path, text)) return GF_INVALID;
    char *leaf;
    int dir = gf_parent(text, &leaf);
    if (dir < 0) { fields.data[0] = errno; return GF_SYSTEM; }
    int rc = gf_metadata_at(dir, leaf, AT_SYMLINK_NOFOLLOW, fields.data);
    int saved = errno;
    close(dir);
    if (rc) { fields.data[0] = saved; return GF_SYSTEM; }
    return GF_OK;
}

int gf_metadata_at(int dir, const char *leaf, int flags, int64_t *out) {
    struct statx extended;
    int rc;
    if (statx) rc = statx(dir, leaf, flags, STATX_BASIC_STATS | STATX_BTIME, &extended);
    else { rc = -1; errno = ENOSYS; }
    int saved = errno;
    if (!rc && (extended.stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS) {
        out[1] = kind_of(extended.stx_mode);
        store_unsigned(&out[2], extended.stx_size);
        out[3] = extended.stx_mode & 07777;
        out[4] = extended.stx_uid; out[5] = extended.stx_gid;
        store_unsigned(&out[6], (uint64_t)makedev(extended.stx_dev_major, extended.stx_dev_minor));
        store_unsigned(&out[7], extended.stx_ino);
        out[8] = extended.stx_nlink;
        out[9] = extended.stx_atime.tv_sec; out[10] = extended.stx_atime.tv_nsec;
        out[11] = extended.stx_mtime.tv_sec; out[12] = extended.stx_mtime.tv_nsec;
        out[13] = extended.stx_ctime.tv_sec; out[14] = extended.stx_ctime.tv_nsec;
        if (extended.stx_mask & STATX_BTIME) {
            out[15] = extended.stx_btime.tv_sec; out[16] = extended.stx_btime.tv_nsec; out[17] = 1;
        }
    } else if (!rc || saved == ENOSYS || saved == EPERM) {
        struct stat basic;
        rc = fstatat(dir, leaf, &basic, flags);
        saved = errno;
        if (!rc) {
            out[1] = kind_of(basic.st_mode); store_unsigned(&out[2], (uint64_t)basic.st_size);
            out[3] = basic.st_mode & 07777; out[4] = basic.st_uid; out[5] = basic.st_gid;
            store_unsigned(&out[6], (uint64_t)basic.st_dev); store_unsigned(&out[7], (uint64_t)basic.st_ino);
            store_unsigned(&out[8], (uint64_t)basic.st_nlink);
            out[9] = basic.st_atim.tv_sec; out[10] = basic.st_atim.tv_nsec;
            out[11] = basic.st_mtim.tv_sec; out[12] = basic.st_mtim.tv_nsec;
            out[13] = basic.st_ctim.tv_sec; out[14] = basic.st_ctim.tv_nsec;
        }
    }
    errno = saved;
    return rc;
}

/* Every returned parent descriptor is owned, including a duplicated root. */
static int relative_walk(int root, char *text, char **leaf, int64_t *cleanup) {
    int fd = fcntl(root,F_DUPFD_CLOEXEC,0);
    if (fd < 0) return -1;
    char *part = text, *slash;
    while ((slash = strchr(part,'/'))) {
        *slash = 0;
        fd = walk_child(fd,part,cleanup);
        if (fd < 0) return -1;
        part = slash+1;
    }
    *leaf = part;
    return fd;
}
int gf_parent_relative_checked(int root, char *text, char **leaf, int64_t *cleanup) {
    char *separator = strrchr(text,'/');
    if (!separator) { *leaf = text; return fcntl(root,F_DUPFD_CLOEXEC,0); }
    *separator = 0;
    struct open_how how = { .flags = O_PATH | O_DIRECTORY | O_CLOEXEC,
                            .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS };
    int fd = (int)syscall(SYS_openat2,root,text,&how,sizeof how);
    int saved = errno; *separator = '/';
    if (fd >= 0) { *leaf = separator+1; return fd; }
    if (saved == ENOSYS || saved == EPERM) return relative_walk(root,text,leaf,cleanup);
    errno = saved; return -1;
}

int gf_parent_relative(int root, char *text, char **leaf) { return gf_parent_relative_checked(root,text,leaf,NULL); }
