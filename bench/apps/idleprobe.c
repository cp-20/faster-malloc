/* idleprobe.c -- round 6, item 3 (ADR 0029's reopening trigger):
 * after a burst drains, the process goes COMPLETELY idle -- no
 * allocation activity at all -- and RSS is sampled for 6 s.
 * Census-driven release machinery (v2's, and glibc's alike) only
 * runs on allocator activity, so the question is how much a
 * silent process retains, per allocator.  Output uses write()
 * and a stack buffer; stdio is warmed up before the drain so the
 * idle window itself performs no allocation.
 *
 * Shape mirrors bursty: random-slot ramp to ~90 MB live, cohort
 * drain to ~10 MB live, then silence.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define NSLOTS 400000

static void **slots;
static uint32_t *sizes;
static uint64_t live_bytes;
static uint64_t rng_state = 0x243F6A8885A308D3ull;

static uint32_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 32) ^ (uint32_t)x;
}

static uint32_t pick_size(void)
{
    uint32_t r = rnd() % 100;
    if (r < 55)
        return 32 + rnd() % 224;
    if (r < 80)
        return 256 + rnd() % 1792;
    if (r < 93)
        return 2048 + rnd() % 6144;
    if (r < 99)
        return 8192 + rnd() % 24576;
    return 40000 + rnd() % 80000;
}

static void put_slot(size_t i)
{
    if (slots[i]) {
        live_bytes -= sizes[i];
        free(slots[i]);
    }
    uint32_t sz = pick_size();
    void *p = malloc(sz);
    if (!p) {
        slots[i] = NULL;
        sizes[i] = 0;
        return;
    }
    memset(p, 0xA5, sz < 64 ? sz : 64);
    slots[i] = p;
    sizes[i] = sz;
    live_bytes += sz;
}

static long rss_kb(void)
{
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    const char *p = strstr(buf, "VmRSS:");
    if (!p)
        return -1;
    return strtol(p + 6, NULL, 10);
}

static void report(const char *tag, long ms)
{
    char buf[128];
    int n = 0;
    const char *t = tag;
    while (*t)
        buf[n++] = *t++;
    buf[n++] = ' ';
    char num[24];
    int ni = 0;
    long v = ms;
    if (v == 0)
        num[ni++] = '0';
    while (v > 0) {
        num[ni++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (ni > 0)
        buf[n++] = num[--ni];
    const char mid[] = " ms rss=";
    for (size_t i = 0; mid[i]; i++)
        buf[n++] = mid[i];
    ni = 0;
    v = rss_kb();
    if (v == 0)
        num[ni++] = '0';
    while (v > 0) {
        num[ni++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (ni > 0)
        buf[n++] = num[--ni];
    const char tail[] = " live_kb=";
    for (size_t i = 0; tail[i]; i++)
        buf[n++] = tail[i];
    ni = 0;
    v = (long)(live_bytes / 1024);
    if (v == 0)
        num[ni++] = '0';
    while (v > 0) {
        num[ni++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (ni > 0)
        buf[n++] = num[--ni];
    buf[n++] = '\n';
    ssize_t w = write(1, buf, (size_t)n);
    (void)w;
}

int main(void)
{
    slots = calloc(NSLOTS, sizeof(void *));
    sizes = calloc(NSLOTS, sizeof(uint32_t));

    /* Ramp to ~90 MB live via random-slot replacement (bursty's
     * phase shape: spans end up era-mixed). */
    size_t nalloc = 0;
    while (live_bytes < 90ull << 20) {
        size_t i;
        if (nalloc < NSLOTS && (rnd() & 3)) {
            i = nalloc++;
        } else {
            i = rnd() % (nalloc ? nalloc : 1);
        }
        put_slot(i);
    }
    report("ramp", 0);

    /* Drain the oldest ~7/8 by allocation era: free in slot order
     * for slots filled early (sequential fill dominates the first
     * era), keeping the tail.  Era-exactness is not the point; a
     * scattered survivor set is. */
    for (size_t i = 0; i < nalloc; i++) {
        if ((i % 8) != 7 && slots[i]) {
            live_bytes -= sizes[i];
            free(slots[i]);
            slots[i] = NULL;
            sizes[i] = 0;
        }
    }
    report("drain", 0);

    /* Zero trickle: pure sleep, sampled. */
    for (long ms = 500; ms <= 6000; ms += 500) {
        struct timespec ts = { 0, 500 * 1000 * 1000L };
        nanosleep(&ts, NULL);
        report("idle", ms);
    }
    return 0;
}
