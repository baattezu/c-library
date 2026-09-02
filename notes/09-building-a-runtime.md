# 09 — Building your own runtime

> You said: *"I know many people build their own runtime to build the actual
> thing."* That's exactly right, and it's the most important thing to understand
> about working in C++ at scale. This note explains what that means, what's in
> such a runtime, and how to build one.

---

## What "building a runtime" means

In Java, the runtime is given: the JVM provides memory management, threads,
scheduling, class loading, JIT, I/O, and a vast standard library. You write
*application code* on top of a large, opinionated platform.

In C++ the standard library gives you `vector`, `string`, algorithms, and
threads — and stops. No networking. No async. No HTTP. No JSON. No DI. No
logging. No metrics. No scheduler. No memory pooling.

So every serious C++ shop builds — or adopts — a **runtime**: the layer between
the language and the application. This is why Google has Abseil, Facebook has
folly, ScyllaDB has Seastar, and every game studio has an engine core.

**This is not a weakness. It is the trade.** You give up "batteries included"
and get the ability to make every policy decision — allocation strategy,
scheduling, I/O model, data layout — fit *your* workload. That's how you get
10-100x over a general-purpose platform on a specific problem.

---

## What's in a runtime

```
┌───────────────────────────────────────────────────────────┐
│ APPLICATION                                               │
├───────────────────────────────────────────────────────────┤
│ SERVICES     RPC · HTTP · serialization · config · DI     │
├───────────────────────────────────────────────────────────┤
│ CONCURRENCY  task scheduler · futures/coroutines ·        │
│              lock-free queues · timers                    │
├───────────────────────────────────────────────────────────┤
│ I/O          event loop · buffers · sockets · files       │
├───────────────────────────────────────────────────────────┤
│ MEMORY       allocators · arenas · pools · ref-counting   │
├───────────────────────────────────────────────────────────┤
│ FOUNDATION   containers · strings · error handling ·      │
│              logging · metrics · assertions               │
├───────────────────────────────────────────────────────────┤
│ C++ standard library + OS                                 │
└───────────────────────────────────────────────────────────┘
```

You have already built three of these layers in this repo:
- **memory**: `09-projects/01-arena-allocator` (arena + `std::pmr` integration)
- **containers**: `09-projects/02-my-vector`
- **concurrency**: `09-projects/03-thread-pool` (work-stealing pool + parallel_for)
- **I/O**: `08-systems/03-kqueue-event-loop`

That's a real, if minimal, runtime skeleton.

---

## Layer by layer

### 1. Foundation

**Containers.** Almost every large C++ codebase replaces parts of the STL:
- a hash map with open addressing (`absl::flat_hash_map`, `boost::unordered_flat_map`,
  `ankerl::unordered_dense`) — typically **2-5x** faster than `std::unordered_map`,
  which the standard *requires* to be node-based
- `small_vector` / `InlinedVector` — N elements inline, heap only beyond that
- intrusive containers — the links live *inside* the object, so no allocation
  per node and O(1) removal given the object
- a string with a different SSO size, or an interned-string/symbol table

**Error handling.** Pick one policy and enforce it: `std::expected` everywhere,
or exceptions everywhere, or error codes. Mixing is where the pain is.

**Logging.** Structured, level-filtered, and **allocation-free on the hot path**.
The trick: format lazily. Push the arguments into a ring buffer as binary and
let a background thread do the formatting. (See `quill`, `spdlog`,
Bloomberg's `binlog`.)

**Metrics.** Per-thread counters aggregated on read (never a shared atomic in a
hot path — see the 52x false-sharing measurement in `07-performance/02`), plus
HDR histograms for latency percentiles.

### 2. Memory

This is the layer with the biggest payoff, and the one Java simply won't let you
build.

- **Global allocator**: link jemalloc or tcmalloc/mimalloc instead of the system
  malloc. Often a 10-30% win on a threaded server for zero code changes, plus
  free heap profiling.
- **Arenas** per request/frame — measured **5.4x** in `09-projects/01`, and
  structurally leak-proof.
- **Pools** for fixed-size objects (connections, sessions, buffers). Free list
  threaded through the free slots themselves: zero metadata overhead.
- **Buffer management**: a pooled, reference-counted, slice-able byte buffer.
  This is Netty's `ByteBuf`, and every network runtime has one.
- **`std::pmr`** to plug all of the above into standard containers without
  changing their types. Underused and excellent.

### 3. I/O

- an event loop per core, with `SO_REUSEPORT` (`notes/08`)
- a **timing wheel** for O(1) timer insert/expire
- buffered reads/writes; scatter-gather via `writev`
- a pluggable backend: kqueue on macOS, epoll on Linux, io_uring where available

### 4. Concurrency

- a work-stealing scheduler (`09-projects/03`)
- lock-free SPSC/MPSC queues for cross-core messages
- futures with continuations, or coroutines (below)
- thread pinning and NUMA awareness

### 5. Services
RPC framing, HTTP, serialization, config, service discovery, health checks.
Here it usually pays to adopt rather than build: gRPC, Cap'n Proto, protobuf.

---

## Coroutines: getting readable async back

The problem with event loops: your logic gets shredded across callbacks and
explicit state machines, because there is no per-connection stack to hold the
state implicitly.

C++20 coroutines solve this at the language level. A function containing
`co_await`, `co_yield`, or `co_return` is compiled into a **state machine**: the
compiler finds every local that lives across a suspension point, packs them into
a heap-allocated *coroutine frame*, and turns the function body into a resumable
switch.

```cpp
task<std::string> handle(Socket& s) {
    auto request  = co_await s.read_until("\r\n\r\n");  // suspends the TASK,
    auto user     = co_await db.query(parse(request));  // never the THREAD
    co_return render(user);
}
```
Linear, readable code — compiled into the same callback machinery you wrote by
hand in `08-systems/03`.

**The catch:** C++20 shipped only the *language* half. There is no `task<>`, no
scheduler, no `sync_wait` in the standard library. You must write the promise
types and awaiters yourself, or use a library:
- `cppcoro` (the original reference)
- `folly::coro`
- Boost.Asio's coroutine support
- `stdexec` — the reference implementation of **senders/receivers**, the
  proposed C++26 async model, and the likely long-term answer

Also: each coroutine frame is a **heap allocation** unless the compiler can
elide it (HALO). For a million concurrent tasks that matters, so runtimes pool
coroutine frames.

> **Comparison:** Go's goroutines and Java's virtual threads (Loom) solve the
> same problem by giving you *cheap growable stacks* managed by a userspace
> scheduler. C++ chose stackless coroutines instead: no stack to grow, smaller
> per-task memory, but the transformation is visible in your types and it's
> viral (a function that awaits must itself be a coroutine).

---

## The build/adopt decision

**Adopt** (do not write these yourself):
- cryptography and TLS — ever. Use BoringSSL/OpenSSL.
- HTTP parsing — `llhttp`, `picohttpparser`
- JSON — `simdjson` (read), `nlohmann/json` (convenience), `glaze`
- compression — zstd, lz4
- hash maps — Abseil, boost, ankerl
- allocators — jemalloc/mimalloc unless you have a *specific* pattern
- date/time — `std::chrono` (it's genuinely good now)

**Build** when:
- the general-purpose version is a measured bottleneck AND your workload has a
  property the general version can't exploit (fixed sizes, a lifetime phase, a
  known access pattern, single-threaded ownership)
- you need control the library won't give you (allocation policy, exact layout)
- the library's dependency/ABI/licence cost exceeds the code you'd write

**Be honest about maintenance cost.** Your custom hash map has no fuzzers, no
sanitizer CI across 6 platforms, and no other users finding your bugs. That cost
is real and permanent.

---

## A staged plan for building your own

1. **Foundation** — `Fd` RAII wrapper, `Result<T,E>`, assertions, a scope guard,
   a timer, structured logging. (You've written half of these already.)
2. **Memory** — arena (done), fixed-size pool, buffer pool with slices,
   `std::pmr` adapters.
3. **Containers** — `small_vector`, an open-addressing hash map, an intrusive list.
4. **Concurrency** — SPSC ring buffer, MPSC queue, the work-stealing pool
   (done), a timing wheel.
5. **I/O** — the event loop (done), plus: timeouts, graceful shutdown, an
   `epoll` backend so it runs on Linux, `SO_REUSEPORT` per-core loops.
6. **Protocol** — length-prefixed framing, then HTTP/1.1 with a real parser.
7. **Async** — a minimal `task<T>` coroutine type over your event loop. This is
   the capstone: when `co_await socket.read()` works, you have a runtime.
8. **Measure** — connections/sec, p99 latency, allocations/request,
   syscalls/request, bytes/connection. Then optimise against those numbers.

Each stage is a weekend. The whole thing is a serious portfolio project and
genuinely rare expertise.

---

## The honest summary

Building a runtime is how you get:
- **control** — every allocation, every syscall, every byte of layout
- **predictability** — no GC pauses, no JIT warmup, flat p99
- **understanding** — you can no longer be confused by any system, in any
  language, because you've built the thing underneath it

And it costs you: time, bugs the standard library would have prevented, and the
permanent obligation to maintain what you wrote.

**Do it once, deliberately, to learn.** After that, adopt aggressively and build
only what your measurements demand.

Next: `notes/10-undefined-behaviour.md`, then `notes/11-exam-cram-sheet.md`.
