/* TSan regression: concurrent sampling and the broad-distribution fallback. */
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

static pthread_barrier_t start;
static void *worker(void *unused)
{
    (void)unused;
    void *(*volatile allocate)(size_t) = malloc;
    void (*volatile release)(void *) = free;
    pthread_barrier_wait(&start);
    for (int i = 0; i < 100000; i++) {
        void *p = allocate(16 + (i % 4000));
        assert(p);
        release(p);
    }
    return NULL;
}
int main(void)
{
    pthread_t threads[8];
    assert(!pthread_barrier_init(&start, NULL, 8));
    for (int i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&start));
    puts("jitprofile ok (8 threads, 800000 allocations)");
    return 0;
}
