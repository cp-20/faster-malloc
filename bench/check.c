/*
 * check.c -- standalone reproduction of the v2 free-list logic, with
 * invariant checking, so that list bugs can be found without a debugger
 * attached to the real allocator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define NC 8
#define LIMIT 4

typedef struct Node {
    struct Node *next;
    int cls;
} Node;

static Node *head[NC];
static Node *tail[NC];
static uint32_t cnt[NC];
static uint32_t shadow[NC]; /* expected number of blocks in the local list */

static void check(int cls, const char *where)
{
    uint32_t n = 0;
    Node *p = head[cls];
    Node *last = NULL;
    while (p) {
        n++;
        last = p;
        p = p->next;
        if (n > 100000) { printf("CYCLE at %s\n", where); exit(1); }
    }
    if (n != shadow[cls]) {
        printf("LEN MISMATCH cls=%d at %s: walk=%u shadow=%u\n", cls, where, n,
               shadow[cls]);
        exit(1);
    }
    if (n && last != tail[cls]) {
        printf("TAIL MISMATCH cls=%d at %s\n", cls, where);
        exit(1);
    }
}

static void do_free(Node *p)
{
    int cls = p->cls;
    Node *old = head[cls];
    p->next = old;
    head[cls] = p;
    if (!old) tail[cls] = p;
    shadow[cls]++;
    if (++cnt[cls] >= LIMIT) {
        /* flush */
        head[cls] = NULL;
        tail[cls] = NULL;
        cnt[cls] = 0;
        shadow[cls] = 0;
    }
    check(cls, "free");
}

static Node *do_alloc(int cls)
{
    Node *p = head[cls];
    if (!p) {
        /* refill: fresh span of 8 nodes */
        Node *first = NULL, *last = NULL;
        for (int i = 0; i < 8; i++) {
            Node *n = calloc(1, sizeof(Node));
            n->cls = cls;
            if (!first) first = n; else last->next = n;
            last = n;
        }
        head[cls] = first;
        tail[cls] = last;
        shadow[cls] = 8;
        p = head[cls];
    }
    head[cls] = p->next;
    shadow[cls]--;
    check(cls, "alloc");
    return p;
}

int main(void)
{
    srand(1234);
    for (int it = 0; it < 2000000; it++) {
        int cls = rand() % NC;
        if (rand() & 1) {
            Node *p = do_alloc(cls);
            do_free(p);
        } else {
            do_alloc(cls);
        }
    }
    printf("ok\n");
    return 0;
}
