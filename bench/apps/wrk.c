/* wrk.c -- minimal keep-alive HTTP load generator with latency
 * percentiles, for the nginx app cell.  Not a wrk replacement; just
 * enough to drive a local server at a fixed connection count and
 * report requests/sec plus p50/p99 per-request latency.
 *
 *   wrk -t THREADS -c CONNS -d SECONDS HOST PORT PATH [PATH2]
 *
 * With two paths, requests alternate 4:1 between PATH and PATH2.
 * A request completes when the full Content-Length body has been
 * read.  Any short read, connect failure or framing error counts
 * as an error and is reported; the cell harness requires 0.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char *g_host = "127.0.0.1";
static int g_port = 18099;
static const char *g_path[2];
static volatile int g_stop;
static double g_deadline_s;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct {
    uint32_t *lat; /* microseconds, dynamic */
    size_t n, cap;
    uint64_t reqs, errs, bytes;
} Stat;

static void stat_push(Stat *s, uint32_t v)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 65536;
        s->lat = realloc(s->lat, s->cap * sizeof(uint32_t));
        if (!s->lat)
            exit(2);
    }
    s->lat[s->n++] = v;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* One connection: loop requests until the deadline. */
static void run_conn(Stat *st, int seed)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        st->errs++;
        return;
    }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        st->errs++;
        close(fd);
        return;
    }
    char req[1024];
    char buf[65536];
    unsigned ctr = (unsigned)seed;
    while (!g_stop && now_s() < g_deadline_s) {
        const char *path = g_path[(ctr % 5 == 4 && g_path[1]) ? 1 : 0];
        ctr++;
        int rl = snprintf(req, sizeof req,
                          "GET %s HTTP/1.1\r\nHost: %s\r\n"
                          "Connection: keep-alive\r\n\r\n",
                          path, g_host);
        double t0 = now_s();
        if (send(fd, req, (size_t)rl, MSG_NOSIGNAL) != rl) {
            st->errs++;
            break;
        }
        /* Read one response: headers, then Content-Length body. */
        size_t have = 0, need = 0;
        int got_headers = 0;
        long clen = -1;
        int bad = 0;
        while (!got_headers || have < need) {
            ssize_t r = recv(fd, buf + have, sizeof buf - have, 0);
            if (r <= 0) {
                bad = 1;
                break;
            }
            have += (size_t)r;
            st->bytes += (uint64_t)r;
            if (!got_headers) {
                char *e = strstr(buf, "\r\n\r\n");
                if (!e) {
                    if (have == sizeof buf)
                        bad = 1;
                    continue;
                }
                got_headers = 1;
                char *cl = strcasestr(buf, "Content-Length:");
                if (cl && cl < e)
                    clen = strtol(cl + 15, NULL, 10);
                if (clen < 0) {
                    bad = 1;
                    break;
                }
                size_t hdr = (size_t)(e + 4 - buf);
                need = hdr + (size_t)clen;
                if (need > sizeof buf) {
                    /* Response bigger than the buffer: drain the
                     * rest in place. */
                    size_t extra = need - have;
                    while (extra) {
                        ssize_t rr = recv(fd, buf,
                                          extra < sizeof buf ? extra
                                                             : sizeof buf,
                                          0);
                        if (rr <= 0) {
                            bad = 1;
                            break;
                        }
                        extra -= (size_t)rr;
                        st->bytes += (uint64_t)rr;
                    }
                    have = need;
                    break;
                }
            }
        }
        if (bad || have < need) {
            st->errs++;
            break;
        }
        stat_push(st, (uint32_t)((now_s() - t0) * 1e6));
        st->reqs++;
        /* Any pipelined remainder would corrupt framing; we never
         * pipeline, and nginx answers in order, so `have` may only
         * exceed `need` if the server misbehaves: treat as error. */
        if (have > need) {
            st->errs++;
            break;
        }
    }
    close(fd);
}

typedef struct {
    int conns, seed;
    Stat st;
} Worker;

static void *worker_main(void *arg)
{
    Worker *w = arg;
    for (int i = 0; i < w->conns; i++)
        run_conn(&w->st, w->seed + i);
    return NULL;
}

int main(int argc, char **argv)
{
    int threads = 2, conns = 20;
    double dur = 10.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc)
            threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc)
            conns = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dur = atof(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            g_port = atoi(argv[++i]);
        else if (argv[i][0] == '/') {
            if (!g_path[0])
                g_path[0] = argv[i];
            else
                g_path[1] = argv[i];
        }
    }
    if (!g_path[0])
        g_path[0] = "/";
    if (conns < threads)
        conns = threads;

    Worker *ws = calloc((size_t)threads, sizeof(Worker));
    pthread_t *th = calloc((size_t)threads, sizeof(pthread_t));
    g_deadline_s = now_s() + dur;
    double t0 = now_s();
    for (int i = 0; i < threads; i++) {
        ws[i].conns = conns / threads + (i < conns % threads ? 1 : 0);
        ws[i].seed = i * 1000;
        pthread_create(&th[i], NULL, worker_main, &ws[i]);
    }
    for (int i = 0; i < threads; i++)
        pthread_join(th[i], NULL);
    double el = now_s() - t0;

    uint64_t reqs = 0, errs = 0, bytes = 0;
    size_t total = 0;
    for (int i = 0; i < threads; i++) {
        reqs += ws[i].st.reqs;
        errs += ws[i].st.errs;
        bytes += ws[i].st.bytes;
        total += ws[i].st.n;
    }
    uint32_t *all = malloc((total ? total : 1) * sizeof(uint32_t));
    size_t off = 0;
    for (int i = 0; i < threads; i++) {
        memcpy(all + off, ws[i].st.lat, ws[i].st.n * sizeof(uint32_t));
        off += ws[i].st.n;
    }
    qsort(all, total, sizeof(uint32_t), cmp_u32);
    uint32_t p50 = total ? all[total / 2] : 0;
    uint32_t p99 = total ? all[(size_t)(total * 0.99)] : 0;
    uint32_t pmax = total ? all[total - 1] : 0;
    printf("{\"requests\":%llu,\"rps\":%.1f,\"p50_us\":%u,\"p99_us\":%u,"
           "\"pmax_us\":%u,\"errors\":%llu,\"bytes\":%llu}\n",
           (unsigned long long)reqs, reqs / el, p50, p99, pmax,
           (unsigned long long)errs, (unsigned long long)bytes);
    return errs ? 1 : 0;
}
