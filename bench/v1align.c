/* v1 alignment API edges: indirect calls defeat compiler builtin assumptions. */
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int (*volatile pm)(void **, size_t, size_t) = posix_memalign;
    void *(*volatile aa)(size_t, size_t) = aligned_alloc;
    void *(*volatile ma)(size_t, size_t) = memalign;
    void *(*volatile va)(size_t) = valloc;
    void *(*volatile pa)(size_t) = pvalloc;
    const size_t invalid[] = { 3, 24, 0 };
    int sentinel;
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        void *out = &sentinel;
        assert(pm(&out, invalid[i], 128) == EINVAL);
        assert(out == &sentinel);
        errno = 0;
        assert(aa(invalid[i], 128) == NULL && errno == EINVAL);
        errno = 0;
        assert(ma(invalid[i], 128) == NULL && errno == EINVAL);
    }
    void *out = &sentinel;
    /* Far beyond the process address space, without size arithmetic overflow. */
    const size_t huge = SIZE_MAX / 4;
    assert(pm(&out, 4096, huge) == ENOMEM);
    assert(out == &sentinel);
    assert(aa(4096, huge) == NULL);
    assert(ma(4096, huge) == NULL);
    assert(va(huge) == NULL);
    assert(pa(huge) == NULL);
    for (size_t align = sizeof(void *); align <= 262144; align *= 2) {
        assert(pm(&out, align, 128) == 0);
        assert(out && ((uintptr_t)out & (align - 1)) == 0);
        free(out);
        out = aa(align, align);
        assert(out && ((uintptr_t)out & (align - 1)) == 0);
        free(out);
        out = ma(align, 128);
        assert(out && ((uintptr_t)out & (align - 1)) == 0);
        free(out);
    }
    out = va(128);
    assert(out && ((uintptr_t)out & 4095) == 0);
    free(out);
    out = pa(128);
    assert(out && ((uintptr_t)out & 4095) == 0);
    assert(malloc_usable_size(out) >= 4096);
    free(out);
    puts("v1 alignment ok");
}
