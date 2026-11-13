/*
 * stress.c -- randomized correctness stress test for a malloc replacement.
 *
 * Keeps a shadow model of every live allocation (its size and a magic
 * pattern) and verifies on every free that the memory was not corrupted.
 * Exercises small/large/aligned allocations, realloc growth and shrinkage,
 * calloc zeroing, and interleaved multithreaded use.
 *
 * Slots are claimed under a lock *before* the allocation happens, so two
 * threads can never own the same slot.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <malloc.h>
#include <assert.h>

#define N_SLOTS 4096

static _Thread_local uint64_t rng_state = 0x243F6A8885A308D3ull;

static inline uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

typedef struct {
    void *ptr;
    size_t size;
    unsigned char tag;
    int reserved;
} Slot;

static Slot slots[N_SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int failures;
static pthread_mutex_t fail_lock = PTHREAD_MUTEX_INITIALIZER;

static void fail(const char *fmt, ...)
{
    (void)fmt;
    pthread_mutex_lock(&fail_lock);
    failures++;
    pthread_mutex_unlock(&fail_lock);
}

static void check_slot(Slot *s, const char *what)
{
    unsigned char *p = s->ptr;
    for (size_t i = 0; i < s->size; i++) {
        if (p[i] != (unsigned char)(s->tag + (i & 0x3f))) {
            pthread_mutex_lock(&fail_lock);
            if (failures++ < 10)
                fprintf(stderr, "CORRUPTION (%s): size=%zu byte %zu got %02x want %02x\n",
                        what, s->size, i, p[i],
                        (unsigned char)(s->tag + (i & 0x3f)));
            pthread_mutex_unlock(&fail_lock);
            return;
        }
    }
}

static void fill(Slot *s)
{
    unsigned char *p = s->ptr;
    for (size_t i = 0; i < s->size; i++)
        p[i] = (unsigned char)(s->tag + (i & 0x3f));
}

static void *worker(void *arg)
{
    unsigned long seed = (unsigned long)(uintptr_t)arg;
    rng_state = seed * 2654435761u + 1;

    for (int iter = 0; iter < 20000; iter++) {
        /* --- pick a slot and free whatever is in it --- */
        int i = (int)(rng() % N_SLOTS);
        pthread_mutex_lock(&lock);
        Slot *s = &slots[i];
        if (s->reserved) {
            pthread_mutex_unlock(&lock);
            continue; /* another thread owns it right now */
        }
        s->reserved = 1;
        void *oldp = s->ptr;
        size_t oldsz = s->size;
        unsigned char oldtag = s->tag;
        s->ptr = NULL;
        s->size = 0;
        pthread_mutex_unlock(&lock);

        if (oldp) {
            unsigned char *op = oldp;
            for (size_t k = 0; k < oldsz; k++)
                if (op[k] != (unsigned char)(oldtag + (k & 0x3f))) {
                    pthread_mutex_lock(&fail_lock);
                    if (failures++ < 10)
                        fprintf(stderr, "CORRUPTION (free): size=%zu byte %zu\n",
                                oldsz, k);
                    pthread_mutex_unlock(&fail_lock);
                    break;
                }
            free(oldp);
        }

        /* --- allocate into the slot we reserved --- */
        size_t sz;
        switch (rng() % 6) {
        case 0: sz = 1 + rng() % 32; break;
        case 1: sz = 1 + rng() % 256; break;
        case 2: sz = 1 + rng() % 4096; break;
        case 3: sz = 1 + rng() % 40000; break;
        case 4: sz = 1 + rng() % 200000; break;
        default: sz = 1 + rng() % 16; break;
        }
        void *p = malloc(sz);
        if (!p) {
            pthread_mutex_lock(&lock);
            slots[i].reserved = 0;
            pthread_mutex_unlock(&lock);
            fail("malloc(%zu) returned NULL\n", sz);
            continue;
        }

        pthread_mutex_lock(&lock);
        s->ptr = p;
        s->size = sz;
        s->tag = (unsigned char)(rng() | 1);
        fill(s);
        pthread_mutex_unlock(&lock);

        /* --- occasionally realloc in place --- */
        if ((rng() & 15) == 0) {
            size_t ns = 1 + rng() % (sz + 8192);
            void *q = realloc(p, ns);
            if (!q) {
                fail("realloc returned NULL\n");
                pthread_mutex_lock(&lock);
                s->reserved = 0;
                pthread_mutex_unlock(&lock);
                continue;
            }
            pthread_mutex_lock(&lock);
            if (s->ptr == p) { /* still ours */
                unsigned char *np = q;
                for (size_t k = 0; k < sz && k < ns; k++)
                    if (np[k] != (unsigned char)(s->tag + (k & 0x3f))) {
                        pthread_mutex_lock(&fail_lock);
                        if (failures++ < 10)
                            fprintf(stderr, "REALLOC CORRUPTION at %zu\n", k);
                        pthread_mutex_unlock(&fail_lock);
                        break;
                    }
                s->ptr = q;
                s->size = ns;
                s->tag = (unsigned char)(rng() | 1);
                fill(s);
            }
            pthread_mutex_unlock(&lock);
        }

        /* --- occasionally test calloc and aligned allocation --- */
        if ((rng() & 63) == 0) {
            size_t cn = 1 + rng() % 100;
            size_t cs = 1 + rng() % 64;
            unsigned char *cp = calloc(cn, cs);
            if (!cp) {
                fail("calloc NULL\n");
            } else {
                for (size_t k = 0; k < cn * cs; k++)
                    if (cp[k]) {
                        fail("CALLOC NOT ZERO\n");
                        break;
                    }
                free(cp);
            }
        }
        if ((rng() & 127) == 0) {
            size_t al = 16 << (rng() % 10); /* 16 .. 8192 */
            /* C11 requires the size of an aligned allocation to be a
             * multiple of the alignment. */
            size_t asz = al * (1 + rng() % 8);
            void *ap = aligned_alloc(al, asz);
            if (!ap || ((uintptr_t)ap & (al - 1)) != 0) {
                fail("aligned_alloc misaligned/null\n");
            } else {
                memset(ap, 0xA5, asz);
                for (size_t k = 0; k < asz; k++)
                    if (((unsigned char *)ap)[k] != 0xA5) {
                        fail("ALIGNED CORRUPTION\n");
                        break;
                    }
                free(ap);
            }
        }
        pthread_mutex_lock(&lock);
        s->reserved = 0;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* Keep live islands and local free lists while other threads trim/realloc.
 * 896-byte blocks straddle page boundaries: check every live byte. */
static void *purge_worker(void *arg)
{
    unsigned char tag = (uintptr_t)arg + 31;
    void *p[2048];
    for (int round = 0; round < 24; round++) {
        for (int i = 0; i < 2048; i++) {
            p[i] = malloc(896);
            assert(p[i]);
            memset(p[i], tag, 896);
        }
        for (int i = 0; i < 2048; i++)
            if (i % 16) free(p[i]);
        malloc_trim(0);
        /* Refill the purged class while its live islands still pin spans. */
        for (int i = 0; i < 2048; i++) if (i % 16) {
            p[i] = malloc(896);
            assert(p[i]);
            memset(p[i], tag, 896);
        }
        for (int i = 0; i < 2048; i++) if (i % 16) {
            for (int j = 0; j < 896; j++)
                if (((unsigned char *)p[i])[j] != tag)
                    fail("purge refill corrupted\n");
            free(p[i]);
        }
        for (int i = 0; i < 2048; i += 16) {
            unsigned char *q = realloc(p[i], 1792);
            assert(q);
            for (int j = 0; j < 896; j++)
                if (q[j] != tag) fail("purge live data corrupted\n");
            free(q);
        }
        malloc_trim(0);
    }
    return NULL;
}

int main(void)
{
    int nthreads = 4;
    pthread_t th[8], purge[8];
    for (int i = 0; i < 8; i++)
        assert(!pthread_create(&purge[i], NULL, purge_worker, (void *)(uintptr_t)i));
    for (int i = 0; i < nthreads; i++)
        assert(!pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)(i + 1)));
    for (int i = 0; i < nthreads; i++)
        assert(!pthread_join(th[i], NULL));

    for (int i = 0; i < 8; i++)
        assert(!pthread_join(purge[i], NULL));

    /* single-threaded edge cases */
    void *zero = malloc(0);
    if (!zero)
        fail("malloc(0) returned NULL\n");
    free(zero);
    for (size_t sz = 1; sz <= 300000; sz = sz * 3 / 2 + 1) {
        void *q = malloc(sz);
        if (!q) {
            fail("edge malloc NULL\n");
            continue;
        }
        memset(q, 0x5A, sz);
        q = realloc(q, sz * 2);
        if (!q) {
            fail("edge realloc NULL\n");
            continue;
        }
        for (size_t k = 0; k < sz; k++)
            if (((unsigned char *)q)[k] != 0x5A) {
                fail("edge realloc corruption\n");
                break;
            }
        free(q);
    }

    for (int i = 0; i < N_SLOTS; i++)
        if (slots[i].ptr) {
            check_slot(&slots[i], "final");
            free(slots[i].ptr);
        }

    if (failures) {
        fprintf(stderr, "STRESS FAILED: %d failures\n", failures);
        return 1;
    }
    printf("stress ok\n");
    return 0;
}
