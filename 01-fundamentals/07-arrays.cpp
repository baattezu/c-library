// ===========================================================================
//  ARRAYS — C arrays, std::array, std::vector, std::span
//  run: make run F=01-fundamentals/07-arrays.cpp
// ===========================================================================
//
//  Java has one array: `int[] a = new int[10]`. It's a heap object, it knows its
//  length, it bounds-checks every access, and the GC owns it.
//
//  C++ has four things, and choosing correctly is a real skill:
//
//    int a[10]           C array   — raw, no size info, decays to a pointer
//    std::array<int,10>  fixed     — size in the TYPE, stack, zero overhead
//    std::vector<int>    dynamic   — heap, growable. The default choice.
//    std::span<int>      view      — non-owning (pointer + length). For params.
//
// ===========================================================================

#include <algorithm>
#include <array>
#include <iostream>
#include <numeric>
#include <span>
#include <vector>

// ---------------------------------------------------------------------------
// 1. C ARRAYS AND THE DECAY PROBLEM
// ---------------------------------------------------------------------------
// ⚠️ Look at this signature. It LOOKS like it takes an array of 10 ints.
// It does not. It takes an `int*`. The `[10]` is a lie the compiler ignores.
// This is "array-to-pointer decay" and it is the source of endless C bugs:
// inside this function, sizeof(a) is 8 (a pointer), not 40.
void takes_c_array(int a[10]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsizeof-array-argument"   // demonstrating the bug
    std::cout << "  inside takes_c_array: sizeof(a)=" << sizeof(a)
              << "  <- it's a POINTER, the size info is GONE\n";
#pragma clang diagnostic pop
    a[0] = 111;   // and there is no way to bounds-check
}

void demo_c_arrays() {
    std::cout << "\n=== 1. C arrays ===\n";

    int a[5] = {1, 2, 3, 4, 5};
    int b[5] = {};        // all zeros
    int c[]  = {1, 2, 3}; // size deduced == 3

    std::cout << "  sizeof(a)=" << sizeof(a)
              << " -> count = " << (sizeof(a)/sizeof(a[0]))
              << "  (the old C idiom)\n";
    std::cout << "  std::size(a)=" << std::size(a) << "  (C++17, safer)\n";
    std::cout << "  b[0]=" << b[0] << " sizeof(c)=" << sizeof(c) << "\n";

    takes_c_array(a);
    std::cout << "  a[0] is now " << a[0] << "\n";

    // 2D arrays are CONTIGUOUS, unlike Java's array-of-arrays.
    //   Java:  int[][] m = new int[3][4]  -> 1 array of 3 references + 3 arrays.
    //          Four separate heap objects, scattered. Row access = 2 derefs.
    //   C++:   int m[3][4]                -> 48 contiguous bytes. One object.
    //          m[i][j] is *(base + i*4 + j). One deref, perfect locality.
    // This alone can make numeric C++ several times faster than Java.
    int m[3][4] = {{1,2,3,4},{5,6,7,8},{9,10,11,12}};
    std::cout << "  2D array is " << sizeof(m) << " CONTIGUOUS bytes; "
                 "&m[0][0]=" << static_cast<void*>(&m[0][0])
              << " &m[1][0]=" << static_cast<void*>(&m[1][0])
              << " (delta " << (reinterpret_cast<char*>(&m[1][0]) -
                                reinterpret_cast<char*>(&m[0][0])) << ")\n";
}

// ---------------------------------------------------------------------------
// 2. std::array — a C array that behaves like a proper value
// ---------------------------------------------------------------------------
void demo_std_array() {
    std::cout << "\n=== 2. std::array<T,N> ===\n";

    std::array<int, 5> a{1, 2, 3, 4, 5};

    std::cout << "  size()=" << a.size() << " sizeof=" << sizeof(a)
              << "  (ZERO overhead vs int[5] — the size lives in the TYPE)\n";

    // Unlike a C array, std::array:
    //   - knows its size (as a compile-time constant)
    //   - can be copied, assigned, returned from a function, compared with ==
    //   - has begin()/end() so all algorithms work
    //   - offers .at(i) which THROWS std::out_of_range (Java-like)
    std::array<int, 5> copy = a;      // a real copy, unlike C arrays
    copy[0] = 99;
    std::cout << "  a[0]=" << a[0] << " copy[0]=" << copy[0]
              << "  equal? " << std::boolalpha << (a == copy) << "\n";

    // .at() is checked; operator[] is NOT. Know which you're using.
    try { std::cout << a.at(10) << "\n"; }
    catch (const std::out_of_range& e) { std::cout << "  a.at(10) threw: " << e.what() << "\n"; }
    // a[10];   // UB, silent, no exception. This is the C++ default.

    std::cout << "  sum via std::accumulate: "
              << std::accumulate(a.begin(), a.end(), 0) << "\n";
}

// ---------------------------------------------------------------------------
// 3. std::vector — the workhorse. Understand its GROWTH.
// ---------------------------------------------------------------------------
void demo_vector() {
    std::cout << "\n=== 3. std::vector — and the geometric growth you must know ===\n";

    std::vector<int> v;
    std::cout << "  watching capacity as we push_back 20 times:\n    ";
    std::size_t last_cap = 0;
    for (int i = 0; i < 20; ++i) {
        v.push_back(i);
        if (v.capacity() != last_cap) {
            std::cout << "size=" << v.size() << "/cap=" << v.capacity() << "  ";
            last_cap = v.capacity();
        }
    }
    std::cout << "\n";

    // libc++ (this platform) grows by 2x. libstdc++ also 2x. MSVC uses 1.5x.
    // AMORTISED O(1) push_back: doubling means n pushes cost O(n) total copies.
    // But each growth is: allocate new buffer, MOVE/COPY every element, free old.
    //
    // ⭐ THE #1 EASY OPTIMISATION IN ALL OF C++:
    //     v.reserve(expected_size);
    // If you know (or can estimate) the final size, one allocation replaces
    // log2(n) allocations plus O(n) element moves. In a hot path this is often
    // a 2-5x win for free.
    std::vector<int> w;
    w.reserve(1000);
    std::cout << "  after reserve(1000): size=" << w.size()
              << " cap=" << w.capacity() << "  (no elements yet, memory ready)\n";

    // size vs capacity:
    //   size()     = how many elements exist (constructed)
    //   capacity() = how many fit before the next reallocation
    //   resize(n)  = CHANGES size, constructing/destroying elements
    //   reserve(n) = changes ONLY capacity. Never changes size.
    //   shrink_to_fit() = non-binding request to release spare capacity
    //   clear()    = size -> 0, but capacity is KEPT (deliberately! reuse it)

    // Layout: a vector is 3 pointers (begin, end, capacity_end) = 24 bytes on
    // the stack, plus one heap allocation for the elements.
    std::cout << "  sizeof(std::vector<int>) = " << sizeof(std::vector<int>)
              << " bytes (3 pointers) + one heap buffer\n";

    // ⚠️ std::vector<bool> IS NOT A VECTOR. It's a bit-packed specialisation:
    // operator[] returns a PROXY object, not bool&. You cannot take &v[0].
    // It is a famous standard-library mistake. Use std::vector<char>,
    // std::deque<bool>, or std::bitset<N> instead.
    std::vector<bool> bits{true, false, true};
    std::cout << "  vector<bool> is bit-packed (a proxy type) — avoid it. "
                 "size=" << bits.size() << "\n";

    // emplace_back vs push_back:
    //   push_back(T)      constructs a T, then moves/copies it in
    //   emplace_back(args...) constructs the T IN PLACE from the args. No temp.
    struct P { int x, y; P(int a, int b) : x(a), y(b) {} };
    std::vector<P> ps;
    ps.push_back(P{1,2});     // build temp, then move
    ps.emplace_back(3, 4);    // build directly in the vector's storage
    std::cout << "  emplace_back constructs in place: ps[1]={"
              << ps[1].x << "," << ps[1].y << "}\n";
}

// ---------------------------------------------------------------------------
// 4. std::span — how you should write array parameters in C++20
// ---------------------------------------------------------------------------
// A span is a (pointer, length) pair. Non-owning, 16 bytes, trivially copyable.
// It accepts C arrays, std::array, std::vector, and any contiguous range — so
// you write ONE function instead of three overloads, and you never lose the size.
double average(std::span<const double> xs) {
    if (xs.empty()) return 0.0;
    double sum = 0;
    for (double x : xs) sum += x;      // range-for works; .size() is known
    return sum / static_cast<double>(xs.size());
}

void demo_span() {
    std::cout << "\n=== 4. std::span — the right way to pass arrays ===\n";

    double c_arr[]              = {1.0, 2.0, 3.0};
    std::array<double, 3> s_arr = {4.0, 5.0, 6.0};
    std::vector<double> vec     = {7.0, 8.0, 9.0};

    std::cout << "  avg(C array)   = " << average(c_arr) << "\n";
    std::cout << "  avg(std::array)= " << average(s_arr) << "\n";
    std::cout << "  avg(vector)    = " << average(vec)   << "\n";
    std::cout << "  avg(subspan)   = " << average(std::span{vec}.subspan(1)) << "\n";
    std::cout << "  sizeof(span)   = " << sizeof(std::span<const double>)
              << " bytes (ptr + len)\n";

    // ⚠️ A span does NOT own. If the underlying vector reallocates or dies, the
    // span dangles. Same rule as always: know the lifetime.
}

// ---------------------------------------------------------------------------
// 5. WHICH ONE DO I USE?
// ---------------------------------------------------------------------------
void demo_decision_guide() {
    std::cout << "\n=== 5. Decision guide ===\n";
    std::cout <<
      "  Size known at COMPILE time, small, want it on the stack?  std::array\n"
      "  Size known only at RUNTIME, or it grows?                   std::vector\n"
      "  Just passing a contiguous range to a function?             std::span\n"
      "  Talking to a C API?                                        v.data(), v.size()\n"
      "  A raw C array?                                             almost never\n"
      "\n"
      "  For a Java dev: std::vector IS your ArrayList, but with the elements\n"
      "  stored INLINE and CONTIGUOUSLY. ArrayList<Integer> in Java is an array of\n"
      "  POINTERS to boxed Integers scattered on the heap — every element access\n"
      "  is a potential cache miss. std::vector<int> is 4*n contiguous bytes.\n"
      "  That is often a 5-10x difference on a traversal-heavy workload, and it\n"
      "  is the single clearest illustration of why this language matters.\n";
}

int main() {
    demo_c_arrays();
    demo_std_array();
    demo_vector();
    demo_span();
    demo_decision_guide();
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * C arrays DECAY to pointers when passed to functions — size info is lost.
//    `void f(int a[10])` is really `void f(int* a)`.
//  * std::array<T,N>: size in the type, zero overhead, copyable, comparable.
//  * std::vector<T>: 3 pointers + one heap buffer. Grows 2x (libc++/libstdc++).
//    push_back is amortised O(1). **reserve() when you know the size.**
//  * size() vs capacity(); resize() vs reserve(); clear() keeps capacity.
//  * `at()` throws, `operator[]` is unchecked UB.
//  * std::vector<bool> is a bit-packed proxy type — avoid it.
//  * emplace_back constructs in place; push_back moves a temporary in.
//  * std::span<const T> is the modern array parameter: works with everything,
//    keeps the length, owns nothing.
//  * 2D C arrays are contiguous; Java's int[][] is an array of references.
// ===========================================================================
