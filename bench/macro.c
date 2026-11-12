/*
 * macro -- realistic workload benchmarks for allocator comparison.
 *
 * Each scenario models a real program's allocation behaviour rather than a
 * single operation in a tight loop:
 *
 *   server    a multithreaded request server (larson-like): per-request
 *             objects with a header plus payload buffers, a mix of
 *             short-lived requests and long-lived sessions, and a share
 *             of frees performed by a different thread than the allocator.
 *   compiler  a compiler/IR pass: build a large tree of small nodes with
 *             interned strings, walk it, then tear it down in bulk.
 *             Repeated for many "translation units".
 *   jsondoc   document processing: parse-and-build many small object
 *             trees (objects, arrays, strings, numbers), serialise each
 *             into a realloc-grown buffer, retain every Nth document in
 *             a cache with LRU-ish eviction, free the rest.
 *   kvstore   a cache/KV store: chained hash table of malloc'd keys and
 *             values, zipf-ish key popularity, insert/lookup/delete churn,
 *             value updates through realloc, periodic table growth.
 *   game      a game loop: per-frame allocations (events, particles,
 *             transient entities) freed in bulk at frame end, plus a
 *             persistent entity pool with spawn/despawn churn.
 *   locality  reuse locality, measured directly instead of inferred
 *             from timing: a working set is freed completely, in a
 *             scrambled (strided) order like object-graph teardown,
 *             then a burst one eighth its size is reallocated.  The
 *             metric is the number of distinct 4 KiB pages the burst's
 *             blocks land on -- the footprint a CPU's caches and TLB
 *             must hold for that burst.  An allocator that groups
 *             freed blocks by span serves the burst from a few spans;
 *             one that recycles them in free order sprays it across
 *             the whole set.  Fewer pages = better shaping.
 *   trim      memory return: build a large mixed working set, free it
 *             all, then call malloc_trim(0) and report the resident
 *             set at each stage (live / after free / after trim), so
 *             an allocator's willingness to give memory back is a
 *             number, not an impression.
 *
 * Every scenario is deterministic (fixed per-thread xorshift seeds), runs
 * under any allocator via LD_PRELOAD, and prints one JSON line:
 *   {"scenario":..., "ops":..., "ns_per_op":..., "rss_kb":...}
 * where "ops" is the number of allocation operations performed (malloc,
 * calloc, realloc each count once), so ns_per_op is comparable with the
 * micro benchmarks while the *pattern* is a real workload's.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline uint64_t xr(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* lognormal-ish size: mostly small, occasionally large */
static size_t pick_size(uint64_t *s, size_t base, int doublings)
{
    size_t z = base;
    while (doublings-- > 0 && (xr(s) & 3) == 0)
        z <<= 1;
    return z + (xr(s) % base);
}

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

static size_t rss_now_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char line[256];
    size_t rss = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%zu", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

static _Atomic uint64_t g_ops;
static volatile uint64_t g_sink;

static void report(const char *sc, uint64_t t0, uint64_t t1)
{
    uint64_t ops = atomic_load(&g_ops);
    double ns = ops ? (double)(t1 - t0) / (double)ops : 0;
    printf("{\"scenario\":\"%s\",\"ops\":%llu,\"ns_per_op\":%.3f,\"secs\":%.3f,\"rss_kb\":%zu}\n",
           sc, (unsigned long long)ops, ns, (double)(t1 - t0) / 1e9, rss_kb());
    fflush(stdout);
}

/* ================================================================== */
/* server: multithreaded request handling with cross-thread frees      */
/* ================================================================== */

#define SRV_SLOTS 2048
#define SRV_XFER_CAP 8192

typedef struct {
    void *hdrs[SRV_SLOTS];
    void *pay[SRV_SLOTS][3];
    int npay[SRV_SLOTS];
    int session[SRV_SLOTS]; /* long-lived: freed only at the end */
} SrvPool;

/* Cross-thread free queue: Vyukov's bounded MPMC queue with per-slot
 * sequence numbers. (A naive claim-counter ring is NOT safe here: the
 * tail counter advances on claim, not on completion, so a slot can be
 * reused while an earlier consumer is still waiting on it, and two
 * consumers can load the same pointer -- that bug produced double frees
 * under glibc itself during development of this benchmark.) */
typedef struct {
    _Atomic uint64_t seq;
    void *data;
} XferCell;

static XferCell g_xfer[SRV_XFER_CAP];
static _Atomic uint64_t g_xfer_enq, g_xfer_deq;
static _Atomic int g_workers_done;
static int g_nthreads;

/* called once from the main thread before workers start */
static void xfer_init(void)
{
    for (int i = 0; i < SRV_XFER_CAP; i++)
        atomic_store_explicit(&g_xfer[i].seq, (uint64_t)i, memory_order_relaxed);
    atomic_store(&g_xfer_enq, 0);
    atomic_store(&g_xfer_deq, 0);
    atomic_store(&g_workers_done, 0);
}

static int xfer_push(void *p)
{
    uint64_t pos = atomic_load_explicit(&g_xfer_enq, memory_order_relaxed);
    for (;;) {
        XferCell *c = &g_xfer[pos & (SRV_XFER_CAP - 1)];
        uint64_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)pos;
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&g_xfer_enq, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
        } else if (diff < 0) {
            return 0; /* full */
        } else {
            pos = atomic_load_explicit(&g_xfer_enq, memory_order_relaxed);
        }
    }
    XferCell *c = &g_xfer[pos & (SRV_XFER_CAP - 1)];
    c->data = p;
    atomic_store_explicit(&c->seq, pos + 1, memory_order_release);
    return 1;
}

static void *xfer_pop(void)
{
    uint64_t pos = atomic_load_explicit(&g_xfer_deq, memory_order_relaxed);
    for (;;) {
        XferCell *c = &g_xfer[pos & (SRV_XFER_CAP - 1)];
        uint64_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)(pos + 1);
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&g_xfer_deq, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
        } else if (diff < 0) {
            return NULL; /* empty */
        } else {
            pos = atomic_load_explicit(&g_xfer_deq, memory_order_relaxed);
        }
    }
    XferCell *c = &g_xfer[pos & (SRV_XFER_CAP - 1)];
    void *p = c->data;
    atomic_store_explicit(&c->seq, pos + SRV_XFER_CAP, memory_order_release);
    return p;
}

static void xfer_drain_some(uint64_t *s)
{
    (void)s;
    for (int k = 0; k < 4; k++) {
        void *p = xfer_pop();
        if (!p)
            return;
        free(p); /* allocated by another thread: a cross-thread free */
    }
}

typedef struct {
    uint64_t seed;
    uint64_t iters;
    uint64_t end_ns; /* when this worker's main loop finished */
} SrvArg;

static void *srv_worker(void *argp)
{
    SrvArg *a = argp;
    uint64_t s = a->seed;
    SrvPool *pool = calloc(1, sizeof(SrvPool));
    if (!pool)
        return NULL;
    uint64_t local_ops = 0;
    for (uint64_t i = 0; i < a->iters; i++) {
        int slot = (int)(xr(&s) % SRV_SLOTS);
        /* retire the previous occupant of the slot */
        if (pool->hdrs[slot]) {
            for (int j = 0; j < pool->npay[slot]; j++) {
                void *q = pool->pay[slot][j];
                /* ~25% of payload frees are handed to another thread */
                if ((xr(&s) & 3) == 0 && xfer_push(q)) {
                    /* freed elsewhere */
                } else {
                    free(q);
                }
            }
            free(pool->hdrs[slot]);
            pool->hdrs[slot] = NULL;
        }
        xfer_drain_some(&s);
        /* build a new request: header + 1..3 payload buffers */
        size_t hsz = 32 + (xr(&s) % 96);
        void *h = malloc(hsz);
        if (!h)
            return NULL;
        memset(h, 1, hsz < 64 ? hsz : 64);
        int np = 1 + (int)(xr(&s) % 3);
        for (int j = 0; j < np; j++) {
            size_t psz = pick_size(&s, 64, 5); /* 64 .. ~6 KiB */
            pool->pay[slot][j] = malloc(psz);
            if (!pool->pay[slot][j])
                return NULL;
            memset(pool->pay[slot][j], 2, psz < 64 ? psz : 64);
            local_ops++;
        }
        pool->hdrs[slot] = h;
        pool->npay[slot] = np;
        pool->session[slot] = (xr(&s) % 20) == 0; /* 5% long-lived */
        local_ops += 2;                           /* header alloc + payload allocs approx */
        g_sink += (uintptr_t)h;
    }
    /* The timed region ends here.  Teardown (freeing the live pool and
     * spin-draining the transfer queue) is excluded: with more threads
     * than CPUs, early finishers would otherwise burn the shared CPU
     * spinning while the last workers are still being timed. */
    a->end_ns = now_ns();
    atomic_fetch_add(&g_ops, local_ops);
    /* teardown: free everything still live, drain the transfer queue */
    for (int slot = 0; slot < SRV_SLOTS; slot++) {
        if (pool->hdrs[slot]) {
            for (int j = 0; j < pool->npay[slot]; j++)
                free(pool->pay[slot][j]);
            free(pool->hdrs[slot]);
        }
    }
    /* drain the transfer queue until every worker is done and it is empty */
    atomic_fetch_add(&g_workers_done, 1);
    for (;;) {
        void *p = xfer_pop();
        if (p) {
            free(p);
            continue;
        }
        if (atomic_load(&g_workers_done) == g_nthreads) {
            p = xfer_pop(); /* one last sweep */
            if (!p)
                break;
            free(p);
        }
    }
    free(pool);
    return NULL;
}

static int sc_server(int nthreads, uint64_t iters_per_thread)
{
    pthread_t *th = malloc((size_t)nthreads * sizeof(pthread_t));
    SrvArg *args = malloc((size_t)nthreads * sizeof(SrvArg));
    g_nthreads = nthreads;
    xfer_init();
    uint64_t t0 = now_ns();
    for (int i = 0; i < nthreads; i++) {
        args[i].seed = 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1) + 12345;
        args[i].iters = iters_per_thread;
        pthread_create(&th[i], NULL, srv_worker, &args[i]);
    }
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i], NULL);
    uint64_t t1 = 0;
    for (int i = 0; i < nthreads; i++)
        if (args[i].end_ns > t1)
            t1 = args[i].end_ns; /* last worker's main-loop end */
    free(th);
    free(args);
    report("server", t0, t1);
    return 0;
}

/* ================================================================== */
/* compiler: build a tree of nodes + interned strings, walk, teardown  */
/* ================================================================== */

typedef struct Node {
    struct Node *child[3];
    char *name;      /* interned-style string */
    void *attr;      /* attribute block */
    uint64_t v;
} Node;

static uint64_t g_comp_ops;

static Node *comp_build(uint64_t *s, int depth)
{
    /* node structs come in a few realistic sizes (all >= sizeof(Node)) */
    static const size_t node_sizes[] = {48, 56, 64, 80, 96, 128, 160, 192};
    Node *n = malloc(node_sizes[xr(s) % 8]);
    if (!n)
        return NULL;
    g_comp_ops++;
    memset(n, 0, sizeof(*n));
    size_t nlen = 4 + (xr(s) % 28);
    n->name = malloc(nlen);
    g_comp_ops++;
    if (n->name)
        memset(n->name, 'a' + (int)(*s % 26), nlen);
    if ((xr(s) & 3) == 0) {
        size_t asz = 16 + (xr(s) % 240);
        n->attr = malloc(asz);
        g_comp_ops++;
        if (n->attr)
            memset(n->attr, 7, asz < 32 ? asz : 32);
    }
    n->v = xr(s);
    if (depth > 0) {
        int kids = 1 + (int)(xr(s) % 3); /* branching factor 1..3, mean 2 */
        for (int i = 0; i < kids; i++)
            n->child[i] = comp_build(s, depth - 1);
    }
    return n;
}

static uint64_t comp_walk(const Node *n)
{
    if (!n)
        return 0;
    uint64_t acc = n->v + (n->name ? (unsigned char)n->name[0] : 0);
    for (int i = 0; i < 3; i++)
        acc += comp_walk(n->child[i]);
    return acc;
}

static void comp_free(Node *n)
{
    if (!n)
        return;
    for (int i = 0; i < 3; i++)
        comp_free(n->child[i]);
    free(n->name);
    free(n->attr);
    free(n);
}

static int sc_compiler(int units)
{
    uint64_t s = 0xC0FFEE1234567ull;
    uint64_t t0 = now_ns();
    for (int u = 0; u < units; u++) {
        Node *root = comp_build(&s, 11); /* ~4k nodes per unit on average */
        g_sink += comp_walk(root);
        comp_free(root);
    }
    uint64_t t1 = now_ns();
    atomic_store(&g_ops, g_comp_ops);
    report("compiler", t0, t1);
    return 0;
}

/* ================================================================== */
/* jsondoc: build object trees, serialise with realloc, cache some     */
/* ================================================================== */

typedef struct JVal JVal;
struct JVal {
    int type; /* 0 null, 1 num, 2 str, 3 arr, 4 obj */
    double num;
    char *str;      /* string value or object key */
    JVal **items;   /* array elements or object values */
    char **keys;    /* object keys */
    size_t len, cap;
};

static uint64_t g_json_ops;

static void *jmalloc(size_t n)
{
    g_json_ops++;
    return malloc(n);
}
static void *jrealloc(void *p, size_t n)
{
    g_json_ops++;
    return realloc(p, n);
}

static JVal *json_build(uint64_t *s, int depth)
{
    JVal *v = jmalloc(sizeof(JVal));
    if (!v)
        return NULL;
    memset(v, 0, sizeof(*v));
    uint64_t r = xr(s) % 100;
    if (depth <= 0 || r < 30) {
        v->type = (r & 1) ? 1 : 2;
        if (v->type == 2) {
            size_t n = 3 + (xr(s) % 45);
            v->str = jmalloc(n + 1);
            if (v->str) {
                memset(v->str, 'x', n);
                v->str[n] = 0;
            }
        } else {
            v->num = (double)(xr(s) % 100000) / 7.0;
        }
        return v;
    }
    v->type = (r & 1) ? 4 : 3;
    size_t n = 1 + (xr(s) % 6);
    for (size_t i = 0; i < n; i++) {
        if (v->len == v->cap) {
            v->cap = v->cap ? v->cap * 2 : 4;
            v->items = jrealloc(v->items, v->cap * sizeof(JVal *));
            if (v->type == 4)
                v->keys = jrealloc(v->keys, v->cap * sizeof(char *));
        }
        if (v->type == 4) {
            size_t kn = 3 + (xr(s) % 20);
            char *k = jmalloc(kn + 1);
            if (k) {
                memset(k, 'k', kn);
                k[kn] = 0;
            }
            v->keys[v->len] = k;
        }
        v->items[v->len++] = json_build(s, depth - 1);
    }
    return v;
}

static void json_serialise(const JVal *v, char **buf, size_t *len, size_t *cap)
{
    /* append a rough textual form; grows the buffer with realloc */
    char tmp[64];
    int tl;
    switch (v->type) {
    case 1: tl = snprintf(tmp, sizeof tmp, "%.1f,", v->num); break;
    case 2: tl = snprintf(tmp, sizeof tmp, "\"%s\",", v->str ? v->str : ""); break;
    case 3: tl = snprintf(tmp, sizeof tmp, "["); break;
    case 4: tl = snprintf(tmp, sizeof tmp, "{"); break;
    default: tl = snprintf(tmp, sizeof tmp, "null,"); break;
    }
    if (*len + (size_t)tl + 2 > *cap) {
        *cap = (*cap + (size_t)tl + 64) * 2;
        *buf = jrealloc(*buf, *cap);
    }
    memcpy(*buf + *len, tmp, (size_t)tl);
    *len += (size_t)tl;
    for (size_t i = 0; i < v->len; i++) {
        if (v->type == 4 && v->keys[i]) {
            size_t kn = strlen(v->keys[i]);
            if (*len + kn + 4 > *cap) {
                *cap = (*cap + kn + 64) * 2;
                *buf = jrealloc(*buf, *cap);
            }
            memcpy(*buf + *len, v->keys[i], kn);
            *len += kn;
            (*buf)[(*len)++] = ':';
        }
        json_serialise(v->items[i], buf, len, cap);
    }
    if (v->type == 3 || v->type == 4) {
        if (*len + 2 > *cap) {
            *cap = (*cap + 64) * 2;
            *buf = jrealloc(*buf, *cap);
        }
        (*buf)[(*len)++] = (v->type == 3) ? ']' : '}';
        (*buf)[(*len)++] = ',';
    }
}

static void json_free(JVal *v)
{
    if (!v)
        return;
    for (size_t i = 0; i < v->len; i++) {
        json_free(v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

#define JSON_CACHE 64

static int sc_jsondoc(int docs)
{
    uint64_t s = 0x150A5EED5ull;
    JVal *cache[JSON_CACHE] = {0};
    char *cache_txt[JSON_CACHE] = {0};
    uint64_t t0 = now_ns();
    for (int d = 0; d < docs; d++) {
        JVal *doc = json_build(&s, 5);
        char *buf = NULL;
        size_t len = 0, cap = 0;
        json_serialise(doc, &buf, &len, &cap);
        if (buf)
            g_sink += (unsigned char)buf[len / 2];
        int slot = d % JSON_CACHE;
        if ((d & 15) == 0) { /* retain every 16th in the cache */
            json_free(cache[slot]);
            free(cache_txt[slot]);
            cache[slot] = doc;
            cache_txt[slot] = buf;
        } else {
            json_free(doc);
            free(buf);
        }
    }
    for (int i = 0; i < JSON_CACHE; i++) {
        json_free(cache[i]);
        free(cache_txt[i]);
    }
    uint64_t t1 = now_ns();
    atomic_store(&g_ops, g_json_ops);
    report("jsondoc", t0, t1);
    return 0;
}

/* ================================================================== */
/* kvstore: chained hash table, zipf-ish churn, realloc growth         */
/* ================================================================== */

typedef struct KEnt {
    struct KEnt *next;
    char *key;
    void *val;
    size_t vlen;
    uint64_t klen;
} KEnt;

#define KV_KEYS 30000

static uint64_t g_kv_ops;

static uint64_t kv_hash(const char *k, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)k[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int sc_kvstore(uint64_t nops)
{
    uint64_t s = 0x4B15EEDABCull;
    size_t nbuckets = 1024;
    KEnt **tab = calloc(nbuckets, sizeof(KEnt *));
    g_kv_ops = 1;
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < nops; i++) {
        /* zipf-ish: square a uniform to bias towards low key ids */
        uint64_t u = xr(&s) % KV_KEYS;
        uint64_t kid = (u * u) / KV_KEYS;
        char kbuf[64];
        int klen = snprintf(kbuf, sizeof kbuf, "key-%llu-%c%c",
                            (unsigned long long)kid,
                            'a' + (int)(kid % 26), 'a' + (int)((kid / 26) % 26));
        uint64_t h = kv_hash(kbuf, (size_t)klen);
        size_t b = (size_t)(h & (nbuckets - 1));
        KEnt *e = tab[b];
        while (e && !(e->klen == (uint64_t)klen && !memcmp(e->key, kbuf, (size_t)klen)))
            e = e->next;
        uint64_t act = xr(&s) % 100;
        if (!e && act < 55) { /* insert */
            e = malloc(sizeof(KEnt));
            g_kv_ops++;
            e->key = malloc((size_t)klen + 1);
            g_kv_ops++;
            memcpy(e->key, kbuf, (size_t)klen + 1);
            e->klen = (uint64_t)klen;
            e->vlen = pick_size(&s, 24, 5);
            e->val = malloc(e->vlen);
            g_kv_ops++;
            memset(e->val, 3, e->vlen < 64 ? e->vlen : 64);
            e->next = tab[b];
            tab[b] = e;
        } else if (e && act < 75) { /* update value via realloc */
            size_t nv = pick_size(&s, 24, 6);
            e->val = realloc(e->val, nv);
            g_kv_ops++;
            e->vlen = nv;
            memset(e->val, 4, nv < 64 ? nv : 64);
        } else if (e && act < 85) { /* delete */
            KEnt **pp = &tab[b];
            while (*pp != e)
                pp = &(*pp)->next;
            *pp = e->next;
            free(e->key);
            free(e->val);
            free(e);
        } else if (e) { /* lookup hit: touch */
            g_sink += ((unsigned char *)e->val)[0];
        }
        /* grow the table when the load factor passes 2 */
        if ((i & 0xFFFF) == 0 && nbuckets < (1u << 17)) {
            /* cheap population estimate: buckets in use sampled */
            size_t used = 0;
            for (size_t k = 0; k < nbuckets; k += 64)
                if (tab[k])
                    used++;
            if (used > (nbuckets / 64) * 0 + 40 && (i > 0)) {
                size_t nb = nbuckets * 2;
                KEnt **nt = calloc(nb, sizeof(KEnt *));
                g_kv_ops++;
                for (size_t k = 0; k < nbuckets; k++) {
                    KEnt *c = tab[k];
                    while (c) {
                        KEnt *nx = c->next;
                        size_t bb = (size_t)(kv_hash(c->key, (size_t)c->klen) & (nb - 1));
                        c->next = nt[bb];
                        nt[bb] = c;
                        c = nx;
                    }
                }
                free(tab);
                tab = nt;
                nbuckets = nb;
            }
        }
    }
    for (size_t k = 0; k < nbuckets; k++) {
        KEnt *c = tab[k];
        while (c) {
            KEnt *nx = c->next;
            free(c->key);
            free(c->val);
            free(c);
            c = nx;
        }
    }
    free(tab);
    uint64_t t1 = now_ns();
    atomic_store(&g_ops, g_kv_ops);
    report("kvstore", t0, t1);
    return 0;
}

/* ================================================================== */
/* game: per-frame allocations with bulk teardown + persistent pool    */
/* ================================================================== */

typedef struct Ent {
    struct Ent *next;
    void *comp[2]; /* components: transform, physics state */
    int hp;
} Ent;

static int sc_game(int frames)
{
    uint64_t s = 0x6A4E5EEDull;
    uint64_t ops = 0;
    Ent *entities = NULL;
    int nent = 0;
    uint64_t t0 = now_ns();
    for (int f = 0; f < frames; f++) {
        /* frame-transient allocations: events, particles, messages */
        int ntrans = 400 + (int)(xr(&s) % 400);
        void **trans = malloc((size_t)ntrans * sizeof(void *));
        ops++;
        for (int i = 0; i < ntrans; i++) {
            size_t z = pick_size(&s, 32, 4); /* 32 .. ~1 KiB */
            trans[i] = malloc(z);
            ops++;
            if (trans[i])
                memset(trans[i], f & 0xff, z < 64 ? z : 64);
        }
        /* persistent entity churn: spawn a few, despawn a few */
        int spawn = (int)(xr(&s) % 12);
        for (int i = 0; i < spawn; i++) {
            Ent *e = malloc(sizeof(Ent));
            ops++;
            e->comp[0] = malloc(48);
            e->comp[1] = malloc(80);
            ops += 2;
            e->hp = 100;
            e->next = entities;
            entities = e;
            nent++;
        }
        int despawn = (int)(xr(&s) % 12);
        for (int i = 0; i < despawn && entities; i++) {
            Ent *e = entities;
            entities = e->next;
            free(e->comp[0]);
            free(e->comp[1]);
            free(e);
            nent--;
        }
        /* walk the persistent pool (the "update" pass) */
        for (Ent *e = entities; e; e = e->next) {
            e->hp -= 1;
            g_sink += ((unsigned char *)e->comp[0])[0];
        }
        /* end of frame: bulk-free the transients */
        for (int i = 0; i < ntrans; i++)
            free(trans[i]);
        free(trans);
        (void)nent;
    }
    while (entities) {
        Ent *e = entities;
        entities = e->next;
        free(e->comp[0]);
        free(e->comp[1]);
        free(e);
    }
    uint64_t t1 = now_ns();
    atomic_store(&g_ops, ops);
    report("game", t0, t1);
    return 0;
}

/* ================================================================== */
/* locality: page concentration of a reallocation burst                */
/* ================================================================== */

#define LOC_N 32768 /* blocks in the working set (64 B each) */
#define LOC_SZ 64

static int cmp_uintptr(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return (x > y) - (x < y);
}

static int sc_locality(int bursts)
{
    const int M = LOC_N / 8; /* burst size: 4096 blocks = 256 KiB */
    void **ws = calloc(LOC_N, sizeof(void *));
    uintptr_t *addr = malloc((size_t)M * sizeof(uintptr_t));
    if (!ws || !addr)
        return 1;
    /* Build the working set: LOC_N blocks, all touched. */
    for (int i = 0; i < LOC_N; i++) {
        ws[i] = malloc(LOC_SZ);
        if (!ws[i])
            return 1;
        memset(ws[i], 1, LOC_SZ);
    }
    uint64_t timed_ns = 0;
    uint64_t pages_total = 0;
    for (int b = 0; b < bursts; b++) {
        uint64_t t0 = now_ns();
        /* Free the whole set in a scrambled (strided) order, the way
         * object-graph teardown scrambles address order.  7919 is odd,
         * so k*7919 mod 2^15 is a permutation. */
        for (int k = 0; k < LOC_N; k++) {
            int idx = (int)(((uint64_t)k * 7919) % LOC_N);
            free(ws[idx]);
            ws[idx] = NULL;
        }
        /* The burst: reallocate an eighth of the set, touch it all. */
        for (int k = 0; k < M; k++) {
            void *p = malloc(LOC_SZ);
            if (!p)
                return 1;
            memset(p, 2, LOC_SZ);
            ws[k] = p;
            addr[k] = (uintptr_t)p;
        }
        uint64_t t1 = now_ns();
        timed_ns += t1 - t0;
        /* Metric, untimed: distinct 4 KiB pages the burst landed on.
         * The burst is 256 KiB of blocks, so 64 pages is the floor. */
        qsort(addr, (size_t)M, sizeof(uintptr_t), cmp_uintptr);
        uint64_t pages = 0;
        uintptr_t prev = 0;
        for (int k = 0; k < M; k++) {
            uintptr_t pg = addr[k] >> 12;
            if (pg != prev) {
                pages++;
                prev = pg;
            }
        }
        pages_total += pages;
        /* Top the working set back up (untimed, pages not counted). */
        for (int k = M; k < LOC_N; k++) {
            ws[k] = malloc(LOC_SZ);
            if (!ws[k])
                return 1;
            memset(ws[k], 1, LOC_SZ);
        }
    }
    uint64_t ops = (uint64_t)bursts * (LOC_N + M);
    printf("{\"scenario\":\"locality\",\"ops\":%llu,\"ns_per_op\":%.3f,"
           "\"secs\":%.3f,\"rss_kb\":%zu,\"pages_per_burst\":%.1f}\n",
           (unsigned long long)ops, (double)timed_ns / (double)ops,
           (double)timed_ns / 1e9, rss_kb(),
           bursts ? (double)pages_total / bursts : 0.0);
    fflush(stdout);
    for (int i = 0; i < LOC_N; i++)
        free(ws[i]);
    free(ws);
    free(addr);
    return 0;
}

/* ================================================================== */
/* trim: build a working set, free it, malloc_trim, watch RSS          */
/* ================================================================== */

#include <malloc.h>

static int sc_trim(int cycles)
{
    static const size_t sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    const int nsmall = 120000, nlarge = 200;
    void **ptrs = calloc((size_t)nsmall + nlarge, sizeof(void *));
    if (!ptrs)
        return 1;
    size_t rss_live = 0, rss_freed = 0, rss_trim = 0;
    for (int c = 0; c < cycles; c++) {
        for (int i = 0; i < nsmall; i++) {
            size_t z = sizes[i % 8] + (size_t)(i % 13);
            ptrs[i] = malloc(z);
            if (!ptrs[i])
                return 1;
            memset(ptrs[i], 1, z);
        }
        for (int i = 0; i < nlarge; i++) {
            ptrs[nsmall + i] = malloc(128 * 1024);
            if (!ptrs[nsmall + i])
                return 1;
            memset(ptrs[nsmall + i], 2, 128 * 1024);
        }
        if (c == cycles - 1)
            rss_live = rss_now_kb();
        for (int i = 0; i < nsmall + nlarge; i++) {
            free(ptrs[i]);
            ptrs[i] = NULL;
        }
        if (c == cycles - 1) {
            rss_freed = rss_now_kb();
            malloc_trim(0);
            rss_trim = rss_now_kb();
        }
    }
    printf("{\"scenario\":\"trim\",\"ops\":%d,\"ns_per_op\":0,\"secs\":0,"
           "\"rss_kb\":%zu,\"rss_live_kb\":%zu,\"rss_after_free_kb\":%zu,"
           "\"rss_after_trim_kb\":%zu}\n",
           cycles * (nsmall + nlarge), rss_kb(), rss_live, rss_freed,
           rss_trim);
    fflush(stdout);
    free(ptrs);
    return 0;
}

/* ================================================================== */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: macro <scenario> [args]\n"
                "  server   <threads> <iters/thread>\n"
                "  compiler <translation-units>\n"
                "  jsondoc  <documents>\n"
                "  kvstore  <operations>\n"
                "  game     <frames>\n"
                "  locality <bursts>\n"
                "  trim     <cycles>\n");
        return 2;
    }
    const char *sc = argv[1];
    if (!strcmp(sc, "server")) {
        int nt = argc > 2 ? atoi(argv[2]) : 4;
        uint64_t it = argc > 3 ? strtoull(argv[3], NULL, 10) : 300000;
        return sc_server(nt, it);
    }
    if (!strcmp(sc, "compiler"))
        return sc_compiler(argc > 2 ? atoi(argv[2]) : 30);
    if (!strcmp(sc, "jsondoc"))
        return sc_jsondoc(argc > 2 ? atoi(argv[2]) : 3000);
    if (!strcmp(sc, "kvstore"))
        return sc_kvstore(argc > 2 ? strtoull(argv[2], NULL, 10) : 1000000);
    if (!strcmp(sc, "game"))
        return sc_game(argc > 2 ? atoi(argv[2]) : 1500);
    if (!strcmp(sc, "locality"))
        return sc_locality(argc > 2 ? atoi(argv[2]) : 10);
    if (!strcmp(sc, "trim"))
        return sc_trim(argc > 2 ? atoi(argv[2]) : 1);
    fprintf(stderr, "unknown scenario %s\n", sc);
    return 2;
}
