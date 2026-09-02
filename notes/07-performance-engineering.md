# 07 — Performance engineering: the method

> Optimisation without measurement is superstition. This note is the *process*,
> not a bag of tricks. The tricks are in `07-performance/`.

---

## The loop

```
1. DEFINE the goal      "p99 latency under 5ms at 50k rps"  (not "make it fast")
2. MEASURE the baseline reproducibly
3. PROFILE to find where the time actually goes
4. FORM A HYPOTHESIS about the mechanism ("we're missing cache on the index")
5. CHANGE one thing
6. MEASURE again — did it move the number you defined in step 1?
7. KEEP IT or REVERT IT. Then go to 3.
```

Step 4 is the one people skip. "I'll just make it multithreaded" without knowing
*why* it's slow is how you get a 12-thread program that's slower than the
1-thread one. If you can't state the mechanism, you're guessing.

---

## Know your bottleneck class before you optimise

| Class | Symptom | Fix direction |
|---|---|---|
| **Algorithmic** | time explodes with n | better algorithm/data structure |
| **Memory-bound** | low IPC (<1.0), high cache misses | layout, locality, smaller data |
| **Compute-bound** | high IPC (>2), few misses | SIMD, better math, fewer ops |
| **Branch-bound** | high branch-miss rate | branchless, sorting, prediction |
| **Allocation-bound** | lots of time in malloc/free | reserve, arena, pool |
| **Syscall-bound** | high system time | batch, buffer, io_uring |
| **Lock-bound** | poor thread scaling, high sys time | shard, reduce critical sections |
| **I/O-bound** | low CPU, waiting | async, batching, caching, prefetch |

`perf stat` tells you which one in ten seconds:

```sh
perf stat -e cycles,instructions,cache-references,cache-misses,\
branch-instructions,branch-misses,page-faults ./prog
```
- **IPC = instructions/cycles.** < 1.0 → stalled (usually memory). > 2.5 →
  compute-bound.
- **cache-miss rate** = cache-misses / cache-references. > 10% → layout problem.
- **branch-miss rate** > 5% → unpredictable branches.

On macOS, `perf` doesn't exist. Use:
```sh
sample <pid> 10 -f /tmp/out.txt              # quick, no setup, surprisingly good
xctrace record --template 'Time Profiler' --launch ./prog
xctrace record --template 'CPU Counters' --launch ./prog   # for IPC/cache
```
For serious work, run your benchmarks on a Linux box. The tooling gap is real.

---

## How to benchmark without lying to yourself

Every rule below exists because someone published a wrong number by skipping it.
`07-performance/00-benchmark.hpp` implements them all.

1. **Compile with optimisations.** `-O0` measurements are meaningless.
2. **Defeat the optimiser** — `do_not_optimize()` / `clobber_memory()`.
   Otherwise it deletes the code you're timing.
3. **Warm up.** The first pass pays page faults (the kernel zeroes each page on
   first touch), cold caches, and CPU frequency ramp. This repo has a live
   example: the thread-pool scaling test showed a bogus "110% efficiency"
   until a warm-up was added.
4. **Repeat and take the MINIMUM**, not the mean. You're measuring a lower
   bound; noise only ever adds time.
5. **Measure only the thing you care about.** Building the input data inside
   the timed region is the single most common benchmark bug — it also happened
   in the first draft of `03-oop/04` in this repo, and it inverted the result.
6. **Check what the loop is actually bound by.** A float reduction is
   latency-bound on the FP dependency chain, so it hides memory effects
   entirely. (Also a real bug found in `07-performance/01` while writing it.)
7. **Watch for heterogeneous cores.** On Apple Silicon your thread may land on
   an E-core. Take the best of N; expect noise.
8. **A microbenchmark measures a microbenchmark.** Confirm with a profile of the
   real workload before you believe anything.

For serious work: `brew install google-benchmark`, or use
[nanobench](https://github.com/martinus/nanobench) (single header, does the
statistics properly, estimates instructions/cycles).

---

## The optimisation checklist, in priority order

### 1. Algorithm and data structure
Nothing else compares. O(n²)→O(n log n) beats every micro-optimisation combined.
But also: **choose for the machine, not just the big-O.** A linear scan over a
contiguous vector beats a tree or hash map up to surprisingly large n
(`05-stl/01` measures this).

### 2. Memory layout  ← usually the biggest *non*-algorithmic win
Measured in this repo, all at identical big-O:
- random vs sequential access: **45x**
- linked list vs flat array: **687x**
- AoS vs SoA: **3.5x**
- halving a struct: **1.9x**

Techniques: contiguous storage, traverse in layout order, shrink structs
(order members largest-first), split hot/cold fields, use `uint32_t` indices
instead of pointers, intern strings.

### 3. Allocation
Every `new` in a hot path is ~25-100ns plus fragmentation plus cache pollution.
- `reserve()` when you know the size
- reuse `thread_local` buffers (`clear()` keeps capacity)
- arena per request/frame (**5.4x** measured in `09-projects/01`)
- pools for fixed-size objects
- avoid hidden allocations: `for (auto x : v)`, `const std::string&` parameters
  called with literals, `std::function`, `substr` on `std::string`

### 4. Branches
Mispredicts cost ~15-20 cycles. Make branches predictable, hoist them out of
loops, or go branchless (`01-fundamentals/04-if-else.cpp` shows 3-6x from
sorting the data alone).

### 5. Parallelism
Only after 1-4, and only if the work is CPU-bound and shardable. Remember:
false sharing measured **52x** in `07-performance/02`. Adding threads to a
memory-bandwidth-bound loop does nothing.

### 6. SIMD / intrinsics
Last. Check the compiler didn't already do it:
```sh
clang++ -O2 -Rpass=loop-vectorize -Rpass-missed=loop-vectorize \
        -Rpass-analysis=loop-vectorize file.cpp
```

---

## Compiler flags that matter

```sh
-O2                  # the production default
-O3                  # more inlining/unrolling; sometimes SLOWER (I-cache). Measure.
-Os                  # optimise for size; often faster on large codebases
-march=native        # use this CPU's full ISA. Not portable.
-mcpu=apple-m1       # arm64 equivalent for a specific target
-flto                # cross-TU inlining. 5-20% typical. Enable for release.
-fno-omit-frame-pointer   # keep profiling working. Costs ~0.
-g                   # debug info costs NOTHING at runtime. Always ship it,
                     # then strip into a separate symbol file.
-fprofile-generate / -fprofile-use   # PGO: 10-20% for free on branchy code.
                     # Genuinely underused; it's the closest thing C++ has to
                     # the JVM's profile-guided JIT.
```

**Do not use `-Ofast` / `-ffast-math`** unless you fully understand that it
breaks IEEE semantics (NaN/Inf handling, reassociation, no denormals). Never in
finance, science, or anything requiring reproducible output.

---

## Latency vs throughput

They are different goals and they *conflict*.
- **Throughput**: batch aggressively, buffer, amortise. Accept queueing delay.
- **Latency (p99)**: never batch, never queue, keep everything warm,
  pre-allocate everything, avoid anything with a variable cost.

**Tail latency is the real product.** p99 and p999 are what your users
experience. Mean latency is nearly useless.

What creates tails:
- allocation that occasionally hits the slow path or a page fault
- lock contention spikes
- cache/TLB misses on cold data
- GC (not your problem in C++ — this is a genuine advantage)
- the OS scheduler descheduling you
- background work landing on your hot core

Low-latency practice: preallocate everything, no allocation on the hot path,
pin threads to cores, disable frequency scaling and hyperthreading, use huge
pages, busy-poll instead of blocking, keep the hot path in L1 instruction cache.

---

## The rules

1. **Measure first.** The bottleneck is never where you think.
2. **Fix the top item, then re-measure.** The profile changes after every fix.
3. **Big-O first, layout second, micro-optimisation last.**
4. **Zero work beats fast work.** The fastest allocation is the one you don't
   do; the fastest syscall is the one you don't make; the fastest cache miss is
   the one you avoid. Move work to compile time, to startup, or to never.
5. **Optimise the common case; make the rare case correct.**
6. **Keep the slow, obviously-correct version** as a test oracle.
7. **Stop when you hit the goal.** Optimisation has no natural end, and every
   optimisation costs readability. Define "fast enough" in advance.

Next: `notes/08-systems-and-high-load.md`.
