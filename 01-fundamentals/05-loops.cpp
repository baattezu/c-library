// ===========================================================================
//  LOOPS — iteration, iterators, ranges, and the cost of each byte you touch
//  run: make run F=01-fundamentals/05-loops.cpp
//  bench: make opt F=01-fundamentals/05-loops.cpp
// ===========================================================================

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <numeric>
#include <ranges>
#include <string>
#include <vector>

void demo_basic_loops() {
    std::cout << "\n=== 1. The three loops (identical to Java) ===\n";

    for (int i = 0; i < 3; ++i) std::cout << "for " << i << "  ";
    std::cout << "\n";

    int j = 0;
    while (j < 3) { std::cout << "while " << j << "  "; ++j; }
    std::cout << "\n";

    int k = 0;
    do { std::cout << "do " << k << "  "; ++k; } while (k < 3);
    std::cout << "\n";

    // ⚠️ ++i vs i++ : for ints, identical (the compiler doesn't care).
    // For ITERATORS and heavy objects, i++ must copy the old value to return it.
    // Habit: always write ++i in loop headers. It costs nothing and sometimes saves.
}

void demo_range_for() {
    std::cout << "\n=== 2. Range-based for — and the 3 ways to bind the element ===\n";

    std::vector<std::string> names{"alice", "bob", "carol"};

    // (a) BY VALUE — makes a COPY of every element. For std::string that's a
    //     heap allocation per iteration if the string is long. Java devs write
    //     this by reflex because in Java it's a free reference copy. It is NOT
    //     free here. This is one of the top-3 accidental slowdowns for Java→C++.
    for (std::string s : names) { s += "!"; }        // modifies the copy, not the vector

    // (b) BY CONST REFERENCE — no copy, read-only. *** YOUR DEFAULT. ***
    for (const std::string& s : names) std::cout << s << " ";
    std::cout << "  (unchanged by (a), because (a) copied)\n";

    // (c) BY REFERENCE — no copy, mutates the container's elements.
    for (std::string& s : names) s += "!";
    for (const auto& s : names) std::cout << s << " ";
    std::cout << "\n";

    // (d) `auto&&` — the "forwarding" form. Binds to anything, including the
    //     proxy objects returned by vector<bool> and by range views. Use it in
    //     generic code / templates.
    for (auto&& s : names) { (void)s; }

    // C++20: init-statement in a range-for, keeping helpers scoped.
    for (int idx = 0; const auto& s : names) {
        std::cout << "  [" << idx++ << "] " << s << "\n";
    }
}

void demo_iterators() {
    std::cout << "\n=== 3. Iterators — the thing under the range-for ===\n";

    std::vector<int> v{10, 20, 30, 40};

    // A range-based for is *literally* rewritten by the compiler into this:
    for (auto it = v.begin(); it != v.end(); ++it) std::cout << *it << " ";
    std::cout << "\n";

    // An iterator is a GENERALISED POINTER. For vector it usually IS a raw
    // pointer (in release builds). Compare with Java's Iterator interface:
    // Java's is a heap object with virtual calls; C++'s is a value type that
    // compiles away to nothing. That's a zero-cost abstraction.

    // end() is ONE PAST THE LAST element. Dereferencing it is UB.
    // The half-open range [begin, end) makes size == end - begin, and makes
    // empty ranges natural (begin == end).

    std::cout << "  distance = " << std::distance(v.begin(), v.end()) << "\n";
    std::cout << "  reverse: ";
    for (auto it = v.rbegin(); it != v.rend(); ++it) std::cout << *it << " ";
    std::cout << "\n";

    // ITERATOR CATEGORIES (why some algorithms need certain containers):
    //   input/output     : single pass          (istream_iterator)
    //   forward          : multi-pass, ++ only  (forward_list, unordered_*)
    //   bidirectional    : ++ and --            (list, map, set)
    //   random access    : + n, - n, [], <      (vector, deque, array)
    //   contiguous (C++20): random access AND elements are adjacent in memory
    //                       (vector, array, string, span) -> can be memcpy'd,
    //                       can be SIMD'd, can be passed to a C API as T*.
    // std::sort requires random access — that's why you can't std::sort a list
    // (list has its own .sort()).

    // ⚠️ ITERATOR INVALIDATION — the C++ version of ConcurrentModificationException,
    // except it does NOT throw. It is undefined behaviour and often silently works
    // until it corrupts memory in production.
    std::vector<int> w{1,2,3};
    auto it = w.begin();
    w.push_back(4);          // MAY reallocate -> `it` is now dangling
    (void)it;
    // std::cout << *it;     // UB! Run under ASan to see it caught.
    std::cout << "  (see source: push_back can invalidate iterators)\n";
}

void demo_erase_while_iterating() {
    std::cout << "\n=== 4. Removing while iterating (the right ways) ===\n";

    // WRONG: erasing inside a plain range-for is UB.
    // The classic correct pattern (C++98..17): erase returns the next valid iterator.
    std::vector<int> v{1,2,3,4,5,6,7,8};
    for (auto it = v.begin(); it != v.end(); ) {
        if (*it % 2 == 0) it = v.erase(it);   // erase returns next; do NOT ++it here
        else              ++it;
    }
    for (int x : v) std::cout << x << " ";
    std::cout << " <- odds only, via erase-loop\n";

    // BETTER: erase-remove idiom, O(n) instead of O(n^2).
    // std::remove doesn't remove — it shuffles kept elements to the front and
    // returns the new logical end. Then you erase the tail. Since C++20 there's
    // a one-liner: std::erase_if.
    std::vector<int> w{1,2,3,4,5,6,7,8};
    std::erase_if(w, [](int x) { return x % 2 != 0; });   // C++20
    for (int x : w) std::cout << x << " ";
    std::cout << " <- evens only, via std::erase_if\n";

    // For map/set, erase(it) only invalidates that one iterator:
    std::map<int, std::string> m{{1,"a"},{2,"b"},{3,"c"}};
    for (auto it = m.begin(); it != m.end(); ) {
        if (it->first == 2) it = m.erase(it);
        else ++it;
    }
    std::cout << "  map now has " << m.size() << " entries\n";
}

void demo_ranges() {
    std::cout << "\n=== 5. C++20 ranges — like Java Streams, but LAZY and free ===\n";

    std::vector<int> v(20);
    std::iota(v.begin(), v.end(), 1);      // fill 1..20

    // Java: v.stream().filter(x -> x%3==0).map(x -> x*x).limit(4).toList()
    // C++20:
    auto result = v
                | std::views::filter([](int x) { return x % 3 == 0; })
                | std::views::transform([](int x) { return x * x; })
                | std::views::take(4);

    std::cout << "  ";
    for (int x : result) std::cout << x << " ";
    std::cout << "\n";

    // ⭐ THE DIFFERENCE FROM JAVA STREAMS:
    //  - No heap allocation. A view is a lightweight struct holding iterators
    //    and the lambda. It lives on the stack.
    //  - No virtual dispatch. Everything is a template, so the lambda body gets
    //    INLINED into the loop. The optimiser sees one fused loop.
    //  - Fully lazy: nothing runs until you iterate.
    //  - Cost: horrific compile errors and longer compile times. Real tradeoff.
    //
    // ⚠️ DANGLING TRAP: a view does NOT own its data.
    //     auto bad = get_vector() | views::filter(...);   // temp vector dies!
    // Store the vector first, then build the view over it.

    // Useful views: iota, take, drop, reverse, join, split, chunk (C++23),
    // zip (C++23), enumerate (C++23), keys/values (for maps).
    std::cout << "  iota+reverse: ";
    for (int x : std::views::iota(1, 6) | std::views::reverse) std::cout << x << " ";
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
//  THE PERFORMANCE LESSON: LOOP ORDER AND CACHE LINES
// ---------------------------------------------------------------------------
void demo_loop_order() {
    std::cout << "\n=== 6. Loop order: identical work, huge time difference ===\n";

    constexpr int N = 2048;
    // One flat allocation of N*N ints (16 MB) — row-major, like C arrays.
    std::vector<int> m(static_cast<std::size_t>(N) * N, 1);
    auto at = [&](int r, int c) -> int& { return m[static_cast<std::size_t>(r) * N + c]; };

    auto time_it = [](auto&& fn, const char* label) {
        auto t0 = std::chrono::steady_clock::now();
        long long s = fn();
        auto t1 = std::chrono::steady_clock::now();
        std::cout << "  " << label << ": "
                  << std::chrono::duration<double, std::milli>(t1 - t0).count()
                  << " ms (sum=" << s << ")\n";
    };

    // ROW-MAJOR traversal: consecutive iterations touch adjacent addresses.
    // One 64/128-byte cache line fetch serves the next 16-32 ints. The hardware
    // prefetcher recognises the stride and loads ahead. Nearly free.
    time_it([&] {
        long long s = 0;
        for (int r = 0; r < N; ++r)
            for (int c = 0; c < N; ++c) s += at(r, c);
        return s;
    }, "row-major    (cache friendly)");

    // COLUMN-MAJOR traversal: each iteration jumps N*4 = 8192 bytes. Every single
    // access is a cache miss AND a TLB miss. Same instruction count. ~5-10x slower.
    time_it([&] {
        long long s = 0;
        for (int c = 0; c < N; ++c)
            for (int r = 0; r < N; ++r) s += at(r, c);
        return s;
    }, "column-major (cache hostile)");

    std::cout << "\n  This is THE lesson of high-performance programming:\n"
                 "  the instructions are identical; only the MEMORY ACCESS PATTERN\n"
                 "  differs. Modern CPUs are memory machines, not compute machines.\n";
}

int main() {
    demo_basic_loops();
    demo_range_for();
    demo_iterators();
    demo_erase_while_iterating();
    demo_ranges();
    demo_loop_order();
    std::cout << "\n>>> Run with `make opt F=01-fundamentals/05-loops.cpp` for real numbers.\n";
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * `for (const auto& x : c)` is the default. `for (auto x : c)` COPIES.
//  * end() is one-past-the-end; ranges are half-open [begin, end).
//  * Iterator categories: input < forward < bidirectional < random-access <
//    contiguous. Algorithms state which they need (std::sort needs random access).
//  * Iterator invalidation is UB, not an exception. push_back may reallocate.
//  * Erase while iterating: `it = c.erase(it)`, or use std::erase_if (C++20).
//  * Ranges/views are lazy, allocation-free, and inline — unlike Java Streams.
//    But views don't own data: never build one over a temporary.
//  * Traverse memory in the order it is laid out. Row-major, always.
// ===========================================================================
