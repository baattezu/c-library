// ===========================================================================
//  ALLOCATION COST AND VECTORISATION
//  run: make opt F=07-performance/03-allocation-and-simd.cpp
// ===========================================================================

#include "00-benchmark.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

// ===========================================================================
//  PART A — ALLOCATION IS THE #1 HIDDEN COST IN "NORMAL LOOKING" C++
// ===========================================================================
void demo_reserve() {
    bench::header("A1. reserve() — and how much it's actually worth");

    constexpr std::size_t N = 100'000;

    auto int_without = [&]{
        std::vector<int> v;                       // log2(100k) ≈ 17 reallocations,
        for (std::size_t i = 0; i < N; ++i)       // each copying everything so far
            v.push_back(static_cast<int>(i));
        return v.size();
    };
    auto int_with = [&]{
        std::vector<int> v;
        v.reserve(N);                             // ONE allocation
        for (std::size_t i = 0; i < N; ++i) v.push_back(static_cast<int>(i));
        return v.size();
    };

    auto a = bench::run("vector<int>    without reserve", 200, int_without);
    auto b = bench::run("vector<int>    with reserve   ", 200, int_with);
    a.print(); b.print(a.min_ns);

    // The win scales with how expensive it is to relocate an element. For int,
    // reallocation is a memcpy and the gain is modest. For a type that must be
    // moved one-by-one, it's much larger.
    struct Item { std::string name; std::vector<int> tags; };
    constexpr std::size_t M = 20'000;
    auto obj_without = [&]{
        std::vector<Item> v;
        for (std::size_t i = 0; i < M; ++i) v.push_back(Item{"item", {1,2,3}});
        return v.size();
    };
    auto obj_with = [&]{
        std::vector<Item> v;
        v.reserve(M);
        for (std::size_t i = 0; i < M; ++i) v.push_back(Item{"item", {1,2,3}});
        return v.size();
    };
    auto c = bench::run("vector<Item>   without reserve", 100, obj_without);
    auto d = bench::run("vector<Item>   with reserve   ", 100, obj_with);
    c.print(); d.print(c.min_ns);

    bench::note(
      "  Growth is geometric (2x in libc++), so push_back is amortised O(1) —\n"
      "  but 'amortised O(1)' still means ~17 mallocs, ~17 frees, and relocating\n"
      "  ~200k elements' worth of data in total.\n"
      "\n"
      "  NOTE THE HONEST RESULT: for vector<int> the win is modest (~1.3-1.5x),\n"
      "  because reallocation is just a big memcpy and memcpy is very fast. The\n"
      "  win grows with the cost of RELOCATING one element — for types with\n"
      "  their own allocations it's larger, and for types whose move constructor\n"
      "  is not noexcept it's dramatic (vector must COPY them; see\n"
      "  02-memory/05).\n"
      "\n"
      "  Either way reserve() costs one line and never hurts. Do it whenever you\n"
      "  can estimate the size. But don't claim a 10x from it without measuring\n"
      "  — that's exactly the kind of folklore this directory exists to kill.");
}

// ---------------------------------------------------------------------------
void demo_string_allocation() {
    bench::header("A2. Hidden string allocations");

    std::vector<std::string> data;
    for (int i = 0; i < 1000; ++i)
        data.push_back("a-reasonably-long-key-name-number-" + std::to_string(i));

    // ❌ const std::string& forces a temporary std::string (and a heap
    //    allocation) at every call site that passes a literal or a char*.
    auto count_by_cref = [&](const std::string& needle) {
        std::size_t n = 0;
        for (const auto& s : data) if (s == needle) ++n;
        return n;
    };
    // ✅ string_view: no copy, no allocation, accepts everything.
    auto count_by_view = [&](std::string_view needle) {
        std::size_t n = 0;
        for (const auto& s : data) if (s == needle) ++n;
        return n;
    };

    auto a = bench::run("compare via const string& (temp!)", 2000,
                        [&]{ return count_by_cref("a-reasonably-long-key-name-number-500"); });
    auto b = bench::run("compare via string_view          ", 2000,
                        [&]{ return count_by_view("a-reasonably-long-key-name-number-500"); });
    a.print(); b.print(a.min_ns);

    // The copy-vs-move point, measured FAIRLY.
    // (A first attempt wrote `auto tmp = data; auto c = std::move(tmp);` — which
    //  measures a COPY plus a move, and unsurprisingly showed no benefit. To
    //  measure a move alone, ping-pong between two live vectors so no copy
    //  happens inside the timed region.)
    std::vector<std::string> src = data, dst;
    auto copy_one = [&]{ dst = src; return dst.size(); };          // 1001 allocations
    auto move_one = [&]{ dst = std::move(src);                      // 3 pointer swaps
                         src = std::move(dst); return src.size(); };
    auto c = bench::run("copy a vector<string> of 1000", 2000, copy_one);
    auto m = bench::run("move a vector<string> (x2)   ", 2000, move_one);
    c.print(); m.print(c.min_ns);

    bench::note(
      "  The copy does ~1001 allocations (the vector buffer + one per string too\n"
      "  long for SSO) plus 1000 memcpys. The move does ZERO allocations — it\n"
      "  swaps three pointers. Note the benchmark above does TWO moves per\n"
      "  iteration and is still vastly faster than one copy.\n"
      "\n"
      "  THE HIDDEN-ALLOCATION CHECKLIST:\n"
      "   - `const std::string&` parameters called with literals -> string_view\n"
      "   - `for (auto x : container)` -> `for (const auto& x : ...)`\n"
      "   - returning std::string from a getter -> return `const std::string&`\n"
      "     or string_view\n"
      "   - `s.substr()` in a loop -> `string_view::substr` is O(1) and free\n"
      "   - std::function in a hot path -> a template parameter or `auto` lambda\n"
      "   - `map[key]` with a std::string key built per call -> use\n"
      "     heterogeneous lookup (std::less<> as the comparator) so you can\n"
      "     look up with a string_view and skip the construction entirely");
}

// ---------------------------------------------------------------------------
void demo_pool_reuse() {
    bench::header("A3. Buffer reuse — the pattern for a zero-allocation server");

    constexpr int REQUESTS = 20000;

    auto naive = [&]{
        std::size_t total = 0;
        for (int i = 0; i < REQUESTS; ++i) {
            std::vector<char> buffer;                 // allocate...
            buffer.resize(4096);
            std::memset(buffer.data(), i & 0xFF, buffer.size());
            total += buffer[0];
        }                                             // ...and free, every request
        return total;
    };

    auto reused = [&]{
        // thread_local: one buffer per thread, no sharing, no lock, no realloc
        // after the first request. This is THE idiom for per-request scratch.
        static thread_local std::vector<char> buffer;
        std::size_t total = 0;
        for (int i = 0; i < REQUESTS; ++i) {
            buffer.clear();                            // O(1), KEEPS the capacity
            buffer.resize(4096);
            std::memset(buffer.data(), i & 0xFF, buffer.size());
            total += buffer[0];
        }
        return total;
    };

    auto a = bench::run_once_per_sample("fresh vector per request ", naive);
    auto b = bench::run_once_per_sample("reused thread_local buffer", reused);
    a.print(); b.print(a.min_ns);

    bench::note(
      "  ⭐ THE HIGH-LOAD PATTERN, STATED ONCE:\n"
      "     A server that allocates per request has a per-request cost that\n"
      "     grows with heap fragmentation and contends across threads. A server\n"
      "     that RESETS pre-allocated per-thread buffers has a flat, predictable\n"
      "     cost and a p99 that doesn't drift.\n"
      "\n"
      "     Combine three techniques and you reach zero allocations per request:\n"
      "       1. thread_local reusable buffers (this demo)\n"
      "       2. a per-request ARENA, reset at the end (02-memory/08)\n"
      "       3. object POOLS for fixed-size things (connections, sessions)\n"
      "\n"
      "     This is exactly what nginx (ngx_pool_t), Envoy, and every low-latency\n"
      "     trading system do. It is also what you CANNOT do in Java without\n"
      "     off-heap ByteBuffers and a lot of pain — which is why Netty has\n"
      "     PooledByteBufAllocator.");
}

// ===========================================================================
//  PART B — VECTORISATION (SIMD)
// ===========================================================================
void demo_autovectorization() {
    bench::header("B1. Auto-vectorisation: let the compiler do it");

    constexpr std::size_t N = 4'000'000;
    std::vector<float> a(N, 1.5f), b(N, 2.5f), out(N);

    // VECTORISABLE: contiguous, no dependencies, countable trip count.
    // The compiler emits NEON instructions handling 4 floats per instruction.
    auto vectorizable = [&]{
        for (std::size_t i = 0; i < N; ++i) out[i] = a[i] * b[i] + 1.0f;
        return out[0];
    };

    // NOT VECTORISABLE: a loop-carried dependency (out[i] depends on out[i-1]).
    auto dependent = [&]{
        out[0] = a[0];
        for (std::size_t i = 1; i < N; ++i) out[i] = out[i-1] * 0.5f + a[i];
        return out[N-1];
    };

    // FP reduction: NOT vectorised by default, because FP addition isn't
    // associative and the compiler must preserve your exact rounding.
    auto fp_reduction = [&]{
        float s = 0;
        for (std::size_t i = 0; i < N; ++i) s += a[i];
        return s;
    };

    // The manual fix: multiple independent accumulators. Now there are 4
    // independent dependency chains and the CPU can overlap them.
    auto fp_reduction_unrolled = [&]{
        float s0=0, s1=0, s2=0, s3=0;
        std::size_t i = 0;
        for (; i + 3 < N; i += 4) { s0+=a[i]; s1+=a[i+1]; s2+=a[i+2]; s3+=a[i+3]; }
        for (; i < N; ++i) s0 += a[i];
        return s0+s1+s2+s3;
    };

    auto r1 = bench::run_once_per_sample("a[i]*b[i]+1  (vectorises)     ", vectorizable);
    auto r2 = bench::run_once_per_sample("out[i]=f(out[i-1])  (cannot) ", dependent);
    auto r3 = bench::run_once_per_sample("FP sum, 1 accumulator        ", fp_reduction);
    auto r4 = bench::run_once_per_sample("FP sum, 4 accumulators       ", fp_reduction_unrolled);
    r1.print(); r2.print(); r3.print(); r4.print(r3.min_ns);

    bench::note(
      "  WHAT BLOCKS AUTO-VECTORISATION:\n"
      "   1. Loop-carried dependencies (demo 2). Unfixable without changing the\n"
      "      algorithm — sometimes a prefix-sum/scan formulation helps.\n"
      "   2. FP reduction order (demo 3 vs 4). Fix with manual accumulators, or\n"
      "      -ffast-math / #pragma clang fp reassociate(on) if your domain allows\n"
      "      reassociation. NEVER in finance or anything requiring reproducible\n"
      "      results.\n"
      "   3. POSSIBLE ALIASING. If the compiler can't prove `out` and `a` don't\n"
      "      overlap, it must assume they might. Fixes: `__restrict` on raw\n"
      "      pointers, or just use distinct std::vectors (the compiler can often\n"
      "      prove it then).\n"
      "   4. Non-contiguous access (AoS with a big stride), function calls it\n"
      "      can't inline, virtual calls, exceptions in the loop body.\n"
      "   5. Unknown trip count / early exits (`break`).\n"
      "\n"
      "  HOW TO CHECK IF IT VECTORISED:\n"
      "     clang++ -O2 -Rpass=loop-vectorize -Rpass-missed=loop-vectorize \\\n"
      "             -Rpass-analysis=loop-vectorize file.cpp\n"
      "  The compiler will TELL YOU which loops vectorised and why the others\n"
      "  didn't. This is one of the most underused tools in C++.");
}

// ---------------------------------------------------------------------------
void simd_guidance() {
    bench::header("B2. When to write SIMD by hand");
    std::printf("%s",
      "  ORDER OF PREFERENCE:\n"
      "   1. Fix the DATA LAYOUT so the compiler can auto-vectorise. This is\n"
      "      almost always the real answer, and it keeps the code portable.\n"
      "   2. Use a library: Eigen, xsimd, Google Highway, std::simd (C++26).\n"
      "      Portable across NEON/AVX/SVE, and written by people who benchmark.\n"
      "   3. Write intrinsics by hand ONLY for a proven hot kernel.\n"
      "        arm64:  #include <arm_neon.h>   float32x4_t, vmlaq_f32, ...\n"
      "        x86:    #include <immintrin.h>  __m256, _mm256_fmadd_ps, ...\n"
      "      You will write it twice (once per architecture) and maintain it\n"
      "      forever. Only do it when the measurement justifies it.\n"
      "\n"
      "  WIDTHS: NEON 128-bit (4 floats). AVX2 256-bit (8). AVX-512 512-bit (16).\n"
      "  ARM SVE has variable width, which is why portable wrappers exist.\n"
      "\n"
      "  ⚠️ SIMD ONLY HELPS IF YOU ARE COMPUTE-BOUND. If you're waiting on DRAM,\n"
      "     8-wide arithmetic just means you wait 8x more idly. Fix memory first.\n"
      "     Check with `perf stat`: high cache-miss rate means SIMD won't help.\n"
      "\n"
      "  WHERE IT REALLY PAYS: JSON parsing (simdjson is ~3 GB/s), UTF-8\n"
      "  validation, base64, compression, hashing, image/audio, ML kernels,\n"
      "  and columnar database filters. Notice: all of them are\n"
      "  contiguous-array problems. Layout first, always.\n");
}

int main() {
    std::printf("⚠️  Run with `make opt F=07-performance/03-allocation-and-simd.cpp`.\n");
    demo_reserve();
    demo_string_allocation();
    demo_pool_reuse();
    demo_autovectorization();
    simd_guidance();
    return 0;
}
