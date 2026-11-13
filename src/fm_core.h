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
 * of that class as one batch.  The central list is a stack
 * of batches. A refill takes a whole batch and a flush publishes one;
 * slow-path stack ownership is serialized to protect recycled headers.
 * Thread-local allocation and free fast paths take no locks.
 *
 * Shaping (ADR 0014) -- tried, measured, not adopted
 * ------------------
 * A flush publishes the local list in free order, so a batch can mix
 * blocks from many spans and reuse sprays across all of them (the
 * bench/macro `locality` scenario measures the spray directly: a
 * reallocation burst lands on ~513 of the pool's 528 pages).  Round 2
 * implemented span-grouped recycling -- group the flushed list by
 * span into a per-thread stash, drain largest-group-first at refill
 * -- and it worked mechanically (burst footprint 513 -> 68 pages)
 * but cost an extra pointer chase per recycled block, and on every
 * timed workload that tax exceeded the locality it bought (the macro
 * workloads' free order already clusters by span, so there was
 * little left to win).  ADR 0014 has the full experiment, including
 * the two failure modes that had to be fixed first (an O(span) walk
 * per flush, and eager span unmapping).  What round 2 kept is the
 * part with no hot-path cost: batches record their length and
 * span-purity, and malloc_trim (ADR 0015) uses a census over the
 * central list to unmap spans it can prove entirely free.
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
#include <pthread.h>
#include <signal.h>
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
/* Exit hook state: 0 = not armed, 1 = armed, 2 = destructor running (disarmed). */
extern __thread int fm_exit_armed __attribute__((tls_model("initial-exec")));

typedef struct FM_Span {
    uint32_t cls;       /* size class, or LARGE_CLASS */
    uint32_t magic;
    uint32_t nblocks;
    _Atomic uint32_t dead_mark; /* ADR 0020: ms timestamp of the first census
                         * that found this span entirely free; 0 =
                         * unmarked.  A marked span is unmapped by the
                         * automatic release only once the mark is
                         * FM_RELEASE_AGE_MS old.  Written by census
                         * threads and cleared on refill; atomic
                         * heuristic data shared with refill. */
    uintptr_t map_base; /* start of the mapping (for munmap) */
    size_t map_size;
    size_t user_size;   /* bytes requested (large / aligned) */
    size_t align;       /* alignment requested (aligned allocs) */
    uintptr_t lnext;    /* next span in the large-block cache bucket */
    /* ADR 0030 page purge: pages of this span that the census has
     * returned to the OS with MADV_DONTNEED while the span still has
     * live blocks.  Bit p of purged_mask names page p (page 0, which
     * holds this header, is never purged).  Every block STARTING in
     * a purged page is withheld from circulation (it is in no free
     * list, so it can never be handed out or freed again while
     * withheld); withheld_cnt counts them.  The purge decision
     * guarantees every block intersecting a purged page is free and
     * every block starting in one was seized by the census, so the
     * withheld set is exactly "blocks starting in purged pages" and
     * can be recomputed from the mask at any time -- that is what
     * makes reclaim (re-carving) safe.  purge_mark is the ageing
     * timestamp of the purge machinery, mirroring dead_mark: set
     * when a census first finds the span purge-worthy, stamped
     * again when a purge lands (the reclaim age is measured from
     * it). */
    uint32_t purge_mark;
    uint16_t purged_mask;
    uint16_t withheld_cnt;
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
    /* Approximate number of batches on the stack, maintained by the
     * (already slow) push/pop paths.  The automatic span release
     * (ADR 0020) uses it as a gate: a class whose central stack is
     * nearly empty has nothing worth censusing.  It is approximate
     * only in the presence of concurrent updates, which is fine for
     * a gate. */
    _Atomic uint32_t depth;
    /* ADR 0020 automatic-release bookkeeping: when this class was
     * last censused (ms, monotonic) and its current census interval
     * (ms).  The interval is set from the cost of the last census --
     * proportional to the number of blocks it had to walk -- and
     * doubled whenever a census finds nothing to do, so a busy
     * class with a huge central stack (jq building a DOM) spaces
     * its censuses out to seconds while a small class stays
     * responsive.  0 means "not yet censused". */
    _Atomic uint32_t last_census_ms;
    _Atomic uint32_t interval_ms;
    char pad[64 - sizeof(_Atomic(FM_Batch *)) - 3 * sizeof(_Atomic uint32_t)];
} FM_Central;

extern FM_Central fm_central[CLASS_COUNT];
/* Cold-path ownership: censuses share span metadata; detached batch
 * headers must not be recycled while a pop still reads their links. */
static pthread_mutex_t fm_census_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fm_central_lock = PTHREAD_MUTEX_INITIALIZER;

/* Per-thread flush counter per class: the automatic span release
 * runs its census once every FM_RELEASE_EVERY flushes of a class.
 * fm_in_census suppresses the trigger while a census (trim or
 * release) is already running on this thread. */
extern __thread uint32_t fm_flush_seq[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
extern __thread int fm_in_census __attribute__((tls_model("initial-exec")));
extern __thread FM_Batch *fm_batch_pool __attribute__((tls_model("initial-exec")));

/* Large-block cache (ADR 0011): per-thread cache of freed large blocks,
 * bucketed by the highest set bit of the mapping size, so a cached block
 * is never more than 2x the size of the request it can serve.  Without
 * it every large allocation is a fresh mmap and every free an munmap,
 * which measured ~9000 ns/op against glibc's 28 on this host. */
#define FM_LCACHE_BUCKETS 48
/* Byte budget for the per-thread large-block cache.  64 MiB: the round-3
 * sweep (ADR 0019) found the old 4 GiB budget was the main driver of the
 * allocator's peak-RSS excess in real apps; 64 MiB keeps every measured
 * churn workload at full speed while cutting most of that excess. */
#define FM_LCACHE_MAX_BYTES (64ull << 20)
extern __thread void *fm_lcache_head[FM_LCACHE_BUCKETS]
    __attribute__((tls_model("initial-exec")));
extern __thread uint64_t fm_lcache_bytes __attribute__((tls_model("initial-exec")));

/* Batch count field: block count in the low bits; the top bit marks a
 * span-pure batch (every block from one span).  A count of 0 means
 * "unknown" (legacy publishers); consumers must then walk. */
#define FM_COUNT_PURE 0x80000000u
#define FM_COUNT_MASK 0x7FFFFFFFu

/* Automatic release tuning (ADR 0020).  A census opportunity arises
 * once per FM_RELEASE_EVERY flushes of a class in one thread; all
 * classes are then considered.  A class is actually censused only
 * when its central
 * stack holds at least FM_RELEASE_MIN_DEPTH batches and its
 * (adaptive) interval has elapsed.  A span found entirely free is
 * first *marked*; it is unmapped only by a later census that finds
 * it still entirely free with a mark at least FM_RELEASE_AGE_MS
 * old -- the decay that keeps a working set cycling on a
 * millisecond period out of the mmap/fault cycle that sank eager
 * release in ADR 0014 (the cyclic patterns in the bench suite
 * cycle every ~3 ms; real shrinkage stays dead for seconds).
 * FM_RELEASE_KEEP spans per class are always retained as burst
 * absorption. */
#define FM_RELEASE_EVERY 16u
#define FM_RELEASE_MIN_DEPTH 4u
#define FM_RELEASE_KEEP 2u
#define FM_RELEASE_AGE_MS 100u
#define FM_RELEASE_INTERVAL_MS 10u
#define FM_RELEASE_MAX_INTERVAL_MS 5000u

/* ADR 0024: in the census rebuild, a span needs at least this many
 * blocks in central to earn its own span-pure batch; smaller
 * fragments merge into one mixed residue batch.  Bounds the batch
 * count (and batch-pool growth) a regrouping census can create at
 * roughly total_free_blocks / FM_REGROUP_MIN + 1. */
#define FM_REGROUP_MIN 8u

/* ------------------------------------------------------------------ */
/* runtime configuration (ADR 0027)                                    */
/*                                                                     */
/* The tuning constants above are defaults, not destiny: different   */
/* deployments weight speed against footprint differently, so the    */
/* slow-path knobs are runtime-configurable through the environment. */
/*                                                                     */
/*   FM_PROFILE            balanced (default) | throughput |         */
/*                         footprint -- named presets for the whole  */
/*                         knob set                                  */
/*   FM_RELEASE_AGE_MS     dead-span ageing before automatic unmap   */
/*   FM_RELEASE_KEEP       dead spans retained per class             */
/*   FM_RELEASE_MIN_DEPTH  central batches before a class is         */
/*                         eligible for census                       */
/*   FM_RELEASE_INTERVAL_MS     census interval floor                */
/*   FM_RELEASE_MAX_INTERVAL_MS census interval cap                  */
/*   FM_RELEASE_WALK_DIV   walked blocks per ms of census interval   */
/*   FM_REGROUP_MIN        min blocks for a span-pure batch (0024)   */
/*   FM_LCACHE_MAX_BYTES   per-thread large-block cache budget       */
/*   FM_PAGE_PURGE         page-granular return in partial spans     */
/*                         (ADR 0030): 0 = off, 1 = on               */
/*   FM_PURGE_MIN_PAGES    minimum new purge pages per span         */
/*   FM_REALLOC_CACHE_SEARCH  widen plain large realloc search      */
/*   FM_IDLE_RELEASE       opt-in maintenance thread (0036)         */
/*                                                                     */
/* Individual FM_* variables override the profile.  Values are       */
/* parsed once, lazily, on the first slow path (pthread_once), so    */
/* no hot path ever reads the environment and pre-constructor        */
/* callers (ADR 0016) are unaffected.  Malformed or out-of-range     */
/* values fall back to the profile/default value, clamped.           */
/* ------------------------------------------------------------------ */

typedef struct FM_Config {
    uint32_t release_age_ms;
    uint32_t release_keep;
    uint32_t release_min_depth;
    uint32_t release_interval_ms;
    uint32_t release_max_interval_ms;
    uint32_t release_walk_div;
    uint32_t regroup_min;
    uint64_t lcache_max_bytes;
    uint32_t page_purge;
    uint32_t purge_min_pages;
    uint32_t idle_release;
    uint32_t realloc_cache_search;
} FM_Config;

extern FM_Config fm_cfg;
extern pthread_once_t fm_cfg_once;

static void fm_config_init(void)
{
    FM_Config c;
    c.release_age_ms = FM_RELEASE_AGE_MS;
    c.release_keep = FM_RELEASE_KEEP;
    c.release_min_depth = FM_RELEASE_MIN_DEPTH;
    c.release_interval_ms = FM_RELEASE_INTERVAL_MS;
    c.release_max_interval_ms = FM_RELEASE_MAX_INTERVAL_MS;
    c.release_walk_div = 100;
    c.regroup_min = FM_REGROUP_MIN;
    c.lcache_max_bytes = FM_LCACHE_MAX_BYTES;
    c.page_purge = 1;
    c.purge_min_pages = 4;
    c.idle_release = 0;
    c.realloc_cache_search = 0;

    const char *prof = getenv("FM_PROFILE");
    if (prof) {
        if (!strcmp(prof, "throughput")) {
            /* Touch the release machinery as rarely as possible;
             * cache large blocks generously. */
            c.release_age_ms = 1000;
            c.release_keep = 8;
            c.release_min_depth = 16;
            c.release_interval_ms = 50;
            c.release_max_interval_ms = 30000;
            c.release_walk_div = 25;
            c.lcache_max_bytes = 256ull << 20;
            c.page_purge = 0;
        } else if (!strcmp(prof, "footprint")) {
            /* Return memory eagerly; keep almost nothing back. */
            c.release_age_ms = 25;
            c.release_keep = 0;
            c.release_min_depth = 2;
            c.release_interval_ms = 5;
            c.release_max_interval_ms = 1000;
            c.release_walk_div = 400;
            c.lcache_max_bytes = 8ull << 20;
            c.purge_min_pages = 2;
        }
        /* "balanced" and anything unrecognised keep the defaults. */
    }

    /* Individual overrides, clamped to sane ranges. */
    struct { const char *name; uint32_t *dst; uint32_t lo, hi; } num[] = {
        { "FM_RELEASE_AGE_MS", &c.release_age_ms, 0, 600000 },
        { "FM_RELEASE_KEEP", &c.release_keep, 0, 1024 },
        { "FM_RELEASE_MIN_DEPTH", &c.release_min_depth, 1, 65536 },
        { "FM_RELEASE_INTERVAL_MS", &c.release_interval_ms, 1, 600000 },
        { "FM_RELEASE_MAX_INTERVAL_MS", &c.release_max_interval_ms, 1, 3600000 },
        { "FM_RELEASE_WALK_DIV", &c.release_walk_div, 1, 1000000 },
        { "FM_REGROUP_MIN", &c.regroup_min, 1, 4096 },
        { "FM_PAGE_PURGE", &c.page_purge, 0, 1 },
        { "FM_PURGE_MIN_PAGES", &c.purge_min_pages, 1, 15 },
        { "FM_IDLE_RELEASE", &c.idle_release, 0, 1 },
        { "FM_REALLOC_CACHE_SEARCH", &c.realloc_cache_search, 0, 1 },
    };
    for (size_t i = 0; i < sizeof(num) / sizeof(num[0]); i++) {
        const char *v = getenv(num[i].name);
        if (!v || !*v)
            continue;
        char *end = NULL;
        unsigned long long x = strtoull(v, &end, 10);
        if (end == v)
            continue;
        if (x < num[i].lo)
            x = num[i].lo;
        if (x > num[i].hi)
            x = num[i].hi;
        *num[i].dst = (uint32_t)x;
    }
    const char *lb = getenv("FM_LCACHE_MAX_BYTES");
    if (lb && *lb) {
        char *end = NULL;
        unsigned long long x = strtoull(lb, &end, 10);
        if (end != lb) {
            if (x > (16ull << 30))
                x = 16ull << 30;
            c.lcache_max_bytes = x;
        }
    }
    if (c.release_max_interval_ms < c.release_interval_ms)
        c.release_max_interval_ms = c.release_interval_ms;

    fm_cfg = c;
}

/* Read a config field, initialising on first use.  Callers are all
 * slow paths (refill, flush, census, large alloc/free). */
#define FM_CFG() (pthread_once(&fm_cfg_once, fm_config_init), &fm_cfg)
/* The automatic census sets the class's next interval from how
 * many blocks it had to walk (one ms per 100 walked blocks,
 * doubling when it finds nothing, capped at 5 s): the walk is the
 * census's real cost, so a class with a huge, constantly-turning-
 * over stack (jq's DOM phases) spaces its censuses out until the
 * walk is a negligible duty cycle, while a small dead stack is
 * revisited quickly.  An earlier revision also capped how many
 * batches a census detaches; that broke the census's premise --
 * a span is provably dead only when ALL its blocks are visible,
 * and a capped detach sees a scattered fraction of every span --
 * so the interval carries the whole load. */

static inline uint32_t fm_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

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
/* central list: a serialized stack of batches                            */
/* ------------------------------------------------------------------ */

/*
 * A batch is a whole free list (or, since ADR 0014, one span's slice
 * of one), never an arbitrary fragment: publishing is O(1) because
 * the tail pointer is already known, and a refill takes a whole batch,
 * again O(1).  What changed in ADR 0014 is only how a flush *builds*
 * the batches as they are built (ADR 0014 kept only this much of
 * the shaping work: the bookkeeping the trim census needs).
 */

static inline void fm_central_push(uint32_t cls, FM_Batch *b)
{
    pthread_mutex_lock(&fm_central_lock);
    b->next = atomic_load_explicit(&fm_central[cls].head, memory_order_relaxed);
    atomic_store_explicit(&fm_central[cls].head, b, memory_order_relaxed);
    atomic_fetch_add_explicit(&fm_central[cls].depth, 1, memory_order_relaxed);
    pthread_mutex_unlock(&fm_central_lock);
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
    sp->dead_mark = 0;
    sp->map_base = (uintptr_t)base;
    sp->map_size = SPAN_SIZE;
    sp->user_size = 0;
    sp->align = 0;
    sp->purge_mark = 0;
    sp->purged_mask = 0;
    sp->withheld_cnt = 0;

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
static void fm_arm_thread_exit(void);
/* Automatic span release (ADR 0020): defined with the trim census
 * below; called from fm_flush_bin on its cadence. */
static void fm_auto_release_tick(uint32_t flushed_cls);

/*
 * (Historical note: publishing used to be a private O(1) hand-off,
 * fm_publish, that recorded nothing about the batch.  It is now
 * fm_publish2, which also records the batch's length and span-purity
 * when the publisher knows them for free -- the refill below does,
 * for the rest of a fresh span.  A flush still never walks its list:
 * the blocks of a free list are scattered across the heap, so each
 * step is a likely cache miss, and ADR 0014 measured exactly how
 * expensive buying that walk back is.)
 */

/* Take the next block of a class, refilling from the central list or from a
 * fresh span when needed.  Kept out of line so that the malloc fast path
 * needs no callee-saved registers and no stack frame. */
__attribute__((noinline)) void *fm_refill_class(uint32_t cls)
{
    fm_arm_thread_exit();
    uint32_t limit = kClassLimit[cls];

    /* Try the central list first: take a whole batch, O(1). */
    {
        pthread_mutex_lock(&fm_central_lock);
        FM_Batch *b = atomic_load_explicit(&fm_central[cls].head, memory_order_relaxed);
        if (b) {
            atomic_store_explicit(&fm_central[cls].head, b->next, memory_order_relaxed);
            atomic_fetch_sub_explicit(&fm_central[cls].depth, 1, memory_order_relaxed);
        }
        pthread_mutex_unlock(&fm_central_lock);
        if (b) {
            /* A span-pure batch going back into service witnesses
             * its span alive: cancel any dead mark (ADR 0020). */
            if ((b->count & FM_COUNT_MASK) && (b->count & FM_COUNT_PURE))
                ((FM_Span *)((uintptr_t)b->head & SPAN_MASK))->dead_mark = 0;
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
        /* The rest of the fresh span is one span-pure group with a
         * known count -- recorded on the batch (ADR 0015), which is
         * what lets the trim census account for it without walking. */
        fm_publish2(rest, tail, cls, total - keep, 1);
    }

    void *p = fm_head[cls];
    fm_head[cls] = *(void **)p;
    return p;
}

__attribute__((noinline)) static void fm_flush_bin(uint32_t cls)
{
    fm_arm_thread_exit();
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
    /* Publish the whole list in free order, as before
     * ADR 0014: grouping this list by span was implemented and
     * measured, and the walk cost more than the locality it bought
     * (see the ADR).  The count is not recorded -- computing it
     * would need the walk -- so the trim census walks these batches,
     * which is fine: trim is allowed to be slow. */
    fm_publish2(h, t, cls, 0, 0);

    /* Automatic span release (ADR 0020): every FM_RELEASE_EVERY
     * flushes of this class, offer the release machinery a tick.
     * The tick itself decides -- per class, behind depth, rate and
     * age gates -- whether anything is worth censusing, so this
     * stays a couple of TLS/atomic touches on an already cold path. */
    if (!fm_in_census &&
        ((++fm_flush_seq[cls] & (FM_RELEASE_EVERY - 1)) == 0))
        fm_auto_release_tick(cls);
}

/* ------------------------------------------------------------------ */
/* memory return: the trim census (ADR 0015) and the automatic         */
/* span release built on it (ADR 0020)                                 */
/* ------------------------------------------------------------------ */

/*
 * malloc_trim's job: find spans that are entirely free and unmap them.
 * At trim time we can afford to walk.  The census below counts, per
 * span, how many of its blocks sit in the central list (this thread's
 * local list is flushed into it first).  A span whose central count
 * equals its block count has no live blocks and no blocks held
 * anywhere else either -- every block exists exactly once across all
 * the allocator's lists, so if all of them are here, none are
 * anywhere else.  Those spans are unmapped; everything else is put
 * back.  Other threads' local lists are out of reach, which only
 * ever makes the count too small -- the safe direction.
 */

typedef struct FM_CensusEnt {
    uintptr_t span;
    uint32_t cnt;
    uint32_t dead;
    /* ADR 0024: survivor-chain accumulators for the regrouping
     * rebuild.  While the census puts the central stack back, each
     * span's surviving blocks are linked into one chain (ahead ..
     * atail, acnt blocks) so they can be re-published as a single
     * span-pure batch.  Only used during the rebuild phase, after
     * counting is complete. */
    void *ahead;
    void *atail;
    uint32_t acnt;
    /* ADR 0030 page purge.  For candidate spans the coverage pass
     * counts, per page, how many blocks intersecting the page are
     * free in central (page_free); page_tot is the same count over
     * all blocks of the span, computed arithmetically.  A page whose
     * two counts agree is fully free.  pg_new_mask / pg_new_withheld
     * record the purge this census decided to apply: its pages are
     * dropped during the rebuild (their blocks withheld), then
     * MADV_DONTNEED is applied to them.  pg_reclaim asks the rebuild
     * to first re-carve the span's previously withheld blocks back
     * into its survivor chain. */
    uint16_t page_free[16];
    uint16_t page_tot[16];
    uint16_t page_sfree[16];
    uint16_t page_stot[16];
    uint16_t pg_new_mask;
    uint16_t pg_new_withheld;
    uint16_t pg_skip_cnt;
    uint16_t pg_reclaim_cnt;
    uint32_t pg_flags;
} FM_CensusEnt;

/* FM_CensusEnt.pg_flags bits */
#define FM_PG_CANDIDATE 1u /* coverage pass should visit this span */
#define FM_PG_RECLAIM 2u   /* re-carve withheld blocks in the rebuild */

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
            e->ahead = NULL;
            e->atail = NULL;
            e->acnt = 0;
            e->pg_new_mask = 0;
            e->pg_new_withheld = 0;
            e->pg_skip_cnt = 0;
            e->pg_reclaim_cnt = 0;
            e->pg_flags = 0;
            memset(e->page_free, 0, sizeof(e->page_free));
            memset(e->page_tot, 0, sizeof(e->page_tot));
            memset(e->page_sfree, 0, sizeof(e->page_sfree));
            memset(e->page_stot, 0, sizeof(e->page_stot));
            c->used++;
        }
    }
    munmap(old, oldcap * sizeof(FM_CensusEnt));
    return 1;
}

/* Set when a census add had to be dropped (table full and growth
 * failed): counts are then underestimates, which is safe for the
 * dead-span proof but invalidates exact-count assertions. */
static _Thread_local int fm_census_dropped;

/* Add n to the free-block count of `span`.  On allocation failure the
 * add is dropped: the span simply will not look free, which is safe. */
static void fm_census_add(FM_Census *c, uintptr_t span, uint32_t n)
{
    if (!c->ents)
        return;
    if ((c->used + 1) * 2 >= c->cap && !fm_census_grow(c) && c->used + 1 >= c->cap) {
        fm_census_dropped = 1;
        return;
    }
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

/* Set by the most recent census: how many spans it newly marked
 * dead, how many it found marked but not yet aged out, and how many
 * blocks it had to walk one by one (span-pure batches with a known
 * count are accounted wholesale and do not count).  The automatic
 * release reads them to set the class's next interval (ADR 0020). */
static _Thread_local uint32_t fm_census_marked, fm_census_waiting, fm_census_walked;
/* ADR 0030: bytes returned by the most recent census's page purge,
 * and how many spans it reclaimed (re-carved) -- the automatic
 * release reads them alongside `released` when deciding whether
 * the census found anything to do. */
static _Thread_local size_t fm_census_purged;
static _Thread_local uint32_t fm_census_reclaimed;
/* Purge marks/applications get their own counter, separate from
 * fm_census_waiting: unlike a dead span (rare, and gone once
 * released), a partially-free span is the *common* case under
 * churn and can stay purge-worthy indefinitely, so letting purge
 * activity hold the census interval at its floor -- the exemption
 * dead-span release relies on -- would keep every churning class
 * censusing at full rate forever.  Purge findings therefore do
 * not exempt the interval from doubling; a long drain still gets
 * its purges, at widening intervals, each batching everything
 * then eligible. */
static _Thread_local uint32_t fm_census_pg_waiting;

/* ADR 0030: record one free block in a candidate span's per-page
 * coverage counts.  Pages are 4 KiB slices of the 64 KiB span; a
 * block touches the pages between its first and last byte
 * (page_free), and starts in exactly one of them (page_sfree) --
 * the purge decision needs both, for different reasons (see the
 * decision phase in fm_trim_class). */
static inline void fm_pg_cover(FM_CensusEnt *e, uintptr_t span, void *p,
                               size_t bs)
{
    uint32_t idx = (uint32_t)(((uintptr_t)p - span - HEADER_SIZE) / bs);
    uint32_t fp = (uint32_t)((HEADER_SIZE + (size_t)idx * bs) / FM_PAGE_SIZE);
    uint32_t lp = (uint32_t)((HEADER_SIZE + (size_t)(idx + 1) * bs - 1) /
                             FM_PAGE_SIZE);
    for (uint32_t pg = fp; pg <= lp; pg++)
        e->page_free[pg]++;
    e->page_sfree[fp]++;
}

/* qsort comparator: descending free-block count (ADR 0024). */
static int fm_batch_count_desc(const void *a, const void *b)
{
    uint32_t ca = (*(FM_Batch *const *)a)->count & FM_COUNT_MASK;
    uint32_t cb = (*(FM_Batch *const *)b)->count & FM_COUNT_MASK;
    return (ca < cb) - (ca > cb);
}

/* Census one class and unmap its provably-dead spans, retaining up
 * to `keep` dead spans (their blocks stay on the central stack for
 * reuse).  instant != 0 is the malloc_trim behaviour of ADR 0015
 * (every fully-free span dies now); instant == 0 is the automatic
 * release of ADR 0020, where a fully-free span must also carry a
 * dead mark at least FM_RELEASE_AGE_MS old.
 *
 * The same census also runs the page purge of ADR 0030: spans that
 * are only *partially* free cannot be unmapped, but their wholly
 * free, resident pages can be returned with MADV_DONTNEED.  The
 * coverage pass counts, per page, the blocks intersecting it; a
 * page is purgeable when every intersecting and starting block is
 * free in central; at least FM_PURGE_MIN_PAGES pages must qualify.
 * Blocks starting in those pages are withheld from rebuilt chains
 * and accounted in the span header, so the span's effective free count stays exact and
 * the dead-span proof above is unaffected.  Auto mode ages purge
 * decisions on purge_mark exactly like dead-span marks; instant
 * (trim) mode purges immediately.  A span whose circulating supply
 * runs out gets its withheld blocks re-carved (reclaim) instead. */
__attribute__((noinline)) static size_t fm_trim_class(uint32_t cls, uint32_t keep,
                                                      int instant)
{
    pthread_mutex_lock(&fm_census_lock);
    fm_census_walked = fm_census_marked = fm_census_waiting = 0;
    fm_census_reclaimed = fm_census_pg_waiting = 0;
    fm_census_purged = 0;
    int was_in_census = fm_in_census;
    fm_in_census = 1; /* the flush below must not re-trigger release */
    fm_flush_bin(cls);

    /* Detach the whole central stack atomically.  Batches published
     * after this point land on the fresh stack; batches another
     * thread's refill already popped are out of reach.  Both only
     * shrink what the census can see. */
    pthread_mutex_lock(&fm_central_lock);
    FM_Batch *all = atomic_exchange_explicit(&fm_central[cls].head, NULL,
                                             memory_order_acq_rel);
    atomic_store_explicit(&fm_central[cls].depth, 0, memory_order_relaxed);
    pthread_mutex_unlock(&fm_central_lock);
    if (!all) {
        fm_in_census = was_in_census;
        pthread_mutex_unlock(&fm_census_lock);
        return 0;
    }

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
        fm_in_census = was_in_census;
        pthread_mutex_unlock(&fm_census_lock);
        return 0;
    }

    /* Count free blocks per span.  Span-pure batches with a recorded
     * count are accounted wholesale; anything else is walked. */
    fm_census_walked = 0;
    fm_census_dropped = 0;
    for (FM_Batch *b = all; b; b = b->next) {
        uint32_t cnt = b->count & FM_COUNT_MASK;
        if (cnt && (b->count & FM_COUNT_PURE)) {
            fm_census_add(&cen, (uintptr_t)b->head & SPAN_MASK, cnt);
        } else {
            for (void *p = b->head; p; p = *(void **)p) {
                fm_census_walked++;
                fm_census_add(&cen, (uintptr_t)p & SPAN_MASK, 1);
            }
        }
    }

    /* A span is a candidate when every one of its blocks is here.
     * Instant mode kills candidates outright.  Automatic mode only
     * *marks* a candidate the first time; it dies once a later
     * census finds it still a candidate with an aged mark.  A span
     * seen with blocks missing is alive -- clear its mark.
     *
     * The count that matters is the *effective* free count: blocks
     * in central plus blocks withheld in purged pages (ADR 0030),
     * which are free by construction.  Spans that are not fully
     * free are considered for the page purge (and for reclaim of
     * previously withheld blocks) in the phases below. */
    const FM_Config *cfg = FM_CFG();
    const size_t bs = kClassSize[cls];
    uint32_t now = instant ? 0 : fm_now_ms();
    fm_census_marked = 0;
    fm_census_waiting = 0;
    fm_census_purged = 0;
    fm_census_reclaimed = 0;
    fm_census_pg_waiting = 0;
    int any_candidate = 0;
    for (size_t i = 0; i < cen.cap; i++) {
        FM_CensusEnt *e = &cen.ents[i];
        if (!e->span)
            continue;
        FM_Span *sp = (FM_Span *)e->span;
        uint32_t nb = sp->nblocks;
        uint32_t eff = e->cnt + sp->withheld_cnt;
#ifdef FM_DEBUG
        if (eff > nb) {
            fprintf(stderr, "[fm] CENSUS OVERCOUNT cls=%u span=%p cnt=%u "
                            "withheld=%u nblocks=%u\n",
                    cls, (void *)e->span, e->cnt, sp->withheld_cnt, nb);
            abort();
        }
#endif
        if (eff == nb) {
            if (instant) {
                e->dead = 1;
            } else if (!sp->dead_mark) {
                sp->dead_mark = now ? now : 1;
                fm_census_marked++;
            } else if ((uint32_t)(now - sp->dead_mark) >=
                       cfg->release_age_ms) {
                e->dead = 1;
            } else {
                fm_census_waiting++;
            }
            continue;
        }
        sp->dead_mark = 0;
        if (!cfg->page_purge)
            continue;
        /* ADR 0030 reclaim: a span whose withheld blocks are its
         * only free supply left (nothing of it circulates in
         * central) and whose purge has aged gets its blocks back --
         * demand has clearly returned to this span.  Auto mode
         * only: trim's job is to release, not to re-fault pages. */
        if (!instant && sp->purged_mask && e->cnt == 0 &&
            (uint32_t)(now - sp->purge_mark) >= cfg->release_age_ms) {
            e->pg_flags |= FM_PG_RECLAIM;
            continue;
        }
        /* Purge candidacy: enough free bytes in central that a
         * minimum-size purge run could exist.  The coverage pass
         * decides whether it really does. */
        if (e->cnt && (uint64_t)e->cnt * bs >=
                          (uint64_t)(cfg->purge_min_pages - 1) * FM_PAGE_SIZE) {
            e->pg_flags |= FM_PG_CANDIDATE;
            any_candidate = 1;
            for (uint32_t idx = 0; idx < nb; idx++) {
                uint32_t fp = (uint32_t)((HEADER_SIZE + (size_t)idx * bs) /
                                         FM_PAGE_SIZE);
                uint32_t lp = (uint32_t)((HEADER_SIZE +
                                          (size_t)(idx + 1) * bs - 1) /
                                         FM_PAGE_SIZE);
                for (uint32_t pg = fp; pg <= lp; pg++)
                    e->page_tot[pg]++;
                e->page_stot[fp]++;
            }
        } else if (!sp->purged_mask) {
            sp->purge_mark = 0;
        }
    }

    /* ADR 0030 coverage pass: for candidate spans, count per page
     * how many of the blocks intersecting it are free in central.
     * Span-pure batches of non-candidates are skipped wholesale;
     * mixed batches are walked (their blocks decide candidacy
     * block by block).  The walk is deliberately NOT charged to
     * fm_census_walked: that counter prices the census for the
     * release cadence, and charging the scouting pass doubled the
     * interval after every large census, starving the dead-span
     * machinery the purge shares the census with (measured on
     * consprobe: 9 censuses without the charge, 2 with it, and the
     * settled RSS went 9.0 -> 17.7 MB).  The coverage cost is
     * bounded by the candidate mass it can return, and the rebuild
     * walk below keeps the cadence honest. */
    uint32_t walked_pre_coverage = fm_census_walked;
    if (any_candidate) {
        for (FM_Batch *b = all; b; b = b->next) {
            uint32_t cnt = b->count & FM_COUNT_MASK;
            if (cnt && (b->count & FM_COUNT_PURE)) {
                FM_CensusEnt *e =
                    fm_census_find(&cen, (uintptr_t)b->head & SPAN_MASK);
                if (!e || !(e->pg_flags & FM_PG_CANDIDATE))
                    continue;
                for (void *p = b->head; p; p = *(void **)p) {
                    fm_census_walked++;
                    fm_pg_cover(e, e->span, p, bs);
                }
            } else {
                for (void *p = b->head; p; p = *(void **)p) {
                    fm_census_walked++;
                    FM_CensusEnt *e =
                        fm_census_find(&cen, (uintptr_t)p & SPAN_MASK);
                    if (e && (e->pg_flags & FM_PG_CANDIDATE))
                        fm_pg_cover(e, e->span, p, bs);
                }
            }
        }
    }

    /* ADR 0030 purge decision: a page of a candidate span is
     * purgeable when two exact counts agree --
     *
     *   - every block INTERSECTING the page is free in central
     *     (page_free == page_tot): no live block's bytes are
     *     discarded.  Blocks outside central (in some thread's
     *     local list) are invisible to the census, which only ever
     *     makes this count too small -- the safe direction.
     *   - every block STARTING in the page is free in central
     *     (page_sfree == page_stot): a free block's link word lives
     *     in its first bytes, so a block that starts in a purged
     *     page must be one the census itself holds and withholds;
     *     a same-class block sitting in another thread's local
     *     list would otherwise be handed out later with its link
     *     word zeroed.  This count is what makes the purge safe
     *     under concurrency, not just in a quiescent process.
     *
     * Blocks merely straddling INTO a purged page from a kept one
     * are harmless: their link words stay readable, and the tail
     * bytes discarded are a free block's, which mean nothing.
     * Resident pages only (an untouched page has no RSS to give
     * back), page 0 excluded (it holds the header), and the span
     * must yield at least FM_PURGE_MIN_PAGES new pages in total --
     * no contiguity is required.  The withheld set is then exactly
     * "blocks starting in purged pages", recomputable from the
     * mask at any later census (that is what makes reclaim safe).
     * In auto mode the decision is aged like dead-span release:
     * the first census that finds a span purge-worthy only
     * marks it. */
    if (any_candidate) {
        for (size_t i = 0; i < cen.cap; i++) {
            FM_CensusEnt *e = &cen.ents[i];
            if (!e->span || !(e->pg_flags & FM_PG_CANDIDATE))
                continue;
            FM_Span *sp = (FM_Span *)e->span;
            uint32_t nb = sp->nblocks;
            unsigned char vec[16];
            int have_vec = mincore((void *)e->span, SPAN_SIZE, vec) == 0;
            uint16_t new_mask = 0;
            uint32_t new_pages = 0;
            for (uint32_t pg = 1; pg < 16; pg++) {
                if (sp->purged_mask & (uint16_t)(1u << pg))
                    continue;
                if (e->page_tot[pg] && e->page_free[pg] == e->page_tot[pg] &&
                    e->page_sfree[pg] == e->page_stot[pg] &&
                    (!have_vec || (vec[pg] & 1))) {
                    new_mask |= (uint16_t)(1u << pg);
                    new_pages++;
                }
            }
            if (new_pages < cfg->purge_min_pages)
                new_mask = 0, new_pages = 0;
            uint16_t new_withheld = 0;
            if (new_pages) {
                for (uint32_t idx = 0; idx < nb; idx++) {
                    uint32_t fp = (uint32_t)((HEADER_SIZE +
                                              (size_t)idx * bs) /
                                             FM_PAGE_SIZE);
                    if (new_mask & (uint16_t)(1u << fp))
                        new_withheld++;
                }
            }
            if (!new_pages) {
                if (!instant && !sp->purged_mask)
                    sp->purge_mark = 0;
                continue;
            }
            if (instant) {
                e->pg_new_mask = new_mask;
                e->pg_new_withheld = new_withheld;
            } else if (!sp->purge_mark) {
                sp->purge_mark = now ? now : 1;
                fm_census_pg_waiting++;
            } else if ((uint32_t)(now - sp->purge_mark) >=
                       cfg->release_age_ms) {
                e->pg_new_mask = new_mask;
                e->pg_new_withheld = new_withheld;
                sp->purge_mark = now;
            } else {
                fm_census_pg_waiting++;
            }
        }
    }

    fm_census_walked = walked_pre_coverage;
    /* Retention quota (ADR 0020): a census in its automatic form
     * keeps up to `keep` of the dead spans -- their blocks stay on
     * the central stack, immediately reusable -- and unmaps only
     * the excess.  malloc_trim passes keep == 0 and releases all. */
    if (keep) {
        uint32_t kept = 0;
        for (size_t i = 0; i < cen.cap && kept < keep; i++) {
            FM_CensusEnt *e = &cen.ents[i];
            if (e->span && e->dead) {
                e->dead = 0;
                kept++;
            }
        }
    }

    /* Rebuild, regrouped by span (ADR 0024).  The pre-0024 rebuild
     * preserved the incoming batch boundaries, so the stack a
     * refill saw was a pile of mixed batches whose span composition
     * nobody knew: refill placement was span-blind, and recycled
     * blocks sprayed across every span of the class, pinning
     * partially-free spans forever.  The census has just visited
     * every block anyway, so grouping the survivors per span here
     * costs no extra walk -- span-pure batches splice wholesale and
     * mixed batches are already being walked block by block.  Each
     * span's survivors accumulate into one chain on its census
     * entry; blocks of dead spans are dropped.  Spans whose census
     * entry is missing (an add was dropped when the table could not
     * grow) keep their blocks in the residue chain: losing blocks
     * would leak them, grouping them wrongly would corrupt the
     * census's counts, so they stay mixed and merely ungrouped. */
    /* ADR 0030 reclaim, ahead of the rebuild: re-carve each flagged
     * span's withheld blocks (exactly the blocks starting in its
     * purged pages -- recomputed from the mask) into its survivor
     * chain, so they flow through the normal regroup publish below.
     * Writing the links re-faults the pages: they are back in
     * service, which is the point. */
    for (size_t i = 0; i < cen.cap; i++) {
        FM_CensusEnt *e = &cen.ents[i];
        if (!e->span || !(e->pg_flags & FM_PG_RECLAIM))
            continue;
        FM_Span *sp = (FM_Span *)e->span;
        uint32_t total = 0;
        for (uint32_t idx = 0; idx < sp->nblocks; idx++) {
            uint32_t fp = (uint32_t)((HEADER_SIZE + (size_t)idx * bs) /
                                     FM_PAGE_SIZE);
            if (!(sp->purged_mask & (uint16_t)(1u << fp)))
                continue;
            void *blk = (void *)(e->span + HEADER_SIZE + (size_t)idx * bs);
            if (e->atail)
                *(void **)e->atail = blk;
            else
                e->ahead = blk;
            e->atail = blk;
            e->acnt++;
            total++;
        }
#ifdef FM_DEBUG
        if (total != sp->withheld_cnt) {
            fprintf(stderr, "[fm] RECLAIM COUNT cls=%u span=%p carved=%u "
                            "withheld=%u\n",
                    cls, (void *)e->span, total, sp->withheld_cnt);
            abort();
        }
#endif
        e->pg_reclaim_cnt = (uint16_t)total;
        sp->purged_mask = 0;
        sp->withheld_cnt = 0;
        sp->purge_mark = 0;
        fm_census_reclaimed++;
    }

    void *res_head = NULL, *res_tail = NULL;
    uint32_t res_cnt = 0;
    for (FM_Batch *b = all; b;) {
        FM_Batch *nextb = b->next;
        uint32_t cnt = b->count & FM_COUNT_MASK;
        FM_CensusEnt *be = NULL;
        int splice = 0;
        if (cnt && (b->count & FM_COUNT_PURE)) {
            be = fm_census_find(&cen, (uintptr_t)b->head & SPAN_MASK);
            /* A batch of a span being purged cannot be spliced
             * wholesale: the blocks inside the purge run must be
             * withheld, so the chain is walked block by block. */
            splice = !(be && be->pg_new_mask);
        }
        if (splice) {
            FM_CensusEnt *e = be;
            if (e && e->dead) {
                /* the whole chain dies with its span */
            } else if (e) {
                if (e->atail)
                    *(void **)e->atail = b->head;
                else
                    e->ahead = b->head;
                e->atail = b->tail;
                e->acnt += cnt;
            } else {
                if (res_tail)
                    *(void **)res_tail = b->head;
                else
                    res_head = b->head;
                res_tail = b->tail;
                res_cnt += cnt;
            }
        } else {
            void *p = b->head;
            while (p) {
                void *nx = *(void **)p;
                FM_CensusEnt *e = fm_census_find(&cen, (uintptr_t)p & SPAN_MASK);
                if (e && e->dead) {
                    /* block dies with its span */
                } else if (e && e->pg_new_mask &&
                           (e->pg_new_mask &
                            (uint16_t)(1u << (((uintptr_t)p - e->span) /
                                              FM_PAGE_SIZE)))) {
                    /* Block starting in one of this census's newly
                     * purged pages: withhold it -- it joins no
                     * chain, and its link word is about to be
                     * discarded with its page.  Blocks merely
                     * straddling into a purged page test negative
                     * here (their first page is a kept one) and
                     * circulate safely. */
                    e->pg_skip_cnt++;
                } else if (e) {
                    if (e->atail)
                        *(void **)e->atail = p;
                    else
                        e->ahead = p;
                    e->atail = p;
                    e->acnt++;
                } else {
                    if (res_tail)
                        *(void **)res_tail = p;
                    else
                        res_head = p;
                    res_tail = p;
                    res_cnt++;
                }
                p = nx;
            }
        }
        fm_batch_release(b);
        b = nextb;
    }

    /* ADR 0030: apply this census's purges.  The withheld blocks
     * were already dropped from the rebuilt chains; discarding the
     * pages now cannot lose a link word anyone will read. */
    for (size_t i = 0; i < cen.cap; i++) {
        FM_CensusEnt *e = &cen.ents[i];
        if (!e->span || !e->pg_new_mask)
            continue;
        FM_Span *sp = (FM_Span *)e->span;
#ifdef FM_DEBUG
        if (!fm_census_dropped && e->pg_skip_cnt != e->pg_new_withheld) {
            fprintf(stderr, "[fm] PURGE SKIP COUNT cls=%u span=%p "
                            "skipped=%u expected=%u\n",
                    cls, (void *)e->span, e->pg_skip_cnt,
                    e->pg_new_withheld);
            abort();
        }
#endif
        for (uint32_t a = 1; a < 16;) {
            if (!(e->pg_new_mask & (uint16_t)(1u << a))) {
                a++;
                continue;
            }
            uint32_t b = a + 1;
            while (b < 16 && (e->pg_new_mask & (uint16_t)(1u << b)))
                b++;
            madvise((void *)(e->span + (size_t)a * FM_PAGE_SIZE),
                    (size_t)(b - a) * FM_PAGE_SIZE, MADV_DONTNEED);
            fm_census_purged += (size_t)(b - a) * FM_PAGE_SIZE;
            a = b;
        }
        sp->purged_mask |= e->pg_new_mask;
        sp->withheld_cnt = (uint16_t)(sp->withheld_cnt +
                                      (fm_census_dropped ? e->pg_skip_cnt
                                                         : e->pg_new_withheld));
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

    /* Form the new stack: one span-pure batch per surviving span
     * with at least FM_REGROUP_MIN blocks in central; smaller
     * fragments merge into a single mixed residue batch.  Batches
     * are pushed largest-free-count first, so the batch a refill
     * pops from the top of the stack is the FULLEST span's -- the
     * span least likely to empty soon -- while the freest spans'
     * blocks rest at the bottom, untouched, so those spans can
     * still empty completely as their last live blocks die.  The
     * residue is pushed last (top): its fragments come from the
     * fullest spans of all. */
    FM_Batch **order = NULL;
    size_t norder = 0;
    if (cen.used)
        order = mmap(NULL, cen.used * sizeof(FM_Batch *),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    if (order == MAP_FAILED)
        order = NULL;
    for (size_t i = 0; i < cen.cap; i++) {
        FM_CensusEnt *e = &cen.ents[i];
        if (!e->span || !e->acnt)
            continue;
        *(void **)e->atail = NULL;
#ifdef FM_DEBUG
        if (!fm_census_dropped && !e->dead &&
            e->acnt != e->cnt + e->pg_reclaim_cnt - e->pg_skip_cnt) {
            fprintf(stderr, "[fm] REGROUP COUNT cls=%u span=%p acnt=%u "
                            "cnt=%u reclaim=%u skip=%u\n",
                    cls, (void *)e->span, e->acnt, e->cnt,
                    e->pg_reclaim_cnt, e->pg_skip_cnt);
            abort();
        }
#endif
        if (e->acnt >= FM_CFG()->regroup_min && order) {
            FM_Batch *nb = fm_batch_alloc();
            if (nb) {
                nb->head = e->ahead;
                nb->tail = e->atail;
                nb->count = e->acnt | FM_COUNT_PURE;
                nb->next = NULL;
                order[norder++] = nb;
                continue;
            }
        }
        if (e->acnt >= FM_CFG()->regroup_min) {
            /* No sort array or no batch header: publish directly
             * (unsorted) rather than dropping the blocks. */
            fm_publish2(e->ahead, e->atail, cls, e->acnt, 1);
        } else {
            if (res_tail)
                *(void **)res_tail = e->ahead;
            else
                res_head = e->ahead;
            res_tail = e->atail;
            res_cnt += e->acnt;
        }
    }
    if (order) {
        qsort(order, norder, sizeof(FM_Batch *), fm_batch_count_desc);
        for (size_t i = 0; i < norder; i++)
            fm_central_push(cls, order[i]);
        munmap(order, cen.used * sizeof(FM_Batch *));
    }
    if (res_head) {
        *(void **)res_tail = NULL;
        fm_publish2(res_head, res_tail, cls, res_cnt, 0);
    }
    fm_census_destroy(&cen);
    fm_in_census = was_in_census;
    pthread_mutex_unlock(&fm_census_lock);
    return released;
}

/* Consider one class for automatic release (ADR 0020): census it
 * in the retaining, ageing form if its central stack is deep enough
 * and its interval has elapsed.  The interval is then re-derived
 * from the census just done: proportional to the blocks it walked
 * (the census's real cost), doubled when it found nothing at all,
 * capped at FM_RELEASE_MAX_INTERVAL_MS.  A huge stack that is
 * being actively recycled (jq's DOM phases) is mostly *not* dead,
 * and walking it is the tax this round originally shipped with --
 * spacing those censuses to seconds makes the tax negligible
 * while leaving small dead stacks on the fast cadence their
 * release needs. */
static void fm_release_maybe(uint32_t cls, uint32_t now)
{
    const FM_Config *cfg = FM_CFG();
    FM_Central *c = &fm_central[cls];
    if (atomic_load_explicit(&c->depth, memory_order_relaxed) <
        cfg->release_min_depth)
        return;
    uint32_t interval = atomic_load_explicit(&c->interval_ms,
                                              memory_order_relaxed);
    uint32_t last = atomic_load_explicit(&c->last_census_ms, memory_order_relaxed);
    if (last && (uint32_t)(now - last) <
                    (interval ? interval : cfg->release_interval_ms))
        return;
    atomic_store_explicit(&c->last_census_ms, now, memory_order_relaxed);
    size_t released = fm_trim_class(cls, cfg->release_keep, 0);
    uint32_t next = fm_census_walked / cfg->release_walk_div;
    if (next < cfg->release_interval_ms)
        next = cfg->release_interval_ms;
    if (released == 0 && fm_census_reclaimed == 0 &&
        fm_census_marked == 0 && fm_census_waiting == 0)
        next *= 2;
    if (next > cfg->release_max_interval_ms)
        next = cfg->release_max_interval_ms;
    atomic_store_explicit(&c->interval_ms, next, memory_order_relaxed);
}

static void fm_idle_kick(uint32_t now);

static void fm_auto_release_tick(uint32_t flushed_cls)
{
    (void)flushed_cls;
    uint32_t now = fm_now_ms();
    if (FM_CFG()->idle_release && !fm_in_census)
        fm_idle_kick(now);
    /* Consider every class, not just the one whose flush ticked: a
     * class that stops being flushed would otherwise never be
     * visited again, and its dead spans are exactly the ones worth
     * releasing.  The per-class gates are two atomic loads, so a
     * full sweep per tick is cheap; the census behind them is what
     * the gates protect. */
    for (uint32_t cls = 0; cls < CLASS_COUNT; cls++)
        fm_release_maybe(cls, now);
}

/* ------------------------------------------------------------------ */
/* Idle release (ADR 0029 design 1; round-6 prototype, opt-in)         */
/*                                                                     */
/* The census machinery only runs on allocator activity, so a        */
/* process that goes completely silent keeps whatever its last       */
/* burst left behind (measured in ADR 0032: every allocator in the   */
/* field is flat while silent).  With FM_IDLE_RELEASE=1 a detached   */
/* timer thread watches the flush-tick timestamp; after a full       */
/* second without one it runs the same ageing censuses the free      */
/* path would have run, so dead-span marks age out and purge         */
/* decisions land while the process is silent.  Fork prepare drains  */
/* census ownership before the snapshot; the child disables its      */
/* inherited timer until exec.  No signal handler runs a census.     */
/* ------------------------------------------------------------------ */

static _Atomic uint32_t fm_last_activity_ms;
static _Atomic int fm_idle_thread_state; /* -1 disabled in fork child, 0 none, 1 starting/running */

/* ADR 0040: shipped v2/JIT DSOs require ELF NODELETE, including custom builds.
 * dlclose retains worker code, TLS callbacks and allocator state until exit. */
static _Atomic int fm_atfork_ready;

static void fm_fork_prepare(void)
{
    pthread_mutex_lock(&fm_census_lock);
    pthread_mutex_lock(&fm_central_lock);
}

static void fm_fork_parent(void)
{
    pthread_mutex_unlock(&fm_central_lock);
    pthread_mutex_unlock(&fm_census_lock);
}

static void fm_idle_after_fork_child(void)
{
    /* A multithreaded fork child may only call async-signal-safe
     * functions before exec: never spawn a timer in that window. */
    atomic_store_explicit(&fm_idle_thread_state, -1, memory_order_relaxed);
    atomic_store_explicit(&fm_last_activity_ms, 0, memory_order_relaxed);
    fm_fork_parent();
}

static void *fm_idle_thread_main(void *arg)
{
    (void)arg;
    uint32_t last_sweep = 0;
    for (;;) {
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        uint32_t now = fm_now_ms();
        uint32_t last = atomic_load_explicit(&fm_last_activity_ms,
                                              memory_order_relaxed);
        if (!last || (uint32_t)(now - last) < 1000 ||
            (last_sweep && (uint32_t)(now - last_sweep) < 1000))
            continue;
        const FM_Config *cfg = FM_CFG();
        for (uint32_t cls = 0; cls < CLASS_COUNT; cls++) {
            FM_Central *c = &fm_central[cls];
            if (atomic_load_explicit(&c->depth, memory_order_relaxed) >=
                cfg->release_min_depth)
                fm_trim_class(cls, cfg->release_keep, 0);
        }
        /* Sweep at most once a second while silence lasts; each
         * sweep's censuses are a full ageing step apart, which is
         * what the mark-age-release pipeline needs. */
        last_sweep = fm_now_ms();
    }
    return NULL;
}

static void fm_idle_kick(uint32_t now)
{
    if (!atomic_load_explicit(&fm_atfork_ready, memory_order_acquire) ||
        atomic_load_explicit(&fm_idle_thread_state, memory_order_relaxed) < 0)
        return;
    atomic_store_explicit(&fm_last_activity_ms, now, memory_order_relaxed);
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &fm_idle_thread_state, &expected, 1, memory_order_acq_rel,
            memory_order_relaxed))
        return;
    /* Keep application signal handlers off the maintenance thread.
     * It inherits this mask; restore the creator's exact mask. */
    sigset_t blocked, previous;
    sigfillset(&blocked);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
        atomic_store_explicit(&fm_idle_thread_state, 0, memory_order_relaxed);
        return;
    }
    pthread_attr_t attr;
    int err = pthread_attr_init(&attr);
    if (!err) {
        err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (!err) {
            pthread_t th;
            /* Compile-time faults affect only this timer, never application threads. */
#ifdef FM_TEST_IDLE_CREATE_FAILURES
            static int failures = FM_TEST_IDLE_CREATE_FAILURES;
            if (failures != 0) {
                if (failures > 0) failures--;
                err = EAGAIN;
            } else
#endif
                err = pthread_create(&th, &attr, fm_idle_thread_main, NULL);
        }
        pthread_attr_destroy(&attr);
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (err != 0)
        atomic_store_explicit(&fm_idle_thread_state, 0, memory_order_relaxed);
}

static inline size_t fm_trim_spans(void)
{
    size_t released = 0;
    for (uint32_t cls = 0; cls < CLASS_COUNT; cls++)
        released += fm_trim_class(cls, 0, 1);
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
    size_t map_len = FM_ALIGN_UP(bytes + HEADER_SIZE + SPAN_SIZE, FM_PAGE_SIZE);
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
            fm_lcache_bytes + ms <= FM_CFG()->lcache_max_bytes) {
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
__attribute__((noinline)) static void *fm_large_alloc_search(size_t size, int *zeroed, int wide)
{
    fm_arm_thread_exit();
    size_t need_map = FM_ALIGN_UP(size + HEADER_SIZE, FM_PAGE_SIZE);
    int nb = 63 - __builtin_clzll(need_map);
    /* Search the need's own bucket, then the next one up: a block's
     * usable length can spill one bucket above its request size
     * because of the untrimmed head slack. Opt-in realloc growth
     * searches every remaining bucket, still bounded to eight entries
     * per bucket and preferring smaller capacities first. */
    for (int b = nb; b < FM_LCACHE_BUCKETS && (wide || b <= nb + 1); b++) {
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

static void *fm_large_alloc2(size_t size, int *zeroed)
{
    return fm_large_alloc_search(size, zeroed, 0);
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
/* thread-exit drain                                                   */
/* ------------------------------------------------------------------ */

/* Return this thread's free blocks to the central lists and release its
 * cached mappings.  Use only core helpers: TLS teardown must not malloc. */
static void fm_thread_exit_flush(void)
{
    for (uint32_t c = 0; c < CLASS_COUNT; c++)
        if (fm_head[c])
            fm_flush_bin(c);
    fm_large_cache_drain();
}

static pthread_key_t fm_exit_key;
static pthread_once_t fm_exit_once = PTHREAD_ONCE_INIT;

static void fm_exit_key_dtor(void *v)
{
    (void)v;
    if (fm_exit_armed == 2) return;
    /* Flush calls the arming slow path too; disarm before touching any bin
     * so pthread_setspecific is never called during this drain. */
    fm_exit_armed = 2;
    fm_thread_exit_flush();
}

static void fm_exit_make_key(void)
{
    pthread_key_create(&fm_exit_key, fm_exit_key_dtor);
    /* Register once, before any timer can start (including create retries). */
#ifdef FM_TEST_IDLE_ATFORK_FAIL
    /* Model registration failure without leaving callbacks registered. */
    return;
#endif
    if (pthread_atfork(fm_fork_prepare, fm_fork_parent,
                       fm_idle_after_fork_child) == 0)
        atomic_store_explicit(&fm_atfork_ready, 1, memory_order_release);
}

static void fm_arm_thread_exit(void)
{
    if (fm_exit_armed != 0) return;
    /* Mark armed first: pthread initialization can reenter the allocator. */
    fm_exit_armed = 1;
    pthread_once(&fm_exit_once, fm_exit_make_key);
    pthread_setspecific(fm_exit_key, (const void *)&fm_exit_key);
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
    if (sp->align == 0) {
        size_t old_len = sp->map_size;
        char *old_raw = (char *)sp->map_base;
        size_t off = (size_t)((char *)p - old_raw);
        /* Grow in place when the existing mapping already has room:
         * the common case after a large-cache reuse handed out an
         * oversized mapping.  Capacity is measured from the user
         * pointer to the end of the mapping.  (mremap-based growth
         * was tried three ways and rejected -- ADR 0018.) */
        if (size <= old_len - off) {
            sp->user_size = size;
            return p;
        }
    } else if (size <= sp->user_size) {
        return p;
    }
    void *q = sp->align ? fm_aligned_alloc_impl(sp->align, size)
                       : fm_large_alloc_search(size, NULL,
                                                FM_CFG()->realloc_cache_search);
    if (!q)
        return NULL;
    memcpy(q, p, sp->user_size);
    fm_large_free(sp, p);
    return q;
}

#endif /* FM_CORE_H */
