/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "fs_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

/* Context -> entry -> allocator. Never wait for another lock with allocator held.
 * Allocation owner bookkeeping is separate from entry state and protected by
 * allocator. Context W drains all of that context's operations/reservations. */
enum { CONTEXTS = 64, RESOURCES = 4096, SLOT_BITS = 12 };
enum { RESOURCE_FILE = 1, RESOURCE_ROOT = 2 };
typedef struct { pthread_rwlock_t lock; int64_t id; bool reserved; } context_slot;
typedef struct { pthread_rwlock_t lock; int64_t id; int64_t owner; int fd; int flags; int kind; } resource_slot;
static context_slot contexts[CONTEXTS];
static resource_slot resources[RESOURCES];
static int64_t owners[RESOURCES];
static uint64_t serial;
static pthread_mutex_t allocator = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int init_error;

static void initialize(void) {
    pthread_rwlockattr_t attr;
    int rc = pthread_rwlockattr_init(&attr);
    if (rc) { init_error = rc; return; }
#ifdef __GLIBC__
    rc = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    int nc = 0, nf = 0;
    if (!rc) for (; nc < CONTEXTS; ++nc) {
        rc = pthread_rwlock_init(&contexts[nc].lock, &attr);
        if (rc) break;
    }
    if (!rc) for (; nf < RESOURCES; ++nf) {
        rc = pthread_rwlock_init(&resources[nf].lock, &attr);
        if (rc) break;
        resources[nf].fd = -1;
    }
    pthread_rwlockattr_destroy(&attr);
    if (rc) {
        while (nf) pthread_rwlock_destroy(&resources[--nf].lock);
        while (nc) pthread_rwlock_destroy(&contexts[--nc].lock);
        init_error = rc;
    }
}
static int ready(void) {
    int rc = pthread_once(&once, initialize);
    return rc ? rc : init_error;
}
static int64_t new_id(int index) {
    if (serial == (uint64_t)INT64_MAX >> SLOT_BITS) return 0;
    return (int64_t)(++serial << SLOT_BITS) | index;
}
static int result(gf_meta meta, int error) {
    meta.data[0] = error;
    return error ? GF_SYSTEM : GF_OK;
}
static bool meta_valid(gf_meta meta) {
    if (meta.len != 3 || !meta.data) return false;
    meta.data[0] = meta.data[1] = meta.data[2] = 0;
    return true;
}
static context_slot *lock_context(int64_t id, bool exclusive, int *error) {
    *error = ready();
    if (*error) return NULL;
    if (id <= 0 || (id & (RESOURCES - 1)) >= CONTEXTS) { *error = EBADF; return NULL; }
    context_slot *ctx = &contexts[id & (RESOURCES - 1)];
    *error = exclusive ? pthread_rwlock_wrlock(&ctx->lock) : pthread_rwlock_rdlock(&ctx->lock);
    if (*error) return NULL;
    if (ctx->id != id) { pthread_rwlock_unlock(&ctx->lock); *error = EBADF; return NULL; }
    return ctx;
}
static resource_slot *lock_resource(int64_t context, int64_t id, int kind, bool exclusive,
                            context_slot **owner, int *error) {
    *owner = lock_context(context, false, error);
    if (!*owner) return NULL;
    resource_slot *file = &resources[(uint64_t)id & (RESOURCES - 1)];
    *error = exclusive ? pthread_rwlock_wrlock(&file->lock) : pthread_rwlock_rdlock(&file->lock);
    if (!*error) {
        /* Global IDs are unique; validate the owner too, without reading allocator state. */
        if (id > 0 && file->id == id && file->fd >= 0 && file->kind == kind) {
            if (file->owner == context) return file;
        }
        pthread_rwlock_unlock(&file->lock);
        *error = EBADF;
    }
    pthread_rwlock_unlock(&(*owner)->lock);
    *owner = NULL;
    return NULL;
}
static void unlock_resource(context_slot *ctx, resource_slot *file) {
    pthread_rwlock_unlock(&file->lock);
    pthread_rwlock_unlock(&ctx->lock);
}
static void release_slot(int index) {
    resources[index].id = 0;
    resources[index].owner = 0;
    resources[index].fd = -1;
    resources[index].flags = 0;
    resources[index].kind = 0;
    pthread_mutex_lock(&allocator);
    owners[index] = 0;
    pthread_mutex_unlock(&allocator);
}
/* Reserve before filesystem effects. The caller holds Context R. */
static int reserve_slot(int64_t context, int *index, int64_t *id) {
    *index = -1; *id = 0;
    int error = 0;
    pthread_mutex_lock(&allocator);
    for (int i = 0; i < RESOURCES; ++i) if (!owners[i]) { *index = i; break; }
    if (*index < 0) error = EMFILE;
    else if (!(*id = new_id(*index))) error = EOVERFLOW;
    else owners[*index] = context;
    pthread_mutex_unlock(&allocator);
    return error;
}
int64_t gf_context_create(gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    int error = ready();
    if (error) return result(meta, error);
    int index = -1; int64_t id = 0;
    pthread_mutex_lock(&allocator);
    for (int i = 0; i < CONTEXTS; ++i) if (!contexts[i].reserved) { index = i; break; }
    if (index < 0) error = EMFILE;
    else if (!(id = new_id(index))) error = EOVERFLOW;
    else contexts[index].reserved = true;
    pthread_mutex_unlock(&allocator);
    if (error) return GF_LIMIT;
    context_slot *ctx = &contexts[index];
    error = pthread_rwlock_wrlock(&ctx->lock);
    if (error) {
        pthread_mutex_lock(&allocator); ctx->reserved = false; pthread_mutex_unlock(&allocator);
        return result(meta, error);
    }
    ctx->id = id;
    pthread_rwlock_unlock(&ctx->lock);
    meta.data[1] = id;
    return GF_OK;
}
int64_t gf_context_close(int64_t id, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    int error;
    context_slot *ctx = lock_context(id, true, &error);
    if (!ctx) return result(meta, error);
    ctx->id = 0;
    int indices[RESOURCES], count = 0;
    pthread_mutex_lock(&allocator);
    for (int i = 0; i < RESOURCES; ++i) if (owners[i] == id) indices[count++] = i;
    pthread_mutex_unlock(&allocator);
    int first_error = 0;
    for (int i = 0; i < count; ++i) {
        int index = indices[i]; resource_slot *file = &resources[index];
        /* No operation on this context can hold/claim the entry while Context W is held. */
        pthread_rwlock_wrlock(&file->lock);
        if (close(file->fd) < 0 && !first_error) first_error = errno;
        release_slot(index);
        pthread_rwlock_unlock(&file->lock);
    }
    pthread_mutex_lock(&allocator); ctx->reserved = false; pthread_mutex_unlock(&allocator);
    pthread_rwlock_unlock(&ctx->lock);
    return result(meta, first_error);
}
/* flags: access low2, creation bits2..3, append bit4, truncate bit5. */
static bool options_valid(int64_t options, int64_t permissions) {
    return options >= 0 && options <= 63 && (options & 3) <= 2 && ((options >> 2) & 3) <= 2 &&
           !((options & 3) == 0 && (options & 48)) && permissions >= 0 && permissions <= 0777;
}
static int64_t open_resource(int64_t context, int64_t source, bool relative, gf_input path,
                             int64_t options, int64_t permissions, int kind, gf_meta meta) {
    if (meta.len != 3 || !meta.data || gf_overlap(meta.data, 24, path.data, path.len)) return GF_INVALID;
    meta_valid(meta);
    char text[GF_MAX_PATH + 1];
    if (!options_valid(options,permissions) ||
        !(relative ? gf_relative_copy(path,text) : gf_path_copy(path,text)) ||
        (relative && path.len == 1 && path.data[0] == '.' && kind == RESOURCE_FILE && options != 0)) return GF_INVALID;
    int error; context_slot *ctx = NULL; resource_slot *anchor = NULL;
    if (relative) anchor = lock_resource(context,source,RESOURCE_ROOT,false,&ctx,&error);
    else ctx = lock_context(context,false,&error);
    if (!ctx) return result(meta,error);
    int index; int64_t id;
    error = reserve_slot(context,&index,&id);
    if (error) {
        if (anchor) unlock_resource(ctx,anchor); else pthread_rwlock_unlock(&ctx->lock);
        return GF_LIMIT;
    }
    resource_slot *target = &resources[index];
    error = pthread_rwlock_wrlock(&target->lock);
    if (error) {
        pthread_mutex_lock(&allocator); owners[index] = 0; pthread_mutex_unlock(&allocator);
        if (anchor) unlock_resource(ctx,anchor); else pthread_rwlock_unlock(&ctx->lock);
        return result(meta,error);
    }
    char *leaf;
    int dir = relative ? gf_parent_relative(anchor->fd,text,&leaf) : gf_parent(text,&leaf);
    int fd = -1, code = GF_OK;
    if (dir < 0) { error = errno; code = GF_SYSTEM; }
    else {
        int access = (int)(options & 3), creation = (int)((options >> 2) & 3);
        int flags = kind == RESOURCE_ROOT ? O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC :
                    (access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : O_RDWR) |
                    O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
        if (creation) flags |= O_CREAT;
        if (creation == 1) flags |= O_EXCL;
        if (options & 16) flags |= O_APPEND;
        fd = openat(dir,leaf,flags,(mode_t)permissions);
        if (fd < 0) { error = errno; code = GF_SYSTEM; }
        else if (kind == RESOURCE_FILE) {
            struct stat st;
            if (fstat(fd,&st) < 0) { error = errno; code = GF_SYSTEM; }
            else if (!S_ISREG(st.st_mode)) code = GF_NOT_REGULAR;
            else if ((options & 32) && ftruncate(fd,0) < 0) { error = errno; code = GF_SYSTEM; }
        }
        close(dir);
    }
    if (code == GF_OK) {
        target->id = id; target->owner = context; target->fd = fd;
        target->flags = (int)options; target->kind = kind;
        meta.data[1] = id;
    } else {
        if (fd >= 0) close(fd);
        release_slot(index);
        meta.data[0] = code == GF_SYSTEM ? error : 0;
    }
    pthread_rwlock_unlock(&target->lock);
    if (anchor) unlock_resource(ctx,anchor); else pthread_rwlock_unlock(&ctx->lock);
    return code;
}
int64_t gf_file_open(int64_t context, gf_input path, int64_t options, int64_t permissions, gf_meta meta) {
    return open_resource(context,0,false,path,options,permissions,RESOURCE_FILE,meta);
}
int64_t gf_root_open(int64_t context, gf_input path, gf_meta meta) {
    return open_resource(context,0,false,path,0,0,RESOURCE_ROOT,meta);
}
int64_t gf_root_open_at(int64_t context, int64_t root, gf_input path, gf_meta meta) {
    return open_resource(context,root,true,path,0,0,RESOURCE_ROOT,meta);
}
int64_t gf_file_open_at(int64_t context, int64_t root, gf_input path,
                        int64_t options, int64_t permissions, gf_meta meta) {
    return open_resource(context,root,true,path,options,permissions,RESOURCE_FILE,meta);
}

int64_t gf_file_duplicate(int64_t context, int64_t id, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *source = lock_resource(context, id, RESOURCE_FILE, false, &ctx, &error);
    if (!source) return result(meta, error);
    int index; int64_t copy_id;
    error = reserve_slot(context, &index, &copy_id);
    if (error) { unlock_resource(ctx, source); return GF_LIMIT; }
    resource_slot *copy = &resources[index];
    /* A reserved free slot cannot be a live lock dependency. Any previous close
     * released allocator before unlocking it, and never needs source's lock. */
    error = pthread_rwlock_wrlock(&copy->lock);
    if (!error) {
        int fd = fcntl(source->fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) { error = errno; release_slot(index); }
        else {
            copy->kind = RESOURCE_FILE; copy->id = copy_id; copy->owner = context; copy->fd = fd; copy->flags = source->flags;
            meta.data[1] = copy_id;
        }
        pthread_rwlock_unlock(&copy->lock);
    } else {
        pthread_mutex_lock(&allocator); owners[index] = 0; pthread_mutex_unlock(&allocator);
    }
    unlock_resource(ctx, source);
    return result(meta, error);
}
static int64_t close_resource(int64_t context, int64_t id, int kind, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context, id, kind, true, &ctx, &error);
    if (!file) return result(meta, error);
    int rc = close(file->fd), saved = rc < 0 ? errno : 0;
    release_slot((int)(id & (RESOURCES - 1)));
    unlock_resource(ctx, file);
    return result(meta, saved);
}
int64_t gf_file_close(int64_t context, int64_t id, gf_meta meta) {
    return close_resource(context,id,RESOURCE_FILE,meta);
}
int64_t gf_root_close(int64_t context, int64_t id, gf_meta meta) {
    return close_resource(context,id,RESOURCE_ROOT,meta);
}

int64_t gf_file_read(int64_t context, int64_t id, gf_output out, int64_t offset,
                     bool positioned, gf_meta meta) {
    if (meta.len != 3 || !meta.data || gf_overlap(meta.data, 24, out.data, out.len)) return GF_INVALID;
    meta_valid(meta);
    if (out.len < 0 || out.len > GF_MAX_BYTES || (out.len && !out.data) ||
        (positioned && (offset < 0 || offset > INT64_MAX - out.len))) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context, id, RESOURCE_FILE, !positioned, &ctx, &error);
    if (!file) return result(meta, error);
    if ((file->flags & 3) == 1) error = EBADF;
    else if (out.len) {
        ssize_t n;
        do { n = positioned ? pread(file->fd, out.data, (size_t)out.len, (off_t)offset)
                            : read(file->fd, out.data, (size_t)out.len); } while (n < 0 && errno == EINTR);
        if (n < 0) error = errno;
        else { meta.data[1] = n; meta.data[2] = !n; }
    }
    unlock_resource(ctx, file);
    return result(meta, error);
}
int64_t gf_file_write(int64_t context, int64_t id, gf_input data, int64_t offset,
                      bool positioned, bool all, gf_meta meta) {
    if (meta.len != 3 || !meta.data || gf_overlap(meta.data, 24, data.data, data.len)) return GF_INVALID;
    meta_valid(meta);
    if (data.len < 0 || data.len > GF_MAX_BYTES || (data.len && !data.data) ||
        (positioned && (offset < 0 || offset > INT64_MAX - data.len))) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context, id, RESOURCE_FILE, true, &ctx, &error);
    if (!file) return result(meta, error);
    if ((file->flags & 3) == 0) error = EBADF;
    else if (positioned && (file->flags & 16)) error = EINVAL;
    else while (meta.data[1] < data.len) {
        int64_t used = meta.data[1]; ssize_t n;
        do { n = positioned ? pwrite(file->fd, data.data + used, (size_t)(data.len - used), (off_t)(offset + used))
                            : write(file->fd, data.data + used, (size_t)(data.len - used));
        } while (n < 0 && errno == EINTR);
        if (n <= 0) { error = n < 0 ? errno : EIO; break; }
        meta.data[1] += n;
        if (!all) break;
    }
    unlock_resource(ctx, file);
    return result(meta, error);
}
int64_t gf_file_seek(int64_t context, int64_t id, int64_t offset, int64_t origin, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    if (origin < 0 || origin > 2 || (origin == 0 && offset < 0)) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context, id, RESOURCE_FILE, true, &ctx, &error);
    if (!file) return result(meta, error);
    off_t position = lseek(file->fd, (off_t)offset, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END);
    if (position < 0) error = errno; else meta.data[1] = position;
    unlock_resource(ctx, file);
    return result(meta, error);
}
int64_t gf_file_truncate(int64_t context, int64_t id, int64_t size, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    if (size < 0) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context, id, RESOURCE_FILE, true, &ctx, &error);
    if (!file) return result(meta, error);
    if ((file->flags & 3) == 0) error = EBADF;
    else if (ftruncate(file->fd, (off_t)size) < 0) error = errno;
    unlock_resource(ctx, file);
    return result(meta, error);
}
int64_t gf_file_sync(int64_t context, int64_t id, bool data_only, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context, id, RESOURCE_FILE, true, &ctx, &error);
    if (!file) return result(meta, error);
    int rc;
    do { rc = data_only ? fdatasync(file->fd) : fsync(file->fd); } while (rc < 0 && errno == EINTR);
    if (rc < 0) error = errno;
    unlock_resource(ctx, file);
    return result(meta, error);
}
/* Metadata changes target the held inode, including after rename/unlink.
 * Access mode does not replace the kernel's ownership/permission checks. */
int64_t gf_file_set_permissions(int64_t context, int64_t id, int64_t mode, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    if (mode < 0 || mode > 07777) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context,id,RESOURCE_FILE,true,&ctx,&error);
    if (!file) return result(meta,error);
    if (fchmod(file->fd,(mode_t)mode) < 0) error = errno;
    unlock_resource(ctx,file);
    return result(meta,error);
}
int64_t gf_file_set_owner(int64_t context, int64_t id, int64_t uid, int64_t gid, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    if (uid < -1 || uid > (int64_t)UINT32_MAX-1 || gid < -1 || gid > (int64_t)UINT32_MAX-1) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context,id,RESOURCE_FILE,true,&ctx,&error);
    if (!file) return result(meta,error);
    if (fchown(file->fd,(uid_t)uid,(gid_t)gid) < 0) error = errno;
    unlock_resource(ctx,file);
    return result(meta,error);
}
static bool metadata_time(int64_t action, int64_t seconds, int64_t nanos, struct timespec *time) {
    if (action < 0 || action > 2 || (action == 2 && (nanos < 0 || nanos > 999999999))) return false;
    time->tv_sec = action == 2 ? (time_t)seconds : 0;
    time->tv_nsec = action == 0 ? UTIME_OMIT : action == 1 ? UTIME_NOW : (long)nanos;
    return true;
}
int64_t gf_file_set_times(int64_t context, int64_t id,
                          int64_t accessed_action, int64_t accessed_seconds, int64_t accessed_nanos,
                          int64_t modified_action, int64_t modified_seconds, int64_t modified_nanos, gf_meta meta) {
    _Static_assert(sizeof(time_t) == sizeof(int64_t), "goose-fs requires 64-bit time_t");
    if (!meta_valid(meta)) return GF_INVALID;
    struct timespec times[2];
    if (!metadata_time(accessed_action,accessed_seconds,accessed_nanos,&times[0]) ||
        !metadata_time(modified_action,modified_seconds,modified_nanos,&times[1])) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context,id,RESOURCE_FILE,true,&ctx,&error);
    if (!file) return result(meta,error);
    /* Do not retry metadata mutations after an error, even EINTR. */
    if (futimens(file->fd,times) < 0) error = errno;
    unlock_resource(ctx,file);
    return result(meta,error);
}

static int64_t resource_metadata(int64_t context, int64_t id, int kind, gf_meta fields) {
    if (fields.len != 18 || !fields.data) return GF_INVALID;
    memset(fields.data, 0, 18 * sizeof(int64_t));
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context, id, kind, false, &ctx, &error);
    if (!file) { fields.data[0] = error; return GF_SYSTEM; }
    int rc = gf_metadata_at(file->fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, fields.data);
    error = rc < 0 ? errno : 0;
    unlock_resource(ctx, file);
    fields.data[0] = error;
    return error ? GF_SYSTEM : GF_OK;
}
int64_t gf_file_metadata(int64_t context, int64_t id, gf_meta fields) {
    return resource_metadata(context,id,RESOURCE_FILE,fields);
}
int64_t gf_root_metadata(int64_t context, int64_t id, gf_meta fields) {
    return resource_metadata(context,id,RESOURCE_ROOT,fields);
}

/* No blocking flock while registry locks are held: it could prevent the
 * Context's owner from closing the descriptors needed to release contention. */
int64_t gf_file_try_lock(int64_t context, int64_t id, int64_t mode, gf_meta meta) {
    if (!meta_valid(meta)) return GF_INVALID;
    if (mode < 0 || mode > 2) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *file = lock_resource(context, id, RESOURCE_FILE, true, &ctx, &error);
    if (!file) return result(meta, error);
    int operation = mode == 0 ? LOCK_UN : (mode == 1 ? LOCK_SH : LOCK_EX) | LOCK_NB;
    int rc;
    do { rc = flock(file->fd, operation); } while (rc < 0 && errno == EINTR);
    if (rc < 0) error = errno;
    unlock_resource(ctx, file);
    return result(meta, error);
}
_Bool gf_lock_contended(int64_t error) { return error == EAGAIN || error == EWOULDBLOCK; }
/* Called by Goose only after an unsuccessful attempt has released all locks. */
int64_t gf_lock_pause(void) {
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000 };
    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR) return errno;
    }
    return 0;
}

/* Root-relative one-shot operations retain the same bounded legacy data format,
 * but expose typed entry points rather than extending the legacy operation ABI. */
enum { ROOT_READ, ROOT_LIST, ROOT_CREATE, ROOT_MKDIR, ROOT_UNLINK, ROOT_RMDIR };
static int64_t root_call(int64_t context, int64_t id, int op, gf_input path,
                          gf_input data, gf_output out, int64_t entries, gf_meta meta) {
    if (meta.len != 3 || !meta.data || gf_overlap(meta.data,24,path.data,path.len) ||
        gf_overlap(meta.data,24,data.data,data.len) || gf_overlap(meta.data,24,out.data,out.len)) return GF_INVALID;
    meta_valid(meta);
    char text[GF_MAX_PATH+1];
    if (!gf_relative_copy(path,text) || (op >= ROOT_CREATE && path.len == 1 && path.data[0] == '.') ||
        data.len < 0 || data.len > GF_MAX_BYTES || (data.len && !data.data) ||
        out.len < 0 || out.len > GF_MAX_BYTES || (out.len && !out.data) ||
        entries < 0 || entries > GF_MAX_ENTRIES || gf_overlap(out.data,out.len,path.data,path.len) ||
        gf_overlap(out.data,out.len,data.data,data.len)) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *root = lock_resource(context,id,RESOURCE_ROOT,false,&ctx,&error);
    if (!root) return result(meta,error);
    char *leaf;
    int dir = gf_parent_relative(root->fd,text,&leaf);
    if (dir < 0) { error = errno; unlock_resource(ctx,root); return result(meta,error); }
    int code;
    if (op == ROOT_READ) code = gf_read_regular(dir,leaf,out,&meta.data[1]);
    else if (op == ROOT_LIST) code = gf_list_dir(dir,leaf,out,entries,&meta.data[1],&meta.data[2]);
    else if (op == ROOT_CREATE) code = gf_create_file(dir,leaf,data);
    else {
        int rc = op == ROOT_MKDIR ? mkdirat(dir,leaf,0700) : unlinkat(dir,leaf,op == ROOT_RMDIR ? AT_REMOVEDIR : 0);
        code = rc < 0 ? GF_SYSTEM : GF_OK;
    }
    error = code == GF_SYSTEM ? errno : 0;
    close(dir); unlock_resource(ctx,root);
    if (code != GF_OK) meta.data[1] = meta.data[2] = 0;
    meta.data[0] = error;
    return code;
}
int64_t gf_root_read_link(int64_t context, int64_t id, gf_input path, int64_t limit, gf_output out, gf_meta meta) {
    if (!gf_read_link_arguments(path,limit,out,meta)) return GF_INVALID;
    char text[GF_MAX_PATH+1];
    if (!gf_relative_copy(path,text)) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *root = lock_resource(context,id,RESOURCE_ROOT,false,&ctx,&error);
    if (!root) return result(meta,error);
    char *leaf;
    int dir = gf_parent_relative(root->fd,text,&leaf);
    if (dir < 0) { error = errno; unlock_resource(ctx,root); return result(meta,error); }
    int code = gf_read_link_owned_parent(dir,leaf,limit,out,meta);
    unlock_resource(ctx,root);
    return code;
}
int64_t gf_root_read(int64_t context, int64_t root, gf_input path, gf_output out, gf_meta meta) {
    return root_call(context,root,ROOT_READ,path,(gf_input){NULL,0},out,0,meta);
}
int64_t gf_root_list(int64_t context, int64_t root, gf_input path, gf_output out, int64_t entries, gf_meta meta) {
    return root_call(context,root,ROOT_LIST,path,(gf_input){NULL,0},out,entries,meta);
}
int64_t gf_root_list_sized(int64_t context, int64_t id, gf_input path, int64_t bytes,
                            gf_output out, int64_t entries, gf_meta meta) {
    if (!gf_list_arguments(path,out,bytes,entries,meta)) return GF_INVALID;
    char text[GF_MAX_PATH+1];
    if (!gf_relative_copy(path,text)) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *root = lock_resource(context,id,RESOURCE_ROOT,false,&ctx,&error);
    if (!root) { meta.data[0] = error; return GF_SYSTEM; }
    char *leaf;
    int dir = gf_parent_relative(root->fd,text,&leaf);
    if (dir < 0) { error = errno; unlock_resource(ctx,root); meta.data[0] = error; return GF_SYSTEM; }
    int code = gf_list_dir_sized(dir,leaf,out,bytes,entries,&meta.data[1],&meta.data[2],&meta.data[3]);
    error = code == GF_SYSTEM ? errno : 0;
    if (close(dir) < 0 && (code == GF_OK || code == GF_NEED_SPACE)) { code = GF_SYSTEM; error = errno; }
    unlock_resource(ctx,root);
    if (code != GF_OK) meta.data[1] = meta.data[2] = 0;
    if (code != GF_NEED_SPACE) meta.data[3] = 0;
    meta.data[0] = error;
    return code;
}
int64_t gf_root_create(int64_t context, int64_t root, gf_input path, gf_input data, gf_meta meta) {
    return root_call(context,root,ROOT_CREATE,path,data,(gf_output){NULL,0},0,meta);
}
int64_t gf_root_mkdir(int64_t context, int64_t root, gf_input path, gf_meta meta) {
    return root_call(context,root,ROOT_MKDIR,path,(gf_input){NULL,0},(gf_output){NULL,0},0,meta);
}
int64_t gf_root_unlink(int64_t context, int64_t root, gf_input path, bool directory, gf_meta meta) {
    return root_call(context,root,directory ? ROOT_RMDIR : ROOT_UNLINK,path,(gf_input){NULL,0},(gf_output){NULL,0},0,meta);
}
int64_t gf_root_metadata_at(int64_t context, int64_t id, gf_input path, gf_meta fields) {
    if (fields.len != 18 || !fields.data || gf_overlap(fields.data,18*(int64_t)sizeof(int64_t),path.data,path.len)) return GF_INVALID;
    memset(fields.data,0,18*sizeof(int64_t));
    char text[GF_MAX_PATH+1];
    if (!gf_relative_copy(path,text)) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *root = lock_resource(context,id,RESOURCE_ROOT,false,&ctx,&error);
    if (!root) { fields.data[0] = error; return GF_SYSTEM; }
    char *leaf;
    int dir = gf_parent_relative(root->fd,text,&leaf);
    if (dir < 0) { error = errno; unlock_resource(ctx,root); fields.data[0] = error; return GF_SYSTEM; }
    int rc = gf_metadata_at(dir,leaf,AT_SYMLINK_NOFOLLOW,fields.data);
    error = rc < 0 ? errno : 0;
    close(dir); unlock_resource(ctx,root);
    fields.data[0] = error;
    return error ? GF_SYSTEM : GF_OK;
}

int64_t gf_root_rename(int64_t context, int64_t id, gf_input from, gf_input to, int64_t flags, gf_meta meta) {
    if (!gf_rename_arguments(from,to,flags,meta)) return GF_INVALID;
    char source[GF_MAX_PATH+1], target[GF_MAX_PATH+1];
    if (!gf_relative_copy(from,source) || !gf_relative_copy(to,target) ||
        (from.len == 1 && from.data[0] == '.') || (to.len == 1 && to.data[0] == '.')) return GF_INVALID;
    context_slot *ctx; int error;
    resource_slot *root = lock_resource(context,id,RESOURCE_ROOT,false,&ctx,&error);
    if (!root) { meta.data[0] = error; return GF_SYSTEM; }
    int code = gf_rename_paths(root->fd,true,source,target,flags,meta.data);
    unlock_resource(ctx,root);
    return code;
}
