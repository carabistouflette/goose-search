/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GOOSE_FS_INTERNAL_H
#define GOOSE_FS_INTERNAL_H
#include "fs.h"
#include <stdbool.h>
#include <string.h>
#if !defined(__linux__) || UINTPTR_MAX != UINT64_MAX
#error "goose-fs currently requires 64-bit Linux"
#endif
enum { GF_OK, GF_INVALID, GF_LIMIT, GF_NOT_REGULAR, GF_SYSTEM, GF_NEED_SPACE };
enum { GF_MAX_PATH = 4095, GF_MAX_BYTES = 1048576, GF_MAX_ENTRIES = 4096 };
bool gf_overlap(const void *a, int64_t an, const void *b, int64_t bn);
bool gf_path_copy(gf_input path, char text[GF_MAX_PATH + 1]);
int gf_parent(char *text, char **leaf);
int gf_parent_checked(char *text, char **leaf, int64_t *cleanup);
bool gf_relative_copy(gf_input path, char text[GF_MAX_PATH + 1]);
int gf_parent_relative(int root, char *text, char **leaf);
int gf_parent_relative_checked(int root, char *text, char **leaf, int64_t *cleanup);
bool gf_read_link_arguments(gf_input path, int64_t limit, gf_output out, gf_meta meta);
int gf_read_link_owned_parent(int dir, const char *leaf, int64_t limit, gf_output out, gf_meta meta);
int gf_read_regular(int dir, const char *leaf, gf_output out, int64_t *length);
int gf_list_dir(int dir, const char *leaf, gf_output out, int64_t cap, int64_t *length, int64_t *count);
int gf_list_dir_sized(int dir, const char *leaf, gf_output out, int64_t bytes, int64_t cap,
                      int64_t *length, int64_t *count, int64_t *needed);
bool gf_list_arguments(gf_input path, gf_output out, int64_t bytes, int64_t entries, gf_meta meta);
int gf_create_file(int dir, const char *leaf, gf_input data);
bool gf_rename_arguments(gf_input from, gf_input to, int64_t flags, gf_meta meta);
int gf_rename_paths(int root, bool relative, char *from, char *to, int64_t flags, int64_t *meta);
int gf_metadata_at(int dir, const char *leaf, int flags, int64_t *out);
#endif
