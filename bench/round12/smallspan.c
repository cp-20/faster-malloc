/* Keep libc's allocations out of the white-box allocator's class state. */
#define malloc test_malloc
#define free test_free
#define calloc test_calloc
#define realloc test_realloc
#define reallocarray test_reallocarray
#define posix_memalign test_posix_memalign
#define aligned_alloc test_aligned_alloc
#define memalign test_memalign
#define valloc test_valloc
#define pvalloc test_pvalloc
#define malloc_usable_size test_malloc_usable_size
#define malloc_trim test_malloc_trim
#define mallopt test_mallopt
#define malloc_stats test_malloc_stats
#include "../../src/fm_static.c"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    for (uint32_t cls = 0; cls < CLASS_COUNT; cls++) {
        void *tail;
        uint32_t n;
        size_t bs = kClassSize[cls];
        void *head = fm_span_new(cls, &tail, &n);
        assert(head);
        FM_Span *sp = (FM_Span *)((uintptr_t)head & SPAN_MASK);
        size_t expected = FM_ALIGN_UP(HEADER_SIZE + (bs > 16320 ? bs : 16320), FM_PAGE_SIZE);
        assert(sp->map_size == expected && n == (expected - HEADER_SIZE) / bs);
        void *p = head;
        for (uint32_t i = 0; i < n; i++) {
            assert(p == (char *)head + i * bs);
            p = *(void **)p;
        }
        assert(!p && tail == (char *)head + (n - 1) * bs);
        /* A live first block pins the short mapping while the census purges
         * central-only pages; intersect/start counts must still protect it. */
        void *rest = *(void **)head;
        memset(head, 0xA5, bs);
        if (n > 1)
            fm_publish2(rest, tail, cls, n - 1, 1);
        fm_trim_class(cls, 0, 1);
        for (size_t i = 0; i < bs; i++)
            assert(((unsigned char *)head)[i] == 0xA5);
        *(void **)head = NULL;
        fm_publish2(head, head, cls, 1, 1);
        /* Includes withheld blocks in its exact whole-span free proof. */
        assert(fm_trim_class(cls, 0, 1) == expected);
        head = fm_span_new(cls, &tail, &n);
        assert(head);
        sp = (FM_Span *)((uintptr_t)head & SPAN_MASK);
        assert(sp->map_size == SPAN_SIZE && n == (SPAN_SIZE - HEADER_SIZE) / bs);
        assert(!munmap(sp, sp->map_size));
    }
    puts("all 129 first-span geometries, live bytes, purge and full-span growth ok");
}
