/* purgeprobe.c -- how much of a drained heap COULD page-granular
 * return recover, and how much does each allocator actually
 * recover via malloc_trim(0)?
 *
 * Allocates a bursty-shaped live set (~90 MB, log-uniform
 * 32..4096 B blocks, first 64 B of each block touched -- the
 * bursty touch pattern), drains it cohort-style to ~10 MB live,
 * then computes from the surviving blocks' own addresses the
 * exact set of wholly-free pages inside partially-free 64 KiB
 * spans (the theoretical page-purge ceiling), and finally calls
 * malloc_trim(0) and re-reads RSS.
 */
#define _GNU_SOURCE
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NSLOTS 400000
#define SPAN 65536ull

static void **slots;
static uint32_t *sizes;
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "VmRSS: %ld kB", &v) == 1)
            break;
    if (f)
        fclose(f);
    return v;
}

static size_t pick_size(void)
{
    uint64_t r = rnd();
    if ((r & 3) == 0)
        return 32 + (size_t)((r >> 8) % 225);
    int e = 5 + (int)((r >> 8) % 8);
    size_t base = (size_t)1 << e;
    size_t v = base + (size_t)((r >> 16) % base);
    return v > 4096 ? 4096 : v;
}

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t ceiling_kb(size_t nalloc, int verbose)
{
    void **live_ptrs = malloc(nalloc * sizeof(void *));
    size_t nl = 0;
    for (size_t i = 0; i < nalloc; i++)
        if (slots[i])
            live_ptrs[nl++] = slots[i];
    int cmp(const void *a, const void *b)
    {
        uintptr_t x = *(uintptr_t *)a, y = *(uintptr_t *)b;
        return (x > y) - (x < y);
    }
    qsort(live_ptrs, nl, sizeof(void *), cmp);
    uint64_t purgeable = 0, spans_touched = 0, spans_partial = 0;
    uintptr_t cur = 0;
    uint16_t live_pages = 0;
    int have = 0;
    for (size_t i = 0; i <= nl; i++) {
        uintptr_t p = i < nl ? (uintptr_t)live_ptrs[i] : 0;
        uintptr_t span = p & ~(SPAN - 1);
        if (i == nl || !have || span != cur) {
            if (have) {
                spans_touched++;
                int free_pages = 0;
                for (int pg = 1; pg < 16; pg++)
                    if (!(live_pages & (uint16_t)(1u << pg)))
                        free_pages++;
                if (free_pages)
                    spans_partial++;
                purgeable += (uint64_t)free_pages * 4096;
            }
            if (i == nl)
                break;
            cur = span;
            live_pages = 0;
            have = 1;
        }
        size_t us = malloc_usable_size(live_ptrs[i]);
        uint32_t fp = (uint32_t)((p - span) / 4096);
        uint32_t lp = (uint32_t)((p - span + us - 1) / 4096);
        for (uint32_t pg = fp; pg <= lp; pg++)
            live_pages |= (uint16_t)(1u << pg);
    }
    free(live_ptrs);
    if (verbose)
        printf("ceiling purgeable_kb=%llu spans=%llu partial_spans=%llu\n",
               (unsigned long long)(purgeable / 1024),
               (unsigned long long)spans_touched,
               (unsigned long long)spans_partial);
    return purgeable / 1024;
}

int main(int argc, char **argv)
{
    int churn = argc > 1 && !strcmp(argv[1], "churn");
    slots = calloc(NSLOTS, sizeof(void *));
    sizes = calloc(NSLOTS, sizeof(uint32_t));
    uint64_t live = 0;
    size_t nalloc = 0;
    /* ramp to ~90 MB live, slots filled in allocation order */
    for (size_t i = 0; i < NSLOTS && live < (90ull << 20); i++) {
        size_t sz = pick_size();
        slots[i] = malloc(sz);
        sizes[i] = (uint32_t)sz;
        memset(slots[i], 1, sz < 64 ? sz : 64);
        live += sz;
        nalloc++;
    }
    printf("ramp   rss=%ld live_kb=%llu nalloc=%zu\n", rss_kb(),
           (unsigned long long)(live / 1024), nalloc);

    /* cohort drain, oldest first, to ~10 MB (bursty's shape) */
    size_t upto = 0;
    while (live > (10ull << 20) && upto < nalloc) {
        free(slots[upto]);
        slots[upto] = NULL;
        live -= sizes[upto];
        upto++;
    }
    printf("drain  rss=%ld live_kb=%llu freed=%zu\n", rss_kb(),
           (unsigned long long)(live / 1024), upto);

    /* theoretical page-purge ceiling: per 64 KiB span holding at
     * least one live block, count pages 1..15 intersected by no
     * live block (usable-size ranges). */
    ceiling_kb(nalloc, 1);

    if (churn) {
        /* bursty idle-2 shape: replace occupied slots in place for
         * ~4 s of wall time (op-counted batches + sleeps), letting
         * the allocator's automatic machinery do whatever it does,
         * then measure again. */
        for (long n = 0; n < 600000; n++) {
            size_t i = (size_t)(rnd() % NSLOTS);
            if (slots[i]) {
                free(slots[i]);
                live -= sizes[i];
                size_t sz = pick_size();
                slots[i] = malloc(sz);
                sizes[i] = (uint32_t)sz;
                memset(slots[i], 1, sz < 64 ? sz : 64);
                live += sz;
                if (i >= nalloc)
                    nalloc = i + 1;
            } else if ((rnd() & 0x3f) == 0) {
                size_t sz = pick_size();
                slots[i] = malloc(sz);
                sizes[i] = (uint32_t)sz;
                memset(slots[i], 1, sz < 64 ? sz : 64);
                live += sz;
                if (i >= nalloc)
                    nalloc = i + 1;
            }
            if (n % 2000 == 1999)
                msleep(20);
        }
        printf("churn  rss=%ld live_kb=%llu\n", rss_kb(),
               (unsigned long long)(live / 1024));
        ceiling_kb(nalloc, 1);
    }

    malloc_trim(0);
    printf("trim   rss=%ld live_kb=%llu\n", rss_kb(),
           (unsigned long long)(live / 1024));
    return 0;
}
