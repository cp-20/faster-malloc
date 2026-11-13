/*
 * fm_static.c -- the non-JIT build of the v2 allocator.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FM_SMALL_FIRST_SPAN /* ADR 0060; JIT guard rejected in ADR 0061. */
#include "fm_core.h"

FM_Config fm_cfg;
pthread_once_t fm_cfg_once = PTHREAD_ONCE_INIT;
FM_Central fm_central[CLASS_COUNT];
__thread void *fm_head[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread void *fm_tail[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread uint32_t fm_cnt[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread int fm_exit_armed __attribute__((tls_model("initial-exec")));
__thread FM_Batch *fm_batch_pool __attribute__((tls_model("initial-exec")));
__thread void *fm_lcache_head[FM_LCACHE_BUCKETS] __attribute__((tls_model("initial-exec")));
__thread uint64_t fm_lcache_bytes __attribute__((tls_model("initial-exec")));
__thread uint32_t fm_flush_seq[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread int fm_in_census __attribute__((tls_model("initial-exec")));

void *fm_generic_malloc(size_t n) { return fm_malloc(n); }
void *(*fm_generic_malloc_ptr)(size_t) = fm_generic_malloc;

void *malloc(size_t n) { return fm_malloc(n); }
void free(void *p) { fm_free(p); }

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
    void *p = fm_malloc(total);
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
    if (align == 0 || align % sizeof(void *) != 0 || (align & (align - 1)) != 0)
        return EINVAL;
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
