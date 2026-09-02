// ===========================================================================
//  FALSE SHARING — the invisible multicore killer
//  run: make opt F=07-performance/02-false-sharing.cpp
// ===========================================================================
//
//  Two threads. Two DIFFERENT variables. No shared data. No locks. No races.
//  Correct code. And it can run 10x slower than it should, because the two
//  variables happen to sit on the SAME CACHE LINE.
//
//  This is the bug that makes people say "we added threads and it got slower."
//  It is invisible in the source, invisible in a profiler's line attribution,
//  and only shows up as high cycles with low IPC.
//
//  ⚠️ APPLE SILICON HAS 128-BYTE CACHE LINES, not 64. Verify:
//        sysctl hw.cachelinesize
//     Code padded to 64 bytes (correct on x86) still false-shares here.
//
// ===========================================================================

#include "00-benchmark.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <thread>
#include <vector>

// std::hardware_destructive_interference_size is the standard way to ask.
// Apple clang doesn't provide it, so we detect and fall back.
#ifdef __cpp_lib_hardware_interference_size
constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
constexpr std::size_t CACHE_LINE = 128;   // Apple Silicon. x86-64 would be 64.
#endif

// ---------------------------------------------------------------------------
//  Two layouts of the same data.
// ---------------------------------------------------------------------------
struct Packed {                       // counters adjacent -> SAME cache line
    std::atomic<std::uint64_t> counter{0};
};

struct alignas(CACHE_LINE) Padded {   // each counter gets its OWN cache line
    std::atomic<std::uint64_t> counter{0};
    // alignas both aligns the start AND pads sizeof up to a multiple of
    // CACHE_LINE, so an ARRAY of these has one element per line.
};

// ---------------------------------------------------------------------------
template <class Counters>
double run_test(int threads, std::uint64_t iters_per_thread) {
    std::vector<Counters> counters(static_cast<std::size_t>(threads));

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    ts.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        ts.emplace_back([&counters, i, iters_per_thread]{
            auto& c = counters[static_cast<std::size_t>(i)].counter;
            for (std::uint64_t k = 0; k < iters_per_thread; ++k)
                // relaxed: we don't need ordering, only atomicity. Even so, the
                // cache-coherence traffic is unavoidable — that's the point.
                c.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : ts) t.join();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// ---------------------------------------------------------------------------
void demo_false_sharing() {
    bench::header("1. The measurement");

    std::printf("  cache line size assumed: %zu bytes\n", CACHE_LINE);
    std::printf("  sizeof(Packed) = %zu, sizeof(Padded) = %zu\n\n",
                sizeof(Packed), sizeof(Padded));

    constexpr std::uint64_t ITERS = 5'000'000;

    std::printf("  %-8s %14s %14s %10s\n", "threads", "packed (ms)", "padded (ms)", "speedup");
    for (int threads : {1, 2, 4, 8}) {
        // Take the best of 3 — thread scheduling on heterogeneous P/E cores is
        // noisy on Apple Silicon.
        double packed = 1e18, padded = 1e18;
        for (int r = 0; r < 3; ++r) {
            packed = std::min(packed, run_test<Packed>(threads, ITERS));
            padded = std::min(padded, run_test<Padded>(threads, ITERS));
        }
        std::printf("  %-8d %14.1f %14.1f %9.2fx\n", threads, packed, padded, packed / padded);
    }

    bench::note(
      "  READING THE TABLE:\n"
      "   - At 1 thread there is no difference: no other core is competing for\n"
      "     the line, so it stays in this core's L1 in the Modified state.\n"
      "   - As threads increase, the PACKED version degrades badly while the\n"
      "     PADDED version stays roughly flat. That gap IS false sharing.\n"
      "   - Notice the packed version can get SLOWER as you add threads. This\n"
      "     is the 'we parallelised it and it regressed' phenomenon.");
}

// ---------------------------------------------------------------------------
void explain_mesi() {
    bench::header("2. Why it happens: cache coherence (MESI)");
    std::printf("%s",
      "  Each core has its own L1 cache. Hardware keeps them coherent with a\n"
      "  protocol (MESI / MOESI). Every cache LINE, in every core's cache, is in\n"
      "  one of these states:\n"
      "\n"
      "     M odified   — I have the only copy and I've changed it\n"
      "     E xclusive  — I have the only copy, unchanged\n"
      "     S hared     — several cores have a read-only copy\n"
      "     I nvalid    — my copy is stale, I must re-fetch\n"
      "\n"
      "  THE RULE: TO WRITE A LINE, A CORE MUST OWN IT EXCLUSIVELY. It must send\n"
      "  an invalidate to every other core holding it, and wait.\n"
      "\n"
      "  So when core 0 writes counter[0] and core 1 writes counter[1], and both\n"
      "  counters share a line:\n"
      "     core 0: 'invalidate!' -> gets the line in M -> writes\n"
      "     core 1: 'invalidate!' -> line migrates from core 0 -> writes\n"
      "     core 0: 'invalidate!' -> line migrates back...\n"
      "  The line PING-PONGS between cores at roughly L3/interconnect latency\n"
      "  (~40-100ns) per transfer. The variables are logically independent; the\n"
      "  hardware cannot tell, because coherence works at LINE granularity.\n"
      "\n"
      "  Hence the name: the sharing is FALSE. No data is actually shared.\n");
}

// ---------------------------------------------------------------------------
void demo_real_world() {
    bench::header("3. Where this actually bites you");
    std::printf("%s",
      "  (a) PER-THREAD STATISTICS\n"
      "        struct Stats { uint64_t requests, errors, bytes; };\n"
      "        std::vector<Stats> per_thread(num_threads);   // ☠️ 3 counters,\n"
      "      24 bytes — five threads' worth fit in one 128-byte line.\n"
      "      FIX: alignas(128) on the struct.\n"
      "\n"
      "  (b) A LOCK NEXT TO THE DATA IT PROTECTS\n"
      "        struct Queue { std::mutex m; size_t head, tail; ... };\n"
      "      Spinning on the mutex invalidates the line holding head/tail.\n"
      "      FIX: pad between the lock and the hot fields, and put producer\n"
      "      state and consumer state on separate lines (see the SPSC queue).\n"
      "\n"
      "  (c) A SHARED PROGRESS COUNTER / WORK INDEX\n"
      "        std::atomic<size_t> next_item;    // every thread hammers it\n"
      "      This isn't even false sharing — it's TRUE sharing, which is worse.\n"
      "      FIX: hand out work in CHUNKS (grab 1000 items per atomic op), or\n"
      "      partition the work up front, or use work stealing with per-thread\n"
      "      deques.\n"
      "\n"
      "  (d) shared_ptr COPIES IN A HOT LOOP\n"
      "      Every copy is an atomic increment on the control block. If several\n"
      "      threads copy the same shared_ptr, that one cache line serialises\n"
      "      all of them. This is a very common, very invisible scaling wall.\n"
      "      FIX: pass `const shared_ptr&` or a raw `T*`/`T&` when the callee\n"
      "      doesn't store it.\n"
      "\n"
      "  (e) ADJACENT ELEMENTS OF AN OUTPUT ARRAY\n"
      "        parallel_for(i) { output[i] = f(input[i]); }\n"
      "      If threads interleave indices (i, i+1, i+2 to different threads),\n"
      "      every write false-shares. FIX: give each thread a CONTIGUOUS BLOCK,\n"
      "      not a strided slice. This is why chunked partitioning beats\n"
      "      round-robin for parallel loops.\n");
}

// ---------------------------------------------------------------------------
void the_fixes() {
    bench::header("4. The fixes, in order of preference");
    std::printf("%s",
      "  1. DON'T SHARE. Accumulate into a thread-local/stack variable and\n"
      "     combine ONCE at the end. Costs nothing and always works.\n"
      "        void worker() {\n"
      "            uint64_t local = 0;              // in a REGISTER\n"
      "            for (...) ++local;\n"
      "            global.fetch_add(local);         // one atomic, at the end\n"
      "        }\n"
      "\n"
      "  2. PAD to a cache line when threads genuinely need separate slots:\n"
      "        struct alignas(128) Counter { std::atomic<uint64_t> v{0}; };\n"
      "     (Or use std::hardware_destructive_interference_size where available.)\n"
      "\n"
      "  3. PARTITION BY CORE. Give each thread its own struct, its own arena,\n"
      "     its own queue. This is the thread-per-core architecture, and false\n"
      "     sharing is one of the reasons it exists.\n"
      "\n"
      "  4. BATCH your shared updates: 1 atomic per 1000 items, not per item.\n"
      "\n"
      "  ⚠️ PADDING IS NOT FREE. alignas(128) turns an 8-byte counter into 128\n"
      "     bytes — a 16x memory increase. Only pad things that are ACTUALLY\n"
      "     written by different threads concurrently. Padding everything wastes\n"
      "     cache and can make you slower.\n"
      "\n"
      "  HOW TO FIND IT IN PRODUCTION (Linux):\n"
      "     perf c2c record ./prog && perf c2c report\n"
      "        -> literally reports the cache lines suffering contention and the\n"
      "           source lines touching them. It is the perfect tool for this.\n"
      "     perf stat -e cycles,instructions,cache-misses ./prog\n"
      "        -> low IPC (<1.0) + poor thread scaling = suspect false sharing.\n"
      "  On macOS: Instruments' 'System Trace' and 'CPU Counters', or just do\n"
      "  what we did here — pad it and measure the difference.\n");
}

int main() {
    std::printf("⚠️  Run with `make opt F=07-performance/02-false-sharing.cpp`.\n");
    demo_false_sharing();
    explain_mesi();
    demo_real_world();
    the_fixes();
    return 0;
}
