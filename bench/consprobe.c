/* consprobe -- the partially-free-span consolidation probe.
 *
 * Phase 1: allocate NSPANS spans' worth of one size class and free
 *          all but every KEEP_EVERY-th block, leaving a sparse set
 *          of survivors sprinkled across every span.
 * Phase 2: churn a rotating FIFO window of live blocks in bursts
 *          (allocate STEP fresh blocks, then free the STEP oldest)
 *          for a while -- realistic steady-state pressure that,
 *          under span-blind recycling, sprays live blocks into
 *          every span and pins them all.  Bursts are larger than
 *          the per-class local-list limit, so the blocks genuinely
 *          circulate through the central stack, where refill-side
 *          placement decisions are made.
 * Phase 3: free the phase-1 survivors and keep the window turning,
 *          so any span the window no longer touches becomes fully
 *          free and can be reaped by the release machinery.
 *
 * The metric is RSS at the end of phase 3: an allocator whose
 * placement concentrates the window into few spans lets the other
 * spans empty and returns them; a span-blind one keeps every span
 * mapped.  Live requested bytes are printed alongside so the
 * retained-free overhead is visible directly.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#define BLK 64              /* request size: class 64, 1023 blocks/span */
#define NSPANS 192
#define PER_SPAN 1023
#define KEEP_EVERY 16
#define WINDOW 49032          /* rotating live window: 48 spans worth */
#define STEP 512            /* burst size: 2x the class local limit */

static void **ring;         /* FIFO of live window blocks */
static size_t ring_cap, ring_head, ring_tail;

static void win_push(void *p)
{
    ring[ring_tail] = p;
    ring_tail = (ring_tail + 1) % ring_cap;
}

static void *win_pop_oldest(void)
{
    void *p = ring[ring_head];
    ring_head = (ring_head + 1) % ring_cap;
    return p;
}

static void churn(long ms)
{
    long t_end = now_ms() + ms;
    while (now_ms() < t_end) {
        for (int k = 0; k < STEP; k++) {
            void *p = malloc(BLK);
            ((char *)p)[0] = 1;
            win_push(p);
        }
        for (int k = 0; k < STEP; k++)
            free(win_pop_oldest());
        msleep(10);
    }
}

int main(void)
{
    const int n1 = NSPANS * PER_SPAN;
    void **p1 = calloc((size_t)n1, sizeof(void *));
    for (int i = 0; i < n1; i++) {
        p1[i] = malloc(BLK);
        ((char *)p1[i])[0] = 1;
    }
    printf("p1 live       rss=%ld live_req_kb=%ld\n", rss_kb(),
           (long)n1 * BLK / 1024);

    int nsurv = 0;
    for (int i = 0; i < n1; i++) {
        if (i % KEEP_EVERY == 0)
            p1[nsurv++] = p1[i];
        else
            free(p1[i]);
    }
    printf("p1 survivors  rss=%ld live_req_kb=%ld nsurv=%d\n", rss_kb(),
           (long)nsurv * BLK / 1024, nsurv);

    ring_cap = WINDOW + STEP;
    ring = calloc(ring_cap, sizeof(void *));
    for (int i = 0; i < WINDOW; i++) {
        void *p = malloc(BLK);
        ((char *)p)[0] = 1;
        win_push(p);
    }
    churn(1500);
    printf("p2 window     rss=%ld live_req_kb=%ld\n", rss_kb(),
           (long)(nsurv + WINDOW) * BLK / 1024);

    for (int i = 0; i < nsurv; i++)
        free(p1[i]);
    churn(700);
    printf("p3 +0.7s      rss=%ld live_req_kb=%ld\n", rss_kb(),
           (long)WINDOW * BLK / 1024);
    churn(1300);
    printf("p3 settled    rss=%ld live_req_kb=%ld\n", rss_kb(),
           (long)WINDOW * BLK / 1024);
    return 0;
}
