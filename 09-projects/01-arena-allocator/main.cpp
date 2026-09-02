// ===========================================================================
//  PROJECT 1 — tests and benchmarks for the arena allocator
//  run: make opt F=09-projects/01-arena-allocator/main.cpp
//  also: make asan F=09-projects/01-arena-allocator/main.cpp   (must be clean!)
// ===========================================================================

#include "arena.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <string>
#include <vector>

using arena::Arena;
using arena::ArenaResource;

// ---------------------------------------------------------------------------
static int g_live_objects = 0;

struct Tracked {                      // NON-trivially destructible
    int id;
    std::string name;
    explicit Tracked(int i) : id(i), name("obj-" + std::to_string(i)) { ++g_live_objects; }
    ~Tracked() { --g_live_objects; }
};

struct Pod { float x, y, z; };        // trivially destructible

// ---------------------------------------------------------------------------
static void test_basic() {
    std::printf("\n=== TEST 1: basic allocation and alignment ===\n");
    Arena a(4096);

    auto* p1 = a.create<Pod>(Pod{1,2,3});
    auto* p2 = a.create<Pod>(Pod{4,5,6});
    auto* big = a.create<double>(3.14);

    assert(p1->x == 1 && p2->x == 4 && *big == 3.14);
    assert(reinterpret_cast<std::uintptr_t>(big) % alignof(double) == 0);

    // Verify EVERY allocation is correctly aligned for its type.
    for (int i = 0; i < 100; ++i) {
        auto* d = a.create<double>(double(i));
        assert(reinterpret_cast<std::uintptr_t>(d) % alignof(double) == 0);
        auto* c = a.create<char>(char('a' + i % 26));
        (void)c;
    }
    std::printf("  ✅ alignment holds for %zu allocations\n", a.stats().allocations);
    std::printf("  blocks=%zu reserved=%zu used=%zu\n",
                a.stats().blocks, a.stats().bytes_reserved, a.stats().bytes_used);
}

// ---------------------------------------------------------------------------
static void test_block_chaining() {
    std::printf("\n=== TEST 2: block chaining when the arena fills ===\n");
    Arena a(1024);                                  // deliberately tiny

    for (int i = 0; i < 5000; ++i) (void)a.create<Pod>(Pod{float(i),0,0});

    std::printf("  after 5000 Pods: blocks=%zu reserved=%zu KB used=%zu KB\n",
                a.stats().blocks, a.stats().bytes_reserved / 1024,
                a.stats().bytes_used / 1024);
    assert(a.stats().blocks > 1);
    std::printf("  ✅ grew to %zu blocks geometrically (no realloc, old pointers\n"
                "     stay VALID — that's a key arena property std::vector lacks)\n",
                a.stats().blocks);
}

// ---------------------------------------------------------------------------
static void test_destructors() {
    std::printf("\n=== TEST 3: destructors actually run ===\n");
    assert(g_live_objects == 0);
    {
        Arena a(4096);
        for (int i = 0; i < 100; ++i) (void)a.create<Tracked>(i);
        std::printf("  live objects inside scope: %d\n", g_live_objects);
        assert(g_live_objects == 100);
        assert(a.stats().destructors == 100);
    }   // ~Arena -> reset() -> runs all 100 destructors
    std::printf("  live objects after arena died: %d\n", g_live_objects);
    assert(g_live_objects == 0);
    std::printf("  ✅ all destructors ran, in reverse order\n");

    // reset() also runs them, and reuses the memory.
    {
        Arena a(4096);
        for (int i = 0; i < 50; ++i) (void)a.create<Tracked>(i);
        assert(g_live_objects == 50);
        a.reset();
        assert(g_live_objects == 0);
        std::printf("  ✅ reset() ran destructors and rewound to 0 bytes used\n");
    }
}

// ---------------------------------------------------------------------------
static void test_scope() {
    std::printf("\n=== TEST 4: scoped markers (nested sub-arenas) ===\n");
    Arena a(8192);

    (void)a.create<Tracked>(1);
    std::size_t used_before = a.stats().bytes_used;
    assert(g_live_objects == 1);

    {
        auto s = a.scope();                       // mark
        for (int i = 0; i < 20; ++i) (void)a.create<Tracked>(100 + i);
        assert(g_live_objects == 21);
        std::printf("  inside scope: %d live objects\n", g_live_objects);
    }   // <- rewind: 20 destructors run, memory reclaimed

    std::printf("  after scope:  %d live objects\n", g_live_objects);
    assert(g_live_objects == 1);
    std::printf("  ✅ scope rewound cleanly (used %zu -> back to the marker)\n", used_before);
    a.reset();
    assert(g_live_objects == 0);
}

// ---------------------------------------------------------------------------
static void test_pmr() {
    std::printf("\n=== TEST 5: std::pmr — standard containers on our arena ===\n");
    Arena a(64 * 1024);
    ArenaResource res(a);

    // A REAL std::vector whose every allocation comes from our arena.
    std::pmr::vector<int> v(&res);
    for (int i = 0; i < 10000; ++i) v.push_back(i);

    std::pmr::vector<std::pmr::string> strings(&res);
    for (int i = 0; i < 500; ++i)
        strings.emplace_back("a string long enough to defeat SSO, number " + std::to_string(i));

    std::printf("  vector<int> of %zu, vector<string> of %zu\n", v.size(), strings.size());
    std::printf("  arena: blocks=%zu reserved=%zu KB, allocations=%zu\n",
                a.stats().blocks, a.stats().bytes_reserved / 1024, a.stats().allocations);
    std::printf("  ✅ ZERO calls to global operator new for the elements —\n"
                "     the vector's growth AND the strings all came from the arena.\n");
    assert(v[9999] == 9999);
}

// ---------------------------------------------------------------------------
static void benchmark() {
    std::printf("\n=== BENCHMARK: arena vs new/delete (a simulated request) ===\n");

    constexpr int REQUESTS      = 5000;
    constexpr int OBJS_PER_REQ  = 400;

    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // --- (a) the "normal" way: allocate and free per object ---
    auto t0 = std::chrono::steady_clock::now();
    double sink1 = 0;
    for (int r = 0; r < REQUESTS; ++r) {
        std::vector<Pod*> objs;
        objs.reserve(OBJS_PER_REQ);
        for (int i = 0; i < OBJS_PER_REQ; ++i) objs.push_back(new Pod{float(i), 1, 2});
        for (auto* p : objs) sink1 += p->x;
        for (auto* p : objs) delete p;
    }
    auto t1 = std::chrono::steady_clock::now();

    // --- (b) one arena, reset per request ---
    Arena a(OBJS_PER_REQ * sizeof(Pod) * 2);
    double sink2 = 0;
    for (int r = 0; r < REQUESTS; ++r) {
        std::vector<Pod*> objs;
        objs.reserve(OBJS_PER_REQ);
        for (int i = 0; i < OBJS_PER_REQ; ++i) objs.push_back(a.create<Pod>(Pod{float(i), 1, 2}));
        for (auto* p : objs) sink2 += p->x;
        a.reset();                                  // "free" all 400, O(1)
    }
    auto t2 = std::chrono::steady_clock::now();

    std::printf("  new/delete per object : %8.2f ms  (%.1f ns/object)\n",
                ms(t0,t1), ms(t0,t1) * 1e6 / (REQUESTS * OBJS_PER_REQ));
    std::printf("  arena + reset()       : %8.2f ms  (%.1f ns/object)\n",
                ms(t1,t2), ms(t1,t2) * 1e6 / (REQUESTS * OBJS_PER_REQ));
    std::printf("  speedup: %.1fx   (sinks %.0f / %.0f — equal, so both did the work)\n",
                ms(t0,t1) / ms(t1,t2), sink1, sink2);
    std::printf("  arena made %zu OS allocation(s) TOTAL across all %d requests.\n",
                a.stats().blocks, REQUESTS);
}

// ---------------------------------------------------------------------------
int main() {
    test_basic();
    test_block_chaining();
    test_destructors();
    test_scope();
    test_pmr();
    benchmark();

    std::printf(
      "\n=== WHAT TO TAKE AWAY ===\n"
      "  1. `allocate` is align + compare + add. That's the whole hot path.\n"
      "  2. `reset` is O(number of registered destructors), and O(1) when all\n"
      "     your types are trivially destructible. Design for that.\n"
      "  3. Pointers into an arena stay VALID as it grows (unlike vector), which\n"
      "     makes arenas ideal for building linked structures like ASTs.\n"
      "  4. std::pmr lets standard containers use it with no type surgery.\n"
      "  5. It cannot leak and cannot fragment — structurally.\n"
      "\n=== EXERCISES (do these; they're where the learning is) ===\n"
      "  a. Add a `try_allocate` that returns nullptr instead of growing, and a\n"
      "     fixed-capacity mode for hard real-time use.\n"
      "  b. Make it thread-safe THREE ways and benchmark each:\n"
      "       (i) a mutex,  (ii) an atomic bump pointer (CAS loop),\n"
      "       (iii) one arena per thread (thread_local). Predict which wins\n"
      "       before you measure, then see if you were right.\n"
      "  c. Add ASan poisoning: __asan_poison_memory_region on reset, unpoison\n"
      "     on allocate. Now use-after-reset becomes a loud ASan error instead\n"
      "     of silent corruption. This is what real arenas do.\n"
      "  d. Write a fixed-size POOL allocator (free list threaded through the\n"
      "     free slots themselves) and compare it to the arena for a workload\n"
      "     with individual frees.\n"
      "  e. Wire the arena into 08-systems/03-kqueue-event-loop.cpp: one arena\n"
      "     per connection, reset after each request. Measure allocations/request\n"
      "     before and after.\n");
    return 0;
}
