from pathlib import Path
s = Path('src/fm_static.c').read_text()
s = s.replace('#include "fm_core.h"', '#include "fm_core_diag.h"')
import re
names = ['malloc', 'free', 'calloc', 'realloc', 'reallocarray', 'posix_memalign', 'aligned_alloc', 'memalign', 'valloc', 'pvalloc']
for n in names:
    s = re.sub('\\b' + n + '\\(', 'diag_' + n + '(', s)
s += '''
/* Diagnostic build only: live usable capacity, not requested bytes or RSS. */
_Atomic long diag_small_live, diag_large_live;
static void account(void *p, int sign) {
    if (!p) return;
    FM_Span *sp = ((uintptr_t)p % SPAN_SIZE == 0 && fm_is_aligned_ptr(p))
        ? (FM_Span *)((char *)p-HEADER_SIZE)
        : (FM_Span *)((uintptr_t)p & ~(uintptr_t)(SPAN_SIZE-1));
    if (sp->cls != LARGE_CLASS) __atomic_fetch_add(&sp->user_size, sign*(long)fm_usable_size(p), __ATOMIC_RELAXED);
    atomic_fetch_add(sp->cls == LARGE_CLASS ? &diag_large_live : &diag_small_live,
                     sign*(long)fm_usable_size(p));
}
void *malloc(size_t n) { void *p=diag_malloc(n); account(p,1); return p; }
void free(void *p) { account(p,-1); diag_free(p); }
void *calloc(size_t n,size_t s) {void *p=diag_calloc(n,s);account(p,1);return p;}
void *realloc(void *p,size_t n) {account(p,-1);void *q=diag_realloc(p,n);if(q || !n) account(q,1);else account(p,1);return q;}
void *reallocarray(void *p,size_t n,size_t s) {if(n && s>SIZE_MAX/n){errno=ENOMEM;return NULL;}return realloc(p,n*s);}
int posix_memalign(void **p,size_t a,size_t n){int r=diag_posix_memalign(p,a,n);if(!r)account(*p,1);return r;}
void *aligned_alloc(size_t a,size_t n){void *p=diag_aligned_alloc(a,n);account(p,1);return p;}
void *memalign(size_t a,size_t n){void *p=diag_memalign(a,n);account(p,1);return p;}
void *valloc(size_t n){void *p=diag_valloc(n);account(p,1);return p;}
void *pvalloc(size_t n){void *p=diag_pvalloc(n);account(p,1);return p;}
'''
Path('work/fm_static_diag.c').write_text(s)
s = Path('src/fm_core.h').read_text().replace('sp->lnext = (uintptr_t)fm_lcache_head[b];', 'sp->user_size = 0; /* diagnostic cached marker */\n            sp->lnext = (uintptr_t)fm_lcache_head[b];')
Path('work/fm_core_diag.h').write_text(s)
