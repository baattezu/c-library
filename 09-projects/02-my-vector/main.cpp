// ===========================================================================
//  PROJECT 2 — tests for my_vector, including the exception-safety tests
//  run:  make opt  F=09-projects/02-my-vector/main.cpp
//  MUST: make asan F=09-projects/02-my-vector/main.cpp    <- must be clean
// ===========================================================================

#include "my_vector.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using mystd::vector;

// ---------------------------------------------------------------------------
static int g_live = 0;
struct Tracked {
    int v;
    explicit Tracked(int x = 0) : v(x) { ++g_live; }
    Tracked(const Tracked& o) : v(o.v) { ++g_live; }
    Tracked(Tracked&& o) noexcept : v(o.v) { o.v = -1; ++g_live; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --g_live; }
    friend bool operator==(const Tracked& a, const Tracked& b) { return a.v == b.v; }
};

#define CHECK(cond) do { if (!(cond)) { \
    std::printf("  ❌ FAILED: %s  (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)
static int g_failures = 0;

// ---------------------------------------------------------------------------
static void test_basics() {
    std::printf("\n=== TEST 1: construction, growth, access ===\n");
    vector<int> v;
    CHECK(v.empty() && v.size() == 0 && v.capacity() == 0);

    for (int i = 0; i < 10; ++i) v.push_back(i);
    CHECK(v.size() == 10);
    CHECK(v.capacity() >= 10);
    CHECK(v[0] == 0 && v[9] == 9);
    CHECK(v.front() == 0 && v.back() == 9);

    std::printf("  growth sequence: ");
    vector<int> g;
    std::size_t cap = 0;
    for (int i = 0; i < 40; ++i) {
        g.push_back(i);
        if (g.capacity() != cap) { std::printf("%zu ", g.capacity()); cap = g.capacity(); }
    }
    std::printf(" (2x geometric)\n");

    vector<int> init{1,2,3,4,5};
    CHECK(init.size() == 5 && init[2] == 3);

    vector<int> sized(7, 42);
    CHECK(sized.size() == 7 && sized[6] == 42);

    bool threw = false;
    try { (void)init.at(99); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);
    std::printf("  ✅ basics\n");
}

// ---------------------------------------------------------------------------
static void test_lifetime() {
    std::printf("\n=== TEST 2: every constructor is matched by a destructor ===\n");
    CHECK(g_live == 0);
    {
        vector<Tracked> v;
        for (int i = 0; i < 100; ++i) v.emplace_back(i);
        CHECK(g_live == 100);

        v.pop_back();
        CHECK(g_live == 99);

        v.clear();
        CHECK(g_live == 0);
        CHECK(v.capacity() > 0);     // clear KEEPS capacity

        for (int i = 0; i < 50; ++i) v.emplace_back(i);
        CHECK(g_live == 50);
    }
    CHECK(g_live == 0);
    std::printf("  ✅ no leaked or double-destroyed objects (g_live back to 0)\n");
}

// ---------------------------------------------------------------------------
static void test_copy_move() {
    std::printf("\n=== TEST 3: Rule of Five ===\n");
    CHECK(g_live == 0);
    {
        vector<Tracked> a;
        for (int i = 0; i < 10; ++i) a.emplace_back(i);

        vector<Tracked> b = a;                  // copy ctor
        CHECK(b.size() == 10 && g_live == 20);
        CHECK(a == b);
        b[0] = Tracked(999);
        CHECK(a[0].v == 0);                     // deep copy: a unaffected

        vector<Tracked> c = std::move(a);       // move ctor
        CHECK(c.size() == 10);
        CHECK(a.size() == 0 && a.capacity() == 0 && a.data() == nullptr);

        vector<Tracked> d;
        d = c;                                  // copy assign
        CHECK(d.size() == 10);

        vector<Tracked> e;
        e = std::move(c);                       // move assign
        CHECK(e.size() == 10 && c.size() == 0);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"   // deliberate test
        e = e;                                  // self-assignment must be safe
#pragma clang diagnostic pop
        CHECK(e.size() == 10);
    }
    CHECK(g_live == 0);
    std::printf("  ✅ copy/move ctor, copy/move assign, self-assignment\n");
    std::printf("  nothrow_move_constructible<vector<int>> = %s\n",
                std::is_nothrow_move_constructible_v<vector<int>> ? "yes ✅" : "NO ❌");
}

// ---------------------------------------------------------------------------
//  THE HARD TEST: strong exception safety during reallocation.
//  We use a type whose COPY constructor throws on the Nth call. Because its
//  move constructor is NOT noexcept, move_if_noexcept will choose to COPY —
//  and when that copy throws mid-reallocation, the vector must be UNCHANGED.
// ---------------------------------------------------------------------------
static int g_throw_after = -1;
static int g_copy_count  = 0;

struct Fragile {
    int v;
    explicit Fragile(int x = 0) : v(x) {}
    Fragile(const Fragile& o) : v(o.v) {
        if (g_throw_after >= 0 && ++g_copy_count > g_throw_after)
            throw std::runtime_error("copy failed");
    }
    // NOT noexcept — deliberately. This forces move_if_noexcept to copy.
    Fragile(Fragile&& o) : v(o.v) {}
    Fragile& operator=(const Fragile&) = default;
    Fragile& operator=(Fragile&&) = default;
};

static void test_exception_safety() {
    std::printf("\n=== TEST 4: strong exception guarantee on reallocation ===\n");

    vector<Fragile> v;
    v.reserve(4);
    for (int i = 0; i < 4; ++i) v.push_back(Fragile(i));
    CHECK(v.size() == 4 && v.capacity() == 4);

    // Now force a reallocation whose 3rd element-copy throws.
    g_copy_count = 0;
    g_throw_after = 2;
    bool threw = false;
    try {
        v.push_back(Fragile(99));          // triggers reallocate(8)
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_after = -1;

    CHECK(threw);
    // THE ASSERTION THAT MATTERS: the vector is exactly as it was.
    CHECK(v.size() == 4);
    CHECK(v.capacity() == 4);
    for (int i = 0; i < 4; ++i) CHECK(v[i].v == i);
    std::printf("  ✅ reallocation threw, and the vector was left UNCHANGED\n"
                "     (size, capacity, and every element value preserved)\n");
    std::printf("  This is the STRONG guarantee. Run this file under `make asan`\n"
                "  to prove the rolled-back buffer was also freed correctly.\n");
}

// ---------------------------------------------------------------------------
static void test_algorithms() {
    std::printf("\n=== TEST 5: works with standard algorithms ===\n");
    vector<int> v{5,3,8,1,9,2};
    std::sort(v.begin(), v.end());
    CHECK(v[0] == 1 && v[5] == 9);
    CHECK(std::is_sorted(v.begin(), v.end()));
    CHECK(std::find(v.begin(), v.end(), 8) != v.end());
    int sum = 0;
    for (int x : v) sum += x;                      // range-for works
    CHECK(sum == 28);
    std::printf("  ✅ sort/find/range-for all work — because we exposed\n"
                "     begin()/end() returning contiguous iterators (raw pointers)\n");
}

// ---------------------------------------------------------------------------
static void benchmark() {
    std::printf("\n=== BENCHMARK vs std::vector ===\n");
    constexpr int N = 2'000'000;
    auto ms = [](auto a, auto b){ return std::chrono::duration<double,std::milli>(b-a).count(); };

    auto t0 = std::chrono::steady_clock::now();
    { std::vector<int> v; v.reserve(N); for (int i=0;i<N;++i) v.push_back(i); }
    auto t1 = std::chrono::steady_clock::now();
    { mystd::vector<int> v; v.reserve(N); for (int i=0;i<N;++i) v.push_back(i); }
    auto t2 = std::chrono::steady_clock::now();
    { std::vector<int> v; for (int i=0;i<N;++i) v.push_back(i); }
    auto t3 = std::chrono::steady_clock::now();
    { mystd::vector<int> v; for (int i=0;i<N;++i) v.push_back(i); }
    auto t4 = std::chrono::steady_clock::now();

    std::printf("  std::vector   reserved: %7.2f ms\n", ms(t0,t1));
    std::printf("  mystd::vector reserved: %7.2f ms\n", ms(t1,t2));
    std::printf("  std::vector   growing : %7.2f ms\n", ms(t2,t3));
    std::printf("  mystd::vector growing : %7.2f ms\n", ms(t3,t4));
    std::printf("\n  If yours is within ~20%% of std::vector, you've understood it.\n"
                "  If it's much slower, likely causes: no trivially-copyable\n"
                "  fast path in reallocate (std::vector uses memcpy for POD),\n"
                "  or a bounds check you didn't mean to add.\n");
}

// ---------------------------------------------------------------------------
int main() {
    test_basics();
    test_lifetime();
    test_copy_move();
    test_exception_safety();
    test_algorithms();
    benchmark();

    std::printf("\n=== RESULT: %s ===\n",
                g_failures == 0 ? "ALL TESTS PASSED ✅" : "THERE WERE FAILURES ❌");

    std::printf(
      "\n=== EXERCISES ===\n"
      "  a. Add insert(pos, value) and erase(pos). Get the iterator\n"
      "     invalidation and the element shifting right. This is where most\n"
      "     hand-rolled vectors have bugs.\n"
      "  b. Add a memcpy fast path: `if constexpr (is_trivially_copyable_v<T>)`\n"
      "     use std::memcpy in reallocate instead of the element loop. Measure.\n"
      "  c. Add an Allocator template parameter and route through\n"
      "     std::allocator_traits. Then plug in the arena from project 1.\n"
      "  d. Implement small-buffer optimisation: keep the first N elements\n"
      "     inline in the object (like boost::small_vector / llvm::SmallVector).\n"
      "     Benchmark it for N=4 with lots of short-lived vectors.\n"
      "  e. Make Fragile's move ctor `noexcept` and re-run test 4. Watch the\n"
      "     strong guarantee become impossible to test — because now the vector\n"
      "     MOVES (which can't throw) instead of copying. That's the whole\n"
      "     move_if_noexcept mechanism, demonstrated.\n");
    return g_failures == 0 ? 0 : 1;
}
