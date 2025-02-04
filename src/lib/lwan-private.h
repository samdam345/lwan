/*
 * lwan - simple web server
 * Copyright (c) 2012 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#pragma once

#include <stdlib.h>
#include <limits.h>

#include "lwan.h"

struct lwan_fd_watch *lwan_watch_fd(struct lwan *l,
                                    int fd,
                                    uint32_t events,
                                    coro_function_t coro_fn,
                                    void *data);
void lwan_unwatch_fd(struct lwan *l, struct lwan_fd_watch *w);

void lwan_set_thread_name(const char *name);

void lwan_response_init(struct lwan *l);
void lwan_response_shutdown(struct lwan *l);

void lwan_socket_init(struct lwan *l);
void lwan_socket_shutdown(struct lwan *l);

void lwan_thread_init(struct lwan *l);
void lwan_thread_shutdown(struct lwan *l);
void lwan_thread_add_client(struct lwan_thread *t, int fd);
void lwan_thread_nudge(struct lwan_thread *t);

void lwan_status_init(struct lwan *l);
void lwan_status_shutdown(struct lwan *l);

void lwan_job_thread_init(void);
void lwan_job_thread_shutdown(void);
void lwan_job_add(bool (*cb)(void *data), void *data);
void lwan_job_del(bool (*cb)(void *data), void *data);

void lwan_tables_init(void);
void lwan_tables_shutdown(void);

void lwan_readahead_init(void);
void lwan_readahead_shutdown(void);
void lwan_readahead_queue(int fd, off_t off, size_t size);
void lwan_madvise_queue(void *addr, size_t size);

char *lwan_process_request(struct lwan *l, struct lwan_request *request,
                           struct lwan_value *buffer, char *next_request);
size_t lwan_prepare_response_header_full(struct lwan_request *request,
     enum lwan_http_status status, char headers[],
     size_t headers_buf_size, const struct lwan_key_value *additional_headers);

void lwan_straitjacket_enforce_from_config(struct config *c);

const char *lwan_get_config_path(char *path_buf, size_t path_buf_len);

uint8_t lwan_char_isspace(char ch) __attribute__((pure));
uint8_t lwan_char_isxdigit(char ch) __attribute__((pure));
uint8_t lwan_char_isdigit(char ch) __attribute__((pure));

static ALWAYS_INLINE size_t lwan_nextpow2(size_t number)
{
#if defined(HAVE_BUILTIN_CLZLL)
    static const int size_bits = (int)sizeof(number) * CHAR_BIT;

    if (sizeof(size_t) == sizeof(unsigned int)) {
        return (size_t)1 << (size_bits - __builtin_clz((unsigned int)number));
    } else if (sizeof(size_t) == sizeof(unsigned long)) {
        return (size_t)1 << (size_bits - __builtin_clzl((unsigned long)number));
    } else if (sizeof(size_t) == sizeof(unsigned long long)) {
        return (size_t)1 << (size_bits - __builtin_clzll((unsigned long long)number));
    } else {
        (void)size_bits;
    }
#endif

    number--;
    number |= number >> 1;
    number |= number >> 2;
    number |= number >> 4;
    number |= number >> 8;
    number |= number >> 16;

    return number + 1;
}


#ifdef HAVE_LUA
#include <lua.h>

lua_State *lwan_lua_create_state(const char *script_file, const char *script);
void lwan_lua_state_push_request(lua_State *L, struct lwan_request *request);
const char *lwan_lua_state_last_error(lua_State *L);
#endif

#ifdef __APPLE__
#  define SECTION_START(name_) \
        __start_ ## name_[] __asm("section$start$__DATA$" #name_)
#  define SECTION_END(name_) \
        __stop_ ## name_[] __asm("section$end$__DATA$" #name_)
#else
#  define SECTION_START(name_) __start_ ## name_[]
#  define SECTION_END(name_) __stop_ ## name_[]
#endif

extern clockid_t monotonic_clock_id;

static inline void *
lwan_aligned_alloc(size_t n, size_t alignment)
{
    void *ret;

    assert((alignment & (alignment - 1)) == 0);

    n = (n + alignment - 1) & ~(alignment - 1);
    if (UNLIKELY(posix_memalign(&ret, alignment, n)))
        return NULL;

    return ret;
}
