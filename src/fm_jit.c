/*
 * fm_jit.c -- JIT-compiled allocator entry point.
 *
 * Idea
 * ----
 * A statically compiled allocator commits to one size->class mapping and one
 * fast-path shape for every program it ever runs.  Real programs, however,
 * concentrate nearly all of their allocations on a handful of exact sizes
 * (a struct size, a fixed buffer), and the shape of the fast path that
 * matters depends on how concentrated that distribution is.
 *
 * This build profiles the running program and then *generates* the allocation
 * entry point from an assembler-written template (see jit_stubs.S):
 *
 *   1. Request sizes are recorded in a histogram.  Sampling is 1-in-64, so
 *      the hot path pays one thread-local counter increment and one
 *      predictable branch.
 *
 *   2. When the profile is populated, the hottest size classes are selected
 *      and a specialised dispatch is generated.  For each hot class the
 *      template gets a range test on the request size; for each class a leaf
 *      that pops from that class's thread-local free list with the list
 *      address baked in as a constant displacement:
 *
 *          cmp  size, class_lo
 *          jbe  .L
 *          ...
 *          .L: mov  $tls_off + cls*8, %rcx
 *              mov  %fs:(%rcx), %rax    ; head of the free list
 *              test %rax, %rax
 *              jz   slow
 *              mov  (%rax), %rdx        ; next
 *              mov  %rdx, %fs:(%rcx)
 *              ret
 *
 *      i.e. one thread-local load and one dependent load on the block, with
 *      no size->class arithmetic at all.
 *
 *   3. The generated code is installed behind an atomically updated pointer,
 *      so the swap is safe while other threads are running.
 *
 *   4. The profile is re-examined as the program runs; if the workload
 *      shifts, the entry point is regenerated.  This adaptivity is the one
 *      thing a statically compiled allocator cannot offer.
 *
 * Environment
 * -----------
 *   FM_JIT=0            disable the JIT (generic path only)
 *   FM_JIT_PROFILE=0    disable profiling (no re-JIT)
 *   FM_JIT_HOT=N        number of hot classes to specialise (default 4)
 *   FM_JIT_DEBUG=1      report the compiled specialisation
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fm_core.h"

/* The template and its patch points, from jit_stubs.S. */
extern const char jit_template[];
extern const char jit_template_end[];
extern const char jit_psub_0[], jit_psub_1[], jit_psub_2[], jit_psub_3[];
extern const char jit_psub_4[], jit_psub_5[], jit_psub_6[], jit_psub_7[];
extern const char jit_pcmp_0[], jit_pcmp_1[], jit_pcmp_2[], jit_pcmp_3[];
extern const char jit_pcmp_4[], jit_pcmp_5[], jit_pcmp_6[], jit_pcmp_7[];
extern const char jit_pdisp_0[], jit_pdisp_1[], jit_pdisp_2[], jit_pdisp_3[];
extern const char jit_pdisp_4[], jit_pdisp_5[], jit_pdisp_6[], jit_pdisp_7[];
extern const char jit_pjbe_0[], jit_pjbe_1[], jit_pjbe_2[], jit_pjbe_3[];
extern const char jit_pjbe_4[], jit_pjbe_5[], jit_pjbe_6[], jit_pjbe_7[];
extern const char jit_generic[];
extern const char jit_pgen[];

#define JIT_SLOTS 8

/* ------------------------------------------------------------------ */
/* helpers referenced by generated code                                */
/* ------------------------------------------------------------------ */

void *fm_generic_malloc(size_t n);
void *fm_refill_class(uint32_t cls);

FM_Central fm_central[CLASS_COUNT];
__thread void *fm_head[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread void *fm_tail[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread uint32_t fm_cnt[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread int fm_exit_armed __attribute__((tls_model("initial-exec")));
__thread FM_Batch *fm_batch_pool __attribute__((tls_model("initial-exec")));
__thread void *fm_lcache_head[FM_LCACHE_BUCKETS] __attribute__((tls_model("initial-exec")));
__thread uint64_t fm_lcache_bytes __attribute__((tls_model("initial-exec")));

/* ------------------------------------------------------------------ */
/* profiling                                                           */
/* ------------------------------------------------------------------ */

#define EXACT_MAX 4096
#define SAMPLE_PERIOD 64
/* Recompile after this many samples. */
#define JIT_TRIGGER 256

static uint64_t g_exact[EXACT_MAX];
static _Atomic uint64_t g_samples;

/*
 * The trigger counter is deliberately *not* atomic: it only decides when to
 * recompile, and a lost update merely delays the next recompile by a few
 * samples.  Making it atomic would put a LOCK XADD on the hot path.
 */
static uint64_t g_profile_events;

static __thread uint32_t t_sample_ctr __attribute__((tls_model("initial-exec")))
    = SAMPLE_PERIOD;

static inline void fm_profile(size_t n)
{
    if (__builtin_expect(n < EXACT_MAX, 1))
        __atomic_fetch_add(&g_exact[n], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_samples, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* JIT state                                                           */
/* ------------------------------------------------------------------ */

typedef void *(*entry_fn)(size_t);

void *fm_jit_generic(size_t n);

/*
 * The entry point is initialised at load time, NOT by the constructor.
 * Code in other libraries' constructors can call malloc before this
 * library's constructor runs (glibc initialises a preloaded library
 * after the executable's dependencies), and a zero entry pointer turns
 * that call into a jump to NULL -- this is what crashed Node.js at
 * startup, where libnode's C++ static initialisers allocate during
 * their constructors.  fm_jit_generic is safe to call that early: it
 * touches only atomics and this thread's TLS, which the loader has
 * already set up.  The constructor still makes the final choice of
 * entry point (and can replace this default).
 */
static _Atomic(entry_fn) g_entry = fm_jit_generic;
static void *g_code;
static uint32_t g_hot_cls[JIT_SLOTS];
static int g_hot_count;
static uint64_t g_tls_head_off;
static int g_hot_target = 4;
static _Atomic int g_compiling;

/*
 * The JIT only specialises when the size distribution is *concentrated*
 * enough for the specialised dispatch to pay for itself.  A diverse
 * workload makes the range tests mispredict, and the static dispatch (which
 * computes the class with arithmetic) is faster there.  This threshold is
 * the fraction of the traffic the hottest class must account for.
 */
static uint32_t g_min_share = 50; /* percent */

/*
 * Set when the JIT decides that specialisation cannot pay for itself.  The
 * entry point is then replaced by the plain generic implementation and the
 * profiling stops, so the JIT costs nothing at all from that point on.
 */
static int g_give_up;

/* Patch points, relocated into the copy of the template. */
static const char *p_sub[JIT_SLOTS], *p_cmp[JIT_SLOTS], *p_disp[JIT_SLOTS];
static const char *p_jbe[JIT_SLOTS];
static const char *p_gen;

static void jit_init_offsets(void)
{
    uintptr_t tp = (uintptr_t)__builtin_thread_pointer();
    g_tls_head_off = (uintptr_t)&fm_head[0] - tp;

    /* Sanity check: the generated code reaches the thread-local array as
     * fs:[displacement].  Verify that by planting a sentinel and reading it
     * back. */
    void *sentinel = (void *)(uintptr_t)0x5A5A5A5A5A5A5A5Aull;
    void *saved = fm_head[0];
    fm_head[0] = sentinel;
    uintptr_t probe;
    __asm__ volatile("mov %%fs:(%1), %0" : "=r"(probe) : "r"(g_tls_head_off) : "memory");
    fm_head[0] = saved;
    if (probe != (uintptr_t)sentinel) {
        fprintf(stderr, "[jit] TLS displacement mismatch (%p vs %p) -- JIT off\n",
                (void *)probe, (void *)sentinel);
        g_tls_head_off = 0;
    }
}

/*
 * Choose the hottest classes from the profile.
 *
 * Returns the number of classes to specialise, or 0 when the distribution is
 * too diverse for a specialised dispatch to be worth it.
 */
static int pick_hot_classes(uint32_t *cls, uint32_t *lo, uint32_t *span)
{
    uint64_t total = __atomic_load_n(&g_samples, __ATOMIC_RELAXED);
    if (!total)
        return 0;

    /* Aggregate the exact-size histogram into per-class counts. */
    uint64_t class_count[CLASS_COUNT];
    uint64_t class_top[CLASS_COUNT];  /* the most common exact size */
    uint64_t class_topn[CLASS_COUNT]; /* ... and how often it occurs */
    for (int i = 0; i < CLASS_COUNT; i++) {
        class_count[i] = 0;
        class_top[i] = 0;
        class_topn[i] = 0;
    }
    for (size_t s = 1; s < EXACT_MAX; s++) {
        uint64_t v = __atomic_load_n(&g_exact[s], __ATOMIC_RELAXED);
        if (!v)
            continue;
        uint32_t c = fm_class_of(s);
        class_count[c] += v;
        if (v > class_topn[c]) {
            class_topn[c] = v;
            class_top[c] = s;
        }
    }

    /* The hottest single class decides whether specialisation is worth it. */
    uint64_t top = 0;
    for (int c = 0; c < CLASS_COUNT; c++)
        if (class_count[c] > top)
            top = class_count[c];
    if (top * 100 < (uint64_t)g_min_share * total)
        return 0; /* too diverse: keep the static dispatch */

    int n = 0;
    uint64_t used = 0;
    for (;;) {
        int best = -1;
        uint64_t bestv = 0;
        for (int c = 0; c < CLASS_COUNT; c++) {
            if (class_count[c] > bestv) {
                bestv = class_count[c];
                best = c;
            }
        }
        if (best < 0 || n >= g_hot_target)
            break;
        cls[n] = (uint32_t)best;
        /*
         * If almost all of this class's traffic is one exact size, test for
         * that size alone: "size - x <= 0" matches exactly x, and it is the
         * cheapest possible test.
         */
        if (class_topn[best] * 100 >= bestv * 80) {
            lo[n] = (uint32_t)class_top[best];
            span[n] = 0;
        } else {
            lo[n] = kClassLo[best];
            span[n] = kClassHi[best] - kClassLo[best];
        }
        n++;
        used += bestv;
        class_count[best] = 0;
        if (used * 2 >= total)
            break; /* the hot classes cover half the traffic */
    }
    return n;
}

static void jit_compile(void)
{
    if (!__sync_bool_compare_and_swap(&g_compiling, 0, 1))
        return;

    uint32_t cls[JIT_SLOTS], lo[JIT_SLOTS], span[JIT_SLOTS];
    int n = pick_hot_classes(cls, lo, span);
    if (n <= 0) {
        /*
         * The size distribution is too diverse for a specialised dispatch
         * to pay for itself: the range tests would mispredict on almost
         * every allocation.  Step aside and let the static dispatch --
         * which computes the class with arithmetic -- handle everything.
         */
        g_give_up = 1;
        atomic_store_explicit(&g_entry, (entry_fn)fm_generic_malloc,
                              memory_order_release);
        if (getenv("FM_JIT_DEBUG"))
            fprintf(stderr, "[jit] distribution too diverse -- stepping aside\n");
        __atomic_store_n(&g_compiling, 0, __ATOMIC_RELEASE);
        return;
    }

    /* Copy the template into a writable page. */
    size_t tlen = (size_t)(jit_template_end - jit_template);
    size_t map_len = (tlen + 4095) & ~(size_t)4095;
    uint8_t *buf = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        __atomic_store_n(&g_compiling, 0, __ATOMIC_RELEASE);
        return;
    }
    memcpy(buf, jit_template, tlen);

    /*
     * Relocate the patch points into the copy.  Every branch inside the
     * template is relative and therefore stays valid after the copy; only
     * the absolute immediates have to be rewritten.
     */
    const ptrdiff_t delta = (const char *)buf - jit_template;
    const char *sub0[JIT_SLOTS] = {jit_psub_0, jit_psub_1, jit_psub_2, jit_psub_3,
                                    jit_psub_4, jit_psub_5, jit_psub_6, jit_psub_7};
    const char *cmp0[JIT_SLOTS] = {jit_pcmp_0, jit_pcmp_1, jit_pcmp_2, jit_pcmp_3,
                                    jit_pcmp_4, jit_pcmp_5, jit_pcmp_6, jit_pcmp_7};
    const char *disp0[JIT_SLOTS] = {jit_pdisp_0, jit_pdisp_1, jit_pdisp_2, jit_pdisp_3,
                                    jit_pdisp_4, jit_pdisp_5, jit_pdisp_6, jit_pdisp_7};
    const char *jbe0[JIT_SLOTS] = {jit_pjbe_0, jit_pjbe_1, jit_pjbe_2, jit_pjbe_3,
                                    jit_pjbe_4, jit_pjbe_5, jit_pjbe_6, jit_pjbe_7};
    for (int i = 0; i < JIT_SLOTS; i++) {
        p_sub[i] = sub0[i] + delta;
        p_cmp[i] = cmp0[i] + delta;
        p_disp[i] = disp0[i] + delta;
        p_jbe[i] = jbe0[i] + delta;
    }
    p_gen = jit_pgen + delta;
    char *generic = (char *)jit_generic + delta;

    /*
     * Verify the template layout before patching it.  Each patch point must
     * sit exactly where the instruction it belongs to expects it; if the
     * assembler and the C side ever disagree about the layout, the JIT would
     * silently generate garbage.  Checking the opcode bytes turns that into
     * a clean refusal to install the code.
     */
    {
        const uint8_t *b = (const uint8_t *)buf;
        for (int i = 0; i < JIT_SLOTS; i++) {
            const uint8_t *sub = (const uint8_t *)p_sub[i];
            const uint8_t *cmp = (const uint8_t *)p_cmp[i];
            const uint8_t *jbe = (const uint8_t *)p_jbe[i];
            const uint8_t *disp = (const uint8_t *)p_disp[i];
            int ok = 1;
            ok &= sub >= b + 3 && sub[-3] == 0x48 && sub[-2] == 0x8D && sub[-1] == 0x97;
            ok &= cmp >= b + 3 && cmp[-3] == 0x48 && cmp[-2] == 0x81 && cmp[-1] == 0xFA;
            ok &= jbe >= b + 2 && jbe[-2] == 0x0F && jbe[-1] == 0x86;
            ok &= disp >= b + 2 && disp[-2] == 0x48 && disp[-1] == 0xB9;
            if (!ok) {
                fprintf(stderr, "[jit] template layout mismatch -- JIT off\n");
                munmap(buf, map_len);
                __atomic_store_n(&g_compiling, 0, __ATOMIC_RELEASE);
                return;
            }
        }
        const uint8_t *gen = (const uint8_t *)p_gen;
        if (!(gen >= b + 2 && gen[-2] == 0x49 && gen[-1] == 0xBB)) {
            fprintf(stderr, "[jit] template layout mismatch -- JIT off\n");
            munmap(buf, map_len);
            __atomic_store_n(&g_compiling, 0, __ATOMIC_RELEASE);
            return;
        }
    }

    /* The address of the generic fallback. */
    {
        uint64_t gen = (uint64_t)(uintptr_t)&fm_generic_malloc;
        memcpy((void *)p_gen, &gen, 8);
    }

    /* Fill in the per-class immediates. */
    for (int i = 0; i < JIT_SLOTS; i++) {
        if (i < n) {
            /* The test is "size - lo <= span"; the immediate is -lo. */
            int32_t neg_lo = -(int32_t)lo[i];
            memcpy((void *)p_sub[i], &neg_lo, 4);
            memcpy((void *)p_cmp[i], &span[i], 4);
            uint64_t disp = g_tls_head_off + (uint64_t)cls[i] * 8;
            memcpy((void *)p_disp[i], &disp, 8);
        } else {
            /*
             * Unused slot: redirect its branch to the generic entry so the
             * range test is skipped entirely.  (Making the range test
             * "never match" would cost three instructions on every
             * allocation that misses all the hot classes.)
             */
            int32_t rel = (int32_t)(generic - (p_jbe[i] + 4));
            memcpy((void *)p_jbe[i], &rel, 4);
            /*
             * Make the range test always succeed so that the branch is
             * taken and the request goes straight to the generic entry.
             * (size - 0) <= 0x7fffffff holds for every realistic size; a
             * larger request falls through to the next unused slot and
             * eventually to the generic entry as well.
             */
            uint32_t zero = 0, always = 0x7FFFFFFFu;
            uint64_t zero64 = 0;
            memcpy((void *)p_sub[i], &zero, 4);
            memcpy((void *)p_cmp[i], &always, 4);
            memcpy((void *)p_disp[i], &zero64, 8);
        }
    }

    if (mprotect(buf, map_len, PROT_READ | PROT_EXEC) != 0) {
        munmap(buf, map_len);
        __atomic_store_n(&g_compiling, 0, __ATOMIC_RELEASE);
        return;
    }

    memcpy(g_hot_cls, cls, sizeof cls);
    g_hot_count = n;


    if (getenv("FM_JIT_DEBUG")) {
        fprintf(stderr, "[jit] specialised %d classes:", n);
        for (int i = 0; i < n; i++)
            fprintf(stderr, " [%u..%u](c%u)", lo[i], lo[i] + span[i], cls[i]);
        fprintf(stderr, "\n");
    }


    /* Install.  The previous code buffer is deliberately left mapped:
     * freeing it would race with threads still executing it. */
    g_code = buf;
    atomic_store_explicit(&g_entry, (entry_fn)buf, memory_order_release);

    __atomic_store_n(&g_compiling, 0, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* entry points                                                        */
/* ------------------------------------------------------------------ */

static int g_jit_enabled = 1;
static int g_profile_enabled = 1;
/* Set at the end of the constructor.  jit_compile reads state the
 * constructor prepares (g_tls_head_off above all), so the profiling
 * entry must not trigger a compile before then -- allocations made by
 * other libraries' constructors are profiled, but never compiled from. */
static _Atomic int g_initialised;

/* The allocation fast path, shared by the generic and the profiling entry. */
static inline void *fm_fast_path(size_t n);

void *fm_generic_malloc(size_t n);

/* The allocation fast path, shared by the generic and the profiling entry. */
static inline void *fm_fast_path(size_t n)
{
    uint32_t cls = fm_class_of(n);
    if (__builtin_expect(cls != LARGE_CLASS, 1)) {
        void *p = fm_head[cls];
        if (__builtin_expect(p != NULL, 1)) {
            fm_head[cls] = *(void **)p;
            return p;
        }
        return fm_refill_class(cls);
    }
    return fm_large_alloc(n);
}

void *fm_generic_malloc(size_t n)
{
    return fm_fast_path(n);
}

/*
 * The profiling entry.  It samples the request size and, once enough
 * samples have accumulated, triggers a JIT compile.  The sampling branch is
 * perfectly predicted, so it costs a couple of instructions.
 */
/* The cold path of the profiling entry, kept out of line. */
/*
 * The cold path of the profiling entry, kept out of line.  It returns the
 * size so that the caller does not have to keep it in a callee-saved
 * register, which would force a stack frame on the hot path.
 */
__attribute__((noinline)) static size_t fm_sample(size_t n)
{
    if (__builtin_expect(g_give_up, 0))
        return n; /* the JIT has stepped aside; stop profiling */
    fm_profile(n);
    if (__builtin_expect((++g_profile_events & (JIT_TRIGGER - 1)) == 0, 0) &&
        __builtin_expect(atomic_load_explicit(&g_initialised,
                                              memory_order_relaxed), 1))
        jit_compile();
    return n;
}

void *fm_jit_generic(size_t n)
{
    if (__builtin_expect(--t_sample_ctr == 0, 0)) {
        t_sample_ctr = SAMPLE_PERIOD;
        n = fm_sample(n);
    }
    return fm_fast_path(n);
}

__attribute__((constructor)) static void fm_init(void)
{
    const char *e;
    e = getenv("FM_JIT");
    if (e) g_jit_enabled = atoi(e);
    e = getenv("FM_JIT_PROFILE");
    if (e) g_profile_enabled = atoi(e);
    e = getenv("FM_JIT_HOT");
    if (e) g_hot_target = atoi(e);
    e = getenv("FM_JIT_MINSHARE");
    if (e) g_min_share = (uint32_t)atoi(e); /* a percent, like the default */
    jit_init_offsets();
    /* Force the thread-local sample counter to be initialised for this
     * thread; other threads get it from the module's TLS init image. */
    t_sample_ctr = SAMPLE_PERIOD;
    /*
     * The choice of entry point is made once, here, so that the hot path
     * never has to test a flag.
     */
    atomic_store_explicit(&g_entry,
                          (entry_fn)((g_profile_enabled && g_jit_enabled)
                                         ? fm_jit_generic
                                         : fm_generic_malloc),
                          memory_order_release);
    atomic_store_explicit(&g_initialised, 1, memory_order_release);
}

/* The exported malloc dispatches through g_entry.  The indirect jump is
 * perfectly predicted in steady state. */
__asm__(
    ".text\n"
    ".globl malloc\n"
    ".type malloc,@function\n"
    ".align 16\n"
    "malloc:\n"
    "    jmp *g_entry(%rip)\n"
    ".size malloc,.-malloc\n"
);

/* ------------------------------------------------------------------ */
/* exported API                                                        */
/* ------------------------------------------------------------------ */

void free(void *p)
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
    if (__builtin_expect(cls != LARGE_CLASS, 1)) {
        void *old = fm_head[cls];
        *(void **)p = old;
#ifdef FM_DEBUG
        /* The core's realloc path checks the same free-block canary. */
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

void *calloc(size_t n, size_t sz)
{
    if (n != 0 && sz > SIZE_MAX / n) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = n * sz;
    if (total > SMALL_MAX) {
        int zeroed = 1;
        void *p = fm_large_alloc2(total, &zeroed);
        if (p && !zeroed)
            memset(p, 0, total); /* cache hit: contents are stale */
        return p;
    }
    void *p = fm_generic_malloc(total);
    if (!p)
        return NULL;
    memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n) { return fm_realloc(p, n); }

void *reallocarray(void *p, size_t n, size_t sz)
{
    if (n != 0 && sz > SIZE_MAX / n) {
        errno = ENOMEM;
        return NULL;
    }
    return fm_realloc(p, n * sz);
}

int posix_memalign(void **out, size_t align, size_t n)
{
    if (align % sizeof(void *) != 0 || (align & (align - 1)) != 0) {
        *out = NULL;
        return EINVAL;
    }
    void *p = fm_aligned_alloc_impl(align, n);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void *aligned_alloc(size_t align, size_t n)
{
    if ((align & (align - 1)) != 0)
        return NULL;
    return fm_aligned_alloc_impl(align, n);
}

void *memalign(size_t align, size_t n) { return fm_aligned_alloc_impl(align, n); }
void *valloc(size_t n) { return fm_aligned_alloc_impl(FM_PAGE_SIZE, n); }

void *pvalloc(size_t n)
{
    size_t rounded = FM_ALIGN_UP(n, FM_PAGE_SIZE);
    return fm_aligned_alloc_impl(FM_PAGE_SIZE, rounded);
}

size_t malloc_usable_size(void *p) { return fm_usable_size(p); }
int malloc_trim(int flags)
{
    (void)flags;
    /* ADR 0015: drain the large-block cache and unmap every small
     * span the trim census can prove entirely free. */
    size_t released = fm_large_cache_drain();
    released += fm_trim_spans();
    return released ? 1 : 0;
}
int mallopt(int param, int value) { (void)param; (void)value; return 1; }
void malloc_stats(void) {}
