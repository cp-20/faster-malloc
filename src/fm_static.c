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
_Atomic int fm_cfg_ready;
FM_Central fm_central[CLASS_COUNT];
__thread FM_HotTls fm_tls __attribute__((tls_model("initial-exec")));
__thread char *fm_bump_ptr[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread char *fm_bump_end[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread int fm_exit_armed __attribute__((tls_model("initial-exec")));
__thread FM_Batch *fm_batch_pool __attribute__((tls_model("initial-exec")));
__thread void *fm_lcache_head[FM_LCACHE_BUCKETS] __attribute__((tls_model("initial-exec")));
__thread uint64_t fm_lcache_bytes __attribute__((tls_model("initial-exec")));
__thread uint32_t fm_lc_hits __attribute__((tls_model("initial-exec")));
__thread uint32_t fm_lc_misses __attribute__((tls_model("initial-exec")));
__thread uint32_t fm_ltick __attribute__((tls_model("initial-exec")));
__thread size_t fm_grow_from __attribute__((tls_model("initial-exec")));
__thread uint32_t fm_lbucket_seen[FM_LCACHE_BUCKETS] __attribute__((tls_model("initial-exec")));
__thread uint64_t fm_lcache_warm_mask __attribute__((tls_model("initial-exec")));
__thread uint32_t fm_flush_seq[CLASS_COUNT] __attribute__((tls_model("initial-exec")));
__thread int fm_in_census __attribute__((tls_model("initial-exec")));

void *fm_generic_malloc(size_t n) { return fm_malloc(n); }
void *(*fm_generic_malloc_ptr)(size_t) = fm_generic_malloc;

void *malloc(size_t n) { return fm_malloc(n); }

void free(void *p) { fm_free(p); }

void *calloc(size_t n, size_t sz) { return fm_calloc(n, sz); }

void *realloc(void *p, size_t n) { return fm_realloc(p, n); }

void *reallocarray(void *p, size_t n, size_t sz)
{
    size_t total;
    if (__builtin_expect(__builtin_mul_overflow(n, sz, &total), 0))
        return fm_calloc_overflow();
    return fm_realloc(p, total);
}

#ifndef malloc
void free_sized(void *p, size_t sz) { (void)sz; fm_free(p); }
void free_aligned_sized(void *p, size_t align, size_t sz) { (void)align; (void)sz; fm_free(p); }

void *_Znwm(size_t n) { return fm_operator_new(n); }
void *_Znam(size_t n) { return fm_operator_new(n); }
void *_ZnwmRKSt9nothrow_t(size_t n, const void *nt) { (void)nt; return fm_operator_new_nothrow(n); }
void *_ZnamRKSt9nothrow_t(size_t n, const void *nt) { (void)nt; return fm_operator_new_nothrow(n); }
void _ZdlPv(void *p) { fm_free(p); }
void _ZdaPv(void *p) { fm_free(p); }
void _ZdlPvm(void *p, size_t sz) { fm_delete_sized(p, sz); }
void _ZdaPvm(void *p, size_t sz) { fm_delete_sized(p, sz); }
void _ZdlPvRKSt9nothrow_t(void *p, const void *nt) { (void)nt; fm_free(p); }
void _ZdaPvRKSt9nothrow_t(void *p, const void *nt) { (void)nt; fm_free(p); }
void *_ZnwmSt11align_val_t(size_t n, size_t al) { return fm_operator_new_aligned(n, al); }
void *_ZnamSt11align_val_t(size_t n, size_t al) { return fm_operator_new_aligned(n, al); }
void *_ZnwmSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, const void *nt) { (void)nt; return fm_new_aligned_slow(n, al, 1); }
void *_ZnamSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, const void *nt) { (void)nt; return fm_new_aligned_slow(n, al, 1); }
void _ZdlPvSt11align_val_t(void *p, size_t al) { (void)al; fm_free(p); }
void _ZdaPvSt11align_val_t(void *p, size_t al) { (void)al; fm_free(p); }
void _ZdlPvmSt11align_val_t(void *p, size_t sz, size_t al) { fm_delete_sized_aligned(p, sz, al); }
void _ZdaPvmSt11align_val_t(void *p, size_t sz, size_t al) { fm_delete_sized_aligned(p, sz, al); }
void _ZdlPvSt11align_val_tRKSt9nothrow_t(void *p, size_t al, const void *nt) { (void)al; (void)nt; fm_free(p); }
void _ZdaPvSt11align_val_tRKSt9nothrow_t(void *p, size_t al, const void *nt) { (void)al; (void)nt; fm_free(p); }
#endif

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
