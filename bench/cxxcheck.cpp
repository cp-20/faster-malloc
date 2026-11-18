// cxxcheck.cpp -- C++ operator new/delete semantics under the interposed
// allocator (ADR 0078): over-aligned new, sized delete, nothrow, new_handler,
// bad_alloc, multithreaded churn.
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>
struct alignas(128) A { char x[200]; };
struct alignas(64) B { int v; };
static int nh = 0;
static void handler() { nh++; std::set_new_handler(nullptr); }
int main()
{
    int bad = 0;
    for (int i = 0; i < 200000; i++) {
        auto *a = new A; if ((uintptr_t)a & 127) bad++; delete a;
        auto *b = new B[i % 17 + 1]; if ((uintptr_t)b & 63) bad++; delete[] b;
        auto *c = new char[i % 3000 + 1]; delete[] c;
        auto *d = new (std::nothrow) int[i % 100 + 1]; delete[] d;
    }
    std::map<std::string, std::vector<int>> m;
    for (int i = 0; i < 100000; i++) m[std::to_string(i)].push_back(i);
    std::vector<std::thread> th;
    for (int t = 0; t < 8; t++)
        th.emplace_back([] {
            std::vector<std::unique_ptr<std::string>> v;
            for (int i = 0; i < 200000; i++) {
                v.emplace_back(new std::string(i % 50, 'x'));
                if (v.size() > 1000) v.clear();
            }
        });
    for (auto &t : th) t.join();
    try { std::set_new_handler(handler); (void)new char[(size_t)1 << 62]; bad++; } catch (const std::bad_alloc &) {}
    if (::operator new((size_t)1 << 62, std::nothrow)) bad++;
    try { (void)::operator new((size_t)1 << 62, std::align_val_t(64)); bad++; } catch (const std::bad_alloc &) {}
    if (nh != 1 || m.size() != 100000) bad++;
    std::printf(bad ? "cxxcheck FAIL %d\n" : "cxxcheck ok\n", bad);
    return bad;
}
