/* Repeated large realloc growth; touch each newly allocated page. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 200;
    if (rounds <= 0) {
        fprintf(stderr, "usage: %s [positive rounds]\n", argv[0]);
        return 1;
    }
    size_t ops = 0;
    double start = now();
    for (long r = 0; r < rounds; r++) {
        size_t old = 40 * 1024;
        unsigned char *p = malloc(old);
        if (!p) {
            perror("malloc");
            return 1;
        }
        ops++;
        for (size_t sz = 80 * 1024; sz <= 32 * 1024 * 1024; sz *= 2) {
            unsigned char *q = realloc(p, sz);
            if (!q) {
                perror("realloc");
                free(p);
                return 1;
            }
            p = q;
            ops++;
            for (size_t off = old; off < sz; off += 4096)
                ((volatile unsigned char *)p)[off] = (unsigned char)r;
            old = sz;
        }
        free(p);
        ops++;
    }
    printf("rgrow rounds=%ld ops=%zu secs=%.6f\n", rounds, ops, now() - start);
    return 0;
}
