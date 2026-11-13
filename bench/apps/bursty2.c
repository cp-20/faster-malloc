/* bursty.c -- a long-running server-shaped allocation pattern:
 * a key/value-ish store whose live set rises in bursts, idles,
 * drains, and idles again.  Exercises the allocator the way a real
 * daemon's heap breathes -- and in particular whether memory given
 * back at drain time actually leaves the process during the idle
 * that follows (ADR 0020's decay, in a realistic shape rather than
 * the decay probe's synthetic one).
 *
 *   phases (op budgets; wall time is whatever the allocator
 *   makes it, roughly 10 s in total):
 *     ramp1   live set grows to ~60 MB
 *     idle1   trickle churn only (sleeps between op batches)
 *     ramp2   live set grows to ~90 MB
 *     drain   whole allocation cohorts expire, live set -> ~10 MB
 *     idle2   continued replacement churn at the low plateau
 *
 * Block sizes are log-uniform in [32, 4096] with a long tail of
 * small blocks; slots are replaced in place (free + alloc), which
 * is the PUT pattern of a cache.  The process samples its own RSS
 * and prints phase-end RSS plus a content checksum, so the cell
 * harness can verify every allocator produced identical work.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

#define NSLOTS 400000

static void **slots;
static uint32_t *sizes;
static uint32_t *cohort; /* allocation era of the block in the slot */
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

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static size_t pick_size(void)
{
    /* log-uniform 32..4096, plus 25% flat small 32..256 */
    uint64_t r = rnd();
    if ((r & 3) == 0)
        return 32 + (size_t)((r >> 8) % 225);
    int e = 5 + (int)((r >> 8) % 8); /* 32..8192 exponent band */
    size_t base = (size_t)1 << e;
    size_t v = base + (size_t)((r >> 16) % base);
    return v > 4096 ? 4096 : v;
}

static uint64_t live_bytes;
static uint64_t ops;
static uint64_t checksum;

static void put_slot(size_t i)
{
    if (slots[i]) {
        checksum += (unsigned char)((char *)slots[i])[0];
        live_bytes -= sizes[i];
        free(slots[i]);
    }
    size_t sz = pick_size();
    slots[i] = malloc(sz);
    sizes[i] = (uint32_t)sz;
    cohort[i] = (uint32_t)(ops / 2048); /* allocation era */
    live_bytes += sz;
    memset(slots[i], (int)(i & 0x7f), sz < 64 ? sz : 64);
    ops++;
}

static void drop_slot(size_t i)
{
    if (slots[i]) {
        checksum += (unsigned char)((char *)slots[i])[0];
        live_bytes -= sizes[i];
        free(slots[i]);
        slots[i] = NULL;
        sizes[i] = 0;
        ops++;
    }
}

/* Drive the live set toward target_bytes with a FIXED op budget.
 * Phases are op-counted, not time-boxed: the RNG stream and the
 * slot sequence are then identical for every allocator, so the
 * live set at each phase end and the final checksum must match
 * exactly across allocators -- that equality is the cell's
 * correctness check.  (An earlier time-boxed version produced a
 * different op count per allocator and "failed" its own check.) */
static void phase(const char *name, uint64_t target_bytes, long op_budget,
                  int trickle)
{
    long done = 0;
    while (done < op_budget) {
        for (int k = 0; k < 2000 && done < op_budget; k++, done++) {
            size_t i = (size_t)(rnd() % NSLOTS);
            if (trickle) {
                if (slots[i] && (rnd() & 0x3f) == 0)
                    put_slot(i);
            } else if (live_bytes < target_bytes) {
                put_slot(i);
            } else if (live_bytes > target_bytes + (4ull << 20)) {
                /* Drain: most slots are empty, so scan forward to
                 * an occupied one (deterministic: the slot state
                 * evolves identically for every allocator). */
                int tries = 0;
                while (!slots[i] && tries++ < 256)
                    i = (i + 1) % NSLOTS;
                drop_slot(i);
            } else if ((rnd() & 7) == 0) {
                put_slot(i); /* steady churn at plateau */
            }
        }
        if (trickle)
            msleep(40);
    }
    printf("%-6s rss=%ld live_kb=%llu ops=%llu\n", name, rss_kb(),
           (unsigned long long)(live_bytes / 1024),
           (unsigned long long)ops);
    fflush(stdout);
}

/* Cohort drain: expire whole allocation eras, oldest first --
 * the way caches and tenant stores actually shrink (things
 * allocated together expire together).  Uniform-random drops
 * instead scatter survivors over every span and pin all of them,
 * for any span-based allocator; that floor is visible in the
 * plateau phases, not here.  A little fresh churn is mixed in,
 * as in real drains. */
static void phase_drain(const char *name, uint64_t target_bytes)
{
    uint32_t era = 0;
    while (live_bytes > target_bytes) {
        era++;
        for (size_t i = 0; i < NSLOTS; i++)
            if (slots[i] && cohort[i] <= era)
                drop_slot(i);
        for (int k = 0; k < 2000; k++) {
            size_t i = (size_t)(rnd() % NSLOTS);
            if (slots[i] && (rnd() & 0xf) == 0)
                put_slot(i);
        }
    }
    printf("%-6s rss=%ld live_kb=%llu ops=%llu\n", name, rss_kb(),
           (unsigned long long)(live_bytes / 1024),
           (unsigned long long)ops);
    fflush(stdout);
}

/* Post-drain churn at the low plateau: occupied slots are
 * replaced in place (free + alloc), which is the only way live
 * blocks ever migrate -- a survivor moves spans only when it is
 * freed and reallocated, and placement (ADR 0024) decides where
 * the reallocation lands.  Sleeps stretch the phase over seconds
 * so dead-span ageing (ADR 0020) has wall-clock time to act. */
static void phase_churn(const char *name, long iterations)
{
    for (long n = 0; n < iterations; n++) {
        size_t i = (size_t)(rnd() % NSLOTS);
        if (slots[i])
            put_slot(i);
        else if ((rnd() & 0x3f) == 0)
            put_slot(i);
        if (n % 2000 == 1999)
            msleep(20);
    }
    printf("%-6s rss=%ld live_kb=%llu ops=%llu\n", name, rss_kb(),
           (unsigned long long)(live_bytes / 1024),
           (unsigned long long)ops);
    fflush(stdout);
}

int main(void)
{
    slots = calloc(NSLOTS, sizeof(void *));
    sizes = calloc(NSLOTS, sizeof(uint32_t));
    cohort = calloc(NSLOTS, sizeof(uint32_t));
    printf("start  rss=%ld\n", rss_kb());
    phase("ramp1", 60ull << 20, 120000, 0);
    phase("idle1", 60ull << 20, 60000, 1);
    phase("ramp2", 90ull << 20, 120000, 0);
    phase_drain("drain", 10ull << 20);
    phase_churn("idle2", 600000);
    /* Diagnostic (bursty2 only): the exact page-purge ceiling over
     * the live set right now, then what an explicit trim leaves. */
    {
        void **lp = malloc(NSLOTS * sizeof(void *));
        size_t nl = 0;
        for (size_t i = 0; i < NSLOTS; i++)
            if (slots[i])
                lp[nl++] = slots[i];
        int cmp(const void *a, const void *b)
        {
            uintptr_t x = *(uintptr_t *)a, y = *(uintptr_t *)b;
            return (x > y) - (x < y);
        }
        qsort(lp, nl, sizeof(void *), cmp);
        uint64_t purgeable = 0, nspans = 0, npartial = 0;
        uintptr_t cur = 0;
        uint16_t live_pages = 0;
        int have = 0;
        for (size_t i = 0; i <= nl; i++) {
            uintptr_t p = i < nl ? (uintptr_t)lp[i] : 0;
            uintptr_t span = p & ~65535ull;
            if (i == nl || !have || span != cur) {
                if (have) {
                    nspans++;
                    int fp = 0;
                    for (int pg = 1; pg < 16; pg++)
                        if (!(live_pages & (uint16_t)(1u << pg)))
                            fp++;
                    if (fp)
                        npartial++;
                    purgeable += (uint64_t)fp * 4096;
                }
                if (i == nl)
                    break;
                cur = span;
                live_pages = 0;
                have = 1;
            }
            size_t us = malloc_usable_size(lp[i]);
            uint32_t f = (uint32_t)((p - span) / 4096);
            uint32_t l = (uint32_t)((p - span + us - 1) / 4096);
            for (uint32_t pg = f; pg <= l; pg++)
                live_pages |= (uint16_t)(1u << pg);
        }
        printf("ceiling purgeable_kb=%llu spans=%llu partial=%llu\n",
               (unsigned long long)(purgeable / 1024),
               (unsigned long long)nspans, (unsigned long long)npartial);
        free(lp);
        malloc_trim(0);
        printf("trim   rss=%ld live_kb=%llu\n", rss_kb(),
               (unsigned long long)(live_bytes / 1024));
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < NSLOTS; i++)
        if (slots[i])
            sum += (unsigned char)((char *)slots[i])[0] * (i + 1);
    printf("final  rss=%ld live_kb=%llu ops=%llu checksum=%llu\n",
           rss_kb(), (unsigned long long)(live_bytes / 1024),
           (unsigned long long)ops,
           (unsigned long long)(sum + checksum));
    return 0;
}
