/* Prove the diagnostic counters observe a forced busy automatic census. */
#include "fm_static.c"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    unsigned long attempts = atomic_load(&diag_attempt);
    unsigned long bails = atomic_load(&diag_bail);
    unsigned long acquired = atomic_load(&diag_acquired);
    assert(!pthread_mutex_lock(&fm_census_lock));
    assert(fm_trim_class(0, 0, 0) == 0);
    assert(atomic_load(&diag_attempt) == attempts + 1);
    assert(atomic_load(&diag_bail) == bails + 1);
    assert(atomic_load(&diag_acquired) == acquired);
    assert(!pthread_mutex_unlock(&fm_census_lock));
    fm_trim_class(0, 0, 1);
    assert(atomic_load(&diag_acquired) == acquired + 1);
    puts("diagnostic busy and explicit counters ok");
}
