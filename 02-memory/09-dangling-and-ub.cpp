// ===========================================================================
//  UNDEFINED BEHAVIOUR — the live-fire range
//  run:  make run  F=02-memory/09-dangling-and-ub.cpp
//  THEN: make asan F=02-memory/09-dangling-and-ub.cpp   <- the whole point
// ===========================================================================
//
//  UNDEFINED BEHAVIOUR means: the standard imposes NO requirements. Your program
//  may crash, may work, may work until Tuesday, may email your password to a
//  stranger. Crucially, the OPTIMISER IS ALLOWED TO ASSUME UB NEVER HAPPENS,
//  so it will delete your null checks and your overflow checks.
//
//  This is the single biggest difference from Java, where every operation is
//  defined (NPE, ArrayIndexOutOfBounds, arithmetic wraps) and the JVM is a
//  memory-safe sandbox.
//
//  THE SKILL you need is not "avoid UB by being careful." Nobody can do that.
//  The skill is: RUN EVERYTHING UNDER SANITIZERS, ALWAYS.
//
//  Each demo below is guarded by a #define. Turn ONE on at a time, rebuild with
//  `make asan`, and read the report. Learning to read ASan output is the highest
//  value-per-minute skill in C++.
//
// ===========================================================================

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ---- flip these to 1, ONE AT A TIME, then run `make asan` on this file ----
#define UB_STACK_USE_AFTER_RETURN   0
#define UB_HEAP_USE_AFTER_FREE      0
#define UB_DOUBLE_FREE              0
#define UB_BUFFER_OVERFLOW          0
#define UB_ITERATOR_INVALIDATION    0
#define UB_DANGLING_STRING_VIEW     0
#define UB_UNINITIALISED_READ       0
#define UB_SIGNED_OVERFLOW          0
#define UB_MEMORY_LEAK              0

// ===========================================================================
// 1. DANGLING POINTER TO A LOCAL  (stack-use-after-return)
// ===========================================================================
#if UB_STACK_USE_AFTER_RETURN
int* make_dangling() {
    int local = 42;
    return &local;          // ☠️ `local` dies here
}
#endif

void demo_1() {
    std::cout << "\n=== 1. Dangling pointer to a local ===\n";
    std::cout <<
      "    int* f() { int x = 42; return &x; }   // x's storage is reused\n"
      "  Symptoms: reads garbage, or 'works' until a function call overwrites\n"
      "  that stack slot. ASan flag: -fsanitize=address detects\n"
      "  'stack-use-after-return' (needs ASAN_OPTIONS=detect_stack_use_after_return=1).\n";
#if UB_STACK_USE_AFTER_RETURN
    int* p = make_dangling();
    std::cout << "  reading dangling: " << *p << "\n";
#endif
}

// ===========================================================================
// 2. USE AFTER FREE
// ===========================================================================
void demo_2() {
    std::cout << "\n=== 2. Heap use-after-free ===\n";
    std::cout <<
      "    int* p = new int(1); delete p; std::cout << *p;\n"
      "  The allocator may have already reused those bytes. This is the #1\n"
      "  exploitable C++ vulnerability class (attacker sprays the heap so the\n"
      "  freed slot holds a pointer they control).\n"
      "  ASan report: 'heap-use-after-free' with BOTH the alloc and free stacks.\n";
#if UB_HEAP_USE_AFTER_FREE
    int* p = new int(1);
    delete p;
    std::cout << "  " << *p << "\n";
#endif
}

// ===========================================================================
// 3. DOUBLE FREE
// ===========================================================================
void demo_3() {
    std::cout << "\n=== 3. Double free ===\n";
    std::cout <<
      "    delete p; delete p;   // corrupts the allocator's free list\n"
      "  Classic cause: a class with a destructor but no copy control\n"
      "  (see 02-memory/06-rule-of-five.cpp part A).\n";
#if UB_DOUBLE_FREE
    int* p = new int(1);
    delete p;
    delete p;
#endif
}

// ===========================================================================
// 4. BUFFER OVERFLOW  (the CVE factory)
// ===========================================================================
void demo_4() {
    std::cout << "\n=== 4. Buffer overflow ===\n";
    std::cout <<
      "    char buf[8]; std::strcpy(buf, \"way too long\");  // ☠️\n"
      "    std::vector<int> v(4); v[10] = 1;                // ☠️ no bounds check\n"
      "  On the stack this overwrites the SAVED RETURN ADDRESS -> arbitrary code\n"
      "  execution. This one bug class is responsible for decades of CVEs.\n"
      "  Defences: std::string/std::vector/std::span, .at(), snprintf not sprintf,\n"
      "  ASan in CI, and -fstack-protector-strong in production builds.\n";
#if UB_BUFFER_OVERFLOW
    std::vector<int> v(4);
    v[10] = 1;                     // ASan: heap-buffer-overflow
    char buf[8];
    std::strcpy(buf, "way way too long");   // ASan: stack-buffer-overflow
    std::cout << buf << "\n";
#endif
}

// ===========================================================================
// 5. ITERATOR / REFERENCE INVALIDATION  (the sneakiest one)
// ===========================================================================
void demo_5() {
    std::cout << "\n=== 5. Iterator & reference invalidation ===\n";

    std::vector<int> v{1, 2, 3};
    std::cout << "  v.capacity() = " << v.capacity() << "\n";
    int& ref = v[0];
    int* ptr = v.data();
    auto it  = v.begin();
    std::cout << "  took a reference, a pointer, and an iterator to v[0]\n";

    v.push_back(4);   // capacity exceeded -> new buffer, old one FREED
    std::cout << "  after push_back, capacity = " << v.capacity()
              << " and data() moved from " << (void*)ptr
              << " to " << (void*)v.data() << "\n";
    std::cout << "  -> ref, ptr, and it are ALL dangling now.\n";
    (void)ref; (void)it;

#if UB_ITERATOR_INVALIDATION
    std::cout << "  reading through the stale pointer: " << *ptr << "\n";  // ☠️
#endif

    std::cout <<
      "\n  INVALIDATION RULES (memorise the vector row at minimum):\n"
      "    vector       : ANY growth invalidates everything. erase invalidates\n"
      "                   from the erase point onward.\n"
      "    deque        : insert/erase in the middle -> all iterators; at the\n"
      "                   ends -> iterators invalid but REFERENCES stay valid.\n"
      "    list/fwd_list: only the erased element.\n"
      "    map/set      : only the erased element (node-based, stable addresses).\n"
      "    unordered_*  : rehash invalidates ITERATORS but not references/pointers.\n"
      "\n  This is the C++ analogue of ConcurrentModificationException — except\n"
      "  it does not throw. It silently reads freed memory.\n";
}

// ===========================================================================
// 6. DANGLING string_view / span
// ===========================================================================
std::string make_string() { return "a temporary string long enough to heap-allocate"; }

void demo_6() {
    std::cout << "\n=== 6. Dangling string_view ===\n";
    std::cout <<
      "    std::string_view sv = make_string();   // ☠️ temp dies at the ;\n"
      "    std::string_view f() { std::string s; return s; }   // ☠️\n"
      "    struct Cfg { std::string_view name; };  // ☠️ if it outlives the owner\n"
      "\n"
      "  string_view and span are BORROWED views. They are perfect as function\n"
      "  parameters and short-lived locals, and dangerous as data members or\n"
      "  return values.\n";
#if UB_DANGLING_STRING_VIEW
    std::string_view sv = make_string();     // ☠️
    std::cout << "  " << sv << "\n";
#endif
    // The safe version:
    std::string owner = make_string();
    std::string_view sv = owner;             // ✅ owner outlives sv
    std::cout << "  safe version: '" << sv.substr(0, 20) << "...'\n";
}

// ===========================================================================
// 7. UNINITIALISED READ
// ===========================================================================
void demo_7() {
    std::cout << "\n=== 7. Reading uninitialised memory ===\n";
    std::cout <<
      "    int x;  if (x == 42) ...    // ☠️ UB — not 'reads garbage', UNDEFINED\n"
      "  The optimiser may assume any value it likes, including different values\n"
      "  on two reads of the same variable.\n"
      "  Detector: MemorySanitizer (-fsanitize=memory) — Linux/clang only,\n"
      "  not available on macOS. So on this machine: ALWAYS write `int x{};`\n"
      "  Also: -Wuninitialized catches the easy cases at compile time.\n";
#if UB_UNINITIALISED_READ
    int x;
    if (x == 42) std::cout << "  the compiler may go either way\n";
#endif
}

// ===========================================================================
// 8. SIGNED OVERFLOW  (the one that deletes your safety checks)
// ===========================================================================
void demo_8() {
    std::cout << "\n=== 8. Signed integer overflow ===\n";
    std::cout <<
      "    int x = INT_MAX; x + 1;      // ☠️ UB (unsigned would just wrap)\n"
      "\n"
      "  THE REASON THIS MATTERS: the optimiser assumes UB is unreachable, so\n"
      "      if (x + 1 < x) { /* overflow check */ }\n"
      "  is compiled to `if (false)` — YOUR CHECK IS DELETED. This has caused\n"
      "  real CVEs. Correct approaches:\n"
      "      if (x > INT_MAX - 1)                        // check BEFORE\n"
      "      __builtin_add_overflow(a, b, &result)       // compiler builtin\n"
      "      use unsigned types where wrapping is intended\n"
      "  Detector: UBSan (-fsanitize=undefined), which the Makefile's `asan`\n"
      "  target enables.\n";
#if UB_SIGNED_OVERFLOW
    int x = 2147483647;
    volatile int y = x + 1;      // UBSan: signed integer overflow
    std::cout << "  " << y << "\n";
#endif
}

// ===========================================================================
// 9. LEAK
// ===========================================================================
void demo_9() {
    std::cout << "\n=== 9. Memory leak ===\n";
    std::cout <<
      "  Not technically UB — just unbounded RSS growth until the OOM killer\n"
      "  arrives at 3am.\n"
      "\n"
      "  ⚠️ PLATFORM NOTE: LeakSanitizer (ASan's leak detector) is NOT SUPPORTED\n"
      "     on macOS/arm64 — setting ASAN_OPTIONS=detect_leaks=1 makes the\n"
      "     program abort at startup. On this machine use Apple's own tool:\n"
      "         make leaks F=02-memory/09-dangling-and-ub.cpp\n"
      "     which runs:\n"
      "         MallocStackLogging=1 leaks --atExit -- ./your-binary\n"
      "     and prints the allocation stack of every leaked block.\n"
      "     On LINUX, ASan detects leaks by default and prints them at exit.\n";
#if UB_MEMORY_LEAK
    int* leaked = new int[1000];
    leaked[0] = 1;
#endif
}

// ===========================================================================
// THE COMPLETE UB CATALOGUE (read this at least once)
// ===========================================================================
void the_catalogue() {
    std::cout << "\n=== The full list of things that are UB in C++ ===\n";
    std::cout <<
      "  MEMORY\n"
      "   - dereferencing null, dangling, or uninitialised pointers\n"
      "   - out-of-bounds array/container access (even just COMPUTING the address)\n"
      "   - use after free / double free / mismatched new-delete forms\n"
      "   - reading uninitialised variables\n"
      "   - misaligned access; violating strict aliasing (reading bytes as the\n"
      "     wrong type — use memcpy or std::bit_cast instead)\n"
      "   - modifying a string literal or any const object\n"
      "\n  ARITHMETIC\n"
      "   - signed integer overflow (unsigned wrapping is FINE)\n"
      "   - division/modulo by zero\n"
      "   - shifting by >= the bit width, or shifting a negative left\n"
      "   - converting a float to an int that cannot represent it\n"
      "\n  OBJECTS & LIFETIME\n"
      "   - using an object before its constructor or after its destructor\n"
      "   - calling a virtual function from a constructor or destructor and\n"
      "     expecting derived behaviour (it is NOT UB, but it silently calls the\n"
      "     base version — Java calls the derived one. Real portability trap.)\n"
      "   - deleting through a base pointer with no virtual destructor\n"
      "   - infinite loop with no side effects (yes, really)\n"
      "   - falling off the end of a value-returning function\n"
      "\n  CONCURRENCY\n"
      "   - a data race: two threads, same memory, at least one write, no\n"
      "     synchronisation. In Java this gives you a stale/torn value; in C++\n"
      "     it is UB and the optimiser may do anything.\n"
      "   - destroying a joinable std::thread -> std::terminate (defined, but fatal)\n"
      "\n  OTHER\n"
      "   - violating the One Definition Rule across translation units\n"
      "   - a noexcept function that throws -> std::terminate\n"
      "   - a destructor that throws during unwinding -> std::terminate\n";
}

void your_defences() {
    std::cout << "\n=== YOUR DEFENCES (in order of value) ===\n";
    std::cout <<
      "  1. TYPES. Use std::string, std::vector, std::span, std::unique_ptr,\n"
      "     std::optional. Most UB is unreachable if you never touch raw memory.\n"
      "  2. SANITIZERS in every test run:\n"
      "       -fsanitize=address,undefined       (make asan)\n"
      "       -fsanitize=thread                  (data races; separate build)\n"
      "       -fsanitize=memory                  (uninit reads; Linux only)\n"
      "  3. WARNINGS as errors: -Wall -Wextra -Wpedantic -Werror\n"
      "  4. -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE\n"
      "     -> makes libc++'s operator[] bounds-checked, even in release. Cheap.\n"
      "  5. Static analysis: clang-tidy (bugprone-*, cppcoreguidelines-*),\n"
      "     and the compiler's own -fanalyzer / --analyze.\n"
      "  6. Fuzzing for anything that parses input: -fsanitize=fuzzer\n"
      "  7. Code review focused on ONE question: who owns this, and when does\n"
      "     it die?\n"
      "\n  Do 1 and 2 and you have eliminated ~95% of the risk. That is the\n"
      "  honest answer to 'isn't C++ unsafe?' — it is, and the tooling is very\n"
      "  good, and you must actually use it.\n";
}

int main() {
    demo_1(); demo_2(); demo_3(); demo_4(); demo_5();
    demo_6(); demo_7(); demo_8(); demo_9();
    the_catalogue();
    your_defences();
    std::cout << "\n>>> NOW GO: set one #define at the top to 1 and run\n"
                 "    `make asan F=02-memory/09-dangling-and-ub.cpp`.\n"
                 "    Read the report. Do this for all nine. It is the single\n"
                 "    best hour you can spend learning C++.\n";
    return 0;
}
