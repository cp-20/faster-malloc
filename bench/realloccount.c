/* Untimed diagnostic: count application realloc calls and large requests.
 * Preload before the measured allocator; never use its output as a timing. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stddef.h>
#include <malloc.h>
#include <stdio.h>
static void *(*next_realloc)(void *, size_t);
static _Atomic unsigned long calls, large, nonnull, large_growth;
__attribute__((constructor)) static void init(void) {
    next_realloc = dlsym(RTLD_NEXT, "realloc");
}
void *realloc(void *p, size_t n) {
    if (!next_realloc) next_realloc = dlsym(RTLD_NEXT, "realloc");
    atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed);
    if (n > 32768) atomic_fetch_add_explicit(&large, 1, memory_order_relaxed);
    if (p) {
        atomic_fetch_add_explicit(&nonnull, 1, memory_order_relaxed);
        if (n > 32768 && n > malloc_usable_size(p))
            atomic_fetch_add_explicit(&large_growth, 1, memory_order_relaxed);
    }
    return next_realloc(p, n);
}
__attribute__((destructor)) static void report(void) {
    fprintf(stderr, "realloc_calls=%lu requests_gt_32768=%lu nonnull=%lu large_growth=%lu\n",
            atomic_load(&calls), atomic_load(&large), atomic_load(&nonnull),
            atomic_load(&large_growth));
}
