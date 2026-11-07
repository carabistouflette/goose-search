/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GOOSE_FS_H
#define GOOSE_FS_H
#include <stdint.h>
typedef struct { const uint8_t *data; int64_t len; } gf_input;
typedef struct { uint8_t *data; int64_t len; } gf_output;
typedef struct { int64_t *data; int64_t len; } gf_meta;
/* Private slice ABI; no pointers retained. meta = errno, byte count, entry count. */
int64_t gf_call(int64_t op, gf_input path, gf_input data, int64_t entries,
                gf_output out, gf_meta meta);
int64_t gf_read_link(gf_input path, int64_t limit, gf_output out, gf_meta meta);
int64_t gf_root_read_link(int64_t context, int64_t root, gf_input path, int64_t limit, gf_output out, gf_meta meta);
int64_t gf_metadata(gf_input path, gf_meta fields);
/* Staging meta4 = errno, written bytes, written entries, required bytes.
 * Only result5 supplies required bytes; written counts are zero on all failures.
 * Byte/entry budget failures return2. Legacy3-slot listing ABIs are unchanged. */
int64_t gf_list(gf_input path, int64_t bytes, gf_output out, int64_t entries, gf_meta meta);
/* Rename meta4 = errno, outcome (0 unattempted/1 applied/2 unknown),
 * both parent fsyncs accepted, first cleanup errno. flags:1 NoReplace,2 sync. */
int64_t gf_rename(gf_input from, gf_input to, int64_t flags, gf_meta meta);
int64_t gf_root_rename(int64_t context, int64_t root, gf_input from, gf_input to, int64_t flags, gf_meta meta);
_Bool gf_name_before(gf_input a, gf_input b);
_Bool gf_missing(int64_t os_error);
/* Retained-resource ABI; meta = errno, count/token/position, eof. */
int64_t gf_context_create(gf_meta meta);
int64_t gf_context_close(int64_t context, gf_meta meta);
int64_t gf_file_open(int64_t context, gf_input path, int64_t options, int64_t permissions, gf_meta meta);
int64_t gf_file_close(int64_t context, int64_t file, gf_meta meta);
int64_t gf_file_duplicate(int64_t context, int64_t file, gf_meta meta);
int64_t gf_file_read(int64_t context, int64_t file, gf_output out, int64_t offset, _Bool positioned, gf_meta meta);
int64_t gf_file_write(int64_t context, int64_t file, gf_input data, int64_t offset, _Bool positioned, _Bool all, gf_meta meta);
int64_t gf_file_seek(int64_t context, int64_t file, int64_t offset, int64_t origin, gf_meta meta);
int64_t gf_file_truncate(int64_t context, int64_t file, int64_t size, gf_meta meta);
int64_t gf_file_sync(int64_t context, int64_t file, _Bool data_only, gf_meta meta);
int64_t gf_file_set_permissions(int64_t context, int64_t file, int64_t mode, gf_meta meta);
int64_t gf_file_set_owner(int64_t context, int64_t file, int64_t uid, int64_t gid, gf_meta meta);
int64_t gf_file_set_times(int64_t context, int64_t file,
                         int64_t accessed_action, int64_t accessed_seconds, int64_t accessed_nanos,
                         int64_t modified_action, int64_t modified_seconds, int64_t modified_nanos, gf_meta meta);
int64_t gf_file_metadata(int64_t context, int64_t file, gf_meta fields);
int64_t gf_file_try_lock(int64_t context, int64_t file, int64_t mode, gf_meta meta);
_Bool gf_lock_contended(int64_t error);
int64_t gf_lock_pause(void);
/* Root tokens use the same registry with strict kind checks. */
int64_t gf_root_open(int64_t context, gf_input path, gf_meta meta);
int64_t gf_root_open_at(int64_t context, int64_t root, gf_input path, gf_meta meta);
int64_t gf_file_open_at(int64_t context, int64_t root, gf_input path, int64_t options, int64_t permissions, gf_meta meta);
int64_t gf_root_close(int64_t context, int64_t root, gf_meta meta);
int64_t gf_root_metadata(int64_t context, int64_t root, gf_meta fields);
int64_t gf_root_read(int64_t context, int64_t root, gf_input path, gf_output out, gf_meta meta);
int64_t gf_root_list(int64_t context, int64_t root, gf_input path, gf_output out, int64_t entries, gf_meta meta);
int64_t gf_root_list_sized(int64_t context, int64_t root, gf_input path, int64_t bytes, gf_output out, int64_t entries, gf_meta meta);
int64_t gf_root_create(int64_t context, int64_t root, gf_input path, gf_input data, gf_meta meta);
int64_t gf_root_mkdir(int64_t context, int64_t root, gf_input path, gf_meta meta);
int64_t gf_root_unlink(int64_t context, int64_t root, gf_input path, _Bool directory, gf_meta meta);
int64_t gf_root_metadata_at(int64_t context, int64_t root, gf_input path, gf_meta fields);
#endif
