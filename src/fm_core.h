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
 * Shaping (ADR 0014)
 * ------------------
 * A flush no longer publishes the local list in free order.  It walks the
 * list once (the flush is already off the hot path) and groups the blocks
 * by span.  The groups go into a small per-thread *span stash* -- a few
 * slots per class, each holding the free blocks of one span -- and only
 * groups that fit nowhere are published, still span-pure, to the central
 * list.  Refills drain the stash's largest group first, so reuse empties
 * span after span instead of skimming a few blocks off every span at
 * once: the pages a burst of allocations lands on stay few and hot.
 *
 * A group that covers its whole span proves the span has no live blocks
 * anywhere (every block of it is in this thread's hands), so the span is
 * unmapped on the spot.  That, plus the census in fm_trim_spans(), is
 * what lets the allocator give memory back (ADR 0015).
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
    uintptr_t lnext;    /* next span in the large-block cache bucket */
    char reserved[64 - 56];
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

/* Large-block cache (ADR 0011): per-thread cache of freed large blocks,
 * bucketed by the highest set bit of the mapping size, so a cached block
 * is never more than 2x the size of the request it can serve.  Without
 * it every large allocation is a fresh mmap and every free an munmap,
 * which measured ~9000 ns/op against glibc's 28 on this host. */
#define FM_LCACHE_BUCKETS 48
#define FM_LCACHE_MAX_BYTES (4ull << 30)
extern __thread void *fm_lcache_head[FM_LCACHE_BUCKETS]
    __attribute__((tls_model("initial-exec")));
extern __thread uint64_t fm_lcache_bytes __attribute__((tls_model("initial-exec")));

/* Span stash (ADR 0014): per-thread, per-class slots, each holding the
 * free blocks of exactly one span, accumulated across flushes.  Cold
 * path only (flush / refill / trim), so plain arrays are fine here. */
#define FM_STASH_SLOTS 16
#define FM_STASH_MAX_BLOCKS 8192
extern __thread uintptr_t fm_stash_span[CLASS_COUNT][FM_STASH_SLOTS]
    __attribute__((tls_model("initial-exec")));
extern __thread void *fm_stash_head[CLASS_COUNT][FM_STASH_SLOTS]
    __attribute__((tls_model("initial-exec")));
extern __thread void *fm_stash_tail[CLASS_COUNT][FM_STASH_SLOTS]
    __attribute__((tls_model("initial-exec")));
extern __thread uint32_t fm_stash_cnt[CLASS_COUNT][FM_STASH_SLOTS]
    __attribute__((tls_model("initial-exec")));
extern __thread uint32_t fm_stash_total[CLASS_COUNT]
    __attribute__((tls_model("initial-exec")));

/* Spare span (ADR 0015): one *completed* span per class is parked
 * instead of unmapped, as release hysteresis.  Workloads that free a
 * whole span and immediately need one again (a game frame's
 * transients) would otherwise pay an mmap + page faults per cycle;
 * the parked span is served first by the next refill, and a second
 * completion while one is parked releases the new arrival.  Trim
 * drains the spare too, so retention is bounded by one span per
 * class per thread. */
extern __thread uintptr_t fm_spare_span[CLASS_COUNT]
    __attribute__((tls_model("initial-exec")));
extern __thread void *fm_spare_head[CLASS_COUNT]
    __attribute__((tls_model("initial-exec")));
extern __thread void *fm_spare_tail[CLASS_COUNT]
    __attribute__((tls_model("initial-exec")));
extern __thread uint32_t fm_spare_cnt[CLASS_COUNT]
    __attribute__((tls_model("initial-exec")));

/* Batch count field: block count in the low bits; the top bit marks a
 * span-pure batch (every block from one span).  A count of 0 means
 * "unknown" (legacy publishers); consumers must then walk. */
#define FM_COUNT_PURE 0x80000000u
#define FM_COUNT_MASK 0x7FFFFFFFu

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
 * A batch is a whole free list (or, since ADR 0014, one span's slice
 * of one), never an arbitrary fragment: publishing is O(1) because
 * the tail pointer is already known, and a refill takes a whole batch,
 * again O(1).  What changed in ADR 0014 is only how a flush *builds*
 * the batches -- see fm_shape_recycle.
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

/* Publish a chain of blocks as one batch, recording its length and
 * whether it is span-pure.  If batch headers run out the blocks are
 * dropped (still owned by the process, just unreachable) -- the
 * failure handling this allocator has always had. */
static inline void fm_publish2(void *head, void *tail, uint32_t cls,
                               uint32_t count, int pure)
{
    FM_Batch *nb = fm_batch_alloc();
    if (!nb)
        return;
    nb->head = head;
    nb->tail = tail;
    nb->count = (count & FM_COUNT_MASK) | (pure ? FM_COUNT_PURE : 0);
    fm_central_push(cls, nb);
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
/* shaping: span-grouped recycling (ADR 0014) and span release (0015)  */
/* ------------------------------------------------------------------ */

/* Unmap a span whose blocks are all in hand.  The caller must hold
 * `cnt` distinct free blocks of the span and cnt must equal the span's
 * block count: then no block of the span can be live or held anywhere
 * else (each block exists exactly once across all lists), so the
 * mapping can go.  Header fields are read into locals first -- the
 * header lives inside the span being unmapped. */
static inline int fm_release_full_span(uintptr_t span, uint32_t cnt)
{
    FM_Span *sp = (FM_Span *)span;
    if (cnt != sp->nblocks)
        return 0;
    uintptr_t base = sp->map_base;
    size_t len = sp->map_size;
    munmap((void *)base, len);
    return 1;
}

/* A span has just been completed: every one of its blocks is in this
 * chain.  Park the first completed span of the class as the spare
 * (release hysteresis, ADR 0015); release any further completion. */
static inline void fm_span_completed(uint32_t cls, uintptr_t span, void *head,
                                     void *tail, uint32_t cnt)
{
    if (fm_spare_span[cls] == 0) {
        fm_spare_span[cls] = span;
        fm_spare_head[cls] = head;
        fm_spare_tail[cls] = tail;
        fm_spare_cnt[cls] = cnt;
        return;
    }
    fm_release_full_span(span, cnt);
}

/* Offer one span-pure group of blocks to the stash.  Returns 1 if the
 * group was consumed (stashed, parked as the spare, or released as a
 * completed span), 0 if the caller should publish it instead. */
static inline int fm_stash_insert(uint32_t cls, uintptr_t span, void *head,
                                  void *tail, uint32_t cnt)
{
    int empty = -1;
    for (int i = 0; i < FM_STASH_SLOTS; i++) {
        if (fm_stash_span[cls][i] == span) {
            *(void **)fm_stash_tail[cls][i] = head;
            fm_stash_tail[cls][i] = tail;
            fm_stash_cnt[cls][i] += cnt;
            fm_stash_total[cls] += cnt;
            if (fm_stash_cnt[cls][i] == ((FM_Span *)span)->nblocks) {
                /* The stash now holds the whole span. */
                void *whead = fm_stash_head[cls][i];
                void *wtail = fm_stash_tail[cls][i];
                uint32_t whole = fm_stash_cnt[cls][i];
                fm_stash_span[cls][i] = 0;
                fm_stash_head[cls][i] = NULL;
                fm_stash_tail[cls][i] = NULL;
                fm_stash_cnt[cls][i] = 0;
                fm_stash_total[cls] -= whole;
                fm_span_completed(cls, span, whead, wtail, whole);
            }
            return 1;
        }
        if (empty < 0 && fm_stash_span[cls][i] == 0)
            empty = i;
    }
    if (cnt == ((FM_Span *)span)->nblocks) {
        /* Arrives already complete: never enters the stash. */
        fm_span_completed(cls, span, head, tail, cnt);
        return 1;
    }
    if (empty >= 0 && fm_stash_total[cls] + cnt <= FM_STASH_MAX_BLOCKS) {
        fm_stash_span[cls][empty] = span;
        fm_stash_head[cls][empty] = head;
        fm_stash_tail[cls][empty] = tail;
        fm_stash_cnt[cls][empty] = cnt;
        fm_stash_total[cls] += cnt;
        return 1;
    }
    return 0;
}

/* Take the stash's largest group of `cls` into the local list.
 * Returns 1 if a group was installed in fm_head/fm_tail. */
static inline int fm_stash_take(uint32_t cls)
{
    int best = -1;
    uint32_t bestcnt = 0;
    for (int i = 0; i < FM_STASH_SLOTS; i++) {
        if (fm_stash_cnt[cls][i] > bestcnt) {
            bestcnt = fm_stash_cnt[cls][i];
            best = i;
        }
    }
    if (best < 0)
        return 0;
    fm_head[cls] = fm_stash_head[cls][best];
    fm_tail[cls] = fm_stash_tail[cls][best];
    fm_stash_total[cls] -= bestcnt;
    fm_stash_span[cls][best] = 0;
    fm_stash_head[cls][best] = NULL;
    fm_stash_tail[cls][best] = NULL;
    fm_stash_cnt[cls][best] = 0;
    return 1;
}

typedef struct FM_Group {
    uintptr_t span;
    void *head;
    void *tail;
    uint32_t cnt;  /* pass 1: blocks counted; pass 2: blocks spliced */
    uint32_t seen; /* pass 1 count, kept for the dispatch total */
} FM_Group;

#define FM_FLUSH_GROUPS 32

/* Find the group for `span`, claiming an empty slot for it if the
 * probe window has one.  NULL when the window is full of other spans
 * (the block is then treated as mixed).  Slots are only ever claimed,
 * never freed, during one recycle, so both passes agree. */
static inline FM_Group *fm_group_for(FM_Group *groups, uintptr_t span)
{
    unsigned h = (unsigned)(span >> 16) & (FM_FLUSH_GROUPS - 1);
    for (int probe = 0; probe < 4; probe++) {
        FM_Group *cand = &groups[(h + probe) & (FM_FLUSH_GROUPS - 1)];
        if (cand->span == span)
            return cand;
        if (cand->span == 0) {
            cand->span = span;
            return cand;
        }
    }
    return NULL;
}

/* Recycle a detached local list: group its blocks by span, hand the
 * groups to the stash (which parks or releases completed spans), and
 * publish whatever the stash cannot take, still span-pure.  Blocks
 * whose span falls out of the group table go to the central list as
 * one mixed batch, as before.
 *
 * Two passes, deliberately.  Pass 1 only *loads*: it counts blocks
 * per span without storing to any block, and it warms every block's
 * first word into cache.  If the whole list turns out to be one span
 * (the common bulk-free case: a tree teardown, a frame's transients)
 * the list is already exactly the group chain and pass 2 is skipped
 * entirely.  Otherwise pass 2 splices the links -- now cache-warm --
 * into per-span chains. */
__attribute__((noinline)) static void fm_shape_recycle(uint32_t cls, void *head,
                                                       void *tail)
{
    FM_Group groups[FM_FLUSH_GROUPS];
    memset(groups, 0, sizeof groups);
    uint32_t total = 0;
    int overflow = 0;

    for (void *p = head; p; p = *(void **)p) {
        FM_Group *g = fm_group_for(groups, (uintptr_t)p & SPAN_MASK);
        if (g)
            g->seen++;
        else
            overflow = 1;
        total++;
    }

    if (!overflow) {
        FM_Group *the = NULL;
        int ndistinct = 0;
        for (int i = 0; i < FM_FLUSH_GROUPS; i++) {
            if (groups[i].seen) {
                ndistinct++;
                the = &groups[i];
            }
        }
        if (ndistinct == 1 && the->seen == total) {
            /* Uniform list: it is already this span's chain. */
            if (!fm_stash_insert(cls, the->span, head, tail, total))
                fm_publish2(head, tail, cls, total, 1);
            return;
        }
    }

    void *mixed_head = NULL, *mixed_tail = NULL;
    uint32_t mixed_cnt = 0;
    void *p = head;
    while (p) {
        void *next = *(void **)p;
        FM_Group *g = fm_group_for(groups, (uintptr_t)p & SPAN_MASK);
        if (g) {
            if (g->cnt)
                *(void **)g->tail = p;
            else
                g->head = p;
            g->tail = p;
            g->cnt++;
        } else {
            if (mixed_tail)
                *(void **)mixed_tail = p;
            else
                mixed_head = p;
            mixed_tail = p;
            mixed_cnt++;
        }
        p = next;
    }

    for (int i = 0; i < FM_FLUSH_GROUPS; i++) {
        FM_Group *g = &groups[i];
        if (!g->cnt)
            continue;
        *(void **)g->tail = NULL;
        if (!fm_stash_insert(cls, g->span, g->head, g->tail, g->cnt))
            fm_publish2(g->head, g->tail, cls, g->cnt, 1);
    }
    if (mixed_head) {
        *(void **)mixed_tail = NULL;
        fm_publish2(mixed_head, mixed_tail, cls, mixed_cnt, 0);
    }
}

/* ------------------------------------------------------------------ */
/* refill / flush                                                      */
/* ------------------------------------------------------------------ */

__attribute__((noinline)) static void fm_flush_bin(uint32_t cls);

/*
 * (Historical note: a flush used to publish the whole local list with
 * an O(1) hand-off, deliberately never walking it -- the blocks of a
 * free list are scattered across the heap, so each step is a likely
 * cache miss.  ADR 0014 reverses that call for the flush path: the
 * walk is what buys span grouping, span release and the stash, and it
 * runs at most once per kClassLimit frees.  Publishing itself is now
 * fm_publish2, above.)
 */

/* Take the next block of a class, refilling from the central list or from a
 * fresh span when needed.  Kept out of line so that the malloc fast path
 * needs no callee-saved registers and no stack frame. */
__attribute__((noinline)) void *fm_refill_class(uint32_t cls)
{
    uint32_t limit = kClassLimit[cls];

    /* The parked spare span first (ADR 0015): a whole span's blocks,
     * the most concentrated refill there is. */
    if (fm_spare_span[cls]) {
        fm_head[cls] = fm_spare_head[cls];
        fm_tail[cls] = fm_spare_tail[cls];
        fm_spare_span[cls] = 0;
        fm_spare_head[cls] = NULL;
        fm_spare_tail[cls] = NULL;
        fm_spare_cnt[cls] = 0;
        void *p = fm_head[cls];
        fm_head[cls] = *(void **)p;
        return p;
    }

    /* The span stash next (ADR 0014): its largest group is one span's
     * blocks, so draining it keeps this burst of allocations on one
     * span's pages instead of skimming every span in the central list. */
    if (fm_stash_take(cls)) {
        void *p = fm_head[cls];
        fm_head[cls] = *(void **)p;
        return p;
    }

    /* Try the central list next: take a whole batch, O(1). */
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

    if (total > keep) {
        /* The rest of the fresh span is one span-pure group: offer it
         * to the stash, where the next refill will drain this same
         * span before touching any other; publish it if the stash
         * cannot take it. */
        if (!fm_stash_insert(cls, (uintptr_t)head & SPAN_MASK, rest, tail,
                             total - keep))
            fm_publish2(rest, tail, cls, total - keep, 1);
    }

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
    /* Shaped recycling (ADR 0014): group by span, park or release
     * completed spans, stash the groups, publish the overflow
     * span-pure.  The old code published the whole list in free order
     * with one CAS; this walks it, on a path that runs at most once
     * per kClassLimit frees. */
    fm_shape_recycle(cls, h, t);
}

/* ------------------------------------------------------------------ */
/* memory return: the trim census (ADR 0015)                           */
/* ------------------------------------------------------------------ */

/*
 * malloc_trim's job: find spans that are entirely free and unmap them.
 * At trim time we can afford to walk.  The census below counts, per
 * span, how many of its blocks sit in the central list (this thread's
 * local list and stash are flushed into it first).  A span whose
 * central count equals its block count has no live blocks and no
 * blocks held anywhere else either -- every block exists exactly once
 * across all the allocator's lists, so if all of them are here, none
 * are anywhere else.  Those spans are unmapped; everything else is
 * put back.  Other threads' local lists and stashes are out of reach,
 * which only ever makes the count too small -- the safe direction.
 */

typedef struct FM_CensusEnt {
    uintptr_t span;
    uint32_t cnt;
    uint32_t dead;
} FM_CensusEnt;

typedef struct FM_Census {
    FM_CensusEnt *ents;
    size_t cap; /* power of two */
    size_t used;
} FM_Census;

static int fm_census_init(FM_Census *c)
{
    c->cap = 256;
    c->used = 0;
    c->ents = mmap(NULL, c->cap * sizeof(FM_CensusEnt), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return c->ents != MAP_FAILED;
}

static void fm_census_destroy(FM_Census *c)
{
    if (c->ents && c->ents != MAP_FAILED)
        munmap(c->ents, c->cap * sizeof(FM_CensusEnt));
    c->ents = NULL;
}

static FM_CensusEnt *fm_census_slot(FM_Census *c, uintptr_t span)
{
    size_t mask = c->cap - 1;
    size_t i = (size_t)(span >> 16) & mask;
    while (c->ents[i].span != 0 && c->ents[i].span != span)
        i = (i + 1) & mask;
    return &c->ents[i];
}

static int fm_census_grow(FM_Census *c)
{
    FM_CensusEnt *old = c->ents;
    size_t oldcap = c->cap;
    FM_CensusEnt *nw = mmap(NULL, oldcap * 2 * sizeof(FM_CensusEnt),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nw == MAP_FAILED)
        return 0;
    c->ents = nw;
    c->cap = oldcap * 2;
    c->used = 0;
    for (size_t i = 0; i < oldcap; i++) {
        if (old[i].span) {
            FM_CensusEnt *e = fm_census_slot(c, old[i].span);
            e->span = old[i].span;
            e->cnt = old[i].cnt;
            e->dead = 0;
            c->used++;
        }
    }
    munmap(old, oldcap * sizeof(FM_CensusEnt));
    return 1;
}

/* Add n to the free-block count of `span`.  On allocation failure the
 * add is dropped: the span simply will not look free, which is safe. */
static void fm_census_add(FM_Census *c, uintptr_t span, uint32_t n)
{
    if (!c->ents)
        return;
    if ((c->used + 1) * 2 >= c->cap && !fm_census_grow(c) && c->used + 1 >= c->cap)
        return;
    FM_CensusEnt *e = fm_census_slot(c, span);
    if (e->span == 0) {
        e->span = span;
        c->used++;
    }
    e->cnt += n;
}

static FM_CensusEnt *fm_census_find(FM_Census *c, uintptr_t span)
{
    if (!c->ents)
        return NULL;
    FM_CensusEnt *e = fm_census_slot(c, span);
    return e->span == span ? e : NULL;
}

__attribute__((noinline)) static size_t fm_trim_class(uint32_t cls)
{
    fm_flush_bin(cls);
    /* Move this thread's spare span and stash into the central list
     * so the census accounts for those blocks too. */
    if (fm_spare_span[cls]) {
        fm_publish2(fm_spare_head[cls], fm_spare_tail[cls], cls,
                    fm_spare_cnt[cls], 1);
        fm_spare_span[cls] = 0;
        fm_spare_head[cls] = NULL;
        fm_spare_tail[cls] = NULL;
        fm_spare_cnt[cls] = 0;
    }
    for (int i = 0; i < FM_STASH_SLOTS; i++) {
        if (fm_stash_span[cls][i]) {
            fm_publish2(fm_stash_head[cls][i], fm_stash_tail[cls][i], cls,
                        fm_stash_cnt[cls][i], 1);
            fm_stash_total[cls] -= fm_stash_cnt[cls][i];
            fm_stash_span[cls][i] = 0;
            fm_stash_head[cls][i] = NULL;
            fm_stash_tail[cls][i] = NULL;
            fm_stash_cnt[cls][i] = 0;
        }
    }

    /* Detach the whole central stack atomically.  Batches published
     * after this point land on the fresh stack; batches another
     * thread's refill already popped are out of reach.  Both only
     * shrink what the census can see. */
    FM_Batch *all = atomic_exchange_explicit(&fm_central[cls].head, NULL,
                                             memory_order_acq_rel);
    if (!all)
        return 0;

    FM_Census cen;
    cen.ents = NULL;
    cen.cap = 0;
    cen.used = 0;
    if (!fm_census_init(&cen)) {
        /* No census: put everything back untouched. */
        while (all) {
            FM_Batch *nx = all->next;
            fm_central_push(cls, all);
            all = nx;
        }
        return 0;
    }

    /* Count free blocks per span.  Span-pure batches with a recorded
     * count are accounted wholesale; anything else is walked. */
    for (FM_Batch *b = all; b; b = b->next) {
        uint32_t cnt = b->count & FM_COUNT_MASK;
        if (cnt && (b->count & FM_COUNT_PURE)) {
            fm_census_add(&cen, (uintptr_t)b->head & SPAN_MASK, cnt);
        } else {
            for (void *p = b->head; p; p = *(void **)p)
                fm_census_add(&cen, (uintptr_t)p & SPAN_MASK, 1);
        }
    }

    /* A span is dead when every one of its blocks is here. */
    for (size_t i = 0; i < cen.cap; i++) {
        FM_CensusEnt *e = &cen.ents[i];
        if (!e->span)
            continue;
        uint32_t nb = ((FM_Span *)e->span)->nblocks;
#ifdef FM_DEBUG
        if (e->cnt > nb) {
            fprintf(stderr, "[fm] CENSUS OVERCOUNT cls=%u span=%p cnt=%u "
                            "nblocks=%u\n",
                    cls, (void *)e->span, e->cnt, nb);
            abort();
        }
#endif
        e->dead = (e->cnt == nb);
    }

    /* Rebuild: keep survivors, drop blocks of dead spans. */
    FM_Batch *survivors = NULL;
    for (FM_Batch *b = all; b;) {
        FM_Batch *nextb = b->next;
        uint32_t cnt = b->count & FM_COUNT_MASK;
        if (cnt && (b->count & FM_COUNT_PURE)) {
            FM_CensusEnt *e = fm_census_find(&cen, (uintptr_t)b->head & SPAN_MASK);
            if (e && e->dead) {
                fm_batch_release(b);
            } else {
                b->next = survivors;
                survivors = b;
            }
        } else {
            void *shead = NULL, *stail = NULL;
            uint32_t scnt = 0;
            void *p = b->head;
            while (p) {
                void *nx = *(void **)p;
                FM_CensusEnt *e = fm_census_find(&cen, (uintptr_t)p & SPAN_MASK);
                if (!(e && e->dead)) {
                    if (stail)
                        *(void **)stail = p;
                    else
                        shead = p;
                    stail = p;
                    scnt++;
                }
                p = nx;
            }
            if (shead) {
                *(void **)stail = NULL;
                b->head = shead;
                b->tail = stail;
                b->count = scnt; /* mixed, count known */
                b->next = survivors;
                survivors = b;
            } else {
                fm_batch_release(b);
            }
        }
        b = nextb;
    }

    /* Unmap the dead spans (once each, from the census). */
    size_t released = 0;
    for (size_t i = 0; i < cen.cap; i++) {
        FM_CensusEnt *e = &cen.ents[i];
        if (e->span && e->dead) {
            FM_Span *sp = (FM_Span *)e->span;
            uintptr_t base = sp->map_base;
            size_t len = sp->map_size;
            released += len;
            munmap((void *)base, len);
        }
    }

    while (survivors) {
        FM_Batch *nx = survivors->next;
        fm_central_push(cls, survivors);
        survivors = nx;
    }
    fm_census_destroy(&cen);
    return released;
}

static inline size_t fm_trim_spans(void)
{
    size_t released = 0;
    for (uint32_t cls = 0; cls < CLASS_COUNT; cls++)
        released += fm_trim_class(cls);
    return released;
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

/* Map memory for a large allocation WITHOUT trimming: the mapping is
 * over-sized by up to one span so the header can sit at a 64 KiB-aligned
 * base inside it, and the head slack simply stays mapped as part of the
 * block (it is never touched, so it costs address space only).  The
 * header records the raw mapping in map_base/map_size, so free releases
 * everything with one munmap.  The trimmed fm_map_aligned costs three
 * syscalls per allocation (mmap + two munmaps); this costs one, which
 * matters: measured against glibc's one-mmap large path, the trims
 * were most of v2's large-allocation deficit. */
static inline char *fm_map_large(size_t bytes, uintptr_t *raw_out, size_t *len_out)
{
    size_t map_len = bytes + HEADER_SIZE + SPAN_SIZE;
    char *raw = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;
    *raw_out = (uintptr_t)raw;
    *len_out = map_len;
    return (char *)FM_ALIGN_UP((uintptr_t)raw, SPAN_SIZE);
}

/* Free a large block: cache it for reuse when it is a plain large
 * allocation (aligned allocations and anything whose user pointer is
 * not at the standard offset keep the munmap path), subject to the
 * per-bucket depth and the per-thread byte cap. */
/* Usable length of a large block's mapping, measured from the header:
 * the raw mapping may start up to one span before the header (the
 * untrimmed head slack of fm_map_large), and only the part from the
 * header on can serve a request.  Bucketing and reuse decisions must
 * use this, never the raw map_size: with the slack included, a cached
 * block can look up to 64 KiB bigger than it is, and reusing it would
 * hand out memory that runs past the end of the mapping. */
static inline size_t fm_large_usable(const FM_Span *sp)
{
    return sp->map_size - ((uintptr_t)sp - sp->map_base);
}

static inline void fm_large_free(FM_Span *sp, void *p)
{
    if (sp->align == 0 && (uintptr_t)p == (uintptr_t)sp + HEADER_SIZE) {
        size_t ms = sp->map_size;
        int b = 63 - __builtin_clzll(fm_large_usable(sp));
        if (b < FM_LCACHE_BUCKETS &&
            fm_lcache_bytes + ms <= FM_LCACHE_MAX_BYTES) {
            sp->lnext = (uintptr_t)fm_lcache_head[b];
            fm_lcache_head[b] = sp;
            fm_lcache_bytes += ms;
            return;
        }
    }
    munmap((void *)sp->map_base, sp->map_size);
}

/* Release every cached large block of this thread to the OS.
 * Backs malloc_trim: the cache is a retention policy, and trim is the
 * release valve that makes the policy safe to keep. */
static inline size_t fm_large_cache_drain(void)
{
    size_t released = 0;
    for (int b = 0; b < FM_LCACHE_BUCKETS; b++) {
        FM_Span *sp = (FM_Span *)fm_lcache_head[b];
        fm_lcache_head[b] = NULL;
        while (sp) {
            FM_Span *next = (FM_Span *)sp->lnext;
            released += sp->map_size;
            munmap((void *)sp->map_base, sp->map_size);
            sp = next;
        }
    }
    fm_lcache_bytes = 0;
    return released;
}

/* Allocate a large block.  *zeroed (when non-NULL) reports whether the
 * memory is known to be zero-filled: fresh anonymous mappings are,
 * cache hits are not.  calloc depends on this distinction. */
__attribute__((noinline)) static void *fm_large_alloc2(size_t size, int *zeroed)
{
    size_t need_map = FM_ALIGN_UP(size + HEADER_SIZE, FM_PAGE_SIZE);
    int nb = 63 - __builtin_clzll(need_map);
    /* Search the need's own bucket, then the next one up: a block's
     * usable length can spill one bucket above its request size
     * because of the untrimmed head slack. */
    for (int b = nb; b <= nb + 1 && b < FM_LCACHE_BUCKETS; b++) {
        /* Walk a few entries of the bucket list for one large enough;
         * blocks in a bucket differ by less than 2x, so the head
         * usually fits. */
        FM_Span **link = (FM_Span **)&fm_lcache_head[b];
        for (int i = 0; i < 8 && *link; i++) {
            FM_Span *sp = *link;
            if (fm_large_usable(sp) >= need_map) {
                *link = (FM_Span *)sp->lnext;
                fm_lcache_bytes -= sp->map_size;
                sp->user_size = size;
                sp->lnext = 0;
                if (zeroed)
                    *zeroed = 0;
                return (void *)((char *)sp + HEADER_SIZE);
            }
            link = (FM_Span **)&sp->lnext;
        }
    }
    uintptr_t raw = 0;
    size_t raw_len = 0;
    char *base = fm_map_large(size, &raw, &raw_len);
    if (!base)
        return NULL;
    FM_Span *sp = (FM_Span *)base;
    sp->cls = LARGE_CLASS;
    sp->magic = FM_SPAN_MAGIC;
    sp->nblocks = 1;
    sp->map_base = raw;
    sp->map_size = raw_len;
    sp->user_size = size;
    sp->align = 0;
    sp->lnext = 0;
    if (zeroed)
        *zeroed = 1;
    return (void *)(base + HEADER_SIZE);
}

__attribute__((noinline)) static void *fm_large_alloc(size_t size)
{
    return fm_large_alloc2(size, NULL);
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
        fm_large_free(sp, p);
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
    fm_large_free(sp, p);
    return q;
}

#endif /* FM_CORE_H */
