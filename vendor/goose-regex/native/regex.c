/* SPDX-License-Identifier: Apache-2.0 */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "regex.h"

enum { SLOTS = 32, MAX_PAIRS = 64, MAX_PATTERN = 16384, GR_MAX_INPUT = 1048576,
       MAX_MEMORY = 8388608, MAX_WORK = 1000000, MAX_DEPTH = 1000 };
typedef union { max_align_t alignment; size_t charge; } header;
typedef struct {
    int64_t token;
    int busy;
    size_t used, limit;
    int failure;
    uint32_t pairs;
    int64_t input_limit;
    pcre2_code *code;
    pcre2_match_data *data;
    pcre2_match_context *match;
} slot;
static slot slots[SLOTS];
static int64_t sequence;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#ifdef GR_TEST
/* Allocation-site injection, configured only while test calls are quiescent. */
static _Thread_local int64_t fail_after = -1;
static void (*find_hook)(void);
void gr_test_find_hook(void (*hook)(void)) { find_hook = hook; }
void gr_test_fail_after(int64_t n) { fail_after = n; }
#endif
static void *allocate(PCRE2_SIZE n, void *opaque) {
    slot *s = opaque;
    if (n > SIZE_MAX - sizeof(header) || n + sizeof(header) > s->limit - s->used) {
        s->failure = GR_LIMIT; return NULL;
    }
#ifdef GR_TEST
    if (fail_after == 0) { s->failure = GR_MEMORY; return NULL; }
    if (fail_after > 0) --fail_after;
#endif
    header *h = malloc(sizeof(header) + n);
    if (!h) { s->failure = GR_MEMORY; return NULL; }
    h->charge = sizeof(header) + n;
    s->used += h->charge;
    return h + 1;
}
static void release(void *p, void *opaque) {
    if (!p) return;
    slot *s = opaque;
    header *h = (header *)p - 1;
    s->used -= h->charge;
    free(h);
}
static int valid(const void *p, int64_t n, size_t width) {
    return n >= 0 && (uint64_t)n <= SIZE_MAX / width &&
        (!n || (p && (uintptr_t)p <= UINTPTR_MAX - (size_t)n * width));
}
static int overlap(const void *a, size_t an, const void *b, size_t bn) {
    if (!an || !bn) return 0;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}
/* Goose can provide unaligned packed-struct slices. Never dereference int64_t*. */
static int64_t get(gr_ints a, size_t i) {
    int64_t value; memcpy(&value, (const uint8_t *)a.data + i * 8, 8); return value;
}
static void put(gr_ints a, size_t i, int64_t value) {
    memcpy((uint8_t *)a.data + i * 8, &value, 8);
}
static void clear(gr_ints a, int64_t value) {
    for (int64_t i = 0; i < a.len; ++i) put(a, (size_t)i, value);
}
static void dispose(slot *s) {
    pcre2_match_data_free(s->data);
    pcre2_match_context_free(s->match);
    pcre2_code_free(s->code);
    s->data = NULL; s->match = NULL; s->code = NULL;
}
static void unlease(slot *s, int remove) {
    pthread_mutex_lock(&mutex);
    if (remove) s->token = 0;
    s->busy = 0;
    pthread_mutex_unlock(&mutex);
}
static slot *lease(int64_t token, int *error) {
    slot *s = NULL;
    *error = GR_HANDLE;
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; i < SLOTS; ++i) {
        if (token > 0 && slots[i].token == token) {
            if (slots[i].busy) *error = GR_BUSY;
            else { s = &slots[i]; s->busy = 1; *error = GR_OK; }
            break;
        }
    }
    pthread_mutex_unlock(&mutex);
    return s;
}
int64_t gr_compile(gr_bytes pattern, gr_ints config, gr_ints meta) {
    if (!valid(pattern.data, pattern.len, 1) || !valid(config.data, config.len, 8) ||
        !valid(meta.data, meta.len, 8) || config.len != 4 || meta.len != 4 ||
        overlap(meta.data, 32, config.data, 32) ||
        overlap(meta.data, 32, pattern.data, (size_t)pattern.len)) return GR_INPUT;
    clear(meta, 0); put(meta, 3, -1);
    if (pattern.len > MAX_PATTERN) return GR_LIMIT;
    int64_t input = get(config, 0), memory = get(config, 1);
    int64_t work = get(config, 2), depth = get(config, 3);
    if (input < 0 || input > GR_MAX_INPUT || memory < 1 || memory > MAX_MEMORY ||
        work < 1 || work > MAX_WORK || depth < 1 || depth > MAX_DEPTH) return GR_INPUT;
    slot *s = NULL;
    pthread_mutex_lock(&mutex);
    if (sequence < INT64_MAX) {
        for (size_t i = 0; i < SLOTS; ++i) if (!slots[i].token && !slots[i].busy) {
            s = &slots[i]; memset(s, 0, sizeof *s);
            s->busy = 1; s->token = ++sequence; break;
        }
    }
    pthread_mutex_unlock(&mutex);
    if (!s) return GR_LIMIT;
    s->limit = (size_t)memory; s->input_limit = input;
    pcre2_general_context *general = pcre2_general_context_create(allocate, release, s);
    pcre2_compile_context *context = general ? pcre2_compile_context_create(general) : NULL;
    int error = 0; PCRE2_SIZE at = 0;
    if (context) {
        pcre2_set_parens_nest_limit(context, 64);
        pcre2_set_newline(context, PCRE2_NEWLINE_LF);
        pcre2_set_bsr(context, PCRE2_BSR_ANYCRLF);
        static const uint8_t empty = 0;
        s->code = pcre2_compile(pattern.len ? pattern.data : &empty, (size_t)pattern.len,
            PCRE2_NEVER_UTF | PCRE2_NEVER_UCP, &error, &at, context);
    }
    int result = GR_OK;
    if (!s->code) {
        result = s->failure ? s->failure : context ? GR_PATTERN : GR_MEMORY;
        put(meta, 2, error);
        if (context && result == GR_PATTERN) put(meta, 3, (int64_t)at);
    } else {
        uint32_t captures = 0;
        if (pcre2_pattern_info(s->code, PCRE2_INFO_CAPTURECOUNT, &captures)) result = GR_BACKEND;
        else if (captures >= MAX_PAIRS) result = GR_LIMIT;
        else {
            s->pairs = captures + 1;
            s->data = pcre2_match_data_create(s->pairs, general);
            s->match = pcre2_match_context_create(general);
            if (!s->data || !s->match) result = s->failure ? s->failure : GR_MEMORY;
            else {
                pcre2_set_match_limit(s->match, (uint32_t)work);
                pcre2_set_depth_limit(s->match, (uint32_t)depth);
                pcre2_set_heap_limit(s->match, (uint32_t)((memory + 1023) / 1024));
            }
        }
    }
    pcre2_compile_context_free(context);
    pcre2_general_context_free(general);
    if (result != GR_OK) dispose(s);
    else { put(meta, 0, s->token); put(meta, 1, s->pairs); }
    unlease(s, result != GR_OK);
    return result;
}
int64_t gr_find(int64_t token, gr_bytes subject, int64_t start, gr_ints offsets, gr_ints meta) {
    if (!valid(subject.data, subject.len, 1) || !valid(offsets.data, offsets.len, 8) ||
        !valid(meta.data, meta.len, 8) || meta.len != 3 || offsets.len > 128 ||
        overlap(meta.data, 24, offsets.data, (size_t)offsets.len * 8) ||
        overlap(meta.data, 24, subject.data, (size_t)subject.len) ||
        overlap(offsets.data, (size_t)offsets.len * 8, subject.data, (size_t)subject.len)) return GR_INPUT;
    clear(meta, 0); clear(offsets, -1);
    if (start < 0 || start > subject.len) return GR_INPUT;
    int error; slot *s = lease(token, &error);
    if (!s) return error;
#ifdef GR_TEST
    if (find_hook) find_hook();
#endif
    int result = GR_OK;
    if (subject.len > s->input_limit) result = GR_LIMIT;
    else if (offsets.len < (int64_t)s->pairs * 2) result = GR_INPUT;
    else {
        s->failure = 0;
        static const uint8_t empty = 0;
        int rc = pcre2_match(s->code, subject.len ? subject.data : &empty,
            (size_t)subject.len, (size_t)start, 0, s->data, s->match);
        if (rc == PCRE2_ERROR_NOMATCH) { /* Empty output, successful search. */ }
        else if (rc <= 0) {
            put(meta, 2, rc);
            if (s->failure) result = s->failure;
            else if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
                rc == PCRE2_ERROR_HEAPLIMIT) result = GR_LIMIT;
            else if (rc == PCRE2_ERROR_NOMEMORY) result = GR_MEMORY;
            else result = GR_BACKEND;
        } else {
            PCRE2_SIZE *v = pcre2_get_ovector_pointer(s->data);
            for (size_t i = 0; i < s->pairs; ++i) {
                if (v[i*2] == PCRE2_UNSET && v[i*2+1] == PCRE2_UNSET) continue;
                if (v[i*2] > v[i*2+1] || v[i*2+1] > (size_t)subject.len) {
                    result = GR_BACKEND; break;
                }
            }
            if (result == GR_OK) {
                for (size_t i = 0; i < s->pairs; ++i) if (v[i*2] != PCRE2_UNSET) {
                    put(offsets, i*2, (int64_t)v[i*2]); put(offsets, i*2+1, (int64_t)v[i*2+1]);
                }
                put(meta, 0, 1); put(meta, 1, s->pairs);
            }
        }
    }
    unlease(s, 0);
    return result;
}
int64_t gr_close(int64_t token) {
    int error; slot *s = lease(token, &error);
    if (!s) return error;
    dispose(s);
    unlease(s, 1);
    return GR_OK;
}
