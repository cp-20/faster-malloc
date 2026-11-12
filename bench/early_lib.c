/*
 * early_lib.c -- a dependency library whose constructor allocates.
 *
 * Linked into early_main as a normal DT_NEEDED dependency, its
 * constructor runs before the constructor of an LD_PRELOADed library
 * (glibc initialises preloads after the executable's dependencies).
 * Any allocator whose entry points are not callable before its own
 * constructor has run fails this test -- the JIT build did exactly
 * that until round 3: its malloc jumped through an entry pointer the
 * constructor was supposed to install, and Node.js (whose libnode
 * constructors allocate heavily) segfaulted at startup.
 */
#include <stdlib.h>
#include <string.h>

static char *g_early;

__attribute__((constructor)) static void early_init(void)
{
    g_early = malloc(64);
    if (g_early)
        strcpy(g_early, "early");
    /* calloc and realloc in a constructor too: all must work. */
    char *z = calloc(16, 8);
    if (z) {
        memset(z, 1, 128);
        g_early = realloc(g_early, 256);
        if (g_early)
            strcat(g_early, "-grown");
        free(z);
    }
}

const char *early_str(void) { return g_early; }
