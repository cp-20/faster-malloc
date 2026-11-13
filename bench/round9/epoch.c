/* White-box grace-period test: a stalled reader pins a detached header.
 * Built as an executable so private epoch state needs no exported test API. */
#ifndef FM_RECLAIM_EPOCH
#define FM_RECLAIM_EPOCH
#endif
#include "../../src/fm_static.c"
#include <assert.h>
#include <stdio.h>
#include <sched.h>
static atomic_int pinned, release_reader, reclaimed;
static FM_Batch header, successor;
static pthread_barrier_t start;
static void *reader(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&start);
    fm_epoch_enter();
    FM_Batch *b = atomic_load(&fm_central[0].head);
    assert(b == &header);
    atomic_store(&pinned, 1);
    while (!atomic_load(&release_reader)) sched_yield();
    assert(b->next == &successor);
    fm_epoch_leave();
    return NULL;
}
static void *retire(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&start);
    while (!atomic_load(&pinned)) sched_yield();
    fm_stack_exclusive();
    assert(atomic_exchange(&fm_central[0].head, NULL) == &header);
    header.next = NULL; /* pool reuse may only happen beyond the grace period */
    atomic_store(&reclaimed, 1);
    fm_stack_resume();
    return NULL;
}
int main(void)
{
    /* Use an otherwise unused class only after thread creation has completed. */
    pthread_t a, b;
    assert(!pthread_barrier_init(&start, NULL, 3));
    assert(!pthread_create(&a, NULL, reader, NULL));
    assert(!pthread_create(&b, NULL, retire, NULL));
    header.next = &successor;
    atomic_store(&fm_central[0].head, &header);
    pthread_barrier_wait(&start);
    while (!(atomic_load(&fm_stack_epoch) & FM_EPOCH_CLOSED)) sched_yield();
    assert(!atomic_load(&reclaimed));
    atomic_store(&release_reader, 1);
    assert(!pthread_join(a, NULL));
    assert(!pthread_join(b, NULL));
    assert(atomic_load(&reclaimed));
    assert(atomic_load(&fm_stack_epoch) == 0);
    assert(!pthread_barrier_destroy(&start));
    puts("epoch stalled-reader grace ok");
}
