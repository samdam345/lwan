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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwan-private.h"

#include "lwan-array.h"
#include "lwan-coro.h"

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#endif

#if !defined(SIGSTKSZ)
#define SIGSTKSZ 16384
#endif

#ifdef HAVE_BROTLI
#define CORO_STACK_MIN (8 * SIGSTKSZ)
#else
#define CORO_STACK_MIN (4 * SIGSTKSZ)
#endif

static_assert(DEFAULT_BUFFER_SIZE < (CORO_STACK_MIN + SIGSTKSZ),
              "Request buffer fits inside coroutine stack");

typedef void (*defer1_func)(void *data);
typedef void (*defer2_func)(void *data1, void *data2);

struct coro_defer {
    union {
        struct {
            defer1_func func;
            void *data;
        } one;
        struct {
            defer2_func func;
            void *data1;
            void *data2;
        } two;
    };
    bool has_two_args;
};

DEFINE_ARRAY_TYPE_INLINEFIRST(coro_defer_array, struct coro_defer)

struct coro {
    struct coro_switcher *switcher;
    coro_context context;
    struct coro_defer_array defer;

    int yield_value;

#if !defined(NDEBUG) && defined(HAVE_VALGRIND)
    unsigned int vg_stack_id;
#endif

    unsigned char stack[] __attribute__((aligned(64)));
};

#if defined(__APPLE__)
#define ASM_SYMBOL(name_) "_" #name_
#else
#define ASM_SYMBOL(name_) #name_
#endif

#define ASM_ROUTINE(name_)                                                     \
    ".globl " ASM_SYMBOL(name_) "\n\t" ASM_SYMBOL(name_) ":\n\t"

/*
 * This swapcontext() implementation was obtained from glibc and modified
 * slightly to not save/restore the floating point registers, unneeded
 * registers, and signal mask.  It is Copyright (C) 2001, 2002, 2003 Free
 * Software Foundation, Inc and are distributed under GNU LGPL version 2.1
 * (or later).  I'm not sure if I can distribute them inside a GPL program;
 * they're straightforward so I'm assuming there won't be any problem; if
 * there is, I'll just roll my own.
 *     -- Leandro
 */
#if defined(__x86_64__)
void __attribute__((noinline, visibility("internal")))
coro_swapcontext(coro_context *current, coro_context *other);
asm(".text\n\t"
    ".p2align 4\n\t"
    ASM_ROUTINE(coro_swapcontext)
    "movq   %rbx,0(%rdi)\n\t"
    "movq   %rbp,8(%rdi)\n\t"
    "movq   %r12,16(%rdi)\n\t"
    "movq   %r13,24(%rdi)\n\t"
    "movq   %r14,32(%rdi)\n\t"
    "movq   %r15,40(%rdi)\n\t"
    "movq   %rdi,48(%rdi)\n\t"
    "movq   %rsi,56(%rdi)\n\t"
    "movq   (%rsp),%rcx\n\t"
    "movq   %rcx,64(%rdi)\n\t"
    "leaq   0x8(%rsp),%rcx\n\t"
    "movq   %rcx,72(%rdi)\n\t"
    "movq   72(%rsi),%rsp\n\t"
    "movq   0(%rsi),%rbx\n\t"
    "movq   8(%rsi),%rbp\n\t"
    "movq   16(%rsi),%r12\n\t"
    "movq   24(%rsi),%r13\n\t"
    "movq   32(%rsi),%r14\n\t"
    "movq   40(%rsi),%r15\n\t"
    "movq   48(%rsi),%rdi\n\t"
    "movq   64(%rsi),%rcx\n\t"
    "movq   56(%rsi),%rsi\n\t"
    "jmpq   *%rcx\n\t");
#elif defined(__i386__)
void __attribute__((noinline, visibility("internal")))
coro_swapcontext(coro_context *current, coro_context *other);
asm(".text\n\t"
    ".p2align 16\n\t"
    ASM_ROUTINE(coro_swapcontext)
    "movl   0x4(%esp),%eax\n\t"
    "movl   %ecx,0x1c(%eax)\n\t" /* ECX */
    "movl   %ebx,0x0(%eax)\n\t"  /* EBX */
    "movl   %esi,0x4(%eax)\n\t"  /* ESI */
    "movl   %edi,0x8(%eax)\n\t"  /* EDI */
    "movl   %ebp,0xc(%eax)\n\t"  /* EBP */
    "movl   (%esp),%ecx\n\t"
    "movl   %ecx,0x14(%eax)\n\t" /* EIP */
    "leal   0x4(%esp),%ecx\n\t"
    "movl   %ecx,0x18(%eax)\n\t" /* ESP */
    "movl   8(%esp),%eax\n\t"
    "movl   0x14(%eax),%ecx\n\t" /* EIP (1) */
    "movl   0x18(%eax),%esp\n\t" /* ESP */
    "pushl  %ecx\n\t"            /* EIP (2) */
    "movl   0x0(%eax),%ebx\n\t"  /* EBX */
    "movl   0x4(%eax),%esi\n\t"  /* ESI */
    "movl   0x8(%eax),%edi\n\t"  /* EDI */
    "movl   0xc(%eax),%ebp\n\t"  /* EBP */
    "movl   0x1c(%eax),%ecx\n\t" /* ECX */
    "ret\n\t");
#else
#define coro_swapcontext(cur, oth) swapcontext(cur, oth)
#endif

#ifndef __x86_64__
static void
coro_entry_point(struct coro *coro, coro_function_t func, void *data)
{
    int return_value = func(coro, data);
    coro_yield(coro, return_value);
}
#else
void __attribute__((noinline, visibility("internal")))
coro_entry_point(struct coro *coro, coro_function_t func, void *data);
asm(".text\n\t"
    ".p2align 4\n\t"
    ASM_ROUTINE(coro_entry_point)
    "pushq %rbx\n\t"
    "movq  %rdi, %rbx\n\t" /* coro = rdi */
    "movq  %rsi, %rdx\n\t" /* func = rsi */
    "movq  %r15, %rsi\n\t" /* data = r15 */
    "call  *%rdx\n\t"      /* eax = func(coro, data) */
    "movq  (%rbx), %rsi\n\t"
    "movl  %eax, 0x68(%rbx)\n\t" /* coro->yield_value eax */
    "popq  %rbx\n\t"
    "leaq  0x50(%rsi), %rdi\n\t" /* get coro context from coro */
    "jmp   " ASM_SYMBOL(coro_swapcontext) "\n\t");
#endif

void coro_deferred_run(struct coro *coro, size_t generation)
{
    struct lwan_array *array = (struct lwan_array *)&coro->defer;
    struct coro_defer *defers = array->base;

    for (size_t i = array->elements; i != generation; i--) {
        struct coro_defer *defer = &defers[i - 1];

        if (defer->has_two_args)
            defer->two.func(defer->two.data1, defer->two.data2);
        else
            defer->one.func(defer->one.data);
    }

    array->elements = generation;
}

ALWAYS_INLINE size_t coro_deferred_get_generation(const struct coro *coro)
{
    const struct lwan_array *array = (struct lwan_array *)&coro->defer;

    return array->elements;
}

void coro_reset(struct coro *coro, coro_function_t func, void *data)
{
    unsigned char *stack = coro->stack;

    coro_deferred_run(coro, 0);
    coro_defer_array_reset(&coro->defer);

#if defined(__x86_64__)
    /* coro_entry_point() for x86-64 has 3 arguments, but RDX isn't
     * stored.  Use R15 instead, and implement the trampoline
     * function in assembly in order to use this register when
     * calling the user function. */
    coro->context[5 /* R15 */] = (uintptr_t)data;
    coro->context[6 /* RDI */] = (uintptr_t)coro;
    coro->context[7 /* RSI */] = (uintptr_t)func;
    coro->context[8 /* RIP */] = (uintptr_t)coro_entry_point;

    /* Ensure stack is properly aligned: it should be aligned to a
     * 16-bytes boundary so SSE will work properly, but should be
     * aligned on an 8-byte boundary right after calling a function. */
    uintptr_t rsp = (uintptr_t)stack + CORO_STACK_MIN;

#define STACK_PTR 9
    coro->context[STACK_PTR] = (rsp & ~0xful) - 0x8ul;
#elif defined(__i386__)
    stack = (unsigned char *)(uintptr_t)(stack + CORO_STACK_MIN);

    /* Make room for 3 args */
    stack -= sizeof(uintptr_t) * 3;
    /* Ensure 4-byte alignment */
    stack = (unsigned char *)((uintptr_t)stack & (uintptr_t)~0x3);

    uintptr_t *argp = (uintptr_t *)stack;
    *argp++ = 0;
    *argp++ = (uintptr_t)coro;
    *argp++ = (uintptr_t)func;
    *argp++ = (uintptr_t)data;

    coro->context[5 /* EIP */] = (uintptr_t)coro_entry_point;

#define STACK_PTR 6
    coro->context[STACK_PTR] = (uintptr_t)stack;
#else
    getcontext(&coro->context);

    coro->context.uc_stack.ss_sp = stack;
    coro->context.uc_stack.ss_size = CORO_STACK_MIN;
    coro->context.uc_stack.ss_flags = 0;
    coro->context.uc_link = NULL;

    makecontext(&coro->context, (void (*)())coro_entry_point, 3, coro, func,
                data);
#endif
}

ALWAYS_INLINE struct coro *
coro_new(struct coro_switcher *switcher, coro_function_t function, void *data)
{
    struct coro *coro = lwan_aligned_alloc(sizeof(struct coro) + CORO_STACK_MIN, 64);

    if (UNLIKELY(!coro))
        return NULL;

    if (UNLIKELY(coro_defer_array_init(&coro->defer) < 0)) {
        free(coro);
        return NULL;
    }

    coro->switcher = switcher;
    coro_reset(coro, function, data);

#if !defined(NDEBUG) && defined(HAVE_VALGRIND)
    unsigned char *stack = coro->stack;
    coro->vg_stack_id = VALGRIND_STACK_REGISTER(stack, stack + CORO_STACK_MIN);
#endif

    return coro;
}

ALWAYS_INLINE int coro_resume(struct coro *coro)
{
    assert(coro);

#if defined(STACK_PTR)
    assert(coro->context[STACK_PTR] >= (uintptr_t)coro->stack &&
           coro->context[STACK_PTR] <=
               (uintptr_t)(coro->stack + CORO_STACK_MIN));
#endif

    coro_swapcontext(&coro->switcher->caller, &coro->context);
    memcpy(&coro->context, &coro->switcher->callee, sizeof(coro->context));

    return coro->yield_value;
}

ALWAYS_INLINE int coro_resume_value(struct coro *coro, int value)
{
    assert(coro);

    coro->yield_value = value;
    return coro_resume(coro);
}

ALWAYS_INLINE int coro_yield(struct coro *coro, int value)
{
    assert(coro);
    coro->yield_value = value;
    coro_swapcontext(&coro->switcher->callee, &coro->switcher->caller);
    return coro->yield_value;
}

void coro_free(struct coro *coro)
{
    assert(coro);
#if !defined(NDEBUG) && defined(HAVE_VALGRIND)
    VALGRIND_STACK_DEREGISTER(coro->vg_stack_id);
#endif
    coro_deferred_run(coro, 0);
    coro_defer_array_reset(&coro->defer);
    free(coro);
}

ALWAYS_INLINE void coro_defer(struct coro *coro, defer1_func func, void *data)
{
    struct coro_defer *defer = coro_defer_array_append(&coro->defer);

    if (UNLIKELY(!defer)) {
        lwan_status_error("Could not add new deferred function for coro %p",
                          coro);
        return;
    }

    defer->one.func = func;
    defer->one.data = data;
    defer->has_two_args = false;
}

ALWAYS_INLINE void
coro_defer2(struct coro *coro, defer2_func func, void *data1, void *data2)
{
    struct coro_defer *defer = coro_defer_array_append(&coro->defer);

    if (UNLIKELY(!defer)) {
        lwan_status_error("Could not add new deferred function for coro %p",
                          coro);
        return;
    }

    defer->two.func = func;
    defer->two.data1 = data1;
    defer->two.data2 = data2;
    defer->has_two_args = true;
}

void *coro_malloc_full(struct coro *coro,
                       size_t size,
                       void (*destroy_func)(void *data))
{
    void *ptr = malloc(size);

    if (LIKELY(ptr))
        coro_defer(coro, destroy_func, ptr);

    return ptr;
}

inline void *coro_malloc(struct coro *coro, size_t size)
{
    return coro_malloc_full(coro, size, free);
}

char *coro_strndup(struct coro *coro, const char *str, size_t max_len)
{
    const size_t len = strnlen(str, max_len) + 1;
    char *dup = coro_malloc(coro, len);

    if (LIKELY(dup)) {
        memcpy(dup, str, len);
        dup[len - 1] = '\0';
    }
    return dup;
}

char *coro_strdup(struct coro *coro, const char *str)
{
    return coro_strndup(coro, str, SSIZE_MAX - 1);
}

char *coro_printf(struct coro *coro, const char *fmt, ...)
{
    va_list values;
    int len;
    char *tmp_str;

    va_start(values, fmt);
    len = vasprintf(&tmp_str, fmt, values);
    va_end(values);

    if (UNLIKELY(len < 0))
        return NULL;

    coro_defer(coro, free, tmp_str);
    return tmp_str;
}
