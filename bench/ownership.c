/* Round 8: four independent producer/consumer pairs, 128 small request sizes.
 * Queue slots transfer ownership with acquire/release; no allocator internals.
 * Optional trim adds concurrent census traffic. Counts and bytes are checked. */
#define _GNU_SOURCE
#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define PAIRS 4
#define RING 256
struct channel { void *slots[RING]; atomic_ulong head, tail; };
static struct channel channels[PAIRS];
static unsigned long iterations = 200000;
static int trim;
static pthread_barrier_t start;
static size_t size_for(unsigned long i) { return 16 + (i % 128) * 256; }
static void *produce(void *arg) {
    struct channel *c = arg;
    pthread_barrier_wait(&start);
    for (unsigned long i = 0; i < iterations; i++) {
        while (i - atomic_load_explicit(&c->tail, memory_order_acquire) >= RING)
            sched_yield();
        size_t n = size_for(i);
        unsigned char *p = malloc(n);
        assert(p);
        p[0] = (unsigned char)i; p[n-1] = (unsigned char)(i ^ 0xa5);
        c->slots[i % RING] = p;
        atomic_store_explicit(&c->head, i+1, memory_order_release);
    }
    return NULL;
}
static void *consume(void *arg) {
    struct channel *c = arg;
    pthread_barrier_wait(&start);
    for (unsigned long i = 0; i < iterations; i++) {
        while (atomic_load_explicit(&c->head, memory_order_acquire) <= i)
            sched_yield();
        unsigned char *p = c->slots[i % RING];
        size_t n = size_for(i);
        assert(p[0] == (unsigned char)i && p[n-1] == (unsigned char)(i ^ 0xa5));
        free(p);
        atomic_store_explicit(&c->tail, i+1, memory_order_release);
        if (trim && i % 8192 == 8191) malloc_trim(0);
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc > 1) iterations = strtoul(argv[1], NULL, 10);
    if (argc > 2) trim = atoi(argv[2]);
    assert(iterations > 0);
    pthread_t threads[PAIRS*2];
    assert(!pthread_barrier_init(&start, NULL, PAIRS*2));
    for (int i=0; i<PAIRS; i++) {
        assert(!pthread_create(&threads[i*2], NULL, produce, &channels[i]));
        assert(!pthread_create(&threads[i*2+1], NULL, consume, &channels[i]));
    }
    for (int i=0; i<PAIRS*2; i++) assert(!pthread_join(threads[i], NULL));
    for (int i=0; i<PAIRS; i++) assert(atomic_load(&channels[i].tail) == iterations);
    assert(!pthread_barrier_destroy(&start));
    printf("ownership ok: %lu transfers, trim=%d\n", iterations*PAIRS, trim);
}
