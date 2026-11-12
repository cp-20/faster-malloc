/*
 * mt.c -- multi-threaded allocator benchmark.
 *
 * Measures the two patterns that matter for servers:
 *
 *   local   each thread allocates and frees on its own.  This is the
 *           scalability test: the allocator must not share mutable state
 *           between threads.
 *
 *   xfree   one thread allocates, another frees.  This is the "larson"
 *           pattern: the free has to travel to a different thread's cache
 *           lines, which is where lock-based allocators fall apart.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

#define RING 4096

/*
 * The ring is a bounded queue: the producer must not lap the consumer, or
 * it would overwrite entries that have not been freed yet and the consumer
 * would free the same pointer twice.  That is a bug in the benchmark, not
 * in the allocator, but it would show up as an allocator crash.
 */

static long g_iters = 200000;
static int g_nthreads = 4;
static int g_mode; /* 0 = local, 1 = xfree */

static inline uint64_t rnd(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

typedef struct {
    void **ring;
    volatile uint64_t head;
    volatile uint64_t tail;
    volatile int done;
    uint64_t seed;
} Chan;

static Chan g_chan;

static void *local_worker(void *arg)
{
    uint64_t s = (uint64_t)(uintptr_t)arg * 2654435761u + 1;
    uint64_t sum = 0;
    for (long i = 0; i < g_iters; i++) {
        size_t sz = 16 + (rnd(&s) % 240);
        void *p = malloc(sz);
        memset(p, 1, sz);
        sum += (uintptr_t)p;
        free(p);
    }
    return (void *)sum;
}

/* One producer allocates, one consumer frees. */
static void *producer(void *arg)
{
    uint64_t s = (uint64_t)(uintptr_t)arg * 11400714819323198485ull + 1;
    (void)s;
    for (long i = 0; i < g_iters; i++) {
        void *p = malloc(96);
        uint64_t h = __atomic_load_n(&g_chan.head, __ATOMIC_RELAXED);
        for (;;) {
            uint64_t t = __atomic_load_n(&g_chan.tail, __ATOMIC_ACQUIRE);
            if (h - t < RING)
                break;
            sched_yield();
        }
        g_chan.ring[h % RING] = p;
        __atomic_store_n(&g_chan.head, h + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_chan.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    uint64_t n = 0;
    for (;;) {
        uint64_t t = __atomic_load_n(&g_chan.tail, __ATOMIC_ACQUIRE);
        uint64_t h = __atomic_load_n(&g_chan.head, __ATOMIC_ACQUIRE);
        if (t == h) {
            if (__atomic_load_n(&g_chan.done, __ATOMIC_ACQUIRE))
                break;
            sched_yield();
            continue;
        }
        void *p = g_chan.ring[t % RING];
        __atomic_store_n(&g_chan.tail, t + 1, __ATOMIC_RELEASE);
        free(p);
        n++;
    }
    return (void *)n;
}

int main(int argc, char **argv)
{
    if (argc > 1) g_mode = atoi(argv[1]);
    if (argc > 2) g_iters = atol(argv[2]);
    if (argc > 3) g_nthreads = atoi(argv[3]);

    if (g_mode == 0) {
        pthread_t th[64];
        for (int i = 0; i < g_nthreads; i++)
            pthread_create(&th[i], NULL, local_worker, (void *)(uintptr_t)(i + 1));
        for (int i = 0; i < g_nthreads; i++)
            pthread_join(th[i], NULL);
    } else {
        g_chan.ring = malloc(RING * sizeof(void *));
        pthread_t prod, cons;
        pthread_create(&prod, NULL, producer, (void *)1);
        pthread_create(&cons, NULL, consumer, NULL);
        pthread_join(prod, NULL);
        pthread_join(cons, NULL);
        free(g_chan.ring);
    }
    return 0;
}
