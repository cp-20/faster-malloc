/*
 * fm_core.h -- shared allocator core for the fastmalloc family.
 *
 * Design summary
 * --------------
 * A span is a 64 KiB-aligned region whose first 64 bytes are a header that
 * records the size class.  Every small allocation is carved out of a span
 * that serves exactly one size class, so free() recovers the class with one
 * AND and one load: there is no per-block metadata and nothing is stored in
 * front of the user's data.
 *
 * Each thread owns one free list per size class.  Allocation is a pop,
 * deallocation is a push, and neither touches shared state.
 *
 * When a local list grows past its limit it is handed to the *central* list
 * of that class in one atomic operation.  The central list is a Treiber stack
 * of batches: a refill takes a whole batch with one CAS, a flush publishes a
 * whole batch with one CAS.  No lock is ever taken on the allocation path.
 *
 * Counting
 * --------
 * fm_cnt[c] counts the frees performed on class c since the last refill or
 * flush.  It is *not* the length of the local list: mallocs pop from the
 * list without touching it, which keeps the allocation fast path down to a
 * single dependent load.  The count is used only to decide when to flush.
 * Because a flush publishes the whole list rather than "count" blocks, the
 * two never have to agree.
 *
 * Hot-path layout
 * ----------------
 * The thread-local state is split into parallel arrays rather than an array
 * of structs.  That matters: the head array has an 8-byte stride, so the
 * compiler folds the index into the addressing mode of a single
 * %fs:0x0(base,index,8) load.  An array of 32-byte structs cannot be
 * addressed that way and costs an extra shift and add on the critical path.
 */
#ifndef FM_CORE_H
#define FM_CORE_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef FM_DEBUG
#include <stdio.h>
#endif

#include "size_classes2.h"

#define FM_SPAN_MAGIC 0x5A6E4C5Au
#define FM_ALIGNED_MAGIC 0xA110CEDAu
#define FM_PAGE_SIZE 4096u
#define FM_ALIGN_UP(x, a) (((uintptr_t)(x) + (a)-1) & ~((uintptr_t)(a)-1))

static inline void *fm_malloc(size_t size);

/* Thread-local free list state, one parallel array per field so that every
 * hot access uses an 8-byte (or 4-byte) stride. */
extern __thread void *fm_head[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
extern __thread void *fm_tail[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
extern __thread uint32_t fm_cnt[CLASS_COUNT] __attribute__((tls_model("initial-exec")));

typedef struct FM_Span {
    uint32_t cls;       /* size class, or LARGE_CLASS */
    uint32_t magic;
    uint32_t nblocks;
    uint32_t pad0;
    uintptr_t map_base; /* start of the mapping (for munmap) */
    size_t map_size;
    size_t user_size;   /* bytes requested (large / aligned) */
    size_t align;       /* alignment requested (aligned allocs) */
    char reserved[64 - 48];
} FM_Span;

typedef struct FM_Batch FM_Batch;
struct FM_Batch {
    FM_Batch *next;
    void *head;
    void *tail;
    uint32_t count;
    uint32_t pad;
};

typedef struct FM_Central {
    _Atomic(FM_Batch *) head;
    char pad[64 - sizeof(_Atomic(FM_Batch *))];
} FM_Central;

extern FM_Central fm_central[CLASS_COUNT];
extern __thread FM_Batch *fm_batch_pool __attribute__((tls_model("initial-exec")));

/* ------------------------------------------------------------------ */
/* batch pool                                                          */
/* ------------------------------------------------------------------ */

static inline FM_Batch *fm_batch_alloc(void)
{
    FM_Batch *b = fm_batch_pool;
    if (__builtin_expect(b != NULL, 1)) {
        fm_batch_pool = b->next;
        b->next = NULL;
        return b;
    }
    char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return NULL;
    FM_Batch *first = (FM_Batch *)page;
    for (int i = 0; i < 127; i++)
        first[i].next = &first[i + 1];
    first[127].next = NULL;
    fm_batch_pool = &first[127];
    return &first[0];
}

static inline void fm_batch_release(FM_Batch *b)
{
    b->next = fm_batch_pool;
    fm_batch_pool = b;
}

/* ------------------------------------------------------------------ */
/* central list: a Treiber stack of batches                            */
/* ------------------------------------------------------------------ */

/*
 * A batch is a whole free list, not a fragment of one.  Publishing a local
 * list to the central list is therefore O(1): the tail pointer is already
 * known, so no list has to be walked.  Walking would be expensive -- the
 * blocks of a free list are scattered across the heap, so each step is a
 * likely cache miss -- and it is on the critical path of every flush.
 *
 * A refill takes a whole batch, again O(1).
 */

static inline void fm_central_push(uint32_t cls, FM_Batch *b)
{
    FM_Batch *old = atomic_load_explicit(&fm_central[cls].head, memory_order_relaxed);
    do {
        b->next = old;
    } while (!atomic_compare_exchange_weak_explicit(&fm_central[cls].head, &old, b,
                                                    memory_order_release,
                                                    memory_order_relaxed));
}

/* ------------------------------------------------------------------ */
/* spans                                                               */
/* ------------------------------------------------------------------ */

/* Map `bytes` usable bytes at a 64 KiB-aligned address, with a 64 byte
 * header in front.  The mapping is trimmed to exactly the pages needed. */
static inline char *fm_map_aligned(size_t bytes)
{
    size_t need = bytes + HEADER_SIZE;
    size_t map_len = need + SPAN_SIZE + FM_PAGE_SIZE;
    char *raw = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;
    uintptr_t base = FM_ALIGN_UP((uintptr_t)raw, SPAN_SIZE);
    if (base != (uintptr_t)raw)
        munmap(raw, (size_t)(base - (uintptr_t)raw));
    size_t keep = FM_ALIGN_UP(need, FM_PAGE_SIZE);
    size_t tail_start = base + keep;
    size_t map_end = (size_t)FM_ALIGN_UP((uintptr_t)raw + map_len, FM_PAGE_SIZE);
    if (tail_start < map_end)
        munmap((void *)tail_start, map_end - tail_start);
    return (char *)base;
}

/* Carve a fresh span of the given class into a linked list of blocks, in
 * ascending address order. */
static inline void *fm_span_new(uint32_t cls, void **tail_out, uint32_t *count_out)
{
    size_t bs = kClassSize[cls];
    size_t n = (SPAN_SIZE - HEADER_SIZE) / bs;
    char *base = fm_map_aligned(SPAN_SIZE - HEADER_SIZE);
    if (!base)
        return NULL;

    FM_Span *sp = (FM_Span *)base;
    sp->cls = cls;
    sp->magic = FM_SPAN_MAGIC;
    sp->nblocks = (uint32_t)n;
    sp->map_base = (uintptr_t)base;
    sp->map_size = SPAN_SIZE;
    sp->user_size = 0;
    sp->align = 0;

    char *p = base + HEADER_SIZE;
    void *head = p;
    void *tail = p;
    for (size_t i = 1; i < n; i++) {
        void *prev = tail;
        tail = (char *)tail + bs;
        *(void **)prev = tail;
    }
    *(void **)tail = NULL;
#ifdef FM_DEBUG
    for (size_t i = 0; i < n; i++)
        ((uint64_t *)((char *)head + i * bs))[1] = 0xDEADDEADDEADDEADull;
#endif

    if (tail_out)
        *tail_out = tail;
    if (count_out)
        *count_out = (uint32_t)n;
    return head;
}

/* ------------------------------------------------------------------ */
/* refill / flush                                                      */
/* ------------------------------------------------------------------ */

__attribute__((noinline)) static void fm_flush_bin(uint32_t cls);

/*
 * Publish the whole local list of `cls` to the central list.  O(1): the tail
 * is already known, so nothing has to be walked.  Walking would be
 * expensive -- the blocks of a free list are scattered across the heap, so
 * every step is a likely cache miss -- and it sits on the critical path of
 * every flush.
 */
__attribute__((noinline)) static void fm_publish(void *head, void *tail, uint32_t cls)
{
    FM_Batch *nb = fm_batch_alloc();
    if (!nb) {
        /* Out of batch headers: the blocks are unreachable but the memory
         * is still owned by the process.  Drop them. */
        return;
    }
    nb->head = head;
    nb->tail = tail;
    nb->count = 0;
    fm_central_push(cls, nb);
}

/* Take the next block of a class, refilling from the central list or from a
 * fresh span when needed.  Kept out of line so that the malloc fast path
 * needs no callee-saved registers and no stack frame. */
__attribute__((noinline)) void *fm_refill_class(uint32_t cls)
{
    uint32_t limit = kClassLimit[cls];

    /* Try the central list first: take a whole batch, O(1). */
    {
        FM_Batch *b = atomic_load_explicit(&fm_central[cls].head, memory_order_relaxed);
        while (b) {
            FM_Batch *next = b->next;
            if (!atomic_compare_exchange_weak_explicit(&fm_central[cls].head, &b, next,
                                                       memory_order_acquire,
                                                       memory_order_relaxed))
                continue; /* b was reloaded with the current head */
            fm_head[cls] = b->head;
            fm_tail[cls] = b->tail;
            fm_batch_release(b);
            void *p = fm_head[cls];
            fm_head[cls] = *(void **)p;
            return p;
        }
    }

    /* Central list is empty: cut a fresh span.  Keep a limit's worth here
     * and publish the rest for other threads to use. */
    void *tail = NULL;
    uint32_t total = 0;
    void *head = fm_span_new(cls, &tail, &total);
    if (!head)
        return NULL;

    uint32_t keep = limit < total ? limit : total;
    void *t = head;
    for (uint32_t i = 1; i < keep; i++)
        t = *(void **)t;
    void *rest = *(void **)t;
    *(void **)t = NULL;

    fm_head[cls] = head;
    fm_tail[cls] = t;
    fm_cnt[cls] = 0;

    if (total > keep)
        fm_publish(rest, tail, cls);

    void *p = fm_head[cls];
    fm_head[cls] = *(void **)p;
    return p;
}

__attribute__((noinline)) static void fm_flush_bin(uint32_t cls)
{
#ifdef FM_DEBUG
    {
        /* Walk the local list and verify every block belongs to a span of
         * this class, and that the list is a proper chain. */
        uint32_t n = 0;
        void *q = fm_head[cls];
        while (q && n < 100000) {
            FM_Span *s2 = (FM_Span *)((uintptr_t)q & SPAN_MASK);
            uintptr_t off = (uintptr_t)q - ((uintptr_t)s2 + HEADER_SIZE);
            if (s2->cls != cls || off % kClassSize[cls] != 0) {
                fprintf(stderr, "[fm] LIST HAS FOREIGN BLOCK cls=%u q=%p "
                                "span_cls=%u off=%lu bs=%u\n",
                        cls, q, s2->cls, (unsigned long)off,
                        kClassSize[cls]);
                abort();
            }
            q = *(void **)q;
            n++;
        }
        if (n != fm_cnt[cls] + 0) {
            /* count is frees-since-flush, not list length; only report */
        }
    }
#endif
    void *h = fm_head[cls];
    void *t = fm_tail[cls];
    fm_head[cls] = NULL;
    fm_tail[cls] = NULL;
    fm_cnt[cls] = 0;
    if (!h)
        return;
    fm_publish(h, t, cls);
}

/* ------------------------------------------------------------------ */
/* class dispatch                                                      */
/* ------------------------------------------------------------------ */

static inline uint32_t fm_class_of(size_t size)
{
    if (__builtin_expect(size <= ARITH_MAX, 1))
        return (uint32_t)((size + 15) >> 4);
    if (__builtin_expect(size <= SMALL_MAX, 1))
        return kClassBig[(size + 15) >> 4];
    return LARGE_CLASS;
}

/* ------------------------------------------------------------------ */
/* large / aligned allocations                                         */
/* ------------------------------------------------------------------ */

__attribute__((noinline)) static void *fm_large_alloc(size_t size)
{
    char *base = fm_map_aligned(size);
    if (!base)
        return NULL;
    FM_Span *sp = (FM_Span *)base;
    sp->cls = LARGE_CLASS;
    sp->magic = FM_SPAN_MAGIC;
    sp->nblocks = 1;
    sp->map_base = (uintptr_t)base;
    sp->map_size = FM_ALIGN_UP(size + HEADER_SIZE, FM_PAGE_SIZE);
    sp->user_size = size;
    sp->align = 0;
    return (void *)(base + HEADER_SIZE);
}

__attribute__((noinline)) static void *fm_aligned_alloc_impl(size_t align, size_t size)
{
    if (align <= 16)
        return fm_malloc(size);
    if (align < SPAN_SIZE) {
        size_t need = size + align + HEADER_SIZE;
        size_t map_len = need + SPAN_SIZE + FM_PAGE_SIZE;
        char *raw = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED)
            return NULL;
        uintptr_t base = FM_ALIGN_UP((uintptr_t)raw, SPAN_SIZE);
        if (base != (uintptr_t)raw)
            munmap(raw, (size_t)(base - (uintptr_t)raw));
        uintptr_t user = FM_ALIGN_UP(base + HEADER_SIZE, align);
        size_t keep = FM_ALIGN_UP(need, FM_PAGE_SIZE);
        if (user + size > base + keep) {
            munmap((void *)base, keep);
            return NULL;
        }
        size_t tail_start = base + keep;
        size_t map_end = (size_t)FM_ALIGN_UP((uintptr_t)raw + map_len, FM_PAGE_SIZE);
        if (tail_start < map_end)
            munmap((void *)tail_start, map_end - tail_start);

        FM_Span *sp = (FM_Span *)base;
        sp->cls = LARGE_CLASS;
        sp->magic = FM_SPAN_MAGIC;
        sp->nblocks = 1;
        sp->map_base = (uintptr_t)base;
        sp->map_size = keep;
        sp->user_size = size;
        sp->align = align;
        return (void *)user;
    }
    /* 64 KiB or more: the returned pointer is itself 64 KiB aligned, so the
     * header cannot live in its own window.  Put it right before the
     * pointer and tag it with a magic number. */
    size_t need = size + align + HEADER_SIZE;
    size_t map_len = need + FM_PAGE_SIZE;
    char *raw = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;
    uintptr_t user = FM_ALIGN_UP((uintptr_t)raw + HEADER_SIZE, align);
    if (user + size > (uintptr_t)raw + map_len) {
        munmap(raw, map_len);
        return NULL;
    }
    FM_Span *sp = (FM_Span *)(user - HEADER_SIZE);
    sp->cls = LARGE_CLASS;
    sp->magic = FM_ALIGNED_MAGIC;
    sp->nblocks = 1;
    sp->map_base = (uintptr_t)raw;
    sp->map_size = map_len;
    sp->user_size = size;
    sp->align = align;
    return (void *)user;
}

static inline int fm_is_aligned_ptr(const void *p)
{
    if (((uintptr_t)p & (SPAN_SIZE - 1)) != 0)
        return 0;
    const FM_Span *sp = (const FM_Span *)((const char *)p - HEADER_SIZE);
    return sp->magic == FM_ALIGNED_MAGIC;
}

/* ------------------------------------------------------------------ */
/* exported entry points                                               */
/* ------------------------------------------------------------------ */

static inline void *fm_malloc(size_t size)
{
    uint32_t cls = fm_class_of(size);
    if (__builtin_expect(cls != LARGE_CLASS, 1)) {
        void *p = fm_head[cls];
        if (__builtin_expect(p != NULL, 1)) {
#ifdef FM_DEBUG
            {
                FM_Span *sp2 = (FM_Span *)((uintptr_t)p & SPAN_MASK);
                uintptr_t off = (uintptr_t)p - ((uintptr_t)sp2 + HEADER_SIZE);
                if (sp2->cls != cls || off % kClassSize[cls] != 0) {
                    fprintf(stderr,
                            "[fm] BAD HEAD cls=%u p=%p span_cls=%u off=%lu "
                            "bs=%u\n",
                            cls, p, sp2->cls, (unsigned long)off,
                            kClassSize[cls]);
                    abort();
                }
            }
#endif
#ifdef FM_DEBUG
            if (((uint64_t *)p)[1] != 0xDEADDEADDEADDEADull) {
                FM_Span *s3 = (FM_Span *)((uintptr_t)p & SPAN_MASK);
                fprintf(stderr,
                        "[fm] CORRUPT HEAD size=%zu cls=%u bs=%u span_cls=%u "
                        "span_nblocks=%u off=%lu canary=%lx\n",
                        size, cls, kClassSize[cls], s3->cls, s3->nblocks,
                        (unsigned long)((uintptr_t)p - ((uintptr_t)s3 + HEADER_SIZE)),
                        (unsigned long)((uint64_t *)p)[1]);
                abort();
            }
            {
                FM_Span *s3 = (FM_Span *)((uintptr_t)p & SPAN_MASK);
                uintptr_t off = (uintptr_t)p - ((uintptr_t)s3 + HEADER_SIZE);
                if (off % kClassSize[cls] != 0 ||
                    off / kClassSize[cls] >= s3->nblocks ||
                    s3->cls != cls) {
                    fprintf(stderr,
                            "[fm] BAD BLOCK size=%zu cls=%u bs=%u p=%p "
                            "span_cls=%u off=%lu nblocks=%u\n",
                            size, cls, kClassSize[cls], p, s3->cls,
                            (unsigned long)off, s3->nblocks);
                    abort();
                }
            }
#endif
            fm_head[cls] = *(void **)p;
            return p;
        }
        return fm_refill_class(cls);
    }
    return fm_large_alloc(size);
}

static inline void fm_free(void *p)
{
    if (!p)
        return;
    if (__builtin_expect(((uintptr_t)p & (SPAN_SIZE - 1)) == 0, 0)) {
        if (fm_is_aligned_ptr(p)) {
            const FM_Span *sp = (const FM_Span *)((const char *)p - HEADER_SIZE);
            munmap((void *)sp->map_base, sp->map_size);
            return;
        }
    }
    FM_Span *sp = (FM_Span *)((uintptr_t)p & SPAN_MASK);
    uint32_t cls = sp->cls;
#ifdef FM_DEBUG
    {
        /* The block must lie inside its span, at a block boundary of its
         * class.  If this ever fails the class mapping is inconsistent. */
        uintptr_t base = (uintptr_t)sp;
        uintptr_t off = (uintptr_t)p - (base + HEADER_SIZE);
        if (cls != LARGE_CLASS &&
            (off % kClassSize[cls] != 0 || off / kClassSize[cls] >= sp->nblocks)) {
            fprintf(stderr,
                    "[fm] BAD BLOCK p=%p base=%p off=%lu cls=%u bs=%u "
                    "nblocks=%u\n",
                    p, (void *)base, (unsigned long)off, cls,
                    kClassSize[cls], sp->nblocks);
            abort();
        }
    }
#endif
    if (__builtin_expect(cls != LARGE_CLASS, 1)) {
        void *old = fm_head[cls];
        *(void **)p = old;
#ifdef FM_DEBUG
        ((uint64_t *)p)[1] = 0xDEADDEADDEADDEADull;
#endif
        fm_head[cls] = p;
        if (!old)
            fm_tail[cls] = p;
        if (++fm_cnt[cls] >= kClassLimit[cls])
            fm_flush_bin(cls);
    } else {
        munmap((void *)sp->map_base, sp->map_size);
    }
}

static inline size_t fm_usable_size(void *p)
{
    if (!p)
        return 0;
    if (((uintptr_t)p & (SPAN_SIZE - 1)) == 0 && fm_is_aligned_ptr(p)) {
        const FM_Span *sp = (const FM_Span *)((const char *)p - HEADER_SIZE);
        return sp->user_size;
    }
    const FM_Span *sp = (const FM_Span *)((uintptr_t)p & SPAN_MASK);
    if (sp->cls == LARGE_CLASS)
        return sp->user_size;
    return kClassSize[sp->cls];
}

static inline void *fm_realloc(void *p, size_t size)
{
    if (!p)
        return fm_malloc(size);
    if (size == 0) {
        fm_free(p);
        return NULL;
    }
    if (((uintptr_t)p & (SPAN_SIZE - 1)) == 0 && fm_is_aligned_ptr(p)) {
        const FM_Span *sp = (const FM_Span *)((const char *)p - HEADER_SIZE);
        if (size <= sp->user_size)
            return p;
        void *q = fm_aligned_alloc_impl(sp->align, size);
        if (!q)
            return NULL;
        memcpy(q, p, sp->user_size);
        munmap((void *)sp->map_base, sp->map_size);
        return q;
    }
    FM_Span *sp = (FM_Span *)((uintptr_t)p & SPAN_MASK);
    if (sp->cls != LARGE_CLASS) {
        size_t old = kClassSize[sp->cls];
        if (size <= old)
            return p;
        void *q = fm_malloc(size);
        if (!q)
            return NULL;
        memcpy(q, p, old);
        fm_free(p);
        return q;
    }
    if (size <= sp->user_size)
        return p;
    void *q = sp->align ? fm_aligned_alloc_impl(sp->align, size) : fm_large_alloc(size);
    if (!q)
        return NULL;
    memcpy(q, p, sp->user_size);
    munmap((void *)sp->map_base, sp->map_size);
    return q;
}

#endif /* FM_CORE_H */
