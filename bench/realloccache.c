/* A distant cached mapping must serve only opt-in realloc growth. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    int wide = argc > 1 && atoi(argv[1]);
    malloc_trim(0);
    unsigned char *p = malloc(128 * 1024);
    unsigned char *cached = malloc(8 * 1024 * 1024);
    assert(p && cached);
    memset(p, 0xa5, 128 * 1024);
    uintptr_t address = (uintptr_t)cached;
    free(cached);
    unsigned char *q = realloc(p, 256 * 1024);
    assert(q && (((uintptr_t)q == address) == wide));
    for (size_t i = 0; i < 128 * 1024; i++) assert(q[i] == 0xa5);
    memset(q, 0x5a, 256 * 1024);
    free(q);
    /* Ordinary calloc retains the narrow search and still clears hits. */
    unsigned char *z = calloc(1, 256 * 1024);
    assert(z && (uintptr_t)z != address);
    for (size_t i = 0; i < 256 * 1024; i++) assert(!z[i]);
    free(z);
    malloc_trim(0);
    puts("realloccache ok");
}
