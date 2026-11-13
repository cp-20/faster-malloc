/* Isolated internal check: discard cold payload, preserve header and reuse. */
#include "fm_static.c"
#include <assert.h>
static int resident(void *p) {
    unsigned char v=0;
    assert(!mincore((void *)FM_ALIGN_UP((uintptr_t)p, FM_PAGE_SIZE), FM_PAGE_SIZE, &v));
    return v & 1;
}
int main(void) {
    assert(FM_CFG()->lcache_cold_purge);
    size_t n=256*1024;
    unsigned char *p=fm_large_alloc2(n, NULL);
    assert(p);
    memset(p,0x5a,n);
    assert(resident(p+FM_PAGE_SIZE));
    FM_Span *sp=(FM_Span *)(p-HEADER_SIZE);
    fm_large_free(sp,p);
    assert(!resident(p+FM_PAGE_SIZE));
    assert(sp->magic==FM_SPAN_MAGIC);
    unsigned char *q=fm_large_alloc2(n,NULL);
    assert(q==p);
    memset(q,0xa5,n);
    fm_large_free(sp,q);
    assert(resident(q+FM_PAGE_SIZE));
    assert(fm_large_cache_drain()>0);
    /* Cache hits stay conservatively non-zeroed: calloc must clear the header page too. */
    p=malloc(n); assert(p);
    volatile unsigned char *v=p;
    for(size_t i=0;i<n;i++) v[i]=0x7b;
    free(p);
    p=calloc(1,n); assert(p);
    v=p;
    for(size_t i=0;i<n;i++) assert(v[i]==0);
    free(p);
    return 0;
}
