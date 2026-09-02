# 06 — Concurrency and the memory model

> You know `synchronized`, `volatile`, `AtomicInteger`, and happens-before. This
> note maps that knowledge onto C++ and then goes past where Java stops.

---

## The dictionary

| Java | C++ | Notes |
|---|---|---|
| `Thread` | `std::thread` | must `join()`/`detach()` or `std::terminate` |
| `Runnable` / lambda | any callable | |
| `synchronized(x){}` | `std::lock_guard lk(m);` | RAII, scoped, no reentrancy by default |
| `ReentrantLock` | `std::recursive_mutex` | needing it is usually a design smell |
| `ReadWriteLock` | `std::shared_mutex` | `shared_lock` for readers |
| `volatile` field | `std::atomic<T>` (seq_cst) | **C++ `volatile` is NOT this** |
| `AtomicInteger` | `std::atomic<int>` | |
| `.compareAndSet` | `.compare_exchange_strong/_weak` | |
| `VarHandle.setRelease` | `.store(v, memory_order_release)` | |
| `VarHandle.getAcquire` | `.load(memory_order_acquire)` | |
| `LongAdder` | per-thread counters, summed | the sharding trick, by hand |
| `Object.wait/notify` | `std::condition_variable` | same spurious-wakeup rules |
| `CountDownLatch` | `std::latch` (C++20) | one-shot |
| `CyclicBarrier` | `std::barrier` (C++20) | reusable, with a completion fn |
| `Semaphore` | `std::counting_semaphore` (C++20) | |
| `ThreadLocal<T>` | `thread_local T` | destroyed at thread exit |
| `ExecutorService` | you build it (`09-projects/03`) | |
| `Future.get()` | `std::future::get()` | far weaker; no `.then()` |
| `CompletableFuture` | nothing in std. Use a library or coroutines. | |
| `ConcurrentHashMap` | nothing in std. `folly::ConcurrentHashMap`, `libcuckoo`, or shard a `std::unordered_map` behind N mutexes. | |
| `BlockingQueue` | you build it (`06-concurrency/03`) | |
| virtual threads (Loom) | C++20 coroutines (much more manual) | |

### The one that will bite you
> **C++'s `volatile` has nothing to do with threading.**

It means "this memory may change outside the abstract machine" — for
memory-mapped hardware registers and `sig_atomic_t` in signal handlers. It gives
you **no atomicity and no ordering**. Java's `volatile` is C++'s
`std::atomic<T>` with `memory_order_seq_cst`. Using C++ `volatile` for
inter-thread communication is a bug that appears to work on x86 and breaks on
ARM.

---

## Data races: the crucial difference from Java

**Java:** a data race gives you a *stale* or *torn* value. Ugly, but the JVM
stays a memory-safe sandbox. Your program keeps its meaning.

**C++:** a data race is **undefined behaviour**. The optimiser is entitled to
assume it cannot happen, and will transform your code accordingly — hoisting
loads out of loops, inventing writes, reordering freely. The result is not
"a wrong number"; it's "a program with no defined meaning."

A data race is precisely: **two threads access the same memory location,
at least one writes, and they aren't ordered by any synchronisation.**

You saw a live demo of this in `06-concurrency/01-threads-and-mutexes.cpp`,
where the racy counter printed the *correct* answer at `-O2` because the
compiler hoisted the increment into a register. That's the worst possible
outcome: it passes your tests and fails in production.

**Therefore: run your tests under ThreadSanitizer.**
```sh
clang++ -std=c++23 -fsanitize=thread -O1 -g yourfile.cpp && ./a.out
```
TSan finds races that did not manifest, by tracking happens-before edges. It's
~10x slower and ~5x more memory, and it is the only reliable way to find these.

---

## The memory model in one page

Both compiler and CPU reorder memory operations. Single-threaded you never
notice — they preserve the illusion of sequential execution *for one thread*.
Multi-threaded, that illusion breaks, and the memory model defines exactly what
you may rely on.

### `happens-before`
The same concept as Java's. If A happens-before B, then everything A wrote is
visible to B. Edges are created by:
- program order within a single thread
- unlock(m) happens-before a later lock(m)
- a release store happens-before an acquire load that reads it
- thread creation happens-before the thread's first action
- a thread's last action happens-before a successful `join()`

### The six orderings

| Order | Guarantee | Java analogue |
|---|---|---|
| `relaxed` | atomicity only, no ordering | `VarHandle` opaque |
| `acquire` (loads) | nothing after it moves before it | `getAcquire` |
| `release` (stores) | nothing before it moves after it | `setRelease` |
| `acq_rel` (RMW) | both | |
| `seq_cst` **(default)** | acquire+release + one global total order | plain `volatile` |
| `consume` | broken spec; every compiler promotes it to acquire. **Never use.** | — |

### The publish/consume pattern (the one you'll actually use)

```cpp
// producer
data.a = 1; data.b = 2;                        // plain, non-atomic writes
ready.store(true, std::memory_order_release);  // one-way barrier: publish

// consumer
while (!ready.load(std::memory_order_acquire)) { /* spin or wait */ }
use(data.a, data.b);                           // guaranteed to see 1 and 2
```
`data` is not atomic and doesn't need to be. **One atomic flag carries the
ordering for arbitrarily much ordinary data.** That's exactly what a mutex does
internally: lock = acquire, unlock = release.

### Practical advice
1. **Use the default (`seq_cst`) until profiling says otherwise.** It's the only
   one humans reason about reliably.
2. Then use acquire/release, which fits ~99% of real patterns.
3. Use `relaxed` only for independent counters where you read the total later.
4. Getting this wrong produces bugs that appear once a month on one machine
   under load. Do not guess.

### x86 vs ARM — why this is not academic
x86-64 is **strongly ordered**: acquire loads and release stores are plain
`mov` instructions, free. Only `seq_cst` stores cost anything.
ARM64 is **weakly ordered** and needs real `DMB` barriers.

**Consequence: incorrect concurrent code often works on x86 and fails on ARM.**
Your laptop is ARM. AWS Graviton, Ampere, and Apple servers are ARM. Test there.

---

## The cost model of synchronisation

From `06-concurrency/01` and `07-performance/02`, measured on this machine:

```
private variable in a register           ~0 ns    (and optimisable away)
uncontended atomic fetch_add (relaxed)   ~5 ns
uncontended atomic fetch_add (seq_cst)   ~7-15 ns
uncontended mutex lock+unlock            ~17-25 ns
CONTENDED atomic (cache line ping-pong)  ~100 ns per bounce  ← the real cost
CONTENDED mutex (goes to the kernel)     ~1-10 µs
thread context switch                    ~1-3 µs + cold cache
thread creation                          ~10-30 µs
```

Two conclusions:
1. **The uncontended cost is the small part.** Contention is what kills you, and
   contention is a *design* property, not a *primitive* property.
2. Measured in `06-concurrency/01`: mutex 27.9 ms, atomic 7.4 ms,
   per-thread-sharded **0.1 ms**. Sharding beat the atomic by ~70x.

> **The fastest synchronisation is no sharing.**

---

## Scaling architecture, ranked

1. **Shard the data by core.** No sharing → no contention → linear scaling.
   This is Seastar/ScyllaDB/DPDK/Redis. It is the actual answer.
2. **Immutable snapshots.** Readers never block; writers publish a new version
   via one release store. (RCU, copy-on-write, persistent data structures.)
3. **Message passing.** Each thread owns its data; communicate through queues.
4. **Fine-grained locks over separate data.** Now you have deadlock risk.
5. **One big lock.** Correct, simple, and it caps you at one core.
6. **Lock-free data structures.** Last resort — see below.

## On writing lock-free code

Don't, at first. Understand it so you can read and debug it.

The hard part isn't the CAS loop; it's **memory reclamation**. When can you free
a node that another thread might still be reading? A GC language answers this
for free — which is why lock-free queues are dramatically easier in Java.
In C++ you need hazard pointers, epoch-based reclamation, or RCU. Plus:

- **The ABA problem**: a value changes A→B→A between your read and your CAS, so
  the CAS succeeds but the world changed. Fixes: tagged pointers, versioned
  counters, deferred reclamation.
- `compare_exchange_weak` may fail *spuriously* on ARM (LL/SC). That's fine
  inside a retry loop and cheaper; use `_strong` when not looping.
- "Lock-free" ≠ "fast". Under contention a good mutex often beats a
  hand-rolled lock-free structure, because the mutex parks losers instead of
  having them burn bandwidth on failed CAS attempts.

Use `folly`, `moodycamel::ConcurrentQueue`, or `boost::lockfree`. They are
written by people who benchmark and who have read the proofs.

---

## The checklist

- [ ] Every `std::thread` is joined (wrap it in RAII; `jthread` when available).
- [ ] No shared mutable state that isn't atomic or mutex-protected.
- [ ] Move constructors are `noexcept`.
- [ ] Lock order is documented; `std::scoped_lock` for multiple mutexes.
- [ ] No unknown code (callbacks, virtuals) called while holding a lock.
- [ ] `cv.wait` always uses the predicate overload.
- [ ] Queues are **bounded** (backpressure), not unbounded.
- [ ] Counters that different threads write are padded to a cache line.
- [ ] Tests run under `-fsanitize=thread` in CI.
- [ ] Benchmarks run on the target architecture (ARM if you deploy on ARM).
- [ ] Nothing blocks an event loop thread.

Next: `notes/07-performance-engineering.md`.
