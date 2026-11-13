/* decayprobe: free a large mixed burst, then stay lightly active
 * (small alloc/free churn in an unrelated class, which drives the
 * release machinery's ticks) and report RSS over time. */
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

int main(void)
{
    static const size_t sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    const int n = 120000;
    void **ptrs = calloc((size_t)n, sizeof(void *));
    for (int i = 0; i < n; i++) {
        size_t z = sizes[i % 8] + (size_t)(i % 13);
        ptrs[i] = malloc(z);
        memset(ptrs[i], 1, z);
    }
    printf("live        rss=%ld\n", rss_kb());
    for (int i = 0; i < n; i++)
        free(ptrs[i]);
    printf("after free  rss=%ld\n", rss_kb());
    for (int t = 0; t < 10; t++) {
        void *tmp[6000];
        for (int i = 0; i < 6000; i++)
            tmp[i] = malloc(48);
        for (int i = 0; i < 6000; i++)
            free(tmp[i]);
        msleep(50);
        printf("t=%3d ms    rss=%ld\n", (t + 1) * 50, rss_kb());
    }
    free(ptrs);
    return 0;
}
