/* Untimed attribution build only: link allocator with
 * -Wl,--wrap=pthread_mutex_lock. Clock reads perturb this diagnostic. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static _Atomic uintptr_t keys[2];
static _Atomic unsigned long counts[2], nanos[2];
extern int __real_pthread_mutex_lock(pthread_mutex_t *);
static unsigned long now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec*1000000000UL+t.tv_nsec;
}
int __wrap_pthread_mutex_lock(pthread_mutex_t *p) {
    int slot;
    for (slot=0; slot<2; slot++) {
        uintptr_t key=atomic_load(&keys[slot]);
        if (!key) { uintptr_t empty=0; atomic_compare_exchange_strong(&keys[slot], &empty, (uintptr_t)p); }
        if (atomic_load(&keys[slot])==(uintptr_t)p) break;
    }
    unsigned long t=now();
    int rc=__real_pthread_mutex_lock(p);
    unsigned long elapsed=now()-t;
    if (slot<2) { atomic_fetch_add(&counts[slot],1);atomic_fetch_add(&nanos[slot],elapsed); }
    return rc;
}
__attribute__((destructor)) static void report(void) {
    for (int i=0;i<2;i++) {
        Dl_info info;
        uintptr_t key=atomic_load(&keys[i]);
        if (key && dladdr((void *)key,&info))
            fprintf(stderr,"mutex_offset=0x%lx calls=%lu inclusive_lock_ns=%lu\n",
                    key-(uintptr_t)info.dli_fbase,atomic_load(&counts[i]),atomic_load(&nanos[i]));
    }
}
