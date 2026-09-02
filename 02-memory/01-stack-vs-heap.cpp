// ===========================================================================
//  STACK vs HEAP — measure the difference, don't take my word for it
//  run:  make opt F=02-memory/01-stack-vs-heap.cpp    <- MUST be -O2
// ===========================================================================

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <vector>

// A helper that stops the optimiser from deleting work we want to measure.
// `asm volatile("" : : "r,m"(v) : "memory")` tells the compiler "this value
// escaped, you can't prove it's unused". Every benchmark needs one of these.
template <class T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

struct Small { int a, b, c, d; };                   // 16 bytes

// ---------------------------------------------------------------------------
void where_does_it_live() {
    std::cout << "\n=== 1. Where do things live? ===\n";

    static int  s_static = 1;                        // .data segment
    thread_local int t_local = 2;                    // thread-local storage
    int         a_stack = 3;                         // stack
    auto        h_heap  = std::make_unique<int>(4);  // heap
    const char* literal = "in .rodata";              // read-only segment

    std::printf("  static      %p\n", (void*)&s_static);
    std::printf("  thread_loc  %p\n", (void*)&t_local);
    std::printf("  literal     %p   \"%s\"\n", (void*)literal, literal);
    std::printf("  heap        %p\n", (void*)h_heap.get());
    std::printf("  stack       %p\n", (void*)&a_stack);
    std::cout <<
      "  Note the magnitudes: .data/.rodata are low, heap is in the middle,\n"
      "  stack is very high. Addresses are randomised (ASLR) each run.\n";
}

// ---------------------------------------------------------------------------
void measure_allocation_cost() {
    std::cout << "\n=== 2. Allocation cost ===\n";

    constexpr int N = 2'000'000;

    // STACK: the compiler bumps the stack pointer ONCE for the whole frame.
    // Creating `Small` here is literally zero instructions beyond the stores.
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        Small s{i, i, i, i};
        do_not_optimize(s);
    }
    auto t1 = std::chrono::steady_clock::now();

    // HEAP: each iteration calls operator new -> malloc -> size class lookup ->
    // thread cache -> maybe a lock -> return pointer; then delete does the
    // reverse. Plus the memory is somewhere else entirely (cache miss later).
    for (int i = 0; i < N; ++i) {
        auto p = std::make_unique<Small>(Small{i, i, i, i});
        do_not_optimize(*p);
    }
    auto t2 = std::chrono::steady_clock::now();

    auto ns = [](auto a, auto b, int n) {
        return std::chrono::duration<double, std::nano>(b - a).count() / n;
    };
    std::printf("  stack construct+destroy: %7.2f ns/op\n", ns(t0, t1, N));
    std::printf("  heap  new+delete:        %7.2f ns/op\n", ns(t1, t2, N));
    std::printf("  ratio: %.1fx\n", ns(t1, t2, N) / (ns(t0, t1, N) + 0.01));
    std::cout <<
      "\n  In Java EVERY object is the second case (though Java's bump-pointer\n"
      "  TLAB allocation is faster than malloc — maybe 10-15ns — the cost is\n"
      "  deferred to GC instead). In C++ most objects are the first case, which\n"
      "  is genuinely, provably FREE.\n";
}

// ---------------------------------------------------------------------------
void measure_locality() {
    std::cout << "\n=== 3. Contiguous vs scattered (the real cost) ===\n";

    constexpr int N = 1'000'000;

    // (a) vector<Small>: ONE allocation, N*16 contiguous bytes.
    std::vector<Small> contiguous(N);
    for (int i = 0; i < N; ++i) contiguous[i] = {i, i, i, i};

    // (b) vector<unique_ptr<Small>>: N+1 allocations, elements scattered.
    //     THIS IS THE MEMORY LAYOUT OF ArrayList<Small> IN JAVA.
    std::vector<std::unique_ptr<Small>> scattered;
    scattered.reserve(N);
    for (int i = 0; i < N; ++i) scattered.push_back(std::make_unique<Small>(Small{i,i,i,i}));

    auto t0 = std::chrono::steady_clock::now();
    long long sum1 = 0;
    for (int rep = 0; rep < 10; ++rep)
        for (const auto& s : contiguous) sum1 += s.a;
    auto t1 = std::chrono::steady_clock::now();

    long long sum2 = 0;
    for (int rep = 0; rep < 10; ++rep)
        for (const auto& p : scattered) sum2 += p->a;
    auto t2 = std::chrono::steady_clock::now();

    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::printf("  contiguous  vector<Small>:            %7.2f ms (sum=%lld)\n", ms(t0,t1), sum1);
    std::printf("  scattered   vector<unique_ptr<Small>>: %7.2f ms (sum=%lld)\n", ms(t1,t2), sum2);
    std::printf("  slowdown from indirection: %.1fx\n", ms(t1,t2)/ms(t0,t1));

    std::cout <<
      "\n  SAME algorithm. SAME number of elements. SAME big-O.\n"
      "  The only difference is whether the bytes are next to each other.\n"
      "  Note: the scattered version is actually *flattered* here, because we\n"
      "  allocated them all in one burst so the allocator kept them nearby. In a\n"
      "  long-running server with mixed allocation, it gets much worse.\n";
}

// ---------------------------------------------------------------------------
void stack_limits() {
    std::cout << "\n=== 4. Stack size limits ===\n";
    std::cout <<
      "  macOS main thread stack:  8 MB (ulimit -s says " ;
    std::system("ulimit -s");
    std::cout <<
      "  macOS non-main threads:   512 KB by DEFAULT (!)\n"
      "  Linux threads:            8 MB by default\n"
      "\n"
      "  So this is a crash, not an exception:\n"
      "      void f() { char buf[16*1024*1024]; }   // stack overflow -> SIGSEGV\n"
      "\n"
      "  And THIS is why you cannot have a million threads:\n"
      "      1,000,000 threads x 512 KB = 512 GB of stack.\n"
      "  Even with lazy page commit, the kernel bookkeeping and context switching\n"
      "  destroy you. That single number is why every high-concurrency system\n"
      "  (nginx, Netty, Node, Go, Seastar, io_uring) uses an EVENT LOOP or\n"
      "  lightweight coroutines instead of thread-per-connection. See 08-systems/.\n";
}

// ---------------------------------------------------------------------------
void the_rules() {
    std::cout << "\n=== 5. Decision rules ===\n";
    std::cout <<
      "  USE THE STACK when:\n"
      "    - the object's lifetime matches a scope (this is most objects)\n"
      "    - the size is known at compile time and is modest (< ~100 KB)\n"
      "    - you want zero allocation cost and perfect cache locality\n"
      "\n"
      "  USE THE HEAP when:\n"
      "    - the size is only known at runtime (-> std::vector, which heaps for you)\n"
      "    - the object must outlive the creating scope\n"
      "    - the object is large (MBs)\n"
      "    - you need polymorphism through a base pointer\n"
      "    - the object must be shared with unclear lifetime (-> shared_ptr)\n"
      "\n"
      "  AND THE REAL ANSWER: prefer a std::vector<T> of VALUES over a\n"
      "  std::vector<unique_ptr<T>>. One allocation, contiguous, no indirection.\n"
      "  Reach for pointers only when you need polymorphism or stable addresses.\n";
}

int main() {
    where_does_it_live();
    measure_allocation_cost();
    measure_locality();
    stack_limits();
    the_rules();
    std::cout << "\n>>> Run this with `make opt` — at -O0 the numbers are noise.\n";
    return 0;
}
