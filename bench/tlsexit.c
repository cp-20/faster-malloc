/*
 * tlsexit.c -- thread-exit regression for allocator-owned TLS caches.
 *
 * Every worker frees all of its allocations before exiting.  Only allocator
 * caches can retain those blocks; sequential joins keep the live footprint
 * to one worker while repeated thread creation exposes stranded caches.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T = 24, ROUNDS = 3, SMALL_BLOCKS = 4000, LARGE_BLOCKS = 8 };

static void touch(void *p, size_t size)
{
    /* Volatile stores fault every page in, even when the payload is never
     * read before free.  Untouched mmap pages would hide large-cache leaks. */
    volatile unsigned char *bytes = p;
    for (size_t i = 0; i < size; i += 4096)
        bytes[i] = 0x5a;
    bytes[size - 1] = 0xa5;
}

static void *worker(void *arg)
{
    (void)arg;
    void *small[SMALL_BLOCKS] = {0};
    void *large[LARGE_BLOCKS] = {0};
    int failed = 1;

    for (int i = 0; i < SMALL_BLOCKS; i++) {
        size_t size = i < SMALL_BLOCKS / 2 ? 64 : 200;
        small[i] = malloc(size);
        if (!small[i])
            goto out;
        touch(small[i], size);
    }
    for (int i = 0; i < SMALL_BLOCKS; i++) {
        free(small[i]);
        small[i] = NULL;
    }
    for (int i = 0; i < LARGE_BLOCKS; i++) {
        large[i] = malloc(128 * 1024);
        if (!large[i])
            goto out;
        touch(large[i], 128 * 1024);
    }
    failed = 0;
out:
    /* No live user blocks survive the worker, including on allocation failure. */
    for (int i = 0; i < SMALL_BLOCKS; i++)
        free(small[i]);
    for (int i = 0; i < LARGE_BLOCKS; i++)
        free(large[i]);
    return failed ? (void *)1 : NULL;
}

static int read_memory(size_t *rss, size_t *peak)
{
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) {
        perror("/proc/self/status");
        return 1;
    }
    *rss = *peak = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        size_t kb;
        if (sscanf(line, "VmRSS: %zu kB", &kb) == 1)
            *rss = kb;
        if (sscanf(line, "VmHWM: %zu kB", &kb) == 1)
            *peak = kb;
    }
    int failed = ferror(fp);
    fclose(fp);
    if (failed || !*rss || !*peak) {
        fprintf(stderr, "could not read VmRSS/VmHWM\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    size_t baseline, rss, peak;
    if (read_memory(&baseline, &peak))
        return 1;

    for (int round = 0; round < ROUNDS; round++) {
        for (int i = 0; i < T; i++) {
            pthread_t thread;
            int err = pthread_create(&thread, NULL, worker, NULL);
            if (err) {
                fprintf(stderr, "pthread_create: %s\n", strerror(err));
                return 1;
            }
            void *result;
            err = pthread_join(thread, &result);
            if (err) {
                fprintf(stderr, "pthread_join: %s\n", strerror(err));
                return 1;
            }
            if (result) {
                fprintf(stderr, "worker allocation failed\n");
                return 1;
            }
        }
    }

    if (read_memory(&rss, &peak))
        return 1;
    printf("tlsexit rss_kb=%zu peak_kb=%zu\n", rss, peak);
    /* Allow stack/TLS bookkeeping and host noise, but not 72 workers'
     * stranded caches (each holds at least 1 MiB of touched large blocks). */
    if (rss > baseline + 32 * 1024) {
        fprintf(stderr, "tlsexit RSS exceeds baseline %zu kB + 32 MiB\n",
                baseline);
        return 1;
    }
    return 0;
}
