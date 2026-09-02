// ===========================================================================
//  A MINIMAL, HONEST BENCHMARK HARNESS
//  #include "00-benchmark.hpp"
// ===========================================================================
//
//  Real projects use Google Benchmark (`brew install google-benchmark`) or
//  nanobench. This is ~80 lines so you can see exactly what a benchmark must
//  do to not lie to you. Every rule here exists because someone published a
//  wrong result by skipping it.
//
//  THE FIVE RULES OF MICROBENCHMARKING
//   1. Compile with optimisations. -O0 numbers are meaningless.
//   2. Stop the optimiser deleting your work (do_not_optimize / clobber).
//   3. WARM UP. First run pays page faults, cold caches, and CPU frequency ramp.
//   4. Repeat and report the MINIMUM (or a low percentile), not the mean.
//      You're measuring a lower bound; noise is one-directional.
//   5. Measure the thing you care about, not the setup around it.
//
//  AND THE ZEROTH RULE: a microbenchmark tells you about a microbenchmark.
//  Always confirm with a profile of the real workload.
//
// ===========================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// Optimisation barriers. These emit NO instructions — they only constrain what
// the compiler is allowed to assume.
// ---------------------------------------------------------------------------

// "Pretend this value escaped into memory the compiler can't see."
// Prevents dead-code elimination of the computation that produced it.
template <class T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

// "Pretend all of memory was written." Prevents the compiler from caching
// loads across this point, e.g. hoisting a loop-invariant read.
inline void clobber_memory() {
    asm volatile("" : : : "memory");
}

// ---------------------------------------------------------------------------
struct Result {
    std::string name;
    double      min_ns;
    double      median_ns;
    double      mean_ns;
    std::size_t iterations;

    void print(double baseline_ns = 0) const {
        std::printf("  %-38s %9.2f ns/op  (median %9.2f)", name.c_str(), min_ns, median_ns);
        if (baseline_ns > 0) std::printf("  %6.2fx", baseline_ns / min_ns);
        std::printf("\n");
    }
};

// ---------------------------------------------------------------------------
//  Run `fn` `iters` times per sample, take `samples` samples, report the best.
//  `fn` should return something; we feed it to do_not_optimize.
// ---------------------------------------------------------------------------
template <class F>
Result run(const std::string& name, std::size_t iters, F&& fn, int samples = 7) {
    // WARM-UP: page faults, cache fill, branch predictor training, CPU boost.
    for (std::size_t i = 0; i < iters / 10 + 1; ++i) do_not_optimize(fn());

    std::vector<double> per_op;
    per_op.reserve(static_cast<std::size_t>(samples));

    for (int s = 0; s < samples; ++s) {
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < iters; ++i) do_not_optimize(fn());
        auto t1 = std::chrono::steady_clock::now();
        per_op.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count()
                         / static_cast<double>(iters));
    }

    std::sort(per_op.begin(), per_op.end());
    double sum = 0; for (double x : per_op) sum += x;
    return Result{ name,
                   per_op.front(),
                   per_op[per_op.size() / 2],
                   sum / static_cast<double>(per_op.size()),
                   iters };
}

// For work that is already a whole batch (e.g. "traverse this whole array"),
// where you want per-CALL timing rather than per-inner-element.
template <class F>
Result run_once_per_sample(const std::string& name, F&& fn, int samples = 7) {
    return run(name, 1, std::forward<F>(fn), samples);
}

inline void header(const char* title) {
    std::printf("\n=== %s ===\n", title);
}

inline void note(const char* text) { std::printf("\n%s\n", text); }

} // namespace bench
