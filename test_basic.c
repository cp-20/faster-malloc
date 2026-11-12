#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
    enum { N = 100000 };
    void *p[N];
    for (int i = 0; i < N; i++) {
        size_t sz = 1 + (i * 37 % 4096);
        p[i] = malloc(sz);
        if (!p[i]) { printf("oom at %d\n", i); return 1; }
        memset(p[i], i & 0xff, sz);
    }
    /* verify before freeing (free clobbers the first 8 bytes with the
     * free-list next pointer) */
    for (int i = 0; i < N; i++) {
        unsigned char *b = p[i];
        size_t sz = 1 + (i * 37 % 4096);
        for (size_t k = 0; k < sz; k++)
            if (b[k] != (unsigned char)(i & 0xff)) {
                printf("corrupt at %d byte %zu\n", i, k);
                return 1;
            }
    }
    for (int i = N - 1; i >= 0; i--)
        free(p[i]);

    /* large */
    for (size_t sz = 32769; sz <= 200000; sz += 1000) {
        char *q = malloc(sz);
        if (!q) { printf("oom large %zu\n", sz); return 1; }
        q[0] = 1; q[sz - 1] = 2;
        if (q[0] != 1 || q[sz - 1] != 2) { printf("large corrupt\n"); return 1; }
        free(q);
    }
    /* aligned */
    for (size_t a = 16; a <= 262144; a *= 2) {
        char *q;
        if (posix_memalign((void **)&q, a, 1000) != 0) { printf("align fail %zu\n", a); return 1; }
        if (((uintptr_t)q & (a - 1)) != 0) { printf("misaligned %zu\n", a); return 1; }
        memset(q, 0x5a, 1000);
        if (q[0] != 0x5a) { printf("align corrupt\n"); return 1; }
        free(q);
    }
    /* calloc/realloc */
    int *c = calloc(1000, sizeof(int));
    for (int i = 0; i < 1000; i++) if (c[i]) { printf("calloc not zero\n"); return 1; }
    c = realloc(c, 2000 * sizeof(int));
    for (int i = 0; i < 1000; i++) if (c[i]) { printf("realloc data lost\n"); return 1; }
    c = realloc(c, 10 * sizeof(int));
    free(c);
    /* realloc shrink within class */
    char *r = malloc(100);
    memset(r, 3, 100);
    r = realloc(r, 20);
    if (r[0] != 3) { printf("shrink data lost\n"); return 1; }
    free(r);
    /* realloc grow across classes */
    char *g = malloc(10);
    memset(g, 9, 10);
    g = realloc(g, 100000);
    if (g[0] != 9) { printf("grow data lost\n"); return 1; }
    free(g);
    printf("ok\n");
    return 0;
}