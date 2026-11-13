"""Build isolated candidates; rejected shapes never enter the allocator default."""
from pathlib import Path
import subprocess
root = Path.cwd()
out = root / 'work/round11'
out.mkdir(parents=True, exist_ok=True)
def baseline(file):
    return subprocess.check_output(['git','show',f'e598f28:src/{file}'])
original = baseline('fm_core.h').decode()
old = '''        if (pthread_mutex_trylock(&fm_census_lock) != 0)
            return 0;'''
assert original.count(old) == 1
shapes = {
    'blocking': original.replace('#define FM_CENSUS_TRYLOCK 1', '#define FM_CENSUS_TRYLOCK 0'),
    'trylock': original,
    'spin': original.replace(old, '''        int busy = pthread_mutex_trylock(&fm_census_lock);
        for (unsigned i = 0; busy && i < 16; ++i) {
            __asm__ __volatile__("pause");
            busy = pthread_mutex_trylock(&fm_census_lock);
        }
        if (busy) return 0;'''),
    'hint': original.replace('static pthread_mutex_t fm_census_lock', 'static atomic_int fm_census_busy;\nstatic pthread_mutex_t fm_census_lock').replace(old, '''        if (atomic_load_explicit(&fm_census_busy, memory_order_relaxed)) {
            if (pthread_mutex_trylock(&fm_census_lock) != 0) return 0;
        } else {
            pthread_mutex_lock(&fm_census_lock);
        }'''),
    'backoff': original.replace('static pthread_mutex_t fm_census_lock', 'static _Thread_local unsigned fm_census_backoff;\nstatic pthread_mutex_t fm_census_lock').replace(old, '''        if (fm_census_backoff) {
            --fm_census_backoff;
            return 0;
        }
        if (pthread_mutex_trylock(&fm_census_lock) != 0) {
            fm_census_backoff = 3;
            return 0;
        }'''),
}
# Hint only predicts contention. Every census still acquires the same mutex.
a = shapes['hint'].index('__attribute__((noinline)) static size_t fm_trim_class')
b = shapes['hint'].index('/* Consider one class', a)
part = shapes['hint'][a:b].replace('    int was_in_census', '    atomic_store_explicit(&fm_census_busy, 1, memory_order_relaxed);\n    int was_in_census').replace('pthread_mutex_unlock(&fm_census_lock);', 'atomic_store_explicit(&fm_census_busy, 0, memory_order_relaxed);\n    pthread_mutex_unlock(&fm_census_lock);')
shapes['hint'] = shapes['hint'][:a] + part + shapes['hint'][b:]
flags = ['cc','-O3','-g','-fPIC','-fno-plt','-Wall','-Wextra','-Wno-unused-parameter']
for name, source in shapes.items():
    folder = out/name
    folder.mkdir(exist_ok=True)
    for f in ['fm_static.c','size_classes2.h']:
        (folder/f).write_bytes(baseline(f))
    (folder/'fm_core.h').write_text(source)
    subprocess.run(flags+['-shared','-Wl,-z,nodelete',str(folder/'fm_static.c'),'-o',str(out/(name+'.so')),'-pthread'],check=True)
    if name in ('blocking','trylock'):
        # Diagnostic-only counters, never used for throughput comparisons.
        diag = source.replace('static pthread_mutex_t fm_census_lock', 'static _Atomic unsigned long diag_attempt, diag_bail, diag_acquired, diag_walked, diag_released, diag_purged, diag_spans;\nstatic pthread_mutex_t fm_census_lock')
        diag = diag.replace('    fm_census_walked = fm_census_marked = fm_census_waiting = 0;', '    atomic_fetch_add(&diag_attempt, 1);\n    fm_census_walked = fm_census_marked = fm_census_waiting = 0;')
        diag = diag.replace(old, '''        if (pthread_mutex_trylock(&fm_census_lock) != 0) {
            atomic_fetch_add(&diag_bail, 1);
            return 0;
        }''')
        diag = diag.replace('    int was_in_census = fm_in_census;', '    atomic_fetch_add(&diag_acquired, 1);\n    int was_in_census = fm_in_census;')
        diag = diag.replace('    return released;\n}\n\n/* Consider one class', '    atomic_fetch_add(&diag_walked, fm_census_walked);\n    atomic_fetch_add(&diag_released, released);\n    atomic_fetch_add(&diag_purged, fm_census_purged);\n    return released;\n}\n\n/* Consider one class')
        diag = diag.replace('    char *base = fm_map_aligned(SPAN_SIZE - HEADER_SIZE);', '    atomic_fetch_add(&diag_spans, 1);\n    char *base = fm_map_aligned(SPAN_SIZE - HEADER_SIZE);')
        diag += '''\n#include <stdio.h>
__attribute__((destructor)) static void diag_report(void) {
    fprintf(stderr, "DIAG attempts=%lu bail=%lu acquired=%lu walked=%lu released=%lu purged=%lu spans=%lu\\n", (unsigned long)diag_attempt, (unsigned long)diag_bail, (unsigned long)diag_acquired, (unsigned long)diag_walked, (unsigned long)diag_released, (unsigned long)diag_purged, (unsigned long)diag_spans);
}
'''
        (folder/'fm_core.h').write_text(diag)
        subprocess.run(flags+['-shared','-Wl,-z,nodelete',str(folder/'fm_static.c'),'-o',str(out/(name+'-diag.so')),'-pthread'],check=True)
        if name == 'trylock':
            subprocess.run(flags+['-I'+str(folder), 'bench/round11/diagnostic-check.c','-o',str(out/'diagnostic-check'),'-pthread'],check=True)
            subprocess.run([str(out/'diagnostic-check')],check=True)
        (folder/'fm_core.h').write_text(source)
subprocess.run(['cc','-O2','-g','bench/ownership.c','-o',str(out/'ownership'),'-pthread'],check=True)
