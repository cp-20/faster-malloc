/* Linux regression: timer spawn-once, fork-child silence, signal ownership. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t signal_tid;
static _Atomic int stop;
static void handler(int sig) { (void)sig; signal_tid = syscall(SYS_gettid); }
static void pause_ms(long ms)
{
    struct timespec t = { ms / 1000, ms % 1000 * 1000000 };
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
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
static void burst(void)
{
    void *p[4096];
    for (int i = 0; i < 4096; i++) {
        p[i] = malloc(896);
        assert(p[i]);
        ((volatile unsigned char *)p[i])[0] = 42;
    }
    for (int i = 0; i < 4096; i++) free(p[i]);
}
static void *once(void *unused)
{
    (void)unused;
    sigset_t before, after;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &before));
    burst();
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &after));
    for (int sig = 1; sig < NSIG; sig++)
        assert(sigismember(&before, sig) == sigismember(&after, sig));
    return NULL;
}
static void *churn(void *unused)
{
    (void)unused;
    while (!atomic_load(&stop)) burst();
    return NULL;
}
int main(int argc, char **argv)
{
    int enabled = argc > 1 && atoi(argv[1]);
    alarm(40);
    struct sigaction sa = { .sa_handler = handler };
    sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGUSR1, &sa, NULL));
    sigset_t before, after;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &before));
    pthread_t starters[8];
    for (int i = 0; i < 8; i++)
        assert(!pthread_create(&starters[i], NULL, once, NULL));
    burst();
    for (int i = 0; i < 8; i++) assert(!pthread_join(starters[i], NULL));
    pause_ms(100);
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &after));
    for (int sig = 1; sig < NSIG; sig++)
        assert(sigismember(&before, sig) == sigismember(&after, sig));
    assert(threads() == 1 + enabled);
    for (int i = 0; i < 30; i++) {
        signal_tid = 0;
        assert(!kill(getpid(), SIGUSR1));
        pause_ms(2);
        assert(signal_tid == syscall(SYS_gettid));
    }
    pthread_t workers[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&workers[i], NULL, churn, NULL));
    for (int round = 0; round < 4; round++) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) {
            /* First child stays silent; others deliberately exercise the
             * allocator's supported Linux post-fork path, beyond POSIX. */
            assert(threads() == 1);
            if (round) burst();
            pause_ms(2200);
            assert(threads() == 1);
            _exit(0);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < 4; i++) assert(!pthread_join(workers[i], NULL));
    burst();
    pause_ms(100);
    assert(threads() == 1 + enabled);
    puts("idlefork ok (4 forks, no child timer, parent spawn-once, signals on caller)");
}
