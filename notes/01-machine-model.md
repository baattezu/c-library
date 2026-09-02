# 01 — The machine model: what a CPU actually does

> This is the note that makes you a performance engineer instead of a person who
> guesses. Nothing here is C++-specific — it's true for Java, Rust, and Go too.
> C++ is just the language that lets you *act* on it.

---

## The central fact

**Modern CPUs are not slow at computing. They are starving for data.**

A single Apple M-series performance core, at ~3.5 GHz:
- issues up to **8 instructions per cycle** (very wide, out-of-order)
- has ~600 instructions in flight at once (reorder buffer)
- can do 4 double-precision FLOPs per cycle per NEON unit

So the CPU can retire roughly **10-20 instructions per nanosecond**.

Now the memory:

| Level | Latency | Size (typical, M-series P-core) | In "wasted instructions" |
|---|---|---|---|
| Register | 0 cycles | ~200 bytes | 0 |
| L1 data cache | ~4 cycles (~1 ns) | 128 KB | ~10 |
| L2 cache | ~15 cycles (~4 ns) | 4-16 MB (shared per cluster) | ~50 |
| L3 / SLC | ~50 cycles (~15 ns) | 8-32 MB | ~200 |
| DRAM | ~250-350 cycles (~80-100 ns) | 8-128 GB | **~1000** |
| NVMe SSD | ~50 µs | TB | ~500,000 |
| Network (same DC) | ~250-500 µs | — | ~5,000,000 |
| Network (cross-region) | ~50 ms | — | ~500,000,000 |

**One DRAM access costs as much as ~1000 arithmetic instructions.**

That single line explains most of performance engineering. Your job is almost
never "do fewer operations." It is almost always "touch less memory, and touch
it in an order the hardware can predict."

---

## Cache lines

Memory does not move in bytes. It moves in **cache lines**.

- x86-64: 64 bytes
- **Apple Silicon (arm64): 128 bytes** ← this machine
- Some ARM server chips: 64 bytes

When you read *one byte*, the CPU fetches the whole aligned line containing it.

**Implications, all of which are load-bearing:**

1. **Sequential access is nearly free.** Reading `int` #0 pulls in #0..#31 (128 B ÷
   4 B). The next 31 reads are L1 hits. Effective cost per element: ~1/32 of a
   miss.

2. **The hardware prefetcher watches your access pattern.** If you walk memory
   with a constant stride, it fetches lines *ahead of you* and the misses vanish
   entirely. Linear forward scans are the fastest thing a CPU does. Random access
   defeats it completely.

3. **Pointer chasing is the worst case.** A linked list: each `node->next` is a
   dependent load — you cannot start the next fetch until this one *returns*.
   No prefetching (the address is unknown), no parallelism. 100ns per node.
   Walking a 1M-node list ≈ 100 ms. Walking a 1M-element vector ≈ 1 ms.
   **This is why `LinkedList` is a trap in every language.**

4. **Small structs win.** If your struct is 64 bytes, two fit in a 128-byte line.
   If it's 72 bytes, you waste 44% of every fetch. Shrinking a hot struct below a
   line boundary is a classic, large win.

5. **False sharing** (see `notes/08`): two *different* variables on the same cache
   line, written by two different cores, cause the line to bounce between caches
   at ~100ns per bounce. Same correctness, 10-50x slower. Fix: pad to a line.

### Compute the cost yourself

```
Array of 1,000,000 ints, sum them:
  4 MB / 128 B per line = 31,250 line fetches
  but prefetched, so ~streaming bandwidth-bound: ~200 GB/s => ~20 µs

Linked list of 1,000,000 ints (24-byte nodes, scattered):
  1,000,000 dependent DRAM accesses × ~80 ns = ~80 ms
  => 4000x slower for identical big-O.
```

Big-O is a model of *operation counts*. The machine bills you for *memory
transactions*. When they disagree, the machine wins.

---

## Virtual memory, pages, and the TLB

Your pointers are **virtual addresses**. The MMU translates them to physical
addresses using page tables. Page size: 4 KB on x86 Linux, **16 KB on macOS
arm64**.

That translation is itself a memory lookup (a multi-level page-table walk), so
the CPU caches translations in the **TLB** (Translation Lookaside Buffer), which
holds maybe 1500-3000 entries.

- 2000 TLB entries × 16 KB = **~32 MB of memory addressable without a TLB miss.**
- Touch more than that randomly and you pay a page-table walk (~30-100 ns) *on
  top of* the cache miss.
- This is why databases use **huge pages** (2 MB / 1 GB) — one TLB entry covers
  512x more memory.

**Practical takeaway:** random access over a 100 GB heap is catastrophically
slower than random access over a 10 MB heap, and it's not just cache — it's TLB.

**Also:** the first touch of a fresh page from the OS triggers a **page fault**,
and the kernel zeroes the page for you. `malloc(1GB)` is fast; *writing* to that
1 GB is not. This is why benchmarks must warm up.

---

## The pipeline and speculation

A modern core is a ~15-20 stage pipeline running **out of order** and
**speculatively**.

- At a branch, the CPU *predicts* the direction and keeps executing. Modern
  predictors are ~95-99% accurate on typical code.
- A **misprediction** flushes the pipeline: ~15-20 cycles wasted.
- See `01-fundamentals/04-if-else.cpp` for a live 3-6x demonstration.

**Indirect branches** (virtual calls, function pointers, `switch` jump tables)
are predicted by a *branch target buffer*. If the target is stable (the same
derived class every time — "monomorphic"), it's nearly free. If it alternates
unpredictably ("megamorphic"), you eat a misprediction each call.

> This is exactly why the JVM's JIT does "monomorphic inline caching" and
> devirtualisation. In C++ you get the same effect by **not using virtual calls
> in hot loops** — use templates/CRTP (`03-oop/04-static-polymorphism-crtp.cpp`)
> or sort your work by type so the predictor wins.

**Data dependencies** matter as much as branches. The CPU can only run things in
parallel if they don't depend on each other:

```cpp
// Serial: each add waits for the previous. ~1 add per 4 cycles (latency-bound).
for (int i=0;i<n;++i) sum += a[i];

// 4 independent accumulators: 4 adds in flight. ~4x faster (throughput-bound).
for (int i=0;i<n;i+=4){ s0+=a[i]; s1+=a[i+1]; s2+=a[i+2]; s3+=a[i+3]; }
sum = s0+s1+s2+s3;
```
(The compiler does this for you at `-O2` *only* for integers — floating-point
addition isn't associative, so it needs `-ffast-math` to reorder FP. That's why
FP reductions are often surprisingly slow.)

---

## SIMD

Every modern core has vector registers:
- arm64 NEON: 128-bit → 4 floats or 4 ints at once. (Apple has 4 NEON units.)
- x86 AVX2: 256-bit → 8 floats. AVX-512: 512-bit → 16 floats.

The compiler **auto-vectorises** simple loops at `-O2`/`-O3`. Requirements:
- No loop-carried dependencies
- Countable trip count
- No aliasing between input and output pointers (this is why `restrict`/
  `__restrict` exists, and why `std::vector<T>` beats raw pointer soup)
- Contiguous access (stride 1)

This is another reason contiguous layouts win: **you cannot vectorise a linked
list.** See `07-performance/07-simd-and-vectorization.cpp`.

---

## Cores, and why 12 cores ≠ 12x

- **Amdahl's law:** if 5% of your work is serial, max speedup is 20x, forever.
- **Contention:** a shared mutex serialises you. A shared *atomic counter*
  serialises you almost as badly (cache line ping-pong).
- **Memory bandwidth is shared.** 12 cores streaming from DRAM saturate the
  memory controller. Adding cores to a bandwidth-bound job does nothing.
- **Context switches cost ~1-3 µs** *plus* the invisible cost of a cold cache
  afterwards, which can be much larger.
- **Apple Silicon has heterogeneous cores** (P-cores and E-cores). Your threads
  may land on slow cores. Benchmarks get noisy. Pin and repeat.

The high-load architecture that wins is **thread-per-core with no shared mutable
state** (Seastar, ScyllaDB, DPDK, Redis's single-threaded core). Not
thread-per-request. See `notes/10-systems-and-networking.md`.

---

## How to see all of this on your machine

```sh
# What is my hardware?
sysctl -a | grep -E 'hw.(l1|l2|l3|cache|perflevel|ncpu|memsize|pagesize)'

# Time something properly (multiple runs, statistics)
make opt F=07-performance/01-cache-locality.cpp

# Sample a running program (macOS built-in profiler)
sample <pid> 5 -f out.txt

# Full instrumented profile with call tree + counters
xctrace record --template 'Time Profiler' --launch ./build/myprog

# See the machine code
make asm F=whatever.cpp
```

On Linux (where you'll deploy), the tools are better:
```sh
perf stat -e cycles,instructions,cache-misses,branch-misses ./prog
perf record -g ./prog && perf report
```
`perf stat` is the single most valuable performance tool in existence. Learn to
read **IPC** (instructions per cycle): below ~1.0 means you are stalled on
memory; above ~2.5 means you are compute-bound.

---

## The performance checklist (in priority order)

1. **Algorithm.** O(n²) → O(n log n) beats every micro-optimisation. Do this first.
2. **Memory layout.** Contiguous, small, hot fields together. This is #2 because
   it's usually a 2-10x win and costs nothing at runtime.
3. **Allocation count.** Every `new`/`malloc` in a hot path is ~50-100ns plus
   fragmentation plus cache pollution. Reserve, pool, arena.
4. **Branches.** Make them predictable, or remove them.
5. **Parallelism.** Only after 1-4, and only if you can avoid sharing.
6. **SIMD / intrinsics.** Last. And check the compiler didn't already do it.

**Never do any of this without measuring first.** The bottleneck is never where
you think it is. Profile, fix the top item, profile again.

---

## Numbers to memorise (the "Latency Numbers Every Programmer Should Know" set)

```
L1 reference                      1 ns
Branch mispredict                 3 ns
L2 reference                      4 ns
Mutex lock/unlock (uncontended)  17 ns
Main memory reference           100 ns
Compress 1 KB with Snappy      2,000 ns  =   2 µs
Send 1 KB over 1 Gbps net     10,000 ns  =  10 µs
SSD random read              150,000 ns  = 150 µs
Read 1 MB sequentially, RAM   20,000 ns  =  20 µs
Read 1 MB sequentially, SSD  200,000 ns  = 200 µs
Round trip within datacenter 500,000 ns  = 500 µs
Read 1 MB sequentially, disk  1,000,000 ns = 1 ms
Send packet CA->NL->CA      150,000,000 ns = 150 ms
```

Next: `notes/02-compilation-and-linking.md`.
