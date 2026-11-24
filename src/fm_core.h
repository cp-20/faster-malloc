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
 * fm_cnt[c] is the number of frees that may still be absorbed locally
 * before the list must be flushed (it counts down from kClassLimit[c] on
 * each free and is reset to kClassLimit[c] on refill/flush).  It is *not*
 * the length of the local list: mallocs pop without touching it, so the
 * allocation fast path stays a single dependent load and the free fast path
 * avoids loading the class limit.  Because a flush publishes the whole list
 * rather than "count" blocks, the counter never has to agree with length.
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
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <dlfcn.h>
#ifdef FM_DEBUG
#include <stdio.h>
#endif

#include "size_classes2.h"

#define FM_SPAN_MAGIC 0x5A6E4C5Au
#define FM_ALIGNED_MAGIC 0xA110CEDAu
#define FM_PAGE_SIZE 4096u
#define FM_ALIGN_UP(x, a) (((uintptr_t)(x) + (a)-1) & ~((uintptr_t)(a)-1))

static inline void *fm_malloc(size_t size);

/* Hot thread-local free-list state grouped into one struct with parallel
 * arrays inside it: head sits at offset 0 so %fs:(base,cls,8) has zero
 * displacement in malloc/free, while cnt and tail share the SAME TLS base
 * register in free via constant SIB displacements (%fs:disp(base,cls,4/8))
 * instead of loading three separate GOT/tpoff values. */
typedef struct FM_HotTls {
    void *head[CLASS_COUNT];
    int32_t cnt[CLASS_COUNT];
    void *tail[CLASS_COUNT];
} FM_HotTls;

extern __thread FM_HotTls fm_tls __attribute__((tls_model("initial-exec")));
extern __thread char *fm_bump_ptr[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
extern __thread char *fm_bump_end[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
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
    /* Early placement scans are paced in flushes, separately from ageing. */
    _Atomic uint32_t prompt_mask;
    char pad[64 - sizeof(_Atomic(FM_Batch *)) - 4 * sizeof(_Atomic uint32_t)];
} FM_Central;

extern FM_Central fm_central[CLASS_COUNT];
/* Cold-path ownership: censuses share span metadata; detached batch
 * headers must not be recycled while a pop still reads their links. */
static pthread_mutex_t fm_census_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fm_central_lock = PTHREAD_MUTEX_INITIALIZER;

/* ADR 0049: measured default; compile with =0 for the blocking control. */
#ifndef FM_CENSUS_TRYLOCK
#define FM_CENSUS_TRYLOCK 1
#endif

/* ADR 0041: experimental synchronous epochs; never enabled by default.
 * Closed admission + zero readers is a grace period for every old header. */
#ifdef FM_RECLAIM_EPOCH
static _Atomic unsigned fm_stack_epoch;
#define FM_EPOCH_CLOSED (1u << 31)
static void fm_epoch_enter(void)
{
    unsigned n = atomic_load_explicit(&fm_stack_epoch, memory_order_relaxed);
    for (;;) {
        if (n & FM_EPOCH_CLOSED) {
            sched_yield();
            n = atomic_load_explicit(&fm_stack_epoch, memory_order_relaxed);
        } else if (atomic_compare_exchange_weak_explicit(&fm_stack_epoch, &n, n + 1,
                    memory_order_acquire, memory_order_relaxed)) {
            return;
        }
    }
}
static void fm_epoch_leave(void)
{
    atomic_fetch_sub_explicit(&fm_stack_epoch, 1, memory_order_release);
}

static void fm_stack_exclusive(void)
{
    pthread_mutex_lock(&fm_central_lock);
    atomic_fetch_or_explicit(&fm_stack_epoch, FM_EPOCH_CLOSED, memory_order_acq_rel);
    while (atomic_load_explicit(&fm_stack_epoch, memory_order_acquire) != FM_EPOCH_CLOSED)
        sched_yield();
}
static void fm_stack_resume(void)
{
    atomic_store_explicit(&fm_stack_epoch, 0, memory_order_release);
    pthread_mutex_unlock(&fm_central_lock);
}
#else
#define fm_stack_exclusive() pthread_mutex_lock(&fm_central_lock)
#define fm_stack_resume() pthread_mutex_unlock(&fm_central_lock)
#endif

/* Per-thread flush counter per class: the automatic span release
 * runs its census once every FM_RELEASE_EVERY flushes of a class.
 * fm_in_census suppresses the trigger while a census (trim or
 * release) is already running on this thread. */
extern __thread uint32_t fm_flush_seq[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
extern __thread int fm_in_census __attribute__((tls_model("initial-exec")));
extern __thread FM_Batch *fm_batch_pool __attribute__((tls_model("initial-exec")));
static FM_Batch *fm_batch_global_head; /* protected by fm_central_lock */

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
/* Adaptive budget (large-cache slow path only): a bucket already proven to
 * reuse cached blocks (fm_lcache_warm_mask, set on a cache hit) may retain
 * twice the configured per-thread budget, capped here at 128 MiB.  Cold
 * buckets keep the configured budget exactly, so a teardown-shaped bucket
 * still stops retaining after FM_LCACHE_HOT_WINDOW frees and the footprint /
 * shortlived profiles keep their small (or zero) cache.  The cap is half the
 * throughput preset's 256 MiB and page purge stays on: this is a bounded
 * reuse boost, not the whole throughput profile. */
#define FM_LCACHE_WARM_BUDGET_CAP (128ull << 20)
extern __thread void *fm_lcache_head[FM_LCACHE_BUCKETS]
    __attribute__((tls_model("initial-exec")));
extern __thread uint64_t fm_lcache_bytes __attribute__((tls_model("initial-exec")));
extern __thread uint64_t fm_lcache_warm_mask __attribute__((tls_model("initial-exec")));
/* Large-block cache hotness (ADR 0011 follow-up): the cache pays off
 * only for sizes the workload allocates again soon; a bucket that
 * keeps receiving frees with no allocation in between (a one-shot
 * teardown, like the large half of a free-everything benchmark) is
 * pure resident retention.  fm_ltick counts this thread's large
 * FREES; fm_lbucket_seen[b] is the tick of the last allocation search
 * that touched bucket b.  A free caches only while fewer than
 * FM_LCACHE_HOT_WINDOW large frees have passed since that bucket was
 * last allocated from -- cycling workloads re-stamp it on every
 * allocation and never leave the window, teardowns fall out of it
 * after FM_LCACHE_HOT_WINDOW frees and unmapping resumes.  Buckets
 * start seen-at-0 with the tick at 0: the very first free of a fresh
 * bucket is always cached, so seeding a cycle still works.  All state
 * is per-thread, so this is race-free. */
#define FM_LCACHE_HOT_WINDOW 8u
extern __thread uint32_t fm_ltick __attribute__((tls_model("initial-exec")));
/* realloc -> large-miss hint (ADR 0075): the old user size of a buffer
 * being grown, or 0. Passed out of band so the shared cache-hit path
 * carries no extra argument. Read and cleared only on a miss. */
extern __thread size_t fm_grow_from __attribute__((tls_model("initial-exec")));
extern __thread uint32_t fm_lbucket_seen[FM_LCACHE_BUCKETS]
    __attribute__((tls_model("initial-exec")));

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
#define FM_PROMPT_MIN_FLUSHES (8u * FM_RELEASE_EVERY)
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
/*                         footprint | shortlived -- named presets   */
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
    uint32_t regroup_only_depth;
    uint64_t lcache_max_bytes;
    uint32_t page_purge;
    uint32_t purge_min_pages;
    uint32_t idle_release;
    uint32_t realloc_cache_search;
    uint32_t local_limit_div;
    uint32_t lcache_cold_purge;
    uint32_t lazy_bump;
    uint32_t locality_sort;
} FM_Config;

extern FM_Config fm_cfg;
extern pthread_once_t fm_cfg_once;
extern _Atomic int fm_cfg_ready;

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
    c.regroup_only_depth = 32;
    c.lcache_max_bytes = FM_LCACHE_MAX_BYTES;
    c.page_purge = 1;
    c.purge_min_pages = 4;
    c.idle_release = 0;
    c.realloc_cache_search = 0;
    c.local_limit_div = 1;
    /* Drop resident pages of cold (not-recently-reused) large blocks on
     * free, moving the balanced default partway toward the footprint
     * profile.  The mapping stays cached, so warm reuse is unchanged;
     * only the resident pages of teardown-shaped blocks are released. */
    c.lcache_cold_purge = 1;
    c.lazy_bump = 1;
    c.locality_sort = 0;

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
            c.lcache_cold_purge = 0;
        } else if (!strcmp(prof, "shortlived")) {
            /* ADR 0052: avoid retaining large temporaries across app phases. */
            c.lcache_max_bytes = 0;
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
            c.local_limit_div = 2;
            c.lcache_cold_purge = 1;
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
        { "FM_REGROUP_ONLY_DEPTH", &c.regroup_only_depth, 0, 65536 },
        { "FM_PAGE_PURGE", &c.page_purge, 0, 1 },
        { "FM_PURGE_MIN_PAGES", &c.purge_min_pages, 1, 15 },
        { "FM_IDLE_RELEASE", &c.idle_release, 0, 1 },
        { "FM_REALLOC_CACHE_SEARCH", &c.realloc_cache_search, 0, 1 },
        { "FM_LOCAL_LIMIT_DIV", &c.local_limit_div, 1, 16 },
        { "FM_LCACHE_COLD_PURGE", &c.lcache_cold_purge, 0, 1 },
        { "FM_LAZY_BUMP", &c.lazy_bump, 0, 1 },
        { "FM_LOCALITY_SORT", &c.locality_sort, 0, 1 },
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
    atomic_store_explicit(&fm_cfg_ready, 1, memory_order_release);
}

/* Read a config field, initialising on first use.  Callers are all
 * slow paths (refill, flush, census, large alloc/free); fast atomic
 * check avoids calling pthread_once through PLT/GOT after init. */
static inline const FM_Config *fm_get_cfg(void)
{
    if (__builtin_expect(!atomic_load_explicit(&fm_cfg_ready, memory_order_acquire), 0))
        pthread_once(&fm_cfg_once, fm_config_init);
    return &fm_cfg;
}
#define FM_CFG() fm_get_cfg()
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
    /* Try global freelist (cross-thread recycling). */
    /* Recycled nodes can leave and re-enter the stack between a load and
     * a CAS. Hold the same fork-aware lock as the central batch lists. */
    pthread_mutex_lock(&fm_central_lock);
    b = fm_batch_global_head;
    if (b)
        fm_batch_global_head = b->next;
    pthread_mutex_unlock(&fm_central_lock);
    if (b) {
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
    /* fm_batch_pool must chain ALL spare headers: pointing it at
     * first[127] (next == NULL) orphaned first[1..126] -- 126 headers
     * leaked per page the moment recycling was exhausted, which flush
     * splitting does in pure-free churn. */
    fm_batch_pool = &first[1];
    return &first[0];
}

static inline void fm_batch_release(FM_Batch *b)
{
    /* The next local publication usually needs the header we just popped. */
    if (!fm_batch_pool) {
        b->next = NULL;
        fm_batch_pool = b;
        return;
    }
    pthread_mutex_lock(&fm_central_lock);
    b->next = fm_batch_global_head;
    fm_batch_global_head = b;
    pthread_mutex_unlock(&fm_central_lock);
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
#ifdef FM_RECLAIM_EPOCH
    fm_epoch_enter();
    FM_Batch *top = atomic_load_explicit(&fm_central[cls].head, memory_order_acquire);
    /* Count pending publications too: a pop must never underflow depth. */
    atomic_fetch_add_explicit(&fm_central[cls].depth, 1, memory_order_relaxed);
    do {
        b->next = top;
    } while (!atomic_compare_exchange_weak_explicit(&fm_central[cls].head, &top, b,
                memory_order_acq_rel, memory_order_acquire));
    fm_epoch_leave();
#else
    fm_stack_exclusive();
    b->next = atomic_load_explicit(&fm_central[cls].head, memory_order_relaxed);
    atomic_store_explicit(&fm_central[cls].head, b, memory_order_relaxed);
    atomic_fetch_add_explicit(&fm_central[cls].depth, 1, memory_order_relaxed);
    fm_stack_resume();
#endif
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

/* Carve full, densely packed spans from a common 2 MiB extent. Each span
 * still owns exactly its own mapping range and can be released independently.
 * The central lock also serializes the cursor across fork. */
static struct {
    char *cursor;
    unsigned remaining;
    unsigned warmup;
    uint32_t opened_ms;
} fm_dense_extent;

static char *fm_map_dense_span(void)
{
    enum { extent_size = 2 * 1024 * 1024, map_len = 2 * extent_size - FM_PAGE_SIZE };
    enum { burst_ms = 100, warmup_extents = 4 };
    /* N32: open the dense extent lazily.  The very first small span gets a
     * standalone 64 KiB mapping from fm_map_aligned instead of reserving a
     * 2 MiB extent, so a start-up heap that never needs dense carves pays
     * neither the 4 MiB alignment window nor a mostly-empty 2 MiB mapping.
     * Every later span switches to the shared dense extent as before. */
    static _Atomic int dense_started;
    fm_stack_exclusive();
    if (!atomic_exchange_explicit(&dense_started, 1, memory_order_relaxed)) {
        fm_stack_resume();
        return fm_map_aligned(SPAN_SIZE - HEADER_SIZE);
    }
    if (!fm_dense_extent.remaining) {
        char *raw = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            fm_stack_resume();
            return fm_map_aligned(SPAN_SIZE - HEADER_SIZE);
        }
        char *base = (char *)FM_ALIGN_UP((uintptr_t)raw, extent_size);
        size_t prefix = (size_t)(base - raw);
        size_t suffix = map_len - prefix - extent_size;
        if (prefix)
            munmap(raw, prefix);
        if (suffix)
            munmap(base + extent_size, suffix);
        /* A rapidly consumed extent predicts dense demand for the next.
         * Sparse/startup heaps get demand-paged base pages. */
        uint32_t now = fm_now_ms();
        if (fm_dense_extent.warmup < warmup_extents)
            fm_dense_extent.warmup++;
        else if (now - fm_dense_extent.opened_ms < burst_ms && fm_dense_extent.cursor)
            (void)madvise(base, extent_size, MADV_HUGEPAGE);
        fm_dense_extent.opened_ms = now;
        fm_dense_extent.cursor = base;
        fm_dense_extent.remaining = extent_size / SPAN_SIZE;
    }
    char *span = fm_dense_extent.cursor;
    fm_dense_extent.cursor += SPAN_SIZE;
    fm_dense_extent.remaining--;
    fm_stack_resume();
    return span;
}

/* Per-thread 4 MiB base-page slab for >4 KiB class spans and <=512 KiB
 * large allocations: amortizes mmap/trim syscalls without hinting THP on
 * potentially sparse allocations, and drains on malloc_trim / thread exit.
 * A 4 MiB slab holds twice as many 64 KiB carves as 2 MiB, so g++/jq/xz
 * medium-buffer churn crosses the mmap/munmap slow path half as often;
 * untouched tail pages stay non-resident, so RSS is unaffected. */
static _Thread_local char *fm_lextent_cur __attribute__((tls_model("initial-exec")));
static _Thread_local size_t fm_lextent_rem __attribute__((tls_model("initial-exec")));

static char *fm_map_large_slab(size_t bytes, uintptr_t *raw_out, size_t *len_out)
{
    size_t need = FM_ALIGN_UP(bytes + HEADER_SIZE, SPAN_SIZE);
    if (fm_lextent_rem >= need) {
        char *base = fm_lextent_cur;
        fm_lextent_cur += need;
        fm_lextent_rem -= need;
        *raw_out = (uintptr_t)base;
        *len_out = need;
        return base;
    }
    if (fm_lextent_rem) {
        munmap(fm_lextent_cur, fm_lextent_rem);
        fm_lextent_cur = NULL;
        fm_lextent_rem = 0;
    }
    size_t slab_len = (4u << 20) + SPAN_SIZE;
    char *raw = mmap(NULL, slab_len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;
    char *aligned = (char *)FM_ALIGN_UP((uintptr_t)raw, SPAN_SIZE);
    size_t head_slack = (size_t)(aligned - raw);
    if (head_slack)
        munmap(raw, head_slack);
    *raw_out = (uintptr_t)aligned;
    *len_out = need;
    fm_lextent_cur = aligned + need;
    fm_lextent_rem = slab_len - head_slack - need;
    return aligned;
}

/* Map and initialise a fresh span for class cls.  Blocks are carved
 * lazily from the span's bump range by the refill path, so creation
 * does not touch every block's memory.  `first_cap_out` receives the
 * initial-carve cap for the class's first span (0 when unrestricted).
 * Small classes own a full 64 KiB dense-extent slot; a one-block large
 * class gets a tight first mapping holding just its block and header. */
static inline char *fm_span_new(uint32_t cls, uint32_t *count_out,
                                uint32_t *first_cap_out)
{
    size_t bs = kClassSize[cls];
    size_t bytes = SPAN_SIZE;
    uint32_t first_cap = 0;
#ifdef FM_SMALL_FIRST_SPAN
    /* ADR 0060 kept startup RSS low by shrinking nblocks on the first
     * span.  That still consumed a full 64 KiB dense-extent slot, so a
     * hot startup class opened a *second* slot as soon as its first
     * 16 KiB was used, exhausting the 2 MiB extent sooner (extra mmap).
     * Give every span the full slot but cap only the first carve to the
     * ADR 0060 budget: later refills bump-carve the same mapping, so a
     * second span is opened only when the first is truly exhausted.
     * Tail pages stay untouched until blocks from them are handed out. */
    static _Atomic unsigned char started[CLASS_COUNT];
    if (!atomic_exchange_explicit(&started[cls], 1, memory_order_relaxed)) {
        uint32_t cap = (uint32_t)((bs > 16320 ? 1u : 16320u / bs));
        first_cap = cap ? cap : 1;
    }
#endif
    size_t n = (bytes - HEADER_SIZE) / bs;
    uintptr_t raw_base = 0;
    size_t raw_len = SPAN_SIZE;
    char *base;
#ifdef FM_SMALL_FIRST_SPAN
    /* N31 (combined with N2): a class whose first carve is a single large
     * block does not need a whole 64 KiB slab at startup.  Map just the
     * block plus its header.  n and map_size stay truthful so census,
     * mincore, page purge and whole-span release keep working.  N2's
     * full-slot carve cap still governs the small (dense-extent) classes,
     * which open a second slot only once the first is exhausted.  The JIT
     * build does not define this macro, so it keeps full-size first spans
     * (ADR 0061). */
    int tight_first = (first_cap == 1 && bs > FM_PAGE_SIZE);
#else
    int tight_first = 0;
#endif
    if (tight_first) {
        raw_len = FM_ALIGN_UP(bs + HEADER_SIZE, FM_PAGE_SIZE);
        base = fm_map_aligned(raw_len - HEADER_SIZE);
        raw_base = (uintptr_t)base;
        n = 1;
    } else if (bs <= FM_PAGE_SIZE) {
        base = fm_map_dense_span();
        raw_base = (uintptr_t)base;
    } else {
        base = fm_map_large_slab(SPAN_SIZE - HEADER_SIZE, &raw_base, &raw_len);
        if (!base) {
            base = fm_map_aligned(SPAN_SIZE - HEADER_SIZE);
            raw_base = (uintptr_t)base;
            raw_len = SPAN_SIZE;
        }
    }
    if (!base && tight_first) {
        /* Tight mapping unavailable: fall back to the normal large slab. */
        raw_len = SPAN_SIZE;
        n = (SPAN_SIZE - HEADER_SIZE) / bs;
        base = fm_map_large_slab(SPAN_SIZE - HEADER_SIZE, &raw_base, &raw_len);
        if (!base) {
            base = fm_map_aligned(SPAN_SIZE - HEADER_SIZE);
            raw_base = (uintptr_t)base;
            raw_len = SPAN_SIZE;
        }
    }
    if (!base)
        return NULL;

    FM_Span *sp = (FM_Span *)base;
    sp->cls = cls;
    sp->magic = FM_SPAN_MAGIC;
    sp->nblocks = (uint32_t)n;
    sp->dead_mark = 0;
    sp->map_base = raw_base;
    sp->map_size = raw_len;
    sp->user_size = 0;
    sp->align = 0;
    sp->purge_mark = 0;
    sp->purged_mask = 0;
    sp->withheld_cnt = 0;

    *count_out = (uint32_t)n;
    *first_cap_out = first_cap;
    return base;
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

static inline uint32_t fm_local_limit(uint32_t cls)
{
    uint32_t limit = kClassLimit[cls];
    /* The generated kClassLimit caps every class at 256 blocks.  For the
     * small classes that dominate parsing/compiler bursts that leaves most
     * of the intended 64 KiB per-class byte budget unused (only 4 KiB for
     * 16 B blocks, 32 KiB for 128 B blocks).  Spend the full budget: a
     * larger local limit means fewer flushes -- fewer central-lock
     * acquisitions and batch round trips -- while retained bytes stay
     * bounded at 64 KiB per class.  The div override still scales it. */
    uint32_t budget = (uint32_t)((64u * 1024u) / kClassSize[cls]);
    if (budget > limit)
        limit = budget;
    uint32_t div = FM_CFG()->local_limit_div;
    if (div > 1) {
        limit /= div;
        if (!limit)
            limit = 1;
    }
    return limit;
}

/* Group a mixed free-list batch by 64 KiB span so consecutive allocations
 * stay on the same span/pages and reduce D1/TLB misses (opt-in FM_LOCALITY_SORT=1). */
static void *fm_regroup_by_span(void *head, void **tail_out)
{
    enum { NBUCKETS = 16 };
    uintptr_t keys[NBUCKETS];
    void *bhead[NBUCKETS];
    void *btail[NBUCKETS];
    int nb = 0;
    void *ov_h = NULL, *ov_t = NULL;

    void *p = head;
    while (p) {
        void *next = *(void **)p;
        uintptr_t sp = (uintptr_t)p & SPAN_MASK;
        int found = -1;
        for (int i = 0; i < nb; i++) {
            if (keys[i] == sp) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            *(void **)p = bhead[found];
            bhead[found] = p;
        } else if (nb < NBUCKETS) {
            keys[nb] = sp;
            bhead[nb] = p;
            btail[nb] = p;
            *(void **)p = NULL;
            nb++;
        } else {
            *(void **)p = ov_h;
            if (!ov_t)
                ov_t = p;
            ov_h = p;
        }
        p = next;
    }

    void *out_h = NULL, *out_t = NULL;
    for (int i = 0; i < nb; i++) {
        if (!out_h) {
            out_h = bhead[i];
            out_t = btail[i];
        } else {
            *(void **)out_t = bhead[i];
            out_t = btail[i];
        }
    }
    if (ov_h) {
        if (!out_h) {
            out_h = ov_h;
            out_t = ov_t;
        } else {
            *(void **)out_t = ov_h;
            out_t = ov_t;
        }
    }
    if (out_t)
        *(void **)out_t = NULL;
    *tail_out = out_t;
    return out_h;
}

/* Take the next block of a class, refilling from the central list or from a
 * fresh span when needed.  Kept out of line so that the malloc fast path
 * needs no callee-saved registers and no stack frame. */
__attribute__((noinline, visibility("hidden"))) void *fm_refill_class(uint32_t cls)
{
    fm_arm_thread_exit();

    /* Try the central list first: take a whole batch, O(1). */
    {
#ifdef FM_RECLAIM_EPOCH
        fm_epoch_enter();
        FM_Batch *b = atomic_load_explicit(&fm_central[cls].head, memory_order_acquire);
        while (b && !atomic_compare_exchange_weak_explicit(&fm_central[cls].head,
                    &b, b->next, memory_order_acq_rel, memory_order_acquire)) {}
        if (b)
            atomic_fetch_sub_explicit(&fm_central[cls].depth, 1, memory_order_relaxed);
        fm_epoch_leave();
        if (b) {
            /* Retired header stays immutable until all stale readers leave. */
            fm_stack_exclusive();
            fm_stack_resume();
        }
#else
        fm_stack_exclusive();
        FM_Batch *b = atomic_load_explicit(&fm_central[cls].head, memory_order_relaxed);
        if (b) {
            atomic_store_explicit(&fm_central[cls].head, b->next, memory_order_relaxed);
            atomic_fetch_sub_explicit(&fm_central[cls].depth, 1, memory_order_relaxed);
        }
        fm_stack_resume();
#endif
        if (b) {
            /* A span-pure batch going back into service witnesses
             * its span alive: cancel any dead mark (ADR 0020). */
            void *bh = b->head;
            void *bt = b->tail;
            if (b->count & FM_COUNT_PURE) {
                ((FM_Span *)((uintptr_t)bh & SPAN_MASK))->dead_mark = 0;
            } else if (FM_CFG()->locality_sort && bh) {
                bh = fm_regroup_by_span(bh, &bt);
            }
            fm_tls.head[cls] = *(void **)bh;
            fm_tls.tail[cls] = bt;
            fm_batch_release(b);
            return bh;
        }
    }

    /* Central list is empty: carve from this class's bump range, or
     * map a fresh span and bump-carve sequentially from it. */
    uint32_t limit = fm_local_limit(cls);
    size_t bs = kClassSize[cls];
    char *cur = fm_bump_ptr[cls];
    char *end = fm_bump_end[cls];
    if (cur >= end) {
        uint32_t total = 0, first_cap = 0;
        char *base = fm_span_new(cls, &total, &first_cap);
        if (!base)
            return NULL;
        cur = base + HEADER_SIZE;
        end = cur + (size_t)total * bs;
        uint32_t avail0 = (uint32_t)((end - cur) / bs);
        uint32_t keep0 = limit < avail0 ? limit : avail0;
        /* First span: carve only the ADR 0060 startup budget.  The
         * remainder stays in this span's bump range, so the next
         * refill reuses the same 64 KiB mapping instead of opening a
         * second one. */
        if (first_cap && keep0 > first_cap)
            keep0 = first_cap;
        int lazy = FM_CFG()->lazy_bump;
        if (lazy) {
            /* N9: linking a fresh span used to write a next pointer into
             * every one of its first `keep0` blocks, faulting up to a whole
             * span of pages before a single allocation was made.  With lazy
             * bump the unlinked tail stays reachable through fm_bump_ptr,
             * so link only the first page worth of blocks and let the rest
             * fault in on demand.  Takes the tighter of this and N58's
             * first-carve cap; span geometry and nblocks are unchanged. */
            uint32_t warm = (uint32_t)(FM_PAGE_SIZE / bs);
            if (warm < 1)
                warm = 1;
            if (keep0 > warm)
                keep0 = warm;
        }

        void *head = cur;
        void *t = cur;
        for (uint32_t i = 1; i < keep0; i++) {
            void *next = (char *)t + bs;
            *(void **)t = next;
            t = next;
        }
        *(void **)t = NULL;
#ifdef FM_DEBUG
        for (uint32_t i = 0; i < keep0; i++)
            ((uint64_t *)(cur + (size_t)i * bs))[1] = 0xDEADDEADDEADDEADull;
#endif
        fm_tls.head[cls] = *(void **)head;
        fm_tls.tail[cls] = t;
        fm_tls.cnt[cls] = (int32_t)limit;

        char *rest_cur = cur + (size_t)keep0 * bs;
        if (lazy) {
            /* Defer touching/linking the remainder of the span until needed
             * so L1D is not polluted by unused blocks. */
            fm_bump_ptr[cls] = rest_cur;
            fm_bump_end[cls] = end;
        } else {
            if (avail0 > keep0) {
                /* Publish remainder as span-pure batch for reuse. */
                uint32_t rcount = avail0 - keep0;
                void *rhead = rest_cur;
                void *rt = rest_cur;
                for (uint32_t i = 1; i < rcount; i++) {
                    void *next = (char *)rt + bs;
                    *(void **)rt = next;
                    rt = next;
                }
                *(void **)rt = NULL;
#ifdef FM_DEBUG
                for (uint32_t i = 0; i < rcount; i++)
                    ((uint64_t *)(rest_cur + (size_t)i * bs))[1] = 0xDEADDEADDEADDEADull;
#endif
                fm_publish2(rhead, rt, cls, rcount, 1);
            }
            fm_bump_ptr[cls] = end;
            fm_bump_end[cls] = end;
        }

        return head;
    }
    uint32_t avail = (uint32_t)((end - cur) / bs);
    uint32_t keep = limit < avail ? limit : avail;

    void *head = cur;
    void *t = cur;
    for (uint32_t i = 1; i < keep; i++) {
        void *next = (char *)t + bs;
        *(void **)t = next;
        t = next;
    }
    *(void **)t = NULL;
#ifdef FM_DEBUG
    for (uint32_t i = 0; i < keep; i++)
        ((uint64_t *)(cur + (size_t)i * bs))[1] = 0xDEADDEADDEADDEADull;
#endif
    fm_bump_ptr[cls] = cur + (size_t)keep * bs;

    fm_tls.head[cls] = *(void **)head;
    fm_tls.tail[cls] = t;
    fm_tls.cnt[cls] = (int32_t)limit;

    return head;
}

__attribute__((noinline)) static void fm_flush_bin(uint32_t cls)
{
    fm_arm_thread_exit();
#ifdef FM_DEBUG
    {
        /* Walk the local list and verify every block belongs to a span of
         * this class, and that the list is a proper chain. */
        uint32_t n = 0;
        void *q = fm_tls.head[cls];
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
        (void)n;
    }
#endif
    void *h = fm_tls.head[cls];
    void *t = fm_tls.tail[cls];
    fm_tls.head[cls] = NULL;
    fm_tls.tail[cls] = NULL;
    fm_tls.cnt[cls] = (int32_t)fm_local_limit(cls);
    if (!h)
        return;
    /* Publish in O(1) without walking the scattered list (ADR 0014);
     * census walks count==0 batches only if/when a census actually runs. */
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

static FM_Census fm_shared_census; /* protected by fm_census_lock */

static int fm_census_acquire(FM_Census *c)
{
    if (!fm_shared_census.ents) {
        fm_shared_census.cap = 256;
        fm_shared_census.used = 0;
        FM_CensusEnt *ents = mmap(NULL, 256 * sizeof(FM_CensusEnt),
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ents == MAP_FAILED)
            return 0;
        fm_shared_census.ents = ents;
    }
    *c = fm_shared_census;
    return 1;
}

static void fm_census_reset(FM_Census *c)
{
    if (c->ents && c->used) {
        for (size_t i = 0; i < c->cap; i++) {
            if (c->ents[i].span)
                c->ents[i].span = 0;
        }
        c->used = 0;
    }
    fm_shared_census = *c;
}

static void fm_census_destroy(FM_Census *c)
{
    if (c->ents && c->ents != MAP_FAILED)
        munmap(c->ents, c->cap * sizeof(FM_CensusEnt));
    c->ents = NULL;
    c->cap = 0;
    c->used = 0;
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
        e->cnt = 0;
        e->dead = 0;
        e->ahead = NULL;
        e->atail = NULL;
        e->acnt = 0;
        e->pg_new_mask = 0;
        e->pg_new_withheld = 0;
        e->pg_skip_cnt = 0;
        e->pg_reclaim_cnt = 0;
        e->pg_flags = 0;
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
    fm_census_walked = fm_census_marked = fm_census_waiting = 0;
    fm_census_reclaimed = fm_census_pg_waiting = 0;
    fm_census_purged = 0;
#if FM_CENSUS_TRYLOCK
    /* Automatic release is opportunistic; explicit trim still waits. */
    if (!instant) {
        if (pthread_mutex_trylock(&fm_census_lock) != 0)
            return 0;
    } else
#endif
        pthread_mutex_lock(&fm_census_lock);
    int was_in_census = fm_in_census;
    fm_in_census = 1; /* the flush below must not re-trigger release */
    fm_flush_bin(cls);

    /* Detach the whole central stack atomically.  Batches published
     * after this point land on the fresh stack; batches another
     * thread's refill already popped are out of reach.  Both only
     * shrink what the census can see. */
    fm_stack_exclusive();
    FM_Batch *all = atomic_exchange_explicit(&fm_central[cls].head, NULL,
                                             memory_order_acq_rel);
    atomic_store_explicit(&fm_central[cls].depth, 0, memory_order_relaxed);
    fm_stack_resume();
    if (!all) {
        fm_in_census = was_in_census;
        pthread_mutex_unlock(&fm_census_lock);
        return 0;
    }

    FM_Census cen;
    if (!fm_census_acquire(&cen)) {
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
            /* Serial pointer chase: overlap the census slot's first
             * touch with the chain hop by prefetching the NEXT block's
             * slot one step ahead (the slot index is computable from
             * the link word without loading the span header). */
            void *p = b->head;
            if (p) {
                __builtin_prefetch(&cen.ents[(((uintptr_t)p >> 16)) &
                                             (cen.cap - 1)], 1, 1);
                for (; p; p = *(void **)p) {
                    void *nx = *(void **)p;
                    if (nx)
                        __builtin_prefetch(&cen.ents[(((uintptr_t)nx >> 16)) &
                                                     (cen.cap - 1)], 1, 1);
                    fm_census_walked++;
                    fm_census_add(&cen, (uintptr_t)p & SPAN_MASK, 1);
                }
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
            memset(e->page_free, 0, sizeof(e->page_free));
            memset(e->page_tot, 0, sizeof(e->page_tot));
            memset(e->page_sfree, 0, sizeof(e->page_sfree));
            memset(e->page_stot, 0, sizeof(e->page_stot));
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
            uint16_t cand_mask = 0;
            uint32_t cand_pages = 0;
            for (uint32_t pg = 1; pg < 16; pg++) {
                if (sp->purged_mask & (uint16_t)(1u << pg))
                    continue;
                if (e->page_tot[pg] && e->page_free[pg] == e->page_tot[pg] &&
                    e->page_sfree[pg] == e->page_stot[pg]) {
                    cand_mask |= (uint16_t)(1u << pg);
                    cand_pages++;
                }
            }
            uint16_t new_mask = 0;
            uint32_t new_pages = 0;
            if (cand_pages >= cfg->purge_min_pages) {
                unsigned char vec[16];
                int have_vec = mincore((void *)e->span, sp->map_size, vec) == 0;
                for (uint32_t pg = 1; pg < 16; pg++) {
                    if ((cand_mask & (uint16_t)(1u << pg)) &&
                        (!have_vec || (vec[pg] & 1))) {
                        new_mask |= (uint16_t)(1u << pg);
                        new_pages++;
                    }
                }
                if (new_pages < cfg->purge_min_pages)
                    new_mask = 0, new_pages = 0;
            }
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
#ifdef FM_DEBUG
            ((uint64_t *)blk)[1] = 0xDEADDEADDEADDEADull;
#endif
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
            if (p)
                __builtin_prefetch(&cen.ents[(((uintptr_t)p >> 16)) &
                                             (cen.cap - 1)], 1, 1);
            while (p) {
                void *nx = *(void **)p;
                if (nx)
                    __builtin_prefetch(&cen.ents[(((uintptr_t)nx >> 16)) &
                                                 (cen.cap - 1)], 1, 1);
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
    FM_Batch *order_stk[256];
    FM_Batch **order = NULL;
    int order_mapped = 0;
    size_t norder = 0;
    size_t free_blocks = 0;
    if (cen.used <= 256) {
        order = order_stk;
    } else {
        order = mmap(NULL, cen.used * sizeof(FM_Batch *),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
        if (order == MAP_FAILED)
            order = NULL;
        else
            order_mapped = 1;
    }
    for (size_t i = 0; i < cen.cap; i++) {
        FM_CensusEnt *e = &cen.ents[i];
        if (!e->span || !e->acnt)
            continue;
        free_blocks += e->acnt;
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
    /* Locality-biased rebuild: place mixed residue deepest and the
     * largest span-pure batches on top so refills consume long runs
     * from one span before touching other spans. */
    if (res_head) {
        *(void **)res_tail = NULL;
        fm_publish2(res_head, res_tail, cls, res_cnt, 0);
    }
    if (order) {
        if (norder > 1)
            qsort(order, norder, sizeof(FM_Batch *), fm_batch_count_desc);
        for (size_t i = norder; i-- > 0;)
            fm_central_push(cls, order[i]);
        if (order_mapped)
            munmap(order, cen.used * sizeof(FM_Batch *));
    }
    /* Rewalking the entire retained pool every 128 flushes grows costly
     * during bulk free. Scale only the early regroup prompt with the supply
     * we actually rebuilt. The timed release and explicit trim still run. */
    size_t flushes = free_blocks / fm_local_limit(cls);
    uint32_t stride = FM_PROMPT_MIN_FLUSHES;
    while (stride <= flushes / 2 && stride < (1u << 30))
        stride *= 2;
    atomic_store_explicit(&fm_central[cls].prompt_mask, stride - 1, memory_order_relaxed);
    fm_census_reset(&cen);
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
static void fm_release_maybe(uint32_t cls, uint32_t now, uint32_t flushed_cls)
{
    const FM_Config *cfg = FM_CFG();
    FM_Central *c = &fm_central[cls];
    if (atomic_load_explicit(&c->depth, memory_order_relaxed) <
        cfg->release_min_depth)
        return;
    uint32_t interval = atomic_load_explicit(&c->interval_ms,
                                              memory_order_relaxed);
    uint32_t last = atomic_load_explicit(&c->last_census_ms, memory_order_relaxed);
    int time_ok = (!last || (uint32_t)(now - last) >=
                               (interval ? interval : cfg->release_interval_ms));
    if (!time_ok) {
        /* Regroup promptly when uncharacterised batches pile up (the
         * historical trigger) or when the stack fragments into many
         * small span-pure batches: flush-time splitting (ADR 0024)
         * publishes one chain per span per flush, so a churn loop can
         * stack thousands of tiny same-set batches whose refill-time
         * consumption sprays across every span.  Rate-limit both to
         * bound cost in contention-heavy phases. */
        if (cls != flushed_cls)
            return;
        uint32_t mask = atomic_load_explicit(&c->prompt_mask, memory_order_relaxed);
        if (fm_flush_seq[cls] & (mask | (FM_PROMPT_MIN_FLUSHES - 1)))
            return;
        fm_stack_exclusive();
        FM_Batch *h = atomic_load_explicit(&c->head, memory_order_relaxed);
        uint32_t hd = atomic_load_explicit(&c->depth, memory_order_relaxed);
        uint32_t hc = h ? (h->count & FM_COUNT_MASK) : 0;
        int unchar = h && hc == 0;
        /* Fragmented stack: many small characterized batches piled up
         * (flush-time churn) -- refills would pop tiny batches and
         * spray.  A regrouped stack's head is its LARGEST chain, so a
         * small head batch is the fragmentation signature; big-head
         * stacks (deep regrouped or DOM-style churn) skip this. */
        int frag = h && hc != 0 && hc < 32 && hd >= 64;
        fm_stack_resume();
        if (!unchar && !frag)
            return;
    }
    atomic_store_explicit(&c->last_census_ms, now, memory_order_relaxed);
    /* Depth-adaptive: shallow stacks regroup cheaply; deep stacks run
     * the full release census (mark/purge/unmap) as before. */
    uint32_t depth = atomic_load_explicit(&c->depth, memory_order_relaxed);
    int mode = depth <= cfg->regroup_only_depth ? 2 : 0;
    size_t released = fm_trim_class(cls, cfg->release_keep, mode);
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
        fm_release_maybe(cls, now, flushed_cls);
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
    fm_stack_exclusive();
}

static void fm_fork_parent(void)
{
    fm_stack_resume();
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
            static _Atomic int failures = FM_TEST_IDLE_CREATE_FAILURES;
            int remaining = atomic_load_explicit(&failures, memory_order_relaxed);
            if (remaining != 0) {
                if (remaining > 0)
                    atomic_fetch_sub_explicit(&failures, 1, memory_order_relaxed);
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
    fm_stack_exclusive();
    if (fm_dense_extent.remaining) {
        size_t bytes = (size_t)fm_dense_extent.remaining * SPAN_SIZE;
        if (munmap(fm_dense_extent.cursor, bytes) == 0) {
            released += bytes;
            fm_dense_extent.remaining = 0;
            fm_dense_extent.cursor = NULL;
        }
    }
    fm_stack_resume();
    for (uint32_t cls = 0; cls < CLASS_COUNT; cls++)
        released += fm_trim_class(cls, 0, 1);
    pthread_mutex_lock(&fm_census_lock);
    fm_census_destroy(&fm_shared_census);
    pthread_mutex_unlock(&fm_census_lock);
    return released;
}

/* ------------------------------------------------------------------ */
/* class dispatch                                                      */
/* ------------------------------------------------------------------ */

static inline size_t fm_class_of(size_t size)
{
    if (__builtin_expect(size <= ARITH_MAX, 1))
        return (size_t)((size + 15) >> 4);
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

/* Adaptive per-bucket budget: a bucket that has actually served a cache hit
 * (fm_lcache_warm_mask) may retain twice the configured per-thread budget,
 * capped at 128 MiB; cold buckets keep the configured cap.  Doubling keeps
 * the footprint profile's 8 MiB small, shortlived's 0 off, and leaves the
 * throughput preset's 256 MiB untouched.  Called only on the large-free
 * slow path, after the bucket-range check. */
static inline uint64_t fm_lcache_budget(int b)
{
    uint64_t budget = FM_CFG()->lcache_max_bytes;
    if ((fm_lcache_warm_mask & (1ULL << b)) &&
        budget < FM_LCACHE_WARM_BUDGET_CAP) {
        budget <<= 1;
        if (budget > FM_LCACHE_WARM_BUDGET_CAP)
            budget = FM_LCACHE_WARM_BUDGET_CAP;
    }
    return budget;
}

static inline void fm_large_free(FM_Span *sp, void *p)
{
    fm_ltick++;
    if (sp->align == 0 && (uintptr_t)p == (uintptr_t)sp + HEADER_SIZE) {
        size_t ms = sp->map_size;
        int b = 63 - __builtin_clzll(fm_large_usable(sp));
        if (b < FM_LCACHE_BUCKETS &&
            fm_lcache_bytes + ms <= fm_lcache_budget(b) &&
            fm_ltick - fm_lbucket_seen[b] <=
                ((fm_lcache_warm_mask & (1ULL << b)) ? 64u : FM_LCACHE_HOT_WINDOW)) {
            /* Only purge cold mappings large enough that the application is
             * unlikely to reuse them soon (bucket b covers usable sizes in
             * [2^b, 2^(b+1)), so b >= 24 means >= 16 MiB).  Medium buffers
             * churned by jq/g++/Python stay resident when re-freed, while
             * sort's one-shot merge buffers (>= 16 MiB) are still purged. */
            if (FM_CFG()->lcache_cold_purge &&
                b >= 24 &&
                !(fm_lcache_warm_mask & (1ULL << b))) {
                uintptr_t pstart = (uintptr_t)sp + FM_PAGE_SIZE;
                uintptr_t pend = (sp->map_base + sp->map_size) & ~(uintptr_t)(FM_PAGE_SIZE - 1);
                if (pend > pstart)
                    (void)madvise((void *)pstart, pend - pstart, MADV_DONTNEED);
            }
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
    if (fm_lextent_rem) {
        if (munmap(fm_lextent_cur, fm_lextent_rem) == 0)
            released += fm_lextent_rem;
        fm_lextent_cur = NULL;
        fm_lextent_rem = 0;
    }
    return released;
}

/* Allocate a large block.  *zeroed (when non-NULL) reports whether the
 * memory is known to be zero-filled: fresh anonymous mappings are,
 * cache hits are not.  calloc depends on this distinction. */
__attribute__((noinline, cold)) static void *fm_large_alloc_map(size_t size, int *zeroed);

__attribute__((noinline)) static void *fm_large_alloc_search(size_t size, int *zeroed,
                                                             int wide)
{
    if (__builtin_expect(size > PTRDIFF_MAX, 0)) {
        errno = ENOMEM;
        return NULL;
    }
    fm_arm_thread_exit();
    size_t need_map = FM_ALIGN_UP(size + HEADER_SIZE, FM_PAGE_SIZE);
    int nb = 63 - __builtin_clzll(need_map);
    if (nb < FM_LCACHE_BUCKETS)
        fm_lbucket_seen[nb] = fm_ltick;
    if (nb + 1 < FM_LCACHE_BUCKETS)
        fm_lbucket_seen[nb + 1] = fm_ltick;
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
                if (b < 64)
                    fm_lcache_warm_mask |= (1ULL << b);
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
    /* Tail call: keeps the miss path's locals and calls out of the
     * cache-hit path's prologue/epilogue. */
    return fm_large_alloc_map(size, zeroed);
}

static void *fm_large_alloc_map(size_t size, int *zeroed)
{
    size_t growing_from = fm_grow_from;
    /* Before mapping fresh pages, carve from a larger cached block (up to
     * 4 MiB) so medium-large bursts and sub-4 MiB growth reuse already-
     * resident mappings left by earlier phases. */
    if (size <= (2u << 20) && !(growing_from && FM_CFG()->realloc_cache_search)) {
        size_t need_map = FM_ALIGN_UP(size + HEADER_SIZE, FM_PAGE_SIZE);
        int nb = 63 - __builtin_clzll(need_map);
        size_t want = size;
        /* Any growth -- including a 1.5x/2x geometric step -- gets one
         * extra request of bounded headroom, so the carved block can serve
         * the next step without a copy. */
        if (growing_from)
            want += size;
        size_t carve = FM_ALIGN_UP(want + HEADER_SIZE, SPAN_SIZE);
        for (int b = nb + 2; b <= 22 && b < FM_LCACHE_BUCKETS; b++) {
            FM_Span **link = (FM_Span **)&fm_lcache_head[b];
            for (int i = 0; i < 8 && *link; i++) {
                FM_Span *sp = *link;
                size_t usable = fm_large_usable(sp);
                if (usable <= (4u << 20) && usable >= need_map) {
                    *link = (FM_Span *)sp->lnext;
                    fm_lcache_bytes -= sp->map_size;
                    if (usable >= carve + SPAN_SIZE + FM_PAGE_SIZE) {
                        uintptr_t map_end = sp->map_base + sp->map_size;
                        uintptr_t rem_addr = (uintptr_t)sp + carve;
                        FM_Span *rem = (FM_Span *)rem_addr;
                        rem->cls = LARGE_CLASS;
                        rem->magic = FM_SPAN_MAGIC;
                        rem->nblocks = 1;
                        rem->map_base = rem_addr;
                        rem->map_size = map_end - rem_addr;
                        rem->user_size = 0;
                        rem->align = 0;
                        int rb = 63 - __builtin_clzll(rem->map_size);
                        rem->lnext = (uintptr_t)fm_lcache_head[rb];
                        fm_lcache_head[rb] = rem;
                        fm_lcache_bytes += rem->map_size;
                        fm_lbucket_seen[rb] = fm_ltick;
                        sp->map_size = rem_addr - sp->map_base;
                    }
                    sp->user_size = size;
                    sp->lnext = 0;
                    if (zeroed)
                        *zeroed = 0;
                    return (void *)((char *)sp + HEADER_SIZE);
                }
                link = (FM_Span **)&sp->lnext;
            }
        }
    }
    uintptr_t raw = 0;
    size_t raw_len = 0;
    /* A growing buffer would otherwise copy and fault a new mapping at
     * every step. ADR 0075 only reserved headroom for sub-doubling growth,
     * so geometric (1.5x/2x) growth -- what parsers and builders use --
     * copied on every step. Reserve one extra request (bounded at 8 MiB)
     * for ANY growth so the next growth fits in place. Untouched reserve
     * pages cost VA, not RSS; if the larger mapping cannot be made we
     * retry the exact request below. Cache lookup and user_size still use
     * the actual request, so malloc_usable_size stays honest. */
    size_t capacity = size;
    const size_t huge_page = 2u << 20;
    if (growing_from) {
        size_t extra = size < (8u << 20) ? size : (8u << 20);
        capacity += extra;
        if (capacity >= huge_page / 2)
            capacity = FM_ALIGN_UP(capacity + HEADER_SIZE + SPAN_SIZE, huge_page)
                     - HEADER_SIZE - SPAN_SIZE;
    }
    char *base = NULL;
    if (capacity + HEADER_SIZE <= 8 * SPAN_SIZE)
        base = fm_map_large_slab(capacity, &raw, &raw_len);
    if (!base)
        base = fm_map_large(capacity, &raw, &raw_len);
    if (!base && capacity != size)
        base = fm_map_large(size, &raw, &raw_len);
    if (!base)
        return NULL;
#ifdef MADV_HUGEPAGE
    /* The copy will touch the old payload densely. Limit the hint to
     * the requested portion so sparse reserve pages stay cheap. */
    if (capacity != size && size >= huge_page / 2)
        (void)madvise(base, FM_ALIGN_UP(size + HEADER_SIZE, FM_PAGE_SIZE), MADV_HUGEPAGE);
#endif
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
    if (align <= 64 && (align & (align - 1)) == 0 && size <= SMALL_MAX) {
        if (align <= 16)
            return fm_malloc(size);
        /* Every small span is 64 KiB-aligned and HEADER_SIZE is 64:
         * rounding up to 32 or 64 selects a class whose block size is a
         * multiple of `align`, so base + 64 + idx * bs is naturally aligned. */
        size_t req = FM_ALIGN_UP(size ? size : 1, align);
        return fm_malloc(req);
    }
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
    for (uint32_t c = 0; c < CLASS_COUNT; c++) {
        if (fm_tls.head[c])
            fm_flush_bin(c);
        char *cur = fm_bump_ptr[c];
        char *end = fm_bump_end[c];
        if (cur < end) {
            size_t bs = kClassSize[c];
            uint32_t rcount = (uint32_t)((end - cur) / bs);
            fm_bump_ptr[c] = end;
            if (rcount > 0) {
                void *rhead = cur;
                void *rt = cur;
                for (uint32_t i = 1; i < rcount; i++) {
                    void *next = (char *)rt + bs;
                    *(void **)rt = next;
                    rt = next;
                }
                *(void **)rt = NULL;
#ifdef FM_DEBUG
                for (uint32_t i = 0; i < rcount; i++)
                    ((uint64_t *)(cur + (size_t)i * bs))[1] = 0xDEADDEADDEADDEADull;
#endif
                fm_publish2(rhead, rt, c, rcount, 1);
            }
        }
    }
    fm_large_cache_drain();
    if (fm_batch_pool) {
        FM_Batch *tail = fm_batch_pool;
        while (tail->next)
            tail = tail->next;
        pthread_mutex_lock(&fm_central_lock);
        tail->next = fm_batch_global_head;
        fm_batch_global_head = fm_batch_pool;
        pthread_mutex_unlock(&fm_central_lock);
        fm_batch_pool = NULL;
    }
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
    size_t cls = fm_class_of(size);
    if (__builtin_expect(cls != LARGE_CLASS, 1)) {
        void *p = fm_tls.head[cls];
        if (__builtin_expect(p != NULL, 1)) {
#ifdef FM_DEBUG
            {
                FM_Span *sp2 = (FM_Span *)((uintptr_t)p & SPAN_MASK);
                uintptr_t off = (uintptr_t)p - ((uintptr_t)sp2 + HEADER_SIZE);
                if (sp2->cls != cls || off % kClassSize[cls] != 0) {
                    fprintf(stderr,
                            "[fm] BAD HEAD cls=%zu p=%p span_cls=%u off=%lu "
                            "bs=%u\n",
                            cls, p, sp2->cls, (unsigned long)off,
                            kClassSize[cls]);
                    abort();
                }
            }
            if (((uint64_t *)p)[1] != 0xDEADDEADDEADDEADull) {
                FM_Span *s3 = (FM_Span *)((uintptr_t)p & SPAN_MASK);
                fprintf(stderr,
                        "[fm] CORRUPT HEAD size=%zu cls=%zu bs=%u span_cls=%u "
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
                            "[fm] BAD BLOCK size=%zu cls=%zu bs=%u p=%p "
                            "span_cls=%u off=%lu nblocks=%u\n",
                            size, cls, kClassSize[cls], p, s3->cls,
                            (unsigned long)off, s3->nblocks);
                    abort();
                }
            }
#endif
            void *next = *(void **)p;
            __builtin_prefetch(next, 0, 1);
            fm_tls.head[cls] = next;
            return p;
        }
        return fm_refill_class((uint32_t)cls);
    }
    return fm_large_alloc(size);
}

__attribute__((noinline, cold)) static void fm_free_slow_large(FM_Span *sp, void *p)
{
    fm_large_free(sp, p);
}

__attribute__((noinline, cold)) static void fm_free_aligned(const void *p)
{
    const FM_Span *sp = (const FM_Span *)((const char *)p - HEADER_SIZE);
    munmap((void *)sp->map_base, sp->map_size);
}

__attribute__((noinline)) static void fm_free_cnt_slow(uint32_t cls)
{
    if (__builtin_expect(fm_tls.cnt[cls] < 0, 0)) {
        /* First free on this thread/class (.tbss 0 decremented to -1). */
        fm_arm_thread_exit();
        uint32_t limit = fm_local_limit(cls);
        if (limit > 1) {
            fm_tls.cnt[cls] = (int32_t)(limit - 1);
            return;
        }
    }
    fm_flush_bin(cls);
}

static inline void fm_tls_push_free(void *p, size_t cls)
{
#if defined(__x86_64__) && !defined(FM_DEBUG) && \
    !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !defined(malloc)
    intptr_t tp_off;
    void *old_h;
    _Bool le;
    __asm__(
        "movq fm_tls@GOTTPOFF(%%rip), %[off]\n\t"
        "movq %%fs:(%[off],%[cls],8), %[old]\n\t"
        "movq %[old], (%[p])\n\t"
        "movq %[p], %%fs:(%[off],%[cls],8)"
        : [off] "=&r"(tp_off), [old] "=&r"(old_h)
        : [cls] "r"(cls), [p] "r"(p)
        : "memory");
    if (__builtin_expect(!old_h, 0)) {
        __asm__("movq %[p], %%fs:%c[toff](%[off],%[cls],8)"
                :
                : [off] "r"(tp_off), [cls] "r"(cls), [p] "r"(p),
                  [toff] "i"(offsetof(FM_HotTls, tail))
                : "memory");
    }
    __asm__("subl $1, %%fs:%c[coff](%[off],%[cls],4)"
            : "=@ccle"(le)
            : [off] "r"(tp_off), [cls] "r"(cls),
              [coff] "i"(offsetof(FM_HotTls, cnt))
            : "memory", "cc");
    if (__builtin_expect(le, 0))
        fm_free_cnt_slow((uint32_t)cls);
#else
    void *old_h = fm_tls.head[cls];
    *(void **)p = old_h;
#ifdef FM_DEBUG
    ((uint64_t *)p)[1] = 0xDEADDEADDEADDEADull;
#endif
    fm_tls.head[cls] = p;
    if (__builtin_expect(!old_h, 0))
        fm_tls.tail[cls] = p;
    if (__builtin_expect(--fm_tls.cnt[cls] <= 0, 0))
        fm_free_cnt_slow((uint32_t)cls);
#endif
}

static inline void fm_free(void *p)
{
    /* NULL has low 16 bits == 0 too, so one branch handles both NULL and
     * 64 KiB-aligned over-aligned allocations off the hot path. */
    if (__builtin_expect(((uintptr_t)p & (SPAN_SIZE - 1)) == 0, 0)) {
        if (!p)
            return;
        if (fm_is_aligned_ptr(p)) {
            fm_free_aligned(p);
            return;
        }
    }
    FM_Span *sp;
#if defined(__x86_64__)
    {
        uintptr_t base = (uintptr_t)p;
        __asm__("andq $-65536, %0" : "+r"(base) : : "cc");
        sp = (FM_Span *)base;
    }
#else
    sp = (FM_Span *)((uintptr_t)p & SPAN_MASK);
#endif
    uint32_t cls32 = sp->cls;
    if (__builtin_expect(cls32 == LARGE_CLASS, 0)) {
        fm_free_slow_large(sp, p);
        return;
    }
    size_t cls = cls32;
#ifdef FM_DEBUG
    {
        /* The block must lie inside its span, at a block boundary of its
         * class.  If this ever fails the class mapping is inconsistent. */
        uintptr_t base = (uintptr_t)sp;
        uintptr_t off = (uintptr_t)p - (base + HEADER_SIZE);
        if (off % kClassSize[cls] != 0 || off / kClassSize[cls] >= sp->nblocks) {
            fprintf(stderr,
                    "[fm] BAD BLOCK p=%p base=%p off=%lu cls=%zu bs=%u "
                    "nblocks=%u\n",
                    p, (void *)base, (unsigned long)off, cls,
                    kClassSize[cls], sp->nblocks);
            abort();
        }
    }
#endif
    fm_tls_push_free(p, cls);
}

__attribute__((noinline, cold)) static void *fm_calloc_overflow(void)
{
    errno = ENOMEM;
    return NULL;
}

__attribute__((noinline)) static void *fm_calloc_slow(size_t total)
{
    if (total > SMALL_MAX) {
        int zeroed = 1;
        void *p = fm_large_alloc2(total, &zeroed);
        if (p && !zeroed)
            memset(p, 0, total); /* cache hit: contents are stale */
        return p;
    }
    void *p = fm_malloc(total);
    if (__builtin_expect(!p, 0))
        return NULL;
    return memset(p, 0, total);
}

static inline void *fm_calloc(size_t n, size_t sz)
{
    size_t total;
    if (__builtin_expect(__builtin_mul_overflow(n, sz, &total), 0))
        return fm_calloc_overflow();
#ifndef FM_DEBUG
    if (__builtin_expect(total <= 64, 1)) {
        size_t cls = (total + 15) >> 4;
        void *p = fm_tls.head[cls];
        if (__builtin_expect(p != NULL, 1)) {
            void *next = *(void **)p;
            __builtin_prefetch(next, 0, 1);
            fm_tls.head[cls] = next;
            if (total <= 16) {
                __builtin_memset(p, 0, 16);
            } else if (total <= 32) {
                __builtin_memset(p, 0, 32);
            } else {
                __builtin_memset(p, 0, 32);
                __builtin_memset((char *)p + total - 32, 0, 32);
            }
            return p;
        }
    }
#endif
    return fm_calloc_slow(total);
}

/* Programs that link their own allocator can pass their pointers to the
 * libc malloc_usable_size symbol; LD_PRELOAD routes that symbol here.  Redis
 * 8 links jemalloc and does exactly this, so a foreign pointer used to crash
 * on the v2 span header.  Recognise a v2 span by its magic and delegate
 * everything else to the next definition (jemalloc's or glibc's). */
static size_t (*fm_next_usable_size)(void *);

__attribute__((noinline, cold)) static size_t fm_foreign_usable_size(void *p)
{
    static _Thread_local int in_foreign;
    if (in_foreign)
        return 0;
    if (!fm_next_usable_size) {
        void *sym = dlsym(RTLD_NEXT, "malloc_usable_size");
        Dl_info self, target;
        if (sym &&
            dladdr((void *)&fm_foreign_usable_size, &self) &&
            dladdr(sym, &target) &&
            self.dli_fbase == target.dli_fbase)
            sym = NULL; /* RTLD_NEXT did not skip us; do not recurse. */
        fm_next_usable_size = (size_t (*)(void *))sym;
    }
    if (!fm_next_usable_size)
        return 0;
    in_foreign = 1;
    size_t n = fm_next_usable_size(p);
    in_foreign = 0;
    return n;
}

static inline size_t fm_usable_size(void *p)
{
    uintptr_t u = (uintptr_t)p;
    if (__builtin_expect(u == 0, 0))
        return 0;
    if (__builtin_expect(u < FM_PAGE_SIZE, 0))
        return 0; /* neither a v2 span nor a valid foreign heap pointer */
    if (__builtin_expect((u & (SPAN_SIZE - 1)) == 0, 0)) {
        if (fm_is_aligned_ptr(p)) {
            const FM_Span *sp = (const FM_Span *)((const char *)p - HEADER_SIZE);
            return sp->user_size;
        }
    }
    const FM_Span *sp = (const FM_Span *)(u & SPAN_MASK);
    if (__builtin_expect(sp->magic == FM_SPAN_MAGIC, 1)) {
        if (sp->cls == LARGE_CLASS)
            return sp->user_size;
        if (sp->cls < CLASS_COUNT)
            return kClassSize[sp->cls];
    }
    return fm_foreign_usable_size(p);
}

__attribute__((noinline)) static void *fm_realloc_slow(void *p, size_t size)
{
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
        /* Incremental growth across the densely spaced medium classes
         * otherwise strands the previous buffer in each class in turn. */
        size_t capacity = size;
        if (old >= 512 && size <= SMALL_MAX && size - old < old)
            capacity = (size_t)1 << (32 - __builtin_clz((unsigned)size - 1));
        void *q = fm_malloc(capacity);
        if (!q && capacity != size)
            q = fm_malloc(size);
        if (!q)
            return NULL;
        memcpy(q, p, old);
        fm_free(p);
        return q;
    }
    if (sp->align == 0) {
        size_t off = (size_t)((char *)p - (char *)sp->map_base);
        if (size <= sp->map_size - off) {
            sp->user_size = size;
            return p;
        }
    } else if (size <= sp->user_size) {
        return p;
    }
    void *q;
    if (sp->align) {
        q = fm_aligned_alloc_impl(sp->align, size);
    } else {
        fm_grow_from = sp->user_size;
        q = fm_large_alloc_search(size, NULL, FM_CFG()->realloc_cache_search);
        fm_grow_from = 0;
    }
    if (!q)
        return NULL;
    memcpy(q, p, sp->user_size);
    fm_large_free(sp, p);
    return q;
}

static inline void *fm_realloc(void *p, size_t size)
{
    if (__builtin_expect(!p, 0))
        return fm_malloc(size);
    if (__builtin_expect(((uintptr_t)p & (SPAN_SIZE - 1)) != 0 && size != 0, 1)) {
        FM_Span *sp = (FM_Span *)((uintptr_t)p & SPAN_MASK);
        uint32_t cls = sp->cls;
        if (__builtin_expect(cls != LARGE_CLASS, 1)) {
            if (__builtin_expect(size <= kClassSize[cls], 1))
                return p;
        } else if (sp->align == 0) {
            size_t off = (size_t)((char *)p - (char *)sp->map_base);
            if (__builtin_expect(size <= sp->map_size - off, 1)) {
                sp->user_size = size;
                return p;
            }
        }
    }
    return fm_realloc_slow(p, size);
}

/* C++ operator new OOM slow path: resolves libstdc++/libc++ new_handler and
 * __cxa_throw_bad_alloc lazily via dlsym so libfastmalloc2.so stays pure C
 * with no libstdc++ dependency while preserving C++ exception semantics. */
__attribute__((noinline, cold)) static void *fm_new_oom(size_t size, size_t align, int nothrow)
{
    typedef void (*new_handler_t)(void);
    typedef new_handler_t (*get_nh_t)(void);
    typedef void (*throw_ba_t)(void) __attribute__((noreturn));
    static get_nh_t get_nh;
    static throw_ba_t throw_ba;
    static _Atomic int resolved;
    if (!atomic_load_explicit(&resolved, memory_order_acquire)) {
        get_nh = (get_nh_t)(uintptr_t)dlsym(RTLD_DEFAULT, "_ZSt15get_new_handlerv");
        throw_ba = (throw_ba_t)(uintptr_t)dlsym(RTLD_DEFAULT, "_ZSt17__throw_bad_allocv");
        if (!throw_ba)
            throw_ba = (throw_ba_t)(uintptr_t)dlsym(RTLD_DEFAULT, "__cxa_throw_bad_alloc");
        atomic_store_explicit(&resolved, 1, memory_order_release);
    }
    for (;;) {
        new_handler_t nh = get_nh ? get_nh() : NULL;
        if (!nh) {
            if (!nothrow) {
                if (throw_ba)
                    throw_ba();
                abort();
            }
            return NULL;
        }
        nh();
        void *p = align ? fm_aligned_alloc_impl(align, size) : fm_malloc(size);
        if (p)
            return p;
    }
}

__attribute__((noinline)) static void *fm_new_slow(size_t size, int nothrow)
{
    void *p = fm_malloc(size);
    if (__builtin_expect(p != NULL, 1))
        return p;
    return fm_new_oom(size, 0, nothrow);
}

__attribute__((noinline)) static void *fm_new_aligned_slow(size_t size, size_t align, int nothrow)
{
    if (align == 0 || (align & (align - 1)) != 0) {
        if (!nothrow)
            return fm_new_oom(size, align, 0);
        return NULL;
    }
    void *p = fm_aligned_alloc_impl(align, size);
    if (__builtin_expect(p != NULL, 1))
        return p;
    return fm_new_oom(size, align, nothrow);
}

static inline void *fm_operator_new(size_t size)
{
#ifndef FM_DEBUG
    if (__builtin_expect(size <= ARITH_MAX, 1)) {
        size_t cls = (size + 15) >> 4;
        void *p = fm_tls.head[cls];
        if (__builtin_expect(p != NULL, 1)) {
            void *next = *(void **)p;
            __builtin_prefetch(next, 0, 1);
            fm_tls.head[cls] = next;
            return p;
        }
    }
#endif
    return fm_new_slow(size, 0);
}

static inline void *fm_operator_new_nothrow(size_t size)
{
#ifndef FM_DEBUG
    if (__builtin_expect(size <= ARITH_MAX, 1)) {
        size_t cls = (size + 15) >> 4;
        void *p = fm_tls.head[cls];
        if (__builtin_expect(p != NULL, 1)) {
            void *next = *(void **)p;
            __builtin_prefetch(next, 0, 1);
            fm_tls.head[cls] = next;
            return p;
        }
    }
#endif
    return fm_new_slow(size, 1);
}

static inline void *fm_operator_new_aligned(size_t size, size_t align)
{
#ifndef FM_DEBUG
    if (__builtin_expect(align >= 16 && align <= 64 && (align & (align - 1)) == 0 &&
                         size <= ARITH_MAX, 1)) {
        size_t mask = align - 1;
        size_t cls = ((size ? size : 1) + mask) & ~mask;
        cls >>= 4;
        void *p = fm_tls.head[cls];
        if (__builtin_expect(p != NULL, 1)) {
            void *next = *(void **)p;
            __builtin_prefetch(next, 0, 1);
            fm_tls.head[cls] = next;
            return p;
        }
    }
#endif
    return fm_new_aligned_slow(size, align, 0);
}

static inline void fm_delete_sized_aligned(void *p, size_t size, size_t align)
{
    if (__builtin_expect(!p, 0))
        return;
#ifndef FM_DEBUG
    if (__builtin_expect(align >= 16 && align <= 64 && (align & (align - 1)) == 0 &&
                         size <= ARITH_MAX, 1)) {
        size_t mask = align - 1;
        size_t cls = (((size ? size : 1) + mask) & ~mask) >> 4;
        fm_tls_push_free(p, cls);
        return;
    }
#endif
    fm_free(p);
}

static inline void fm_delete_sized(void *p, size_t size)
{
    if (__builtin_expect(!p, 0))
        return;
#ifndef FM_DEBUG
    if (__builtin_expect(size <= ARITH_MAX, 1)) {
        size_t cls = (size + 15) >> 4;
        fm_tls_push_free(p, cls);
        return;
    }
#endif
    fm_free(p);
}

/* ------------------------------------------------------------------ */
/* load-time TLS displacement patching (experimental, fail-open)        */
/* ------------------------------------------------------------------ */

#ifndef FM_NO_TLS_PATCH

#include <elf.h>
#include <link.h>
#include <stdio.h>

/*
 * malloc and free reach the thread-local free lists through the only
 * TLS addressing form a shared library has.  The displacement of the
 * module's static TLS block is assigned by the dynamic linker at load
 * time, so the fast path loads it from a GOT entry that ld.so filled
 * (R_X86_64_TPOFF64) before any code ran:
 *
 *     mov  disp32(%rip),%rdx        ; the GOT entry (the tpoff)
 *     mov  %fs:(%rdx,%rcx,8),%rax   ; head of the free list
 *
 * By constructor time the displacement is a constant of this process,
 * and this module's text is ours.  So: parse our own dynamic section
 * for the TPOFF64 slots, scan our own text for the exact 7-byte load
 * form, and rewrite every load whose GOT target is one of those slots
 * into the same-length immediate form:
 *
 *     mov  $imm32,%rdx              ; sign-extended tpoff, no load
 *
 * The immediate is read from the already-relocated GOT slot, so the
 * patch cannot diverge from what the load would have produced.  A
 * candidate is rewritten only if
 *   - its computed GOT target is exactly a TPOFF64 slot of this
 *     module, and
 *   - the slot's value is a plausible tpoff (negative, |v| < 2^31),
 * and the text bytes are verified against the template before and
 * after writing.  Anything that fails verification is left untouched:
 * fail open to the correct, slower form.  FM_TLS_PATCH=0 disables.
 *
 * The patch happens in a constructor, before our threads exist (other
 * preloaded libraries' constructors are the only theoretical race, and
 * a 7-byte same-length overwrite is the smallest such window x86
 * offers).  v1 keeps its table-driven paths and is never patched.
 */

typedef struct FM_TpPatch {
    unsigned char *insn;  /* runtime address of the 7-byte load */
    unsigned char rex_out;/* 0x48 for %rax..%rdi, 0x49 for %r8..%r15 */
    unsigned char reg;    /* low 3 bits of destination register (0..7) */
    int32_t tpoff;
} FM_TpPatch;

static FM_TpPatch fm_tp_patches[64];
static size_t fm_tp_npatches;

static int fm_tp_modrm_ok(unsigned char rex, unsigned char m)
{
    /* mov r64, [rip+disp32]: mod=00, rm=101; for REX.W (0x48), reg != 4 (%rsp);
     * for REX.WR (0x4C, %r8..%r15), all 8 registers including %r12 (4) are GPRs. */
    if ((m & 0xC7) != 0x05)
        return 0;
    return rex == 0x4C || ((m >> 3) & 7) != 4;
}

static void fm_tp_apply(void)
{
    if (!fm_tp_npatches)
        return;
    uintptr_t min_start = (uintptr_t)-1;
    uintptr_t max_end = 0;
    for (size_t i = 0; i < fm_tp_npatches; i++) {
        uintptr_t s = (uintptr_t)fm_tp_patches[i].insn & ~(uintptr_t)0xFFF;
        uintptr_t e = (((uintptr_t)fm_tp_patches[i].insn + 6) & ~(uintptr_t)0xFFF) + 0x1000;
        if (s < min_start)
            min_start = s;
        if (e > max_end)
            max_end = e;
    }
    size_t total_len = max_end - min_start;
    if (mprotect((void *)min_start, total_len, PROT_READ | PROT_WRITE) != 0)
        return; /* fail open: stay as GOT loads */
    for (size_t i = 0; i < fm_tp_npatches; i++) {
        FM_TpPatch *pt = &fm_tp_patches[i];
        unsigned char want[7];
        want[0] = pt->rex_out;
        want[1] = 0xC7;
        want[2] = (unsigned char)(0xC0 | pt->reg);
        want[3] = (unsigned char)(pt->tpoff & 0xFF);
        want[4] = (unsigned char)((pt->tpoff >> 8) & 0xFF);
        want[5] = (unsigned char)((pt->tpoff >> 16) & 0xFF);
        want[6] = (unsigned char)((pt->tpoff >> 24) & 0xFF);
        memcpy(pt->insn, want, 7);
        if (memcmp(pt->insn, want, 7) != 0)
            memcpy(pt->insn, want, 7);
    }
    mprotect((void *)min_start, total_len, PROT_READ | PROT_EXEC);
}

static size_t fm_tp_get_hot_fns(const void **fns)
{
    size_t n = 0;
    extern void free(void *);
    extern void *malloc(size_t);
    extern void *calloc(size_t, size_t);
    extern void *realloc(void *, size_t);
    extern void *reallocarray(void *, size_t, size_t);
    extern void *fm_generic_malloc(size_t);
    fns[n++] = (const void *)(uintptr_t)malloc;
    fns[n++] = (const void *)(uintptr_t)free;
    fns[n++] = (const void *)(uintptr_t)calloc;
    fns[n++] = (const void *)(uintptr_t)realloc;
    fns[n++] = (const void *)(uintptr_t)reallocarray;
    fns[n++] = (const void *)(uintptr_t)fm_generic_malloc;
#ifndef malloc
    extern void free_sized(void *, size_t);
    extern void free_aligned_sized(void *, size_t, size_t);
    extern void *_Znwm(size_t);
    extern void *_Znam(size_t);
    extern void *_ZnwmRKSt9nothrow_t(size_t, const void *);
    extern void *_ZnamRKSt9nothrow_t(size_t, const void *);
    extern void _ZdlPv(void *);
    extern void _ZdaPv(void *);
    extern void _ZdlPvm(void *, size_t);
    extern void _ZdaPvm(void *, size_t);
    extern void _ZdlPvRKSt9nothrow_t(void *, const void *);
    extern void _ZdaPvRKSt9nothrow_t(void *, const void *);
    extern void *_ZnwmSt11align_val_t(size_t, size_t);
    extern void *_ZnamSt11align_val_t(size_t, size_t);
    extern void _ZdlPvSt11align_val_t(void *, size_t);
    extern void _ZdaPvSt11align_val_t(void *, size_t);
    extern void _ZdlPvmSt11align_val_t(void *, size_t, size_t);
    extern void _ZdaPvmSt11align_val_t(void *, size_t, size_t);
    extern void _ZdlPvSt11align_val_tRKSt9nothrow_t(void *, size_t, const void *);
    extern void _ZdaPvSt11align_val_tRKSt9nothrow_t(void *, size_t, const void *);
    fns[n++] = (const void *)(uintptr_t)free_sized;
    fns[n++] = (const void *)(uintptr_t)free_aligned_sized;
    fns[n++] = (const void *)(uintptr_t)_Znwm;
    fns[n++] = (const void *)(uintptr_t)_Znam;
    fns[n++] = (const void *)(uintptr_t)_ZnwmRKSt9nothrow_t;
    fns[n++] = (const void *)(uintptr_t)_ZnamRKSt9nothrow_t;
    fns[n++] = (const void *)(uintptr_t)_ZdlPv;
    fns[n++] = (const void *)(uintptr_t)_ZdaPv;
    fns[n++] = (const void *)(uintptr_t)_ZdlPvm;
    fns[n++] = (const void *)(uintptr_t)_ZdaPvm;
    fns[n++] = (const void *)(uintptr_t)_ZdlPvRKSt9nothrow_t;
    fns[n++] = (const void *)(uintptr_t)_ZdaPvRKSt9nothrow_t;
    fns[n++] = (const void *)(uintptr_t)_ZnwmSt11align_val_t;
    fns[n++] = (const void *)(uintptr_t)_ZnamSt11align_val_t;
    fns[n++] = (const void *)(uintptr_t)_ZdlPvSt11align_val_t;
    fns[n++] = (const void *)(uintptr_t)_ZdaPvSt11align_val_t;
    fns[n++] = (const void *)(uintptr_t)_ZdlPvmSt11align_val_t;
    fns[n++] = (const void *)(uintptr_t)_ZdaPvmSt11align_val_t;
    fns[n++] = (const void *)(uintptr_t)_ZdlPvSt11align_val_tRKSt9nothrow_t;
    fns[n++] = (const void *)(uintptr_t)_ZdaPvSt11align_val_tRKSt9nothrow_t;
#endif
    return n;
}

static int fm_tp_scan_module(uintptr_t bias, const ElfW(Phdr) * phdr, int phnum)
{
    const ElfW(Dyn) *dyn = NULL;
    const unsigned char *text = NULL;
    size_t textsz = 0;

    for (int i = 0; i < phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn) *)(bias + phdr[i].p_vaddr);
        else if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_X)) {
            text = (const unsigned char *)(bias + phdr[i].p_vaddr);
            textsz = phdr[i].p_memsz;
        }
    }
    if (!dyn || !text || !textsz)
        return 0;

    uintptr_t rela = 0;
    size_t relasz = 0;
    uintptr_t lo = (uintptr_t)-1, hi = 0;
    for (int i = 0; i < phnum; i++)
        if (phdr[i].p_type == PT_LOAD) {
            uintptr_t s = bias + phdr[i].p_vaddr;
            if (s < lo)
                lo = s;
            if (s + phdr[i].p_memsz > hi)
                hi = s + phdr[i].p_memsz;
        }
    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA) {
            uintptr_t v = (uintptr_t)d->d_un.d_ptr;
            /* Some loaders bias d_ptr entries in place, some do not.
             * If the value is already a runtime address it sits inside
             * the mapping; otherwise it is a link-time vaddr. */
            if (v < lo || v >= hi)
                v += bias;
            rela = v;
        } else if (d->d_tag == DT_RELASZ) {
            relasz = (size_t)d->d_un.d_val;
        }
    }
    if (!rela || !relasz || relasz % sizeof(ElfW(Rela)) != 0)
        return 0;

    /* Collect this module's TPOFF64 slots (its __thread variables). */
    uintptr_t slots[32];
    size_t nslots = 0;
    const ElfW(Rela) *r = (const ElfW(Rela) *)rela;
    for (size_t i = 0; i < relasz / sizeof(ElfW(Rela)); i++) {
        if (ELF64_R_TYPE(r[i].r_info) != R_X86_64_TPOFF64)
            continue;
        if (nslots == sizeof(slots) / sizeof(slots[0]))
            break;
        slots[nslots++] = bias + (uintptr_t)r[i].r_offset;
    }
    if (!nslots)
        return 0;
#ifdef FM_TP_DEBUG
    fprintf(stderr, "[tp] module: %zu tpoff slots, text=%p sz=%zu\n", nslots,
            (const void *)text, textsz);
    for (size_t s = 0; s < nslots; s++)
        fprintf(stderr, "[tp] slot %zu = %p\n", s, (void *)slots[s]);
#endif

    const char *limenv = getenv("FM_TP_LIMIT");
    size_t tplimit = sizeof(fm_tp_patches) / sizeof(fm_tp_patches[0]);
    if (limenv && *limenv)
        tplimit = (size_t)strtoull(limenv, NULL, 10);

    const void *fns[32];
    size_t nfns = fm_tp_get_hot_fns(fns);
    uintptr_t text_lo = (uintptr_t)text;
    uintptr_t text_hi = text_lo + textsz;

    for (size_t f = 0; f < nfns && fm_tp_npatches < tplimit; f++) {
        uintptr_t fa = (uintptr_t)fns[f];
        if (fa < text_lo || fa >= text_hi)
            continue;
        size_t i_start = (size_t)(fa - text_lo);
        size_t i_end = i_start + 256;
        if (i_end > textsz)
            i_end = textsz;
        for (size_t i = i_start; i + 7 <= i_end; i++) {
            unsigned char rex = text[i];
            if ((rex != 0x48 && rex != 0x4C) || text[i + 1] != 0x8B ||
                !fm_tp_modrm_ok(rex, text[i + 2]))
                continue;
            int32_t disp = (int32_t)((uint32_t)text[i + 3] |
                                     ((uint32_t)text[i + 4] << 8) |
                                     ((uint32_t)text[i + 5] << 16) |
                                     ((uint32_t)text[i + 6] << 24));
            uintptr_t target = (uintptr_t)(text + i) + 7 + (uintptr_t)disp;
            for (size_t s = 0; s < nslots; s++) {
                if (slots[s] != target)
                    continue;
                int64_t v = *(const volatile int64_t *)target;
                /* A tpoff is negative (the TLS block sits below the thread
                 * pointer) and small; anything else is not one. */
                if (v >= 0 || v < -(int64_t)(1u << 31))
                    break;
                int dup = 0;
                for (size_t k = 0; k < fm_tp_npatches; k++) {
                    if (fm_tp_patches[k].insn == text + i) {
                        dup = 1;
                        break;
                    }
                }
                if (dup || fm_tp_npatches >= tplimit)
                    break;
                FM_TpPatch *pt = &fm_tp_patches[fm_tp_npatches++];
                pt->insn = (unsigned char *)(text + i);
                pt->rex_out = (rex == 0x4C) ? 0x49 : 0x48;
                pt->reg = (unsigned char)((text[i + 2] >> 3) & 7);
                pt->tpoff = (int32_t)v;
                if (getenv("FM_TP_TRACE"))
                    fprintf(stderr, "[tp] pick #%zu i=%zu insn=%p rex=%02x reg=%u tpoff=%d tgt=%p\n",
                            fm_tp_npatches - 1, i, pt->insn, pt->rex_out, pt->reg, pt->tpoff,
                            (void *)target);
                break;
            }
            if (fm_tp_npatches >= tplimit)
                break;
        }
    }
    if (fm_tp_npatches)
        fm_tp_apply();
#ifdef FM_TP_DEBUG
    fprintf(stderr, "[tp] patched %zu loads\n", fm_tp_npatches);
    for (size_t i = 0; i < fm_tp_npatches; i++)
        fprintf(stderr, "[tp]   @%p reg=%u tpoff=%d\n", fm_tp_patches[i].insn,
                fm_tp_patches[i].reg, fm_tp_patches[i].tpoff);
#endif
    return fm_tp_npatches != 0;
}

static int fm_tp_find_module(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    uintptr_t anchor = (uintptr_t)data;
#ifdef FM_TP_DEBUG
    fprintf(stderr, "[tp] phdr %s addr=%p anchor=%p\n", info->dlpi_name,
            (void *)(uintptr_t)info->dlpi_addr, (void *)anchor);
#endif
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
            continue;
        uintptr_t s = info->dlpi_addr + p->p_vaddr;
        if (anchor >= s && anchor < s + p->p_memsz)
            return fm_tp_scan_module(info->dlpi_addr, info->dlpi_phdr,
                                     info->dlpi_phnum);
    }
    return 0;
}

__attribute__((constructor)) static void fm_tls_patch_ctor(void)
{
    const char *env = getenv("FM_TLS_PATCH");
    if (env && env[0] == '0' && env[1] == 0)
        return;
    dl_iterate_phdr(fm_tp_find_module, (void *)(uintptr_t)&fm_tls_patch_ctor);
}

#endif /* FM_NO_TLS_PATCH */

#endif /* FM_CORE_H */
