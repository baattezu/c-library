# 03 — Memory layout: where every byte lives

> This is the note that replaces "the GC handles it." After this, you should be
> able to draw the memory of any C++ program on a whiteboard.

---

## The address space of a running process

```
high addresses  0x7FFF...
┌──────────────────────────────┐
│  kernel space (not yours)    │
├──────────────────────────────┤
│  STACK          ↓ grows down │  ← locals, parameters, return addresses,
│                              │    saved registers. 8 MB default on macOS
│  ~~~~~~~~~~~~~~~~~~~~~~~~~~  │    (512 KB for non-main threads!)
│         (unmapped gap)       │
│  ~~~~~~~~~~~~~~~~~~~~~~~~~~  │
│  mmap region                 │  ← shared libraries, large mallocs (>128 KB),
│                              │    memory-mapped files, thread stacks
│  ~~~~~~~~~~~~~~~~~~~~~~~~~~  │
│  HEAP           ↑ grows up   │  ← malloc/new. Managed by the allocator.
├──────────────────────────────┤
│  .bss   (zero-init globals)  │  ← takes NO space in the binary file
├──────────────────────────────┤
│  .data  (init globals)       │  ← initial values stored in the binary
├──────────────────────────────┤
│  .rodata (constants, string  │  ← READ-ONLY. Writing here = SIGSEGV.
│           literals, vtables) │
├──────────────────────────────┤
│  .text  (machine code)       │  ← read-only + executable
└──────────────────────────────┘
low addresses   0x0000...        ← page 0 unmapped, so *nullptr faults
```

Verify it yourself: run `01-fundamentals/01-variables.cpp`, which prints addresses from
each region. Also try `size ./build/01-fundamentals/01-variables` and `vmmap <pid>`.

---

## The stack

A **stack frame** is created per function call and contains: return address,
saved frame pointer, saved callee-saved registers, parameters that didn't fit in
registers, and all local variables.

```
void f() {
    int a;              // frame: [ret addr][saved fp][a][b][buf...]
    double b;
    char buf[64];
}
```

**Key properties:**
- **Allocation is one instruction**: `sub sp, sp, #96` for the *whole frame*.
  Deallocation is `add sp, sp, #96`. This is why stack objects are free.
- Perfectly cache-hot: the top of the stack is always in L1.
- **Fixed size.** macOS main thread: 8 MB. **Non-main threads: 512 KB by
  default.** Linux: usually 8 MB main, 8 MB per thread.
- Overflow = SIGSEGV, usually from deep recursion or a big local array:
  ```cpp
  void f() { char buf[10 * 1024 * 1024]; }   // instant stack overflow
  ```
- **The stack is why 1M threads doesn't work.** 1M × 512 KB = 512 GB of virtual
  address space (and every touched page becomes real RAM). This one fact is the
  entire reason async/event-loop/coroutine architectures exist.

**Variable-length arrays (`int a[n]` with runtime `n`) are NOT standard C++**
(they're a C99 feature that GCC/clang allow as an extension). Use `std::vector`,
or `alloca` if you really know what you're doing (you probably don't).

### Frame pointer & stack traces
`-fomit-frame-pointer` (on by default at -O2) makes profilers and debuggers
struggle to unwind. Always build with `-fno-omit-frame-pointer` in anything you
intend to profile. The Makefile here does.

---

## The heap

`new` / `malloc` ask an **allocator** for memory. The allocator is a library
(not the kernel), which itself gets big chunks from the kernel via `mmap` or
`sbrk` and hands out pieces.

**What a modern allocator actually does** (macOS libmalloc, or jemalloc/tcmalloc
in production):
- **Size classes**: requests are rounded up to buckets (16, 32, 48, 64, 80...).
  Asking for 33 bytes gives you 48. Internal fragmentation is real.
- **Thread-local caches (tcache/magazines)**: small allocations hit a per-thread
  free list with no locking. This is why malloc is ~20-50 ns, not ~500 ns.
- **Large allocations (>~128 KB) go straight to `mmap`** and are returned to the
  OS on free. Small ones are kept in the process forever (that's why RSS never
  goes down after a spike).
- Each block has a **header** (8-16 bytes) with size/state, immediately before
  the pointer it returns. Writing before your pointer corrupts it. That's the
  classic heap-buffer-underflow.

**Costs of heap allocation:**
| | cost |
|---|---|
| fast path (thread cache hit) | ~20-30 ns |
| slow path (refill, or contention) | 100 ns - several µs |
| a page fault on first touch | ~1-3 µs |
| the *pointer chasing* it causes later | the real cost |

**Fragmentation:** after a long run of mixed-size alloc/free, you have plenty of
free bytes but no contiguous run big enough. Java's compacting GC *fixes* this by
moving objects. C++ **cannot move objects** (pointers would break), so C++ can
fragment permanently. This is the one genuine advantage of a moving GC, and the
mitigation is: **don't allocate in patterns that fragment** — use arenas, pools,
and size-class-homogeneous allocations.

> **The trade you are making:** Java gives you fast bump-pointer allocation and
> pauses. C++ gives you slower allocation and no pauses. The winning C++ strategy
> is to *not allocate at all* in the hot path: preallocate, reuse, arena.

---

## Object layout: alignment and padding

Every type has a **size** and an **alignment**. An object of alignment `A` must
be at an address divisible by `A`. The compiler inserts **padding** to make that
work, and `sizeof(T)` is always a multiple of `alignof(T)` (so arrays work).

```cpp
struct Bad {          // 24 bytes
    char  a;          //  offset 0   (1 byte)
                      //  offset 1-7: 7 bytes PADDING
    double b;         //  offset 8   (8 bytes, needs 8-alignment)
    char  c;          //  offset 16  (1 byte)
                      //  offset 17-23: 7 bytes PADDING (to make sizeof % 8 == 0)
};

struct Good {         // 16 bytes — same fields, reordered
    double b;         //  offset 0
    char   a;         //  offset 8
    char   c;         //  offset 9
                      //  offset 10-15: 6 bytes padding
};
```

**Rule: declare members in decreasing size order.** It costs nothing and can
shrink structs 30-50%. In an array of a million of these, `Bad` is 24 MB and
`Good` is 16 MB — a 33% reduction in cache misses, for free.

Tools:
```sh
clang++ -Xclang -fdump-record-layouts -c foo.cpp    # print exact layouts
```

```cpp
#pragma pack(push, 1)     // no padding at all — for wire formats ONLY
struct WireHeader { uint8_t type; uint32_t len; };   // 5 bytes
#pragma pack(pop)
```
⚠️ Packed structs cause **unaligned access**, which is a fault on some ARM
configurations and slow on others. For network protocols, prefer explicit
`memcpy` field-by-field over packed structs.

```cpp
alignas(128) struct CacheLineAligned { std::atomic<int> counter; };
// forces this to start on a 128-byte boundary — the fix for FALSE SHARING.
```

### Empty base optimisation & `[[no_unique_address]]`
```cpp
struct Empty {};
static_assert(sizeof(Empty) == 1);           // must be ≥1 so distinct objects
                                             // have distinct addresses
struct A : Empty { int x; };
static_assert(sizeof(A) == 4);               // EBO: the base takes no space

struct B { [[no_unique_address]] Empty e; int x; };   // C++20: same for members
static_assert(sizeof(B) == 4);
```
This is how `std::vector` stores a stateless allocator for free, and how you
write zero-cost policy classes.

---

## What an object *is*, in bytes

```cpp
struct Plain { int a; int b; };                 // 8 bytes. Nothing else. No header.
struct WithVirtual { virtual ~WithVirtual(); int a; };  // 16: vptr(8) + a(4) + pad(4)
```

**There is no object header in C++.** No class pointer, no hash code, no lock
word, no GC mark bits. A `struct Point { float x, y; }` is *exactly 8 bytes*.

Compare Java (HotSpot, 64-bit, compressed oops):
```
Point object = 12-byte header (mark word 8 + klass 4) + 8 bytes fields
             + padding to 8-byte boundary = 24 bytes
             + you access it through an 8-byte reference
             + it's somewhere random on the heap
```

**3x the memory and an indirection, per object.** Now put a million of them in a
collection and traverse: Java does 1M random-ish pointer dereferences, C++ does
one linear scan. This is the whole ballgame.

The only C++ objects that carry hidden data are **polymorphic** ones: a class
with any virtual function gets a hidden `vptr` (8 bytes) pointing at its class's
vtable. That's the cost of `virtual` — 8 bytes per object plus an indirect call.
See `03-oop/02-virtual-and-vtable.cpp`.

---

## Storage durations (the full list)

| Duration | Created | Destroyed | Example |
|---|---|---|---|
| **automatic** | on entering scope | on leaving scope | `int x;` in a function |
| **static** | before `main` (or first use, for local statics) | after `main` | globals, `static` locals |
| **thread** | on thread start | on thread exit | `thread_local int x;` |
| **dynamic** | at `new` | at `delete` | `new T`, `make_unique<T>` |

**The static initialisation order fiasco:** the order in which globals in
*different* TUs are initialised is **unspecified**. If global A in `a.cpp`
depends on global B in `b.cpp`, you have a 50/50 crash.

Fix — the "construct on first use" idiom, which is also thread-safe since C++11:
```cpp
Registry& registry() {
    static Registry instance;    // initialised on first call, thread-safely
    return instance;
}
```
Or use `constinit` (C++20) to *require* compile-time initialisation:
```cpp
constinit int g_table[256] = { /* ... */ };   // guaranteed no runtime init
```

---

## `thread_local` — the C++ ThreadLocal

```cpp
thread_local std::vector<char> scratch_buffer;
```
Each thread gets its own instance, destroyed at thread exit. Access is roughly
one extra indirection (via the TLS register, `TPIDRRO_EL0` on arm64). Used for:
per-thread arenas, per-thread random state, per-thread logging buffers.
This is a core technique in thread-per-core designs.

---

## Practical layout wins (ranked)

1. **Reorder members largest-first.** Free.
2. **Use the smallest type that works.** `uint16_t` ids instead of `size_t`,
   `float` instead of `double`, indices instead of pointers (a `uint32_t` index
   into a vector is half the size of a pointer and survives reallocation).
3. **Split hot/cold fields.** Put the 8 bytes you touch every iteration in one
   struct and the 200 bytes of metadata in another, indexed in parallel. This is
   the beginning of SoA (`07-performance/02-aos-vs-soa.cpp`).
4. **Avoid `virtual` on small, numerous objects.** 8 bytes of vptr on a 16-byte
   object is a 50% overhead.
5. **`reserve()` your vectors.** One allocation instead of log(n), and no
   intermediate copies.
6. **Arena-allocate related objects together** so they land on the same pages.
   (`09-projects/01-arena-allocator`)

---

## How to inspect memory, hands-on

```cpp
#include <cstdio>
template <class T> void dump(const T& obj, const char* name) {
    const auto* p = reinterpret_cast<const unsigned char*>(&obj);
    std::printf("%s: %zu bytes @ %p\n  ", name, sizeof(T), (const void*)p);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        std::printf("%02X ", p[i]);
        if ((i + 1) % 16 == 0) std::printf("\n  ");
    }
    std::printf("\n");
}
```
(`02-memory/07-alignment-and-layout.cpp` has a complete, runnable version.)

In lldb:
```
(lldb) p sizeof(MyStruct)
(lldb) memory read -s1 -c32 -fx &obj
(lldb) frame variable -L        # shows addresses of locals
(lldb) image dump symtab        # symbols
```

Next: `notes/04-lifetime-and-raii.md`.
