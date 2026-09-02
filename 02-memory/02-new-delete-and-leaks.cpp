// ===========================================================================
//  new / delete: what they REALLY do, and every way they go wrong
//  run:  make run  F=02-memory/02-new-delete-and-leaks.cpp
//  then: make asan F=02-memory/02-new-delete-and-leaks.cpp
// ===========================================================================
//
//  You will (almost) never write new/delete in production C++. But you must
//  understand them, because:
//    - smart pointers are thin wrappers around them
//    - you'll read old code that uses them
//    - you'll write an allocator eventually (09-projects/01)
//    - the failure modes are the failure modes of ALL C++ memory bugs
//
// ===========================================================================

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <string>

// ---------------------------------------------------------------------------
// A tracking allocator so we can SEE every allocation. Replacing global
// operator new/delete is legal and is how leak trackers and pool allocators
// hook into everything.
// ---------------------------------------------------------------------------
namespace track {
    inline std::size_t live_bytes  = 0;
    inline std::size_t total_allocs = 0;
    inline bool        verbose      = false;
}

void* operator new(std::size_t n) {
    // We must use malloc, not new (that would recurse forever).
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc{};          // the required behaviour
    track::live_bytes += n;
    ++track::total_allocs;
    if (track::verbose) std::printf("    [new  %4zu bytes -> %p]\n", n, p);
    return p;
}
void operator delete(void* p) noexcept {
    if (track::verbose && p) std::printf("    [delete %p]\n", p);
    std::free(p);
}
void operator delete(void* p, std::size_t n) noexcept {   // sized delete (C++14)
    if (p) track::live_bytes -= n;
    if (track::verbose && p) std::printf("    [delete %4zu bytes @ %p]\n", n, p);
    std::free(p);
}

// ---------------------------------------------------------------------------
struct Resource {
    int id;
    char payload[64];
    explicit Resource(int i) : id(i), payload{} {
        std::snprintf(payload, sizeof(payload), "resource-%d", i);
        std::printf("    Resource(%d) ctor\n", id);
    }
    ~Resource() { std::printf("    ~Resource(%d) dtor\n", id); }
};

void demo_what_new_does() {
    std::cout << "\n=== 1. `new T(args)` is TWO operations ===\n";
    track::verbose = true;

    // Step 1: operator new(sizeof(Resource))  -> raw bytes
    // Step 2: placement-construct Resource in those bytes
    Resource* r = new Resource(1);

    // `delete r` is also two operations, in the opposite order:
    // Step 1: r->~Resource()                 -> run the destructor
    // Step 2: operator delete(r)             -> return the bytes
    delete r;

    track::verbose = false;
    std::cout << "  -> new = allocate + construct.  delete = destruct + deallocate.\n"
                 "     You can do them separately (see 08-placement-new-and-arena).\n";
}

void demo_arrays() {
    std::cout << "\n=== 2. new[] / delete[] ===\n";

    // For a type with a destructor, new[] stores the ELEMENT COUNT in a hidden
    // "cookie" just before the returned pointer, so delete[] knows how many
    // destructors to run. That's why you must not mix new[] with delete.
    Resource* arr = new Resource[3]{Resource(10), Resource(11), Resource(12)};
    std::cout << "  arr[1].payload = " << arr[1].payload << "\n";
    delete[] arr;                     // runs 3 destructors, in REVERSE order

    std::cout <<
      "\n  ⚠️ MIXING IS UNDEFINED BEHAVIOUR:\n"
      "     new  X   ... delete[] p   -> UB\n"
      "     new  X[] ... delete   p   -> UB (only 1 destructor runs; heap corrupted)\n"
      "     malloc   ... delete   p   -> UB (no constructor ran; wrong deallocator)\n"
      "     new  X   ... free(p)      -> UB (destructor never runs)\n"
      "\n  This is why `std::vector<T>` and `std::make_unique<T[]>(n)` exist.\n"
      "  Never write new[] in real code.\n";
}

void demo_leaks() {
    std::cout << "\n=== 3. The four failure modes ===\n";
    std::cout <<
      "  Uncomment ONE at a time in the source and run `make asan` on this file:\n"
      "\n"
      "  (a) LEAK:\n"
      "        Resource* r = new Resource(99);   // never deleted\n"
      "      -> ASan LeakSanitizer prints the allocation stack at exit.\n"
      "\n"
      "  (b) DOUBLE FREE:\n"
      "        Resource* r = new Resource(1); delete r; delete r;\n"
      "      -> ASan: 'attempting double-free'\n"
      "\n"
      "  (c) USE AFTER FREE:\n"
      "        Resource* r = new Resource(1); delete r; std::cout << r->id;\n"
      "      -> ASan: 'heap-use-after-free', with BOTH the free and alloc stacks\n"
      "\n"
      "  (d) BUFFER OVERFLOW:\n"
      "        int* p = new int[4]; p[4] = 0;\n"
      "      -> ASan: 'heap-buffer-overflow'\n"
      "\n"
      "  ASan works by putting POISONED 'redzones' around every allocation and\n"
      "  keeping freed memory in quarantine. That's why it costs ~2x speed and\n"
      "  ~3x memory — and why you run it in CI, not production.\n";

    // Uncomment to demo:
    // Resource* leaked = new Resource(99);  (void)leaked;
}

void demo_exception_safety() {
    std::cout << "\n=== 4. Why raw new is exception-UNSAFE ===\n";
    std::cout <<
      "  void f() {\n"
      "      Resource* a = new Resource(1);\n"
      "      might_throw();               // <-- if this throws, `a` LEAKS.\n"
      "      delete a;                    //     This line never runs.\n"
      "  }\n"
      "\n"
      "  You could write try/catch around it... in EVERY function... forever.\n"
      "  Or you write:\n"
      "      auto a = std::make_unique<Resource>(1);\n"
      "  and the destructor runs during stack unwinding, always, for free.\n"
      "\n"
      "  There is also a famous subtle case:\n"
      "      f(std::unique_ptr<A>(new A), std::unique_ptr<B>(new B));\n"
      "  Pre-C++17 the evaluation order was unspecified, so `new B` could throw\n"
      "  after `new A` succeeded but before its unique_ptr was constructed -> leak.\n"
      "  make_unique fixes it by construction:\n"
      "      f(std::make_unique<A>(), std::make_unique<B>());\n";

    // Demonstrate RAII surviving an exception:
    try {
        auto r = std::make_unique<Resource>(42);
        std::cout << "  about to throw with a live Resource...\n";
        throw std::runtime_error("boom");
    } catch (const std::exception& e) {
        std::cout << "  caught: " << e.what() << "  <- destructor already ran above\n";
    }
}

void demo_new_variants() {
    std::cout << "\n=== 5. The other forms of new ===\n";

    // nothrow new: returns nullptr instead of throwing bad_alloc.
    int* p = new (std::nothrow) int(5);
    if (p) { std::cout << "  nothrow new -> " << *p << "\n"; delete p; }

    // PLACEMENT NEW: construct an object in memory YOU already have.
    // This is the foundation of vectors, arenas, small-buffer optimisation,
    // and every custom allocator. No allocation happens here at all.
    alignas(Resource) unsigned char storage[sizeof(Resource)];
    Resource* r = new (storage) Resource(7);        // construct in `storage`
    std::cout << "  placement new into stack buffer: " << r->payload << "\n";
    r->~Resource();                                  // you MUST destroy manually
    std::cout << "  (called the destructor explicitly — no delete!)\n";

    // Aligned new (C++17): for over-aligned types.
    struct alignas(64) CacheLine { char data[64]; };
    auto* cl = new CacheLine;
    std::printf("  aligned new -> %p (aligned to 64? %s)\n",
                (void*)cl, (reinterpret_cast<std::uintptr_t>(cl) % 64 == 0) ? "yes" : "no");
    delete cl;
}

void demo_allocator_stats() {
    std::cout << "\n=== 6. What the tracker saw ===\n";
    std::printf("  total allocations this run: %zu\n", track::total_allocs);
    std::printf("  bytes still live:           %zu\n", track::live_bytes);
    std::cout <<
      "  (Overriding global operator new/delete like we did here is exactly how\n"
      "   leak detectors, heap profilers, and pool allocators hook a program.\n"
      "   In production you'd link jemalloc/tcmalloc instead, which replace\n"
      "   malloc itself and give you profiling for free.)\n";
}

int main() {
    demo_what_new_does();
    demo_arrays();
    demo_leaks();
    demo_exception_safety();
    demo_new_variants();
    demo_allocator_stats();

    std::cout << "\n>>> THE RULE: in application code, the words `new` and `delete`\n"
                 "    should not appear. Use make_unique / make_shared / vector /\n"
                 "    string. Reserve raw new for the inside of your own allocator.\n";
    return 0;
}
