/*
 * instr.c -- deterministic workload for callgrind measurement.
 *
 * Runs a fixed number of allocation operations of a given shape so that the
 * instruction count of the allocator can be compared exactly between
 * implementations.  Output is a checksum so nothing can be optimised away.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint64_t rnd_state = 0x123456789abcdefULL;

static inline uint64_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return rnd_state;
}

int main(int argc, char **argv)
{
    /*
     * usage: instr <mode> [iterations] [size]
     *
     * The optional third argument is the block size used by the modes that
     * allocate a single fixed size.
     */
    const char *mode = argc > 1 ? argv[1] : "churn";
    long n = argc > 2 ? atol(argv[2]) : 200000;
    size_t sz = argc > 3 ? (size_t)atol(argv[3]) : 64;

    uint64_t sum = 0;

    if (!strcmp(mode, "churn")) {
        /* same size, alloc+free: the classic hot loop */
        for (long i = 0; i < n; i++) {
            void *p = malloc(sz);
            sum += (uintptr_t)p;
            free(p);
        }
    } else if (!strcmp(mode, "churnmix")) {
        /* a few sizes interleaved, like a real object graph */
        static const size_t sizes[] = {16, 24, 32, 48, 64, 96, 128, 256};
        void *v[256];
        for (long i = 0; i < n; i++) {
            size_t s = sizes[rnd() & 7];
            void *p = malloc(s);
            memset(p, 0, s);
            sum += (uintptr_t)p;
            free(p);
        }
        (void)v;
    } else if (!strcmp(mode, "batch")) {
        /* allocate a batch, keep it, then free the batch */
        long keep = 256;
        void **v = malloc(keep * sizeof(void *));
        for (long i = 0; i < n; i++) {
            for (long j = 0; j < keep; j++)
                v[j] = malloc(sz);
            for (long j = 0; j < keep; j++)
                free(v[j]);
        }
        free(v);
    } else if (!strcmp(mode, "retain")) {
        /* allocate and retain: measures the alloc-only path */
        long keep = 4096;
        void **v = malloc(keep * sizeof(void *));
        for (long i = 0; i < n; i++) {
            long j = i % keep;
            if (v[j]) free(v[j]);
            v[j] = malloc(sz);
        }
        for (long j = 0; j < keep; j++)
            free(v[j]);
        free(v);
    } else if (!strcmp(mode, "mixed")) {
        /* random sizes with random lifetimes, like a parser building objects */
        long live = 1024;
        void **v = malloc(live * sizeof(void *));
        size_t *sz = malloc(live * sizeof(size_t));
        for (long i = 0; i < n; i++) {
            long j = rnd() % live;
            if (v[j]) {
                free(v[j]);
                v[j] = NULL;
            }
            sz[j] = 16 + (rnd() % 240);
            v[j] = malloc(sz[j]);
            sum += (uintptr_t)v[j];
        }
        for (long j = 0; j < live; j++)
            free(v[j]);
        free(v);
        free(sz);
    } else if (!strcmp(mode, "cross")) {
        /* allocate in one "thread", free in another: the larson pattern */
        long live = 4096;
        void **v = malloc(live * sizeof(void *));
        for (long i = 0; i < n; i++) {
            long j = i % live;
            if (v[j]) {
                free(v[j]); /* simulated cross-thread free */
                v[j] = NULL;
            }
            v[j] = malloc(96);
            sum += (uintptr_t)v[j];
        }
        for (long j = 0; j < live; j++)
            free(v[j]);
        free(v);
    } else if (!strcmp(mode, "large")) {
        for (long i = 0; i < n / 100 + 1; i++) {
            void *p = malloc(65536);
            sum += (uintptr_t)p;
            free(p);
        }
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    printf("%llu\n", (unsigned long long)sum);
    return 0;
}
