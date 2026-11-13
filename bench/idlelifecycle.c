/* Linux/glibc regression for the ELF NODELETE allocator contract. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static int threads(void)
{
    DIR *d = opendir("/proc/self/task");
    assert(d);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}
static pthread_barrier_t barrier;
static void *(*alloc_block)(size_t);
static void (*free_block)(void *);
static void *late_exit(void *unused)
{
    (void)unused;
    void *p[4096];
    for (int i = 0; i < 4096; i++) { p[i] = alloc_block(896); assert(p[i]); }
    for (int i = 0; i < 4096; i++) free_block(p[i]);
    pthread_barrier_wait(&barrier);
    pthread_barrier_wait(&barrier);
    return NULL; /* allocator's pthread-key callback runs after dlclose */
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    alarm(20);
    int enabled = atoi(argv[2]);
    assert(threads() == 1);
    assert(!pthread_barrier_init(&barrier, NULL, 2));
    for (int round = 0; round < 12; round++) {
        /* Keep allocations paired with their own allocator. */
        void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
        if (!h) { fprintf(stderr, "%s\n", dlerror()); abort(); }
        void *(*alloc)(size_t) = dlsym(h, "malloc");
        void (*release)(void *) = dlsym(h, "free");
        assert(alloc && release);
        void *p[4096];
        for (int i = 0; i < 4096; i++) { p[i] = alloc(896); assert(p[i]); }
        for (int i = 0; i < 4096; i++) release(p[i]);
        alloc_block = alloc;
        free_block = release;
        pthread_t worker;
        assert(!pthread_create(&worker, NULL, late_exit, NULL));
        pthread_barrier_wait(&barrier);
        assert(!dlclose(h));
        pthread_barrier_wait(&barrier);
        assert(!pthread_join(worker, NULL));
        /* No live handle remains: NODELETE itself must retain the DSO. */
        h = dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD);
        assert(h);
        assert(!dlclose(h));
        usleep(20000);
        assert(threads() == 1 + enabled);
    }
    sleep(3); /* worker executes after all handles have closed */
    assert(threads() == 1 + enabled);
    puts("idlelifecycle ok (retained after dlclose, bounded threads, normal exit)");
    return 0;
}
