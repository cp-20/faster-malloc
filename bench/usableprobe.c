/* usableprobe -- for each request size, print request and
 * malloc_usable_size of a fresh allocation.  The internal
 * fragmentation an allocator charges for a request is
 * usable - request; the footprint ratio is usable / request. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

int main(void)
{
    printf("req,usable\n");
    for (size_t r = 1; r <= 8192; r++) {
        void *p = malloc(r);
        if (!p)
            return 1;
        printf("%zu,%zu\n", r, malloc_usable_size(p));
        free(p);
    }
    for (size_t r = 8193; r <= 1048576; r += 257) {
        void *p = malloc(r);
        if (!p)
            return 1;
        printf("%zu,%zu\n", r, malloc_usable_size(p));
        free(p);
    }
    return 0;
}
