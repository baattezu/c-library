// ===========================================================================
//  POINTERS — the single concept Java hid from you completely
//  run:  make run  F=01-fundamentals/09-pointers.cpp
//  then: make asan F=01-fundamentals/09-pointers.cpp   (uncomment the UB sections!)
// ===========================================================================
//
//  A pointer is a VARIABLE WHOSE VALUE IS A MEMORY ADDRESS. That's it. It is an
//  integer, 8 bytes on a 64-bit machine, that happens to be typed so the
//  compiler knows how to interpret the bytes it points at and how far to jump
//  when you do arithmetic on it.
//
//  In Java, EVERY object variable is secretly a pointer. `String s` is a pointer.
//  You just can't (a) see its value, (b) do arithmetic on it, (c) point it at
//  the stack, (d) point it into the middle of an object, or (e) get it wrong.
//
//  In C++ you can do all five. (e) is why C++ has a reputation.
//
//  THE ONE QUESTION YOU MUST ALWAYS BE ABLE TO ANSWER:
//      "Who owns this memory, and when does it die?"
//  If you can't answer instantly, you have a bug you haven't found yet.
//
// ===========================================================================

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

void demo_basics() {
    std::cout << "\n=== 1. Address-of (&) and dereference (*) ===\n";

    int x = 42;
    int* p = &x;      // & = "give me the address of x"

    std::cout << "  x       = " << x  << "\n";
    std::cout << "  &x      = " << static_cast<void*>(&x) << "  (an address)\n";
    std::cout << "  p       = " << static_cast<void*>(p)  << "  (same address)\n";
    std::cout << "  *p      = " << *p << "  (* = 'the thing at that address')\n";
    std::cout << "  sizeof(p) = " << sizeof(p) << " bytes, regardless of what it points to\n";

    *p = 99;          // write THROUGH the pointer
    std::cout << "  after *p = 99, x = " << x << "   <- we modified x remotely\n";

    // Pointer to pointer. Used for out-parameters in C APIs and for arrays of
    // strings (char**), which is what argv is.
    int** pp = &p;
    std::cout << "  **pp    = " << **pp << "\n";

    // nullptr (C++11) is a typed null. Never use NULL or 0 in modern C++.
    int* nothing = nullptr;
    if (!nothing) std::cout << "  nullptr is falsy; dereferencing it is UB (segfault if lucky)\n";
}

void demo_pointer_arithmetic() {
    std::cout << "\n=== 2. Pointer arithmetic — scaled by sizeof(T) ===\n";

    int arr[5] = {10, 20, 30, 40, 50};
    int* p = arr;      // an array DECAYS to a pointer to its first element

    // p + 1 does NOT add 1 byte. It adds sizeof(int) == 4 bytes. The type of the
    // pointer is what makes arithmetic mean anything.
    std::cout << "  p   -> " << *p     << "  at " << static_cast<void*>(p)   << "\n";
    std::cout << "  p+1 -> " << *(p+1) << "  at " << static_cast<void*>(p+1)
              << "   (address advanced by " << sizeof(int) << " bytes)\n";

    // ⭐ THE BIG REVEAL: `arr[i]` is DEFINED as `*(arr + i)`.
    // Which means `arr[2]`, `*(arr+2)`, `*(2+arr)`, and `2[arr]` are all the
    // same thing. (Yes, `2[arr]` compiles. No, don't write it.)
    std::cout << "  arr[2]=" << arr[2] << "  *(arr+2)=" << *(arr+2)
              << "  2[arr]=" << 2[arr] << "\n";

    // Subtracting pointers gives the number of ELEMENTS between them (ptrdiff_t).
    int* last = &arr[4];
    std::cout << "  last - p = " << (last - p) << " elements\n";

    // ⚠️ There is NO BOUNDS CHECKING. arr[7] reads whatever bytes are there.
    // Java throws ArrayIndexOutOfBoundsException. C++ corrupts your stack and
    // keeps going, and you find out three functions later.
    // Uncomment and run under `make asan` to see it caught:
    // std::cout << arr[7] << "\n";   // UB — ASan will print a stack-buffer-overflow

    // The ONLY legal pointers: into an object, one-past-the-end, or null.
    // Even *computing* arr+9 is UB, let alone dereferencing it.
}

// ---------------------------------------------------------------------------
// 3. THE HEAP: new / delete, and why you should never write them
// ---------------------------------------------------------------------------
struct Widget {
    int id;
    std::string name;
    Widget(int i, std::string n) : id(i), name(std::move(n)) {
        std::cout << "    Widget(" << id << ") constructed\n";
    }
    ~Widget() { std::cout << "    ~Widget(" << id << ") destroyed\n"; }
};

void demo_heap() {
    std::cout << "\n=== 3. Heap allocation, manual ===\n";

    // `new` does TWO things: (1) allocate raw bytes from the heap
    //                        (2) run the constructor in those bytes
    // `delete` does TWO things: (1) run the destructor
    //                           (2) return the bytes to the heap
    Widget* w = new Widget(1, "manual");
    std::cout << "  using " << w->name << " (-> is (*w). shorthand)\n";
    delete w;                 // if you forget: LEAK. No GC will save you.
    w = nullptr;              // defensive: nulling avoids accidental reuse

    // Arrays use new[] / delete[] and MIXING THEM IS UB:
    //   new  + delete[]  -> UB
    //   new[] + delete   -> UB (destructors for elements 1..n-1 never run)
    int* nums = new int[4]{1,2,3,4};
    std::cout << "  heap array: ";
    for (int i = 0; i < 4; ++i) std::cout << nums[i] << " ";
    std::cout << "\n";
    delete[] nums;

    std::cout << "\n  >>> IN REAL CODE YOU NEVER WRITE new/delete. <<<\n"
                 "  Use std::make_unique / std::make_shared / std::vector.\n"
                 "  We show raw new/delete once so you know what the smart pointer\n"
                 "  is actually doing. See 02-memory/04-smart-pointers.cpp.\n";
}

void demo_smart_preview() {
    std::cout << "\n=== 4. The modern way (preview) ===\n";
    {
        auto w = std::make_unique<Widget>(2, "unique_ptr");
        std::cout << "  " << w->name << " — I own it, exclusively\n";
    }   // <-- destructor runs HERE, automatically, exactly once, even if an
        //     exception is thrown. This is RAII. It is C++'s answer to GC, and
        //     it is DETERMINISTIC, which GC is not.
    std::cout << "  scope ended, memory freed with zero runtime overhead\n";
}

// ---------------------------------------------------------------------------
// 5. THE FOUR CLASSIC POINTER BUGS
// ---------------------------------------------------------------------------
void demo_the_four_horsemen() {
    std::cout << "\n=== 5. The four classic pointer bugs ===\n";
    std::cout <<
      "  (a) DANGLING POINTER  — pointer outlives what it points to.\n"
      "        int* f() { int x = 5; return &x; }   // x dies at return\n"
      "        Also: holding a pointer/iterator into a vector across push_back.\n"
      "  (b) USE AFTER FREE    — delete p; then *p. The allocator may have\n"
      "        already handed those bytes to someone else. Silent corruption.\n"
      "  (c) DOUBLE FREE       — delete p; delete p;  Corrupts the allocator's\n"
      "        free list. Often crashes far away from the actual bug.\n"
      "  (d) LEAK              — new without delete. Slow death by RSS growth.\n"
      "\n"
      "  ALL FOUR are eliminated by: unique_ptr / shared_ptr / vector / string.\n"
      "  This is not a style preference. Raw owning pointers are a defect.\n";

    // Demonstrate (a) safely-ish: a dangling pointer into a reallocated vector.
    std::vector<int> v{1,2,3};
    int* into_v = &v[0];
    std::cout << "  before push_back: *into_v = " << *into_v
              << " (capacity " << v.capacity() << ")\n";
    v.push_back(4); v.push_back(5); v.push_back(6);   // forces reallocation
    // *into_v is now DANGLING — the old buffer was freed.
    std::cout << "  after push_back: capacity is now " << v.capacity()
              << " and into_v points at freed memory.\n"
                 "  Uncomment the read below + `make asan` to see ASan catch it.\n";
    // std::cout << *into_v << "\n";   // heap-use-after-free
}

// ---------------------------------------------------------------------------
// 6. WHAT POINTERS ARE ACTUALLY *FOR* (beyond "Java references")
// ---------------------------------------------------------------------------
void demo_real_uses() {
    std::cout << "\n=== 6. Legitimate uses of raw pointers in modern C++ ===\n";

    // (a) NON-OWNING OBSERVATION. A raw pointer that never deletes is fine and
    //     idiomatic. Convention: "raw pointer = I observe, I do not own."
    std::string s = "hello";
    const std::string* observer = &s;         // clearly non-owning
    std::cout << "  observing: " << *observer << "\n";

    // (b) OPTIONAL REFERENCE. A reference can't be null; a pointer can.
    //     `void f(Config* maybe_cfg)` means "pass nullptr if you have none."

    // (c) TALKING TO C APIS. Every syscall, every OS API, every third-party C
    //     library speaks pointers. read(fd, buf, n). This is stage 8.

    // (d) RAW BYTES. Serialisation, network protocols, memory-mapped files.
    //     std::byte* / char* / void* are how you say "just bytes".
    struct Header { std::uint32_t magic; std::uint16_t version; std::uint16_t flags; };
    Header h{0xCAFEBABE, 1, 0};
    // Inspect the raw bytes. NOTE: on arm64/x86 this is LITTLE-ENDIAN, so
    // 0xCAFEBABE appears in memory as BE BA FE CA. Network protocols are
    // BIG-endian, which is why you need htonl/ntohl. (See 08-systems/.)
    const auto* bytes = reinterpret_cast<const unsigned char*>(&h);
    std::cout << "  Header bytes: ";
    for (std::size_t i = 0; i < sizeof(h); ++i)
        std::printf("%02X ", bytes[i]);
    std::cout << "  <- little-endian!\n";

    // memcpy is the ONLY portable, non-UB way to reinterpret bytes as another
    // type (aliasing rules). The compiler turns a small memcpy into one load.
    std::uint32_t magic_copy;
    std::memcpy(&magic_copy, bytes, sizeof(magic_copy));
    std::printf("  magic read back via memcpy: 0x%08X\n", magic_copy);

    // (e) POINTERS AS THE BASIS OF POLYMORPHISM. `Base* p = new Derived;` is
    //     how virtual dispatch happens. (03-oop/)
}

void demo_alignment_and_arithmetic_on_bytes() {
    std::cout << "\n=== 7. void*, std::byte, and alignment ===\n";

    // void* = "address of something, type unknown". Can't dereference it, can't
    // do arithmetic on it (that's a GCC extension, not standard). malloc returns
    // one. You must cast before use.
    void* raw = std::malloc(64);
    std::cout << "  malloc(64) -> " << raw << "\n";

    // std::byte (C++17) is the type-safe "raw byte": it supports only bitwise
    // ops, not arithmetic, so you can't accidentally do math on raw memory.
    auto* bytes = static_cast<std::byte*>(raw);
    bytes[0] = std::byte{0xFF};
    std::cout << "  first byte = 0x" << std::hex
              << static_cast<int>(bytes[0]) << std::dec << "\n";
    std::free(raw);

    // ALIGNMENT: a type of size N generally must live at an address divisible by
    // alignof(T). Unaligned access is UB (and on some CPUs, a hardware fault).
    std::cout << "  alignof(char)=" << alignof(char)
              << " alignof(int)=" << alignof(int)
              << " alignof(double)=" << alignof(double)
              << " alignof(max_align_t)=" << alignof(std::max_align_t) << "\n";
    std::cout << "  malloc always returns memory aligned for max_align_t.\n";
}

int main() {
    demo_basics();
    demo_pointer_arithmetic();
    demo_heap();
    demo_smart_preview();
    demo_the_four_horsemen();
    demo_real_uses();
    demo_alignment_and_arithmetic_on_bytes();

    std::cout << "\n>>> EXERCISE: uncomment each UB line marked in this file, one at a\n"
                 "    time, and run `make asan F=01-fundamentals/09-pointers.cpp`.\n"
                 "    Learn to read ASan reports — it is the most valuable debugging\n"
                 "    skill in C++, and it is how you develop safely at speed.\n";
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * A pointer is an address. sizeof(T*) == 8 on 64-bit, always.
//  * `&x` = address of. `*p` = value at. `p->m` == `(*p).m`.
//  * Pointer arithmetic is scaled by sizeof(T). `a[i]` IS `*(a+i)`.
//  * NO bounds checking. Out-of-range access is UB, not an exception.
//  * new/delete and new[]/delete[] must be paired exactly. Never mix.
//  * The four bugs: dangling, use-after-free, double-free, leak.
//    All four vanish if you use unique_ptr/shared_ptr/vector/string.
//  * Convention: raw pointer = NON-OWNING observer. Owning = smart pointer.
//  * Reinterpreting bytes: use std::memcpy or std::bit_cast, never a
//    reinterpret_cast dereference (strict aliasing UB).
//  * ALWAYS be able to answer: who owns this, and when does it die?
// ===========================================================================
