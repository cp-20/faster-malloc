/* aligncheck.c -- every (alignment, size) pair returns aligned, writable
 * memory from aligned_alloc / posix_memalign (ADR 0078 small-span path). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    int bad = 0;
    for (size_t al = 16; al <= 4096; al *= 2)
        for (size_t sz = 1; sz <= 70000; sz += (sz < 2048 ? 1 : 61)) {
            void *v[40];
            for (int k = 0; k < 40; k++) {
                v[k] = aligned_alloc(al, (sz + al - 1) / al * al);
                if (!v[k] || ((uintptr_t)v[k] & (al - 1))) {
                    if (bad++ < 5) fprintf(stderr, "BAD al=%zu sz=%zu p=%p\n", al, sz, v[k]);
                } else
                    memset(v[k], 0xab, sz);
            }
            void *q;
            if (posix_memalign(&q, al, sz) || ((uintptr_t)q & (al - 1))) {
                if (bad++ < 5) fprintf(stderr, "BAD posix_memalign al=%zu sz=%zu\n", al, sz);
            } else
                free(q);
            for (int k = 0; k < 40; k++) free(v[k]);
        }
    printf(bad ? "aligncheck FAIL %d\n" : "aligncheck ok\n", bad);
    return bad != 0;
}
