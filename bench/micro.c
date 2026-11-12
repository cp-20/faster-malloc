/*
 * micro -- allocator microbenchmarks.
 *
 * Each scenario is a separate subcommand so a runner can drive them with
 * different LD_PRELOAD'd allocators.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline uint64_t xrand(void)
{
    static __thread uint64_t s = 0x243F6A8885A308D3ull;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

static size_t g_warmup = 200000;
static volatile uint64_t g_sink;
static uint64_t g_cross_ops;

static size_t rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char line[256];
    size_t rss = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            sscanf(line + 6, "%zu", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

/* ---- scenarios ---------------------------------------------------- */

static int sc_churn(size_t size, uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        void *p = malloc(size);
        if (!p)
            return 1;
        g_sink += (uintptr_t)p;
        free(p);
    }
    return 0;
}

static int sc_batch(size_t size, uint64_t n, uint64_t rounds)
{
    void **v = malloc(n * sizeof(void *));
    if (!v)
        return 1;
    for (uint64_t r = 0; r < rounds; r++) {
        for (uint64_t i = 0; i < n; i++) {
            v[i] = malloc(size);
            if (!v[i])
                return 1;
        }
        for (uint64_t i = 0; i < n; i++)
            free(v[i]);
    }
    free(v);
    return 0;
}

/* Allocate a random size, keep it alive, then free in random order.
 * This is the common "build an object graph" pattern. */
static int sc_rand(size_t lo, size_t hi, uint64_t n, uint64_t rounds)
{
    void **v = malloc(n * sizeof(void *));
    size_t *sz = malloc(n * sizeof(size_t));
    if (!v || !sz)
        return 1;
    for (uint64_t r = 0; r < rounds; r++) {
        for (uint64_t i = 0; i < n; i++) {
            size_t s = lo + (xrand() % (hi - lo + 1));
            sz[i] = s;
            v[i] = malloc(s);
            if (!v[i])
                return 1;
            memset(v[i], 0, s < 64 ? s : 64);
        }
        for (uint64_t i = n; i > 1; i--) {
            uint64_t j = xrand() % i;
            void *t = v[i - 1];
            v[i - 1] = v[j];
            v[j] = t;
        }
        for (uint64_t i = 0; i < n; i++)
            free(v[i]);
    }
    free(v);
    free(sz);
    return 0;
}

/* Pareto-ish size distribution, the classic allocator stress test. */
static int sc_pareto(uint64_t n, uint64_t rounds)
{
    void **v = malloc(n * sizeof(void *));
    if (!v)
        return 1;
    for (uint64_t r = 0; r < rounds; r++) {
        for (uint64_t i = 0; i < n; i++) {
            uint64_t s = 16;
            while (s < 1024 * 1024 && (xrand() & 7) != 0)
                s <<= 1;
            v[i] = malloc(s);
            if (!v[i])
                return 1;
        }
        for (uint64_t i = 0; i < n; i++)
            free(v[i]);
    }
    free(v);
    return 0;
}

/* Bump-and-free: allocate a lot, hold, release.  Measures how well an
 * allocator returns memory and how fast the steady-state alloc path is. */
static int sc_hold(size_t size, uint64_t n, uint64_t rounds)
{
    void **v = malloc(n * sizeof(void *));
    if (!v)
        return 1;
    for (uint64_t r = 0; r < rounds; r++) {
        for (uint64_t i = 0; i < n; i++) {
            v[i] = malloc(size);
            if (!v[i])
                return 1;
            memset(v[i], 1, size < 256 ? size : 256);
        }
        for (uint64_t i = 0; i < n; i++)
            free(v[i]);
    }
    free(v);
    return 0;
}

typedef struct {
    size_t size;
    uint64_t iters;
    uint64_t ops;
} ThreadArg;

static void *thread_churn(void *arg)
{
    ThreadArg *a = arg;
    for (uint64_t i = 0; i < a->iters; i++) {
        void *p = malloc(a->size);
        g_sink += (uintptr_t)p;
        free(p);
    }
    a->ops = a->iters;
    return NULL;
}

static int sc_threads(int nthreads, size_t size, uint64_t iters)
{
    pthread_t *th = malloc(nthreads * sizeof(pthread_t));
    ThreadArg *args = malloc(nthreads * sizeof(ThreadArg));
    for (int i = 0; i < nthreads; i++) {
        args[i].size = size;
        args[i].iters = iters;
        pthread_create(&th[i], NULL, thread_churn, &args[i]);
    }
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i], NULL);
    free(th);
    free(args);
    return 0;
}

/* Larson-style: one thread allocates, another frees (cross-thread frees). */
typedef struct {
    void **ring;
    size_t cap;
    volatile size_t head, tail;
    volatile int stop;
    size_t size;
    uint64_t freed;
} Cross;

static void *cross_producer(void *arg)
{
    Cross *c = arg;
    while (!c->stop) {
        void *p = malloc(c->size);
        if (!p)
            break;
        size_t h = c->head;
        c->ring[h % c->cap] = p;
        __atomic_store_n(&c->head, h + 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void *cross_consumer(void *arg)
{
    Cross *c = arg;
    uint64_t n = 0;
    while (1) {
        size_t t = __atomic_load_n(&c->tail, __ATOMIC_ACQUIRE);
        size_t h = __atomic_load_n(&c->head, __ATOMIC_ACQUIRE);
        if (t == h) {
            if (c->stop && t == h)
                break;
            continue;
        }
        void *p = c->ring[t % c->cap];
        __atomic_store_n(&c->tail, t + 1, __ATOMIC_RELEASE);
        free(p);
        n++;
    }
    c->freed = n;
    g_sink += n;
    return NULL;
}

static int sc_cross(size_t size, uint64_t cap, uint64_t rounds)
{
    Cross c;
    c.cap = cap;
    c.head = 0;
    c.tail = 0;
    c.stop = 0;
    c.size = size;
    c.freed = 0;
    c.ring = malloc(cap * sizeof(void *));
    pthread_t prod, cons;
    pthread_create(&prod, NULL, cross_producer, &c);
    pthread_create(&cons, NULL, cross_consumer, &c);
    pthread_join(prod, NULL);
    c.stop = 1;
    pthread_join(cons, NULL);
    free(c.ring);
    g_cross_ops = c.freed;
    return 0;
}

static int sc_large(size_t size, uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        void *p = malloc(size);
        if (!p)
            return 1;
        g_sink += (uintptr_t)p;
        free(p);
    }
    return 0;
}

static int sc_realloc(size_t size, uint64_t iters)
{
    void *p = malloc(size);
    for (uint64_t i = 0; i < iters; i++) {
        p = realloc(p, size + 16);
        if (!p)
            return 1;
        g_sink += (uintptr_t)p;
    }
    free(p);
    return 0;
}

static int sc_calloc(size_t n, size_t size, uint64_t rounds)
{
    for (uint64_t r = 0; r < rounds; r++) {
        void *p = calloc(n, size);
        if (!p)
            return 1;
        g_sink += (uintptr_t)p;
        free(p);
    }
    return 0;
}

/* Touch every page: measures the cost of first-touch faults on fresh memory. */
static int sc_touch(size_t size, uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        char *p = malloc(size);
        if (!p)
            return 1;
        memset(p, 1, size);
        g_sink += (uintptr_t)p;
        free(p);
    }
    return 0;
}

/* ---- driver ------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: micro <scenario> [args]\n"
                "  churn <size> <iters>\n"
                "  batch <size> <n> <rounds>\n"
                "  rand <lo> <hi> <n> <rounds>\n"
                "  pareto <n> <rounds>\n"
                "  hold <size> <n> <rounds>\n"
                "  threads <nthreads> <size> <iters>\n"
                "  cross <size> <cap> <rounds>\n"
                "  large <size> <iters>\n"
                "  realloc <size> <iters>\n"
                "  calloc <n> <size> <rounds>\n"
                "  touch <size> <iters>\n");
        return 2;
    }
    const char *env = getenv("MICRO_WARMUP");
    if (env)
        g_warmup = strtoull(env, NULL, 10);
    if (getenv("MICRO_NO_WARMUP"))
        g_warmup = 0;

    const char *sc = argv[1];
    uint64_t t0, t1;
    double ns_per_op = 0;
    uint64_t ops = 0;
    int rc = 0;

    /* warmup: lets a JIT-style allocator compile its specialization and
     * warms the page tables for every allocator equally */
    if (g_warmup) {
        if (!strcmp(sc, "churn")) {
            rc = sc_churn(strtoull(argv[2], NULL, 10), g_warmup);
        } else if (!strcmp(sc, "batch")) {
            rc = sc_batch(strtoull(argv[2], NULL, 10), 1024, g_warmup / 1024 + 1);
        } else if (!strcmp(sc, "rand")) {
            rc = sc_rand(strtoull(argv[2], NULL, 10), strtoull(argv[3], NULL, 10),
                         1024, g_warmup / 1024 + 1);
        } else if (!strcmp(sc, "pareto")) {
            rc = sc_pareto(1024, g_warmup / 1024 + 1);
        } else if (!strcmp(sc, "hold")) {
            rc = sc_hold(strtoull(argv[2], NULL, 10), 1024, g_warmup / 1024 + 1);
        } else if (!strcmp(sc, "threads")) {
            rc = sc_threads(4, strtoull(argv[3], NULL, 10), g_warmup / 4 + 1);
        } else if (!strcmp(sc, "cross")) {
            rc = sc_cross(strtoull(argv[2], NULL, 10), 4096, 1);
        } else if (!strcmp(sc, "large")) {
            rc = sc_large(strtoull(argv[2], NULL, 10), g_warmup);
        } else if (!strcmp(sc, "realloc")) {
            rc = sc_realloc(strtoull(argv[2], NULL, 10), g_warmup);
        } else if (!strcmp(sc, "calloc")) {
            rc = sc_calloc(strtoull(argv[2], NULL, 10), strtoull(argv[3], NULL, 10),
                           g_warmup / 1024 + 1);
        } else if (!strcmp(sc, "touch")) {
            rc = sc_touch(strtoull(argv[2], NULL, 10), g_warmup);
        }
    }

    t0 = now_ns();
    if (!strcmp(sc, "churn")) {
        uint64_t iters = strtoull(argv[3], NULL, 10);
        rc = sc_churn(strtoull(argv[2], NULL, 10), iters);
        ops = iters;
    } else if (!strcmp(sc, "batch")) {
        uint64_t n = strtoull(argv[3], NULL, 10);
        uint64_t rounds = strtoull(argv[4], NULL, 10);
        rc = sc_batch(strtoull(argv[2], NULL, 10), n, rounds);
        ops = n * rounds;
    } else if (!strcmp(sc, "rand")) {
        uint64_t n = strtoull(argv[4], NULL, 10);
        uint64_t rounds = strtoull(argv[5], NULL, 10);
        rc = sc_rand(strtoull(argv[2], NULL, 10), strtoull(argv[3], NULL, 10),
                     n, rounds);
        ops = n * rounds;
    } else if (!strcmp(sc, "pareto")) {
        uint64_t n = strtoull(argv[2], NULL, 10);
        uint64_t rounds = strtoull(argv[3], NULL, 10);
        rc = sc_pareto(n, rounds);
        ops = n * rounds;
    } else if (!strcmp(sc, "hold")) {
        uint64_t n = strtoull(argv[3], NULL, 10);
        uint64_t rounds = strtoull(argv[4], NULL, 10);
        rc = sc_hold(strtoull(argv[2], NULL, 10), n, rounds);
        ops = n * rounds;
    } else if (!strcmp(sc, "threads")) {
        int nt = atoi(argv[2]);
        uint64_t iters = strtoull(argv[4], NULL, 10);
        rc = sc_threads(nt, strtoull(argv[3], NULL, 10), iters);
        ops = (uint64_t)nt * iters;
    } else if (!strcmp(sc, "cross")) {
        uint64_t rounds = strtoull(argv[4], NULL, 10);
        rc = sc_cross(strtoull(argv[2], NULL, 10), strtoull(argv[3], NULL, 10), rounds);
        ops = 0;
    } else if (!strcmp(sc, "large")) {
        uint64_t iters = strtoull(argv[3], NULL, 10);
        rc = sc_large(strtoull(argv[2], NULL, 10), iters);
        ops = iters;
    } else if (!strcmp(sc, "realloc")) {
        uint64_t iters = strtoull(argv[3], NULL, 10);
        rc = sc_realloc(strtoull(argv[2], NULL, 10), iters);
        ops = iters;
    } else if (!strcmp(sc, "calloc")) {
        uint64_t rounds = strtoull(argv[4], NULL, 10);
        rc = sc_calloc(strtoull(argv[2], NULL, 10), strtoull(argv[3], NULL, 10), rounds);
        ops = rounds;
    } else if (!strcmp(sc, "touch")) {
        uint64_t iters = strtoull(argv[3], NULL, 10);
        rc = sc_touch(strtoull(argv[2], NULL, 10), iters);
        ops = iters;
    } else {
        fprintf(stderr, "unknown scenario %s\n", sc);
        return 2;
    }
    t1 = now_ns();

    if (rc) {
        fprintf(stderr, "scenario failed\n");
        return 1;
    }

    if (!strcmp(sc, "cross")) {
        double secs = (double)(t1 - t0) / 1e9;
        printf("{\"scenario\":\"%s\",\"secs\":%.6f,\"ops\":%llu,\"ops_per_sec\":%.0f,\"rss_kb\":%zu}\n",
               sc, secs, (unsigned long long)g_cross_ops,
               secs > 0 ? g_cross_ops / secs : 0.0, rss_kb());
        return 0;
    }

    ns_per_op = (double)(t1 - t0) / (double)ops;
    printf("{\"scenario\":\"%s\",\"ops\":%llu,\"ns_per_op\":%.3f,\"rss_kb\":%zu}\n",
           sc, (unsigned long long)ops, ns_per_op, rss_kb());
    return 0;
}
