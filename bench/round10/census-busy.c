/* A busy automatic census must not flush, detach, or leak prior TLS results. */
#define FM_CENSUS_TRYLOCK 1
#include "../../src/fm_static.c"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    void *p = malloc(896);
    assert(p);
    uint32_t cls = ((FM_Span *)((uintptr_t)p & SPAN_MASK))->cls;
    free(p);
    void *head = fm_head[cls];
    FM_Batch *central = atomic_load(&fm_central[cls].head);
    fm_census_walked = fm_census_marked = fm_census_waiting = 7;
    fm_census_reclaimed = fm_census_pg_waiting = 7;
    fm_census_purged = 7;
    assert(!pthread_mutex_lock(&fm_census_lock));
    assert(fm_trim_class(cls, 0, 0) == 0);
    assert(fm_head[cls] == head);
    assert(atomic_load(&fm_central[cls].head) == central);
    assert(!fm_census_walked && !fm_census_marked && !fm_census_waiting);
    assert(!fm_census_reclaimed && !fm_census_pg_waiting && !fm_census_purged);
    assert(!fm_in_census);
    assert(!pthread_mutex_unlock(&fm_census_lock));
    fm_trim_class(cls, 0, 1);
    puts("busy census skip and explicit trim ok");
}
