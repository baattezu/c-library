# C++ From Scratch — for a Java/Spring engineer who wants to build fast things

You already know how to build software. You know OOP, generics, collections, threads,
HTTP servers, Spring's DI container. What you *don't* know yet is what is underneath
all of that — and that is the entire content of this repo.

This is not "learn syntax." This is **learn the machine**. C++ is the language that
refuses to hide the machine from you. That is why it is painful, and that is exactly
why it makes you better at everything else, including Java.

---

## 0. Why bother — the honest version

You asked "why is this needed to become more than a web developer." Here is the answer,
without motivational fluff.

**A web developer optimizes I/O waits. A systems engineer optimizes the machine.**

In Spring you make things fast by: adding an index, adding a cache, adding a queue,
adding a replica. These are all *architectural* moves — you're moving work around.
They work until they don't. When you hit the wall, the wall is made of:

| Wall you hit | What it actually is | Where you learn it |
|---|---|---|
| "GC pauses spike at p99" | You don't control allocation or object layout | `02-memory/`, `notes/03` |
| "We can't handle 100k connections" | Thread-per-request costs 1MB stack + context switches | `08-systems/`, `notes/08` |
| "CPU is at 90% but throughput is low" | Cache misses, false sharing, branch mispredictions | `07-performance/`, `notes/01` |
| "Adding threads made it slower" | Contention, cache-line ping-pong, Amdahl | `06-concurrency/`, `notes/06` |
| "Latency is 200µs, we need 20µs" | Every abstraction layer is costing you | `07-performance/` |
| "The JVM won't let me do X" | You're on the wrong side of the runtime boundary | `09-projects/` |

Every one of those is a C++ problem wearing a Java costume. Kafka, Cassandra, Netty,
the JVM itself, Postgres, Redis, nginx, Envoy, ClickHouse, V8, io_uring — these are
built by people who think in the terms this repo teaches. When you can read and write
this stuff, you stop being a consumer of infrastructure and start being an author of it.

**Concretely, the skills that transfer:**
- You will finally *understand* why `ArrayList` beats `LinkedList` (cache lines, not big-O).
- You will understand what the JVM's escape analysis, TLABs, and card tables are *for*.
- You will understand `volatile`, `happens-before`, and `VarHandle` because you'll have
  written the same thing by hand with `std::memory_order`.
- You will understand why Netty has a `ByteBuf` pool and off-heap buffers.
- You will be able to profile and reason about *any* system, in any language.

**And the thing you specifically mentioned:** yes, there is no networking in the C++
standard library. No HTTP. No JSON. No async runtime. No dependency injection. You get
`std::vector` and a compiler. Everything else you either pull in as a library or *build*.
That is not a weakness — that's the point. Building your own event loop teaches you more
about Netty in two days than two years of using Netty. That's stage 8 and 9 of this repo.

---

## 1. How to use this repo

Every `.cpp` file here is **runnable and heavily commented**. The comments *are* the
lecture. Read the file top-to-bottom, run it, then change something and run it again.

```sh
# Build and run one file:
make run F=01-fundamentals/09-pointers.cpp

# Same, but with Address+UB sanitizer (catches memory bugs at runtime — use a LOT):
make asan F=02-memory/09-dangling-and-ub.cpp

# Same, but with optimizations on (performance experiments need this!):
make opt F=07-performance/01-cache-locality.cpp

# Look at the actual machine code the compiler produced (this is how you learn cost):
make asm F=03-oop/02-virtual-and-vtable.cpp

# Build everything (checks nothing is broken):
make all

# Clean:
make clean
```

> ⚠️ **Rule: performance files MUST be run with `make opt`.** At `-O0` the compiler emits
> deliberately stupid code and every measurement is a lie. At `-O2` it's real.
> Conversely, **correctness files should be run with `make asan`** so bugs are loud
> instead of silent.

### The directory map

```
notes/           ← READ THESE. Dense written theory. This is the "exam" material.
01-fundamentals/ ← stage 1: syntax + the pointer/reference mental model
02-memory/       ← stage 2: THE most important directory. Ownership, RAII, move.
03-oop/          ← stage 3: classes, vtables, and the real cost of polymorphism
04-templates/    ← stage 4: compile-time programming, concepts, zero-cost generics
05-stl/          ← stage 5: containers, algorithms, and their exact complexity/layout
06-concurrency/  ← stage 6: threads, atomics, the memory model, lock-free
07-performance/  ← stage 7: cache, branches, SoA, benchmarking, SIMD
08-systems/      ← stage 8: syscalls, sockets, kqueue, mmap — no stdlib safety net
09-projects/     ← stage 9: build the things. Allocator, vector, thread pool, HTTP server.
```

---

## 2. The roadmap

You said "as soon as possible." Here are two tracks. **The dates assume you already
program well**, which you do — so we skip "what is a for loop" quickly and spend the
time where the actual difficulty is: memory, lifetime, and the machine.

### Track A — "Exam is tomorrow" (one intense day)

Do exactly this, in order. ~8–10 hours.

1. `notes/11-exam-cram-sheet.md` — skim it now, re-read it at the end. (20 min)
2. `notes/00-mental-model-java-to-cpp.md` — the single highest-value doc for you. (30 min)
3. `01-fundamentals/` — all files, run each. You'll fly through most. Slow down at
   `09-pointers.cpp` and `10-references.cpp`. (90 min)
4. `notes/03-memory-layout.md` + all of `02-memory/`. **This is the exam.**
   If you understand ownership, RAII, and move semantics, you can write C++. (3 hours)
5. `03-oop/02-virtual-and-vtable.cpp` — know what a vtable is and what it costs. (30 min)
6. `05-stl/01-containers-and-complexity.cpp` — *memorize* the complexity table. (45 min)
7. `notes/10-undefined-behaviour.md` — every classic trap in one page. (30 min)
8. `notes/06-concurrency-and-memory-model.md` — read, don't code. (45 min)
9. Re-read `notes/11-exam-cram-sheet.md`. (20 min)

### Track B — "I want real expertise" (8–10 weeks, ~10h/week)

| Week | Focus | Deliverable that proves you learned it |
|---|---|---|
| 1 | `01-fundamentals/` + `notes/00,02,03` | You can explain what a pointer, a reference, and a translation unit are without hand-waving |
| 2 | `02-memory/` (all) + `notes/04,05` | Write `unique_ptr` from scratch. Explain move semantics to someone else. |
| 3 | `03-oop/` + `notes/10` | Explain the vtable layout and the cost of a virtual call in cycles |
| 4 | `04-templates/` + `05-stl/` | Write a `constexpr` compile-time hash; implement a concept-constrained algorithm |
| 5 | **Project:** `09-projects/01-arena-allocator` + `02-my-vector` | Your vector passes all 5 tests incl. the strong exception guarantee, and is within 20% of `std::vector` |
| 6 | `06-concurrency/` + `notes/06` | Explain acquire/release to a Java dev in terms of `happens-before`. Then do the exercises in `09-projects/03`. |
| 7 | `07-performance/` + `notes/01,07` | Measure and *explain* a 10x speedup from data layout alone |
| 8 | `08-systems/` + `notes/08` | Read `03-kqueue-event-loop.cpp` until every line is obvious. Then add timeouts to it. |
| 9–10 | **Project:** extend the event loop into an HTTP server; read `notes/09-building-a-runtime.md` | A server holding 10k concurrent connections on one thread, with zero allocations per request |

After that you are genuinely dangerous. Then go read real code:
[redis](https://github.com/redis/redis) (C — start here, `ae.c` is a beautiful
tiny event loop), [libuv](https://github.com/libuv/libuv),
[Seastar](https://github.com/scylladb/seastar), [folly](https://github.com/facebook/folly).

### The notes index

| Note | What it covers |
|---|---|
| [00 — Mental model: Java → C++](notes/00-mental-model-java-to-cpp.md) | the four differences, full translation table, generics vs templates |
| [01 — The machine model](notes/01-machine-model.md) | cache, TLB, pipeline, SIMD, the latency numbers |
| [02 — Compilation and linking](notes/02-compilation-and-linking.md) | TUs, ODR, `inline`, headers, ABI, CMake |
| [03 — Memory layout](notes/03-memory-layout.md) | address space, stack, heap, alignment, padding |
| [04 — Lifetime and RAII](notes/04-lifetime-and-raii.md) | Rule of Zero/Three/Five, ownership vocabulary, exception safety |
| [05 — Value semantics and move](notes/05-value-semantics-and-move.md) | value categories, forwarding, copy elision, `noexcept` |
| [06 — Concurrency and the memory model](notes/06-concurrency-and-memory-model.md) | Java↔C++ dictionary, orderings, data races, scaling |
| [07 — Performance engineering](notes/07-performance-engineering.md) | the method, how to benchmark honestly, the checklist |
| [08 — Systems and high load](notes/08-systems-and-high-load.md) | C10K, epoll/kqueue/io_uring, framing, backpressure |
| [09 — Building a runtime](notes/09-building-a-runtime.md) | **what "build your own runtime" means, and a staged plan** |
| [10 — Undefined behaviour](notes/10-undefined-behaviour.md) | the complete catalogue and your defences |
| [11 — Cram sheet](notes/11-exam-cram-sheet.md) | everything, compressed |

---

## 3. What "high load" actually means (your stated goal)

Keep this frame in your head for the whole journey. High-load engineering is
**four numbers and one law**:

```
L1 cache hit         ~1 ns      (~4 cycles)      ← you want to live here
L2 cache hit         ~4 ns
L3 cache hit         ~15 ns
Main memory (RAM)    ~80 ns     (~300 cycles)    ← a cache miss costs 100+ ops
NVMe SSD read        ~50 µs     (50,000 ns)
Network same-DC RTT  ~500 µs
Network cross-region ~50 ms
Thread context switch ~1-3 µs   + trashes your cache (the hidden cost)
Mutex uncontended     ~20 ns    Mutex contended: ~1 µs+ (it's a syscall now)
Heap allocation       ~50-100 ns (malloc) — and it can be much worse under contention
Virtual function call ~2-3 ns   if predicted; ~20ns if the branch predictor misses
```

**The law:** *Modern CPUs are not slow at computing. They are slow at waiting for memory.*
A 3 GHz core executes ~4 instructions per cycle = 12 instructions/ns. A cache miss costs
80ns = **~1000 instructions of wasted potential**. This single fact explains 80% of all
real performance work:

- Why arrays beat linked lists (contiguity → prefetching works)
- Why struct-of-arrays beats array-of-structs (you only pull in the bytes you use)
- Why `std::map` (red-black tree, pointer-chasing) loses to `std::vector` + binary search
  for small N
- Why false sharing destroys multithreaded scaling
- Why the JVM's compacting GC exists at all (locality restoration!)

Every directory in this repo is, ultimately, about this one law.

---

## 4. Setup notes for this machine

Detected: **Apple clang 16, arm64 (Apple Silicon), macOS 15.6, 12 cores.**

- `-std=c++23` works. `std::format`, `std::print`, `<ranges>`, `<expected>`,
  concepts, and coroutines are all available.
- **Missing:** `std::jthread` (use `std::thread` + explicit `join()`, or the
  `JoiningThread` wrapper in `06-concurrency/01`),
  `std::hardware_destructive_interference_size` (assume a **128-byte** cache line on
  Apple Silicon — note: not 64! This matters in `07-performance/`), and the C++23
  ranges views `zip`/`enumerate`/`chunk`/`join_with` (`std::ranges::to` *does* work).
- **LeakSanitizer is not supported on macOS/arm64.** `ASAN_OPTIONS=detect_leaks=1`
  makes the program abort at startup. ASan still catches use-after-free, buffer
  overflows, and double-frees. For leaks use `make leaks F=<file.cpp>`, which runs
  Apple's `leaks` tool. On Linux, ASan finds leaks by default.
- **You are on macOS, not Linux.** That means `kqueue`, not `epoll`. The concepts are
  identical; `08-systems/` covers kqueue and points out the `epoll` equivalents, because
  in production you will be on Linux.
- Useful extras (optional): `brew install cmake ninja llvm google-benchmark`.
  The system clang can't do `-fsanitize=thread` on some configs; if you want TSan and
  the newest libc++, `brew install llvm` and use `/opt/homebrew/opt/llvm/bin/clang++`.

---

## 5. The three habits that separate a C++ engineer from a C++ tourist

1. **Always know who owns this memory and when it dies.** Every single time you see a
   pointer, answer: who frees it? If you can't answer instantly, you have a bug.
2. **Compile with `-Wall -Wextra` and run with sanitizers.** The Makefile does this for
   you. Undefined behaviour is silent in C++ — sanitizers make it scream.
3. **Measure, never guess.** Your intuition about performance is wrong, mine is wrong,
   everyone's is wrong. `07-performance/05-benchmark-harness.hpp` exists so you have no
   excuse.

Now go to `notes/00-mental-model-java-to-cpp.md`.
