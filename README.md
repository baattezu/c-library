# C++ from the machine up: a Java engineer's deep dive

I write backends in Java and Kotlin. This repo is how I learned what sits underneath the JVM: memory, caches, the memory model, syscalls and event loops. I worked through it in C++, because C++ doesn't hide any of that.

It holds 44 runnable, heavily commented programs and 12 notes, ordered from fundamentals to building your own runtime pieces.

## What's inside

| Stage | Topics |
|---|---|
| `01-fundamentals/` | types, pointers vs references, arrays, strings |
| `02-memory/` | stack vs heap, RAII, smart pointers, move semantics, rule of five, alignment, placement new, arenas, UB |
| `03-oop/` | vtables and the cost of virtual calls, CRTP, operator overloading |
| `04-templates/` | variadic templates, perfect forwarding, concepts, `constexpr` metaprogramming |
| `05-stl/` | containers and their real complexity and layout, algorithms and ranges, error handling |
| `06-concurrency/` | threads, mutexes, atomics and memory ordering, a condition-variable queue |
| `07-performance/` | cache and data layout, false sharing, allocation, SIMD, a small benchmark harness |
| `08-systems/` | syscalls and file descriptors, a blocking TCP server, a **kqueue event loop** |
| `09-projects/` | **arena allocator**, **my own `vector`**, **thread pool** |

Notes, in [`notes/`](notes/), cover:
- the Java → C++ mental model;
- the machine model (cache, TLB, pipeline);
- compilation and linking;
- memory layout;
- lifetime;
- value semantics;
- the concurrency memory model, mapped to Java's `happens-before`;
- performance engineering;
- high-load I/O (epoll, kqueue, io_uring);
- building a runtime;
- undefined behaviour.

The full study plan is in [`notes/ROADMAP.md`](notes/ROADMAP.md).

## Why a backend engineer bothers

Most backend performance problems are really machine problems: GC pauses, cache misses, false sharing, thread-per-request limits. Writing the low-level version by hand once changed how I read Netty, the JVM memory model and Postgres internals.

## Run

```sh
make run  F=02-memory/03-raii.cpp                  # -O0 -g
make asan F=02-memory/09-dangling-and-ub.cpp       # AddressSanitizer + UBSan
make opt  F=07-performance/02-false-sharing.cpp    # -O2, for measurements
make asm  F=03-oop/02-virtual-and-vtable.cpp       # look at the generated code
make all                                           # build everything
```

clang++, `-std=c++23`, macOS / Linux.
