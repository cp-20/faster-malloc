#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

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
    /* Large realloc growth with a live neighbour: preserve both blocks
     * and touch every page, including after shrinking and regrowing. */
    {
        size_t sz = 40 * 1024;
        unsigned char *q = malloc(sz);
        if (!q) { printf("large-growth malloc null\n"); return 1; }
        size_t neighbour_size = 3 * 1024 * 1024;
        unsigned char *neighbour = malloc(neighbour_size);
        if (!neighbour) { printf("large-growth neighbour null\n"); free(q); return 1; }
        for (size_t off = 0; off < neighbour_size; off++)
            neighbour[off] = (unsigned char)(off * 29 + 17);
        for (size_t off = 4096; off < sz; off += 4096)
            q[off] = (unsigned char)(0x5a ^ (off / 4096));
        q[0] = 0x31;
        q[sz - 1] = 0x73;
        while (sz < 16 * 1024 * 1024) {
            size_t next = sz * 2;
            if (next > 16 * 1024 * 1024)
                next = 16 * 1024 * 1024;
            unsigned char *grown = realloc(q, next);
            if (!grown) { printf("large-growth realloc null\n"); free(q); return 1; }
            q = grown;
            if (q[0] != 0x31 || q[sz - 1] != 0x73) {
                printf("large-growth markers lost at %zu\n", next);
                return 1;
            }
            for (size_t off = 4096; off < sz; off += 4096)
                if (q[off] != (unsigned char)(0x5a ^ (off / 4096))) {
                    printf("large-growth pattern lost at %zu\n", off);
                    return 1;
                }
            for (size_t off = 0; off < neighbour_size; off++)
                if (neighbour[off] != (unsigned char)(off * 29 + 17)) {
                    printf("large-growth neighbour corrupt at %zu after %zu\n", off, next);
                    return 1;
                }
            sz = next;
            for (size_t off = 4096; off < sz; off += 4096)
                q[off] = (unsigned char)(0x5a ^ (off / 4096));
            q[0] = 0x31;
            q[sz - 1] = 0x73;
        }
        for (size_t off = 0; off < 48 * 1024; off++)
            q[off] = (unsigned char)(off * 37 + 11);
        unsigned char *shrunk = realloc(q, 48 * 1024);
        if (!shrunk) { printf("large-shrink realloc null\n"); free(q); return 1; }
        q = shrunk;
        for (size_t off = 0; off < 48 * 1024; off++)
            if (q[off] != (unsigned char)(off * 37 + 11)) {
                printf("large-shrink data lost at %zu\n", off);
                return 1;
            }
        unsigned char *grown = realloc(q, 8 * 1024 * 1024);
        if (!grown) { printf("large-regrowth realloc null\n"); free(q); return 1; }
        q = grown;
        if (malloc_usable_size(q) < 8 * 1024 * 1024) {
            printf("large-regrowth usable size too small\n");
            return 1;
        }
        for (size_t off = 0; off < 48 * 1024; off++)
            if (q[off] != (unsigned char)(off * 37 + 11)) {
                printf("large-regrowth data lost at %zu\n", off);
                return 1;
            }
        for (size_t off = 48 * 1024; off < 8 * 1024 * 1024; off += 4096)
            ((volatile unsigned char *)q)[off] = 0xa5;
        ((volatile unsigned char *)q)[8 * 1024 * 1024 - 1] = 0x5a;
        free(q);
        for (size_t off = 0; off < neighbour_size; off++)
            if (neighbour[off] != (unsigned char)(off * 29 + 17)) {
                printf("large-regrowth neighbour corrupt at %zu\n", off);
                return 1;
            }
        free(neighbour);

        q = malloc(8 * 1024 * 1024);
        if (!q) { printf("large-growth second malloc null\n"); return 1; }
        memset(q, 0xa5, 8 * 1024 * 1024);
        for (size_t off = 0; off < 8 * 1024 * 1024; off++)
            if (((volatile unsigned char *)q)[off] != 0xa5) {
                printf("large-growth second allocation corrupt at %zu\n", off);
                return 1;
            }
        free(q);
        const size_t sizes[] = { 8, 64, 4096, 40 * 1024, 192 * 1024, 3 * 1024 * 1024 };
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            q = malloc(sizes[i]);
            if (!q) { printf("post-growth malloc null at %zu\n", sizes[i]); return 1; }
            memset(q, 0x3c, sizes[i]);
            for (size_t off = 0; off < sizes[i]; off++)
                if (((volatile unsigned char *)q)[off] != 0x3c) {
                    printf("post-growth allocation corrupt at %zu\n", off);
                    return 1;
                }
            free(q);
        }
    }
    /* A 128 KiB request searches both possible buckets of a freed
     * 200 KiB mapping; 40 KiB cannot reach either under this cache policy. */
    {
        malloc_trim(0);
        unsigned char *q = malloc(200 * 1024);
        if (!q) { printf("large-cache seed null\n"); return 1; }
        uintptr_t cached = (uintptr_t)q;
        free(q);
        q = malloc(128 * 1024);
        if (!q) { printf("large-cache reuse null\n"); return 1; }
        if ((uintptr_t)q != cached) {
            printf("large-cache mapping not reused\n");
            return 1;
        }
        for (size_t off = 0; off < 128 * 1024; off++)
            q[off] = (unsigned char)(off * 19 + 7);
        uintptr_t previous = (uintptr_t)q;
        unsigned char *grown = realloc(q, 150 * 1024);
        if (!grown) { printf("large-cache realloc null\n"); free(q); return 1; }
        q = grown;
        if ((uintptr_t)q != previous) {
            printf("large-cache realloc moved despite spare capacity\n");
            return 1;
        }
        for (size_t off = 0; off < 128 * 1024; off++)
            if (q[off] != (unsigned char)(off * 19 + 7)) {
                printf("large-cache realloc data lost at %zu\n", off);
                return 1;
            }
        memset(q + 128 * 1024, 0xa5, 22 * 1024);
        free(q);
    }
    /* large-block cache safety (ADR 0011): a dirty freed large block
     * must never leak stale contents through calloc, and a reused
     * block must be fully writable up to the requested size */
    for (int round = 0; round < 20; round++) {
        unsigned char *d = malloc(65536);
        if (!d) { printf("large malloc null\n"); return 1; }
        memset(d, 0xAA, 65536);
        free(d);
        unsigned char *z = calloc(1, 65536);
        if (!z) { printf("large calloc null\n"); return 1; }
        for (int i = 0; i < 65536; i++)
            if (z[i]) { printf("large calloc not zero\n"); return 1; }
        z = realloc(z, 200000);
        if (!z) { printf("large realloc null\n"); return 1; }
        memset(z, 0xBB, 200000); /* full write: catches undersized reuse */
        free(z);
        unsigned char *z2 = calloc(1, 200000);
        if (!z2) { printf("large calloc2 null\n"); return 1; }
        for (int i = 0; i < 200000; i++)
            if (z2[i]) { printf("large calloc2 not zero\n"); return 1; }
        free(z2);
    }
    /* span release and malloc_trim (ADRs 0014/0015): allocate enough
     * same-class blocks to fill several spans, free them all (which
     * completes the spans), trim, then allocate the same amount again
     * and verify every block is fully writable -- a span released
     * while any block was still live, or a census that dropped live
     * blocks, corrupts this. */
    {
        enum { M = 20000 }; /* 20000 x 64 B = 20 spans of blocks */
        static void *q[20000];
        for (int round = 0; round < 3; round++) {
            for (int i = 0; i < M; i++) {
                q[i] = malloc(64);
                if (!q[i]) { printf("trim-phase malloc null\n"); return 1; }
                memset(q[i], 0x40 + round, 64);
            }
            for (int i = 0; i < M; i++)
                if (((unsigned char *)q[i])[0] != 0x40 + round ||
                    ((unsigned char *)q[i])[63] != 0x40 + round) {
                    printf("trim-phase data lost\n");
                    return 1;
                }
            for (int i = 0; i < M; i++)
                free(q[i]);
            malloc_trim(0);
        }
        /* mixed classes + a live set held ACROSS the trim: the census
         * must not release spans that still have live blocks */
        void *live[500];
        for (int i = 0; i < 500; i++) {
            live[i] = malloc(48 + (size_t)(i % 5) * 16);
            if (!live[i]) { printf("live malloc null\n"); return 1; }
            memset(live[i], 0x5A, 48);
        }
        for (int i = 0; i < M; i++) {
            q[i] = malloc(64);
            if (!q[i]) { printf("trim-phase2 malloc null\n"); return 1; }
            memset(q[i], 0x77, 64);
        }
        for (int i = 0; i < M; i++)
            free(q[i]);
        malloc_trim(0);
        for (int i = 0; i < 500; i++) {
            if (((unsigned char *)live[i])[0] != 0x5A ||
                ((unsigned char *)live[i])[47] != 0x5A) {
                printf("live block corrupted by trim\n");
                return 1;
            }
            free(live[i]);
        }
    }
    printf("ok\n");
    return 0;
}
