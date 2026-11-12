/*
 * fastmalloc -- a thread-local size-class allocator tuned for the common case.
 *
 * Design:
 *   - 64 KiB-aligned spans; every span starts with a 64-byte header carrying
 *     its size class (or LARGE_CLASS).  Free computes the span by masking the
 *     low 16 bits, so a free needs no per-block metadata at all.
 *   - Per-thread LIFO free lists per size class.  Free is a push, malloc is a
 *     pop.  The only shared state is touched on refill/flush, in batches.
 *   - Central per-class free lists protected by small spinlocks.  Blocks are
 *     handed out in batches of 32, so the lock is taken once per ~32 mallocs.
 *   - Large allocations (>= SMALL_MAX) are direct 64 KiB-aligned mmaps with a
 *     header at the span base; free is a munmap.
 *   - calloc zeroes; realloc grows by copy; aligned allocations up to 64 KiB
 *     alignment use a 64 KiB-aligned mmap so free still only needs a mask.
 *
 * FM_JIT adds a runtime-specialized malloc entry: a small x86-64 code
 * generator emits a decision tree over the hottest exact allocation sizes and
 * a constant-offset TLS pop for each of them.  The exported malloc is a
 * 6-byte trampoline through g_jit_entry.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdatomic.h>

#include "size_classes.h"

#define PAGE_SIZE 4096u
#define ALIGN_UP(x, a) (((uintptr_t)(x) + (a)-1) & ~((uintptr_t)(a)-1))
#define ALIGN_DOWN(x, a) ((uintptr_t)(x) & ~((uintptr_t)(a)-1))
#define ALIGNED_MAGIC 0xA110CEDAu

#define PROFILE_TARGET (1u << 20)
#define JIT_HOT_MAX 4
#define SAMPLE_PERIOD 256
#define SAMPLE_TOTAL 65536

typedef struct Span {
    uint32_t cls;        /* class index, or LARGE_CLASS */
    uint32_t magic;      /* SPAN_MAGIC, or ALIGNED_MAGIC for the 64K-aligned case */
    uintptr_t map_base;  /* mmap base (only meaningful for large) */
    size_t map_size;     /* mmap length */
    size_t user_size;    /* requested size (large) */
    size_t align;        /* requested alignment (aligned allocs) */
} Span;

typedef union Central {
    struct {
        void *head;
        atomic_uint lock;
        char pad[64 - sizeof(void *) - sizeof(atomic_uint)];
    } s;
    char _cacheline[64];
} Central;

typedef struct ThreadCache {
    void *head[CLASS_COUNT];
    uint16_t count[CLASS_COUNT];
    /* profiling for the JIT variant */
    uint32_t sample_ctr;
    uint32_t sample_total;
    uint32_t pad;
} ThreadCache;

static Central g_central[CLASS_COUNT];
static __thread ThreadCache tc __attribute__((tls_model("initial-exec")));

void *fm_malloc(size_t n);

#ifdef FM_JIT
static uint32_t g_hist[SMALL_MAX + 1];
static atomic_uint g_samples;
static _Atomic(uintptr_t) g_jit_entry = (uintptr_t)(void *)&fm_malloc;
static uint32_t g_head_off[CLASS_COUNT];
static uint32_t g_count_off[CLASS_COUNT];

void jit_maybe_compile(void);
static void jit_init_offsets(void);
#endif

/* ------------------------------------------------------------------ */
/* locking                                                             */
/* ------------------------------------------------------------------ */

static inline void fm_lock(int c)
{
    atomic_uint *f = &g_central[c].s.lock;
    while (atomic_exchange_explicit(f, 1, memory_order_acquire)) {
        while (atomic_load_explicit(f, memory_order_relaxed))
            __builtin_ia32_pause();
    }
}

static inline void fm_unlock(int c)
{
    atomic_store_explicit(&g_central[c].s.lock, 0, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* span management                                                     */
/* ------------------------------------------------------------------ */

static void *fm_span_chain(int c, void **tail_out)
{
    size_t bs = kClassSize[c];
    size_t n = (SPAN_SIZE - HEADER_SIZE) / bs;
    char *raw = mmap(NULL, 2 * SPAN_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;
    uintptr_t base = ALIGN_UP(raw, SPAN_SIZE);
    if (base != (uintptr_t)raw)
        munmap(raw, (uintptr_t)base - (uintptr_t)raw);
    uintptr_t end = (uintptr_t)raw + 2 * SPAN_SIZE;
    if (base + SPAN_SIZE < end)
        munmap((void *)(base + SPAN_SIZE), end - (base + SPAN_SIZE));

    Span *sp = (Span *)base;
    sp->cls = (uint32_t)c;
    sp->magic = SPAN_MAGIC;
    sp->map_base = (uintptr_t)raw;
    sp->map_size = 0;
    sp->user_size = 0;
    sp->align = 0;

    void *head = NULL, *tail = NULL;
    char *p = (char *)base + HEADER_SIZE;
    for (size_t i = 0; i < n; i++) {
        *(void **)p = head;
        if (head == NULL)
            tail = p;
        head = p;
        p += bs;
    }
    *tail_out = tail;
    return head;
}

/* Pop up to REFILL_BATCH blocks from the central list.  Caller holds lock. */
static void *fm_pop_batch_locked(int c, int *got)
{
    void *h = g_central[c].s.head;
    if (!h) {
        *got = 0;
        return NULL;
    }
    void *q = h;
    int n = 1;
    while (n < REFILL_BATCH) {
        void *nx = *(void **)q;
        if (!nx)
            break;
        q = nx;
        n++;
    }
    g_central[c].s.head = *(void **)q;
    *(void **)q = NULL;
    *got = n;
    return h;
}

static void *fm_refill(int c)
{
    fm_lock(c);
    int got = 0;
    void *h = fm_pop_batch_locked(c, &got);
    fm_unlock(c);
    if (h) {
        void *p = h;
        tc.head[c] = *(void **)p;
        tc.count[c] = 0;
        return p;
    }

    /* central empty: make a fresh span */
    void *tail = NULL;
    void *chain = fm_span_chain(c, &tail);
    if (!chain)
        return NULL;

    fm_lock(c);
    *(void **)tail = g_central[c].s.head;
    g_central[c].s.head = chain;
    h = fm_pop_batch_locked(c, &got);
    fm_unlock(c);

    void *p = h;
    tc.head[c] = *(void **)p;
    tc.count[c] = 0;
    return p;
}

static void fm_flush(int c)
{
    void *h = tc.head[c];
    tc.head[c] = NULL;
    tc.count[c] = 0;
    if (!h)
        return;
    void *q = h;
    while (*(void **)q)
        q = *(void **)q;
    fm_lock(c);
    *(void **)q = g_central[c].s.head;
    g_central[c].s.head = h;
    fm_unlock(c);
}

/* ------------------------------------------------------------------ */
/* large allocations                                                   */
/* ------------------------------------------------------------------ */

static void *fm_large_alloc(size_t n)
{
    size_t need = n + HEADER_SIZE;
    size_t total = ALIGN_UP(need, SPAN_SIZE) + SPAN_SIZE;
    char *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;
    uintptr_t base = ALIGN_UP(raw, SPAN_SIZE);
    Span *sp = (Span *)base;
    sp->cls = LARGE_CLASS;
    sp->magic = SPAN_MAGIC;
    sp->map_base = (uintptr_t)raw;
    sp->map_size = total;
    sp->user_size = n;
    sp->align = 0;
    return (void *)(base + HEADER_SIZE);
}

static void fm_large_free(Span *sp)
{
    munmap((void *)sp->map_base, sp->map_size);
}

/* ------------------------------------------------------------------ */
/* aligned allocations                                                 */
/* ------------------------------------------------------------------ */

static void *fm_aligned_alloc(size_t align, size_t n)
{
    if (align <= 16)
        return fm_malloc(n);
    if (align < SPAN_SIZE) {
        /* One 64 KiB-aligned mapping; the header stays in the same 64 KiB
         * page as the returned pointer, so free() can still mask. */
        size_t total = n + align + HEADER_SIZE + SPAN_SIZE;
        total = ALIGN_UP(total, PAGE_SIZE);
        char *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED)
            return NULL;
        uintptr_t base = ALIGN_UP(raw, SPAN_SIZE);
        uintptr_t user = ALIGN_UP(base + HEADER_SIZE, align);
        if (user + n > (uintptr_t)raw + total) {
            munmap(raw, total);
            return NULL;
        }
        Span *sp = (Span *)base;
        sp->cls = LARGE_CLASS;
        sp->magic = SPAN_MAGIC;
        sp->map_base = (uintptr_t)raw;
        sp->map_size = total;
        sp->user_size = n;
        sp->align = align;
        return (void *)user;
    }
    /* align >= 64 KiB: the returned pointer is 64 KiB-aligned, so the header
     * cannot live in its own 64 KiB page.  Put it immediately before the
     * returned pointer and tag it with a magic number. */
    size_t total = ALIGN_UP(n + align + HEADER_SIZE, PAGE_SIZE);
    char *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;
    uintptr_t user = ALIGN_UP((uintptr_t)raw + HEADER_SIZE, align);
    if (user + n > (uintptr_t)raw + total) {
        munmap(raw, total);
        return NULL;
    }
    Span *sp = (Span *)(user - HEADER_SIZE);
    sp->cls = LARGE_CLASS;
    sp->magic = ALIGNED_MAGIC;
    sp->map_base = (uintptr_t)raw;
    sp->map_size = total;
    sp->user_size = n;
    sp->align = align;
    return (void *)user;
}

static int fm_is_aligned_ptr(void *p)
{
    if (((uintptr_t)p & (SPAN_SIZE - 1)) != 0)
        return 0;
    Span *sp = (Span *)((char *)p - HEADER_SIZE);
    return sp->magic == ALIGNED_MAGIC;
}

static void fm_aligned_free(void *p)
{
    Span *sp = (Span *)((char *)p - HEADER_SIZE);
    munmap((void *)sp->map_base, sp->map_size);
}

/* ------------------------------------------------------------------ */
/* core allocation                                                     */
/* ------------------------------------------------------------------ */

static inline uint32_t fm_class_of(size_t n)
{
    return kClassOf[(n + 15) >> 4];
}

void *fm_malloc(size_t n)
{
#ifdef FM_JIT
    /* While profiling, sample every small allocation.  After the JIT entry
     * is installed, hot calls no longer pay for this. */
    if (__builtin_expect(n <= SMALL_MAX, 1)) {
        if (++tc.sample_ctr >= SAMPLE_PERIOD) {
            tc.sample_ctr = 0;
            atomic_fetch_add_explicit((_Atomic(uint32_t) *)&g_hist[n],
                                      1, memory_order_relaxed);
            if (++tc.sample_total >= SAMPLE_TOTAL) {
                tc.sample_total = 0;
                atomic_fetch_add_explicit(&g_samples, SAMPLE_TOTAL,
                                          memory_order_relaxed);
                if (atomic_load_explicit(&g_samples, memory_order_relaxed) >=
                    PROFILE_TARGET)
                    jit_maybe_compile();
            }
        }
        uint32_t c = fm_class_of(n);
        void *p = tc.head[c];
        if (__builtin_expect(p != NULL, 1)) {
            tc.head[c] = *(void **)p;
            return p;
        }
        return fm_refill(c);
    }
    return fm_large_alloc(n);
#else
    if (__builtin_expect(n <= SMALL_MAX, 1)) {
        uint32_t c = fm_class_of(n);
        void *p = tc.head[c];
        if (__builtin_expect(p != NULL, 1)) {
            tc.head[c] = *(void **)p;
            return p;
        }
        return fm_refill(c);
    }
    return fm_large_alloc(n);
#endif
}

static void fm_free(void *p)
{
    if (!p)
        return;
    if (__builtin_expect(((uintptr_t)p & (SPAN_SIZE - 1)) == 0 &&
                         fm_is_aligned_ptr(p), 0)) {
        fm_aligned_free(p);
        return;
    }
    Span *sp = (Span *)((uintptr_t)p & SPAN_MASK);
    uint32_t c = sp->cls;
    if (__builtin_expect(c != LARGE_CLASS, 1)) {
        *(void **)p = tc.head[c];
        tc.head[c] = p;
        if (++tc.count[c] >= kClassLimit[c])
            fm_flush(c);
    } else {
        fm_large_free(sp);
    }
}

static size_t fm_usable_size(void *p)
{
    if (!p)
        return 0;
    if (((uintptr_t)p & (SPAN_SIZE - 1)) == 0 && fm_is_aligned_ptr(p)) {
        Span *sp = (Span *)((char *)p - HEADER_SIZE);
        return sp->user_size;
    }
    Span *sp = (Span *)((uintptr_t)p & SPAN_MASK);
    if (sp->cls == LARGE_CLASS)
        return sp->user_size;
    return kClassSize[sp->cls];
}

static void *fm_realloc(void *p, size_t n)
{
    if (!p)
        return fm_malloc(n);
    if (n == 0) {
        fm_free(p);
        return NULL;
    }
    if (((uintptr_t)p & (SPAN_SIZE - 1)) == 0 && fm_is_aligned_ptr(p)) {
        Span *sp = (Span *)((char *)p - HEADER_SIZE);
        if (n <= sp->user_size)
            return p;
        void *q = fm_aligned_alloc(sp->align, n);
        if (!q)
            return NULL;
        memcpy(q, p, sp->user_size);
        fm_aligned_free(p);
        return q;
    }
    Span *sp = (Span *)((uintptr_t)p & SPAN_MASK);
    if (sp->cls != LARGE_CLASS) {
        size_t old = kClassSize[sp->cls];
        if (n <= old)
            return p;
        void *q = fm_malloc(n);
        if (!q)
            return NULL;
        memcpy(q, p, old);
        fm_free(p);
        return q;
    }
    if (n <= sp->user_size)
        return p;
    void *q = sp->align ? fm_aligned_alloc(sp->align, n) : fm_large_alloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, sp->user_size);
    fm_large_free(sp);
    return q;
}

/* ------------------------------------------------------------------ */
/* exported API                                                        */
/* ------------------------------------------------------------------ */

#ifndef FM_JIT
void *malloc(size_t n) { return fm_malloc(n); }
#endif

void free(void *p) { fm_free(p); }
void *calloc(size_t n, size_t sz)
{
    if (n != 0 && sz > SIZE_MAX / n) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = n * sz;
    void *p = fm_malloc(total);
    if (!p)
        return NULL;
    if (total > SMALL_MAX || (((uintptr_t)p & (SPAN_SIZE - 1)) == 0 &&
                              fm_is_aligned_ptr(p)))
        return p; /* fresh mmap: already zero */
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
    void *p = fm_aligned_alloc(align, n);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void *aligned_alloc(size_t align, size_t n)
{
    if ((align & (align - 1)) != 0)
        return NULL;
    return fm_aligned_alloc(align, n);
}

void *memalign(size_t align, size_t n) { return fm_aligned_alloc(align, n); }

void *valloc(size_t n) { return fm_aligned_alloc(PAGE_SIZE, n); }

void *pvalloc(size_t n)
{
    size_t rounded = ALIGN_UP(n, PAGE_SIZE);
    return fm_aligned_alloc(PAGE_SIZE, rounded);
}

size_t malloc_usable_size(void *p) { return fm_usable_size(p); }

int malloc_trim(int flags)
{
    (void)flags;
    return 1;
}

int mallopt(int param, int value)
{
    (void)param;
    (void)value;
    return 1;
}

void malloc_stats(void) {}

/* ------------------------------------------------------------------ */
/* JIT variant                                                         */
/* ------------------------------------------------------------------ */

#ifdef FM_JIT

void jitmalloc_compile_now(void)
{
    jit_maybe_compile();
}

static void jit_init_offsets(void)
{
    uintptr_t tp = (uintptr_t)__builtin_thread_pointer();
    for (int c = 0; c < CLASS_COUNT; c++) {
        g_head_off[c] = (uint32_t)((uintptr_t)&tc.head[c] - tp);
        g_count_off[c] = (uint32_t)((uintptr_t)&tc.count[c] - tp);
    }
}

/* ---- tiny x86-64 emitter ------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    size_t hot[JIT_HOT_MAX];
    size_t refill[JIT_HOT_MAX];
} Emit;

static void em_put(Emit *e, const void *bytes, size_t n)
{
    if (e->len + n > e->cap)
        return; /* should not happen with a generous buffer */
    memcpy(e->buf + e->len, bytes, n);
    e->len += n;
}

static void em_u8(Emit *e, uint8_t v) { em_put(e, &v, 1); }
static void em_u32(Emit *e, uint32_t v) { em_put(e, &v, 4); }
static void em_u64(Emit *e, uint64_t v) { em_put(e, &v, 8); }

static void em_cmp_rdi_imm32(Emit *e, uint32_t imm)
{
    em_u8(e, 0x81);
    em_u8(e, 0xFF);
    em_u32(e, imm);
}

static void em_je_rel32(Emit *e, size_t target)
{
    size_t here = e->len;
    int32_t rel = (int32_t)(target - (here + 6));
    em_u8(e, 0x0F);
    em_u8(e, 0x84);
    em_u32(e, (uint32_t)rel);
}

static void em_mov_fs_to_rax(Emit *e, uint32_t disp)
{
    /* mov %fs:disp32, %rax */
    em_u8(e, 0x64);
    em_u8(e, 0x48);
    em_u8(e, 0x8B);
    em_u8(e, 0x04);
    em_u8(e, 0x25);
    em_u32(e, disp);
}

static void em_mov_rdx_to_fs(Emit *e, uint32_t disp)
{
    /* mov %rdx, %fs:disp32 */
    em_u8(e, 0x64);
    em_u8(e, 0x48);
    em_u8(e, 0x89);
    em_u8(e, 0x14);
    em_u8(e, 0x25);
    em_u32(e, disp);
}

static void em_test_rax_rax(Emit *e)
{
    static const uint8_t op[] = {0x48, 0x85, 0xC0};
    em_put(e, op, 3);
}

static void em_mov_rdi_imm32(Emit *e, uint32_t imm)
{
    em_u8(e, 0xBF);
    em_u32(e, imm);
}

static void em_movabs_rax(Emit *e, uint64_t addr)
{
    em_u8(e, 0x48);
    em_u8(e, 0xB8);
    em_u64(e, addr);
}

static void em_jmp_rax(Emit *e)
{
    static const uint8_t op[] = {0xFF, 0xE0};
    em_put(e, op, 2);
}

static void em_ret(Emit *e) { em_u8(e, 0xC3); }

/* ---- compile ------------------------------------------------------- */

void jit_maybe_compile(void)
{
    static int compiling;
    if (!__sync_bool_compare_and_swap(&compiling, 0, 1))
        return;

    uint32_t total = atomic_load_explicit(&g_samples, memory_order_relaxed);
    if (total < PROFILE_TARGET) {
        compiling = 0;
        return;
    }

    jit_init_offsets();

    /* pick the hottest exact sizes */
    uint32_t best[JIT_HOT_MAX] = {0};
    uint32_t best_count[JIT_HOT_MAX] = {0};
    for (uint32_t s = 1; s <= SMALL_MAX; s++) {
        uint32_t v = atomic_load_explicit((_Atomic(uint32_t) *)&g_hist[s],
                                          memory_order_relaxed);
        if (!v)
            continue;
        for (int i = 0; i < JIT_HOT_MAX; i++) {
            if (v > best_count[i]) {
                for (int j = JIT_HOT_MAX - 1; j > i; j--) {
                    best_count[j] = best_count[j - 1];
                    best[j] = best[j - 1];
                }
                best_count[i] = v;
                best[i] = s;
                break;
            }
        }
    }

    int nhot = 0;
    while (nhot < JIT_HOT_MAX && best_count[nhot] > total / 20)
        nhot++;
    if (nhot == 0) {
        compiling = 0;
        return;
    }

    size_t cap = 512 + 64 * JIT_HOT_MAX;
    uint8_t *buf = mmap(NULL, cap, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        compiling = 0;
        return;
    }
    Emit e = {buf, 0, cap, {0}, {0}};

    for (int i = 0; i < nhot; i++) {
        em_cmp_rdi_imm32(&e, best[i]);
        e.hot[i] = e.len;
        em_je_rel32(&e, 0); /* patched later */
    }
    /* fall back to the generic path */
    em_movabs_rax(&e, (uint64_t)&fm_malloc);
    em_jmp_rax(&e);

    for (int i = 0; i < nhot; i++) {
        uint32_t c = fm_class_of(best[i]);
        size_t here = e.len;
        /* patch the je to land here */
        {
            size_t jmp_pos = e.hot[i] + 2;
            int32_t rel = (int32_t)(here - (jmp_pos + 4));
            memcpy(buf + jmp_pos, &rel, 4);
        }
        em_mov_fs_to_rax(&e, g_head_off[c]);
        em_test_rax_rax(&e);
        e.refill[i] = e.len;
        em_je_rel32(&e, 0); /* patched later */
        /* pop */
        static const uint8_t mov_rax_rdx[] = {0x48, 0x8B, 0x10};
        em_put(&e, mov_rax_rdx, 3);
        em_mov_rdx_to_fs(&e, g_head_off[c]);
        em_ret(&e);

        here = e.len;
        {
            size_t jmp_pos = e.refill[i] + 2;
            int32_t rel = (int32_t)(here - (jmp_pos + 4));
            memcpy(buf + jmp_pos, &rel, 4);
        }
        em_mov_rdi_imm32(&e, c);
        em_movabs_rax(&e, (uint64_t)&fm_refill);
        em_jmp_rax(&e);
    }

    if (mprotect(buf, cap, PROT_READ | PROT_EXEC) != 0) {
        compiling = 0;
        return;
    }

    atomic_store_explicit(&g_jit_entry, (uintptr_t)buf, memory_order_release);
    atomic_store_explicit(&g_samples, 0, memory_order_relaxed);
    compiling = 0;
}

/* Trampoline: exported malloc jumps through g_jit_entry. */
__asm__(
    ".text\n"
    ".globl malloc\n"
    ".type malloc,@function\n"
    ".align 16\n"
    "malloc:\n"
    "    jmp *g_jit_entry(%rip)\n"
    "    nop\n    nop\n    nop\n    nop\n"
    "    nop\n    nop\n    nop\n    nop\n"
    ".size malloc,.-malloc\n"
);

#endif /* FM_JIT */