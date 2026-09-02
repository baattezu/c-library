// ===========================================================================
//  CACHE LOCALITY AND DATA LAYOUT — where the 10x lives
//  run: make opt F=07-performance/01-cache-and-data-layout.cpp     ← MUST be -O2
// ===========================================================================
//
//  Everything in this file has the SAME big-O. The differences are 2x to 50x.
//  This is the single most important performance file in the repo.
//
// ===========================================================================

#include "00-benchmark.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

// ===========================================================================
// 1. SEQUENTIAL vs STRIDED vs RANDOM — the cost of an access pattern
// ===========================================================================
void demo_access_patterns() {
    bench::header("1. Access patterns over the SAME 64 MB of data");

    constexpr std::size_t N = 16u * 1024 * 1024;      // 16M ints = 64 MB
    std::vector<int> data(N, 1);

    // Precompute index arrays so we measure the ACCESS, not the index generation.
    std::vector<std::uint32_t> seq(N), rnd(N);
    std::iota(seq.begin(), seq.end(), 0u);
    rnd = seq;
    std::shuffle(rnd.begin(), rnd.end(), std::mt19937{42});

    auto sum_sequential = [&]{
        std::int64_t s = 0;
        for (std::size_t i = 0; i < N; ++i) s += data[i];
        return s;
    };
    auto sum_stride16 = [&]{                 // 16 ints = 64 bytes = one x86 line
        std::int64_t s = 0;
        for (std::size_t i = 0; i < N; i += 16) s += data[i];
        return s;
    };
    auto sum_via_index_seq = [&]{            // same order, but indirected
        std::int64_t s = 0;
        for (std::size_t i = 0; i < N; ++i) s += data[seq[i]];
        return s;
    };
    auto sum_random = [&]{
        std::int64_t s = 0;
        for (std::size_t i = 0; i < N; ++i) s += data[rnd[i]];
        return s;
    };

    auto r1 = bench::run_once_per_sample("sequential (prefetcher wins)", sum_sequential);
    auto r2 = bench::run_once_per_sample("indexed but sequential order", sum_via_index_seq);
    auto r3 = bench::run_once_per_sample("stride 16 (1 elem per line)", sum_stride16);
    auto r4 = bench::run_once_per_sample("RANDOM order (cache+TLB miss)", sum_random);

    // Normalise to nanoseconds per element for a fair comparison.
    std::printf("  %-38s %9.3f ns/element\n", "sequential",        r1.min_ns / N);
    std::printf("  %-38s %9.3f ns/element\n", "indexed sequential",r2.min_ns / N);
    std::printf("  %-38s %9.3f ns/element (1/16 of the elements!)\n",
                                             "stride 16",         r3.min_ns / (N/16));
    std::printf("  %-38s %9.3f ns/element\n", "RANDOM",            r4.min_ns / N);
    std::printf("  random is %.1fx slower per element than sequential\n",
                (r4.min_ns / N) / (r1.min_ns / N));

    bench::note(
      "  WHAT YOU'RE SEEING:\n"
      "   - SEQUENTIAL: the hardware prefetcher detects the stride and fetches\n"
      "     ahead. Effectively you pay one DRAM trip per cache LINE (32 ints on\n"
      "     this 128-byte-line machine), and it's overlapped with computation.\n"
      "     You are bandwidth-bound, not latency-bound. This is the fast path.\n"
      "   - STRIDE 16: one useful int per line fetched. You touch 1/16 of the\n"
      "     elements but fetch a similar number of LINES, so per-element cost\n"
      "     explodes. You are paying for bytes you never read.\n"
      "   - RANDOM: the prefetcher is useless and every access is a full DRAM\n"
      "     round trip PLUS, over 64 MB, a TLB miss. This is the slow path, and\n"
      "     it is what pointer-chasing data structures do on every single node.\n"
      "\n"
      "  ⭐ THE DESIGN RULE: touch memory in the order it is laid out. If your\n"
      "     algorithm can't, consider changing the LAYOUT rather than the\n"
      "     algorithm.");
}

// ===========================================================================
// 2. ARRAY-OF-STRUCTS vs STRUCT-OF-ARRAYS
// ===========================================================================
//  The classic data-oriented-design transformation, and the reason columnar
//  databases (ClickHouse, Parquet, DuckDB, Arrow) exist.
// ===========================================================================
struct ParticleAoS {                    // 48 bytes
    float x, y, z;                      // position   — the hot fields
    float vx, vy, vz;                   // velocity   — the hot fields
    float mass;                         // cold
    std::uint32_t id;                   // cold
    char  name[16];                     // very cold, but rides along in the line
};

struct ParticlesSoA {                   // separate arrays, one per field
    std::vector<float> x, y, z;
    std::vector<float> vx, vy, vz;
    std::vector<float> mass;
    std::vector<std::uint32_t> id;
    explicit ParticlesSoA(std::size_t n)
        : x(n), y(n), z(n), vx(n), vy(n), vz(n), mass(n), id(n) {}
};

void demo_aos_vs_soa() {
    bench::header("2. AoS vs SoA — physics update on 2M particles");

    constexpr std::size_t N = 2'000'000;
    constexpr float DT = 0.016f;

    std::vector<ParticleAoS> aos(N);
    ParticlesSoA soa(N);
    for (std::size_t i = 0; i < N; ++i) {
        float f = static_cast<float>(i % 100);
        aos[i] = {f,f,f, 1.0f,1.0f,1.0f, f, static_cast<std::uint32_t>(i), {}};
        soa.x[i]=f; soa.y[i]=f; soa.z[i]=f;
        soa.vx[i]=1; soa.vy[i]=1; soa.vz[i]=1;
        soa.mass[i]=f; soa.id[i]=static_cast<std::uint32_t>(i);
    }

    auto update_aos = [&]{
        for (std::size_t i = 0; i < N; ++i) {
            aos[i].x += aos[i].vx * DT;
            aos[i].y += aos[i].vy * DT;
            aos[i].z += aos[i].vz * DT;
        }
        return aos[0].x;
    };
    auto update_soa = [&]{
        // Three tight, independent, contiguous loops. Each one auto-vectorises:
        // the compiler emits NEON instructions doing 4 floats at a time.
        for (std::size_t i = 0; i < N; ++i) soa.x[i] += soa.vx[i] * DT;
        for (std::size_t i = 0; i < N; ++i) soa.y[i] += soa.vy[i] * DT;
        for (std::size_t i = 0; i < N; ++i) soa.z[i] += soa.vz[i] * DT;
        return soa.x[0];
    };

    auto a = bench::run_once_per_sample("AoS: vector<Particle>", update_aos);
    auto s = bench::run_once_per_sample("SoA: struct of vectors", update_soa);
    a.print(); s.print(a.min_ns);

    std::printf("\n  sizeof(ParticleAoS) = %zu bytes\n", sizeof(ParticleAoS));
    std::printf("  AoS bytes FETCHED:  %zu MB (whole structs, incl. mass/id/name)\n",
                N * sizeof(ParticleAoS) / (1u<<20));
    std::printf("  SoA bytes FETCHED:  %zu MB (only x,y,z,vx,vy,vz)\n",
                N * 6 * sizeof(float) / (1u<<20));

    bench::note(
      "  WHY SoA WINS (two independent reasons):\n"
      "   1. YOU ONLY FETCH WHAT YOU USE. The AoS version drags mass, id, and a\n"
      "      16-byte name into cache on every particle, because they share the\n"
      "      cache line with x/y/z. That's ~40% of your memory bandwidth spent\n"
      "      on data the loop never reads.\n"
      "   2. IT VECTORISES. `soa.x[i] += soa.vx[i] * DT` over contiguous floats\n"
      "      becomes a NEON loop doing 4 (or 8) at once. The AoS version has a\n"
      "      48-byte stride between consecutive x values, so it cannot.\n"
      "\n"
      "  WHEN AoS IS RIGHT: when you touch MOST fields of ONE object at a time\n"
      "  (an object graph, a tree, a request handler). SoA is for when you touch\n"
      "  ONE field of MANY objects (a physics step, an aggregation, a filter).\n"
      "\n"
      "  ⭐ THIS IS WHY COLUMNAR DATABASES EXIST. `SELECT AVG(price) FROM trades`\n"
      "     in a row store reads every column of every row. In a column store it\n"
      "     reads one contiguous, vectorisable, compressible array. That's the\n"
      "     entire 100x difference between Postgres and ClickHouse on analytics.\n"
      "\n"
      "  AND THE MIDDLE GROUND: 'hot/cold splitting'. Keep {x,y,z,vx,vy,vz} in\n"
      "  one struct and {mass,id,name} in a parallel array. You get SoA's\n"
      "  bandwidth win without giving up object-at-a-time code entirely.");
}

// ===========================================================================
// 3. STRUCT SIZE — the free win
// ===========================================================================
void demo_struct_size() {
    bench::header("3. Shrinking a struct is a free speedup");

    struct Fat  { std::uint64_t id; double a, b, c; char tag; };            // 40
    struct Slim { std::uint32_t id; float  a, b, c; std::uint8_t tag; };    // 20

    constexpr std::size_t N = 4'000'000;
    std::vector<Fat>  fat(N);
    std::vector<Slim> slim(N);
    for (std::size_t i = 0; i < N; ++i) {
        fat[i]  = {i, 1.0, 2.0, 3.0, 'x'};
        slim[i] = {static_cast<std::uint32_t>(i), 1.0f, 2.0f, 3.0f, 1};
    }

    // ⚠️ BENCHMARK PITFALL, WORTH THE DETOUR:
    // The first version of this used `float s = 0; s += f.a;`. That measured
    // almost NO difference (1.1x) — because floating-point addition is not
    // associative, so the compiler cannot reorder or vectorise the reduction.
    // Every add waits ~4 cycles for the previous one, and the loop becomes
    // LATENCY-BOUND on the dependency chain, not memory-bound. Both versions
    // then run at the same speed and the memory difference is invisible.
    //
    // Summing an INTEGER field fixes it: integer adds are 1 cycle, the compiler
    // unrolls with multiple accumulators and vectorises, and the loop becomes
    // bandwidth-bound — which is what we actually wanted to measure.
    //
    // LESSON: always ask "what is this loop actually bottlenecked on?" before
    // believing a benchmark. (Fix for the FP case: use several accumulators by
    // hand, or -ffast-math if your domain tolerates reassociation.)
    auto sum_fat  = [&]{ std::int64_t s=0; for (const auto& f : fat)  s += static_cast<std::int64_t>(f.id); return s; };
    auto sum_slim = [&]{ std::int64_t s=0; for (const auto& f : slim) s += static_cast<std::int64_t>(f.id); return s; };

    auto rf = bench::run_once_per_sample("scan vector<Fat>  (40 B/elem)", sum_fat);
    auto rs = bench::run_once_per_sample("scan vector<Slim> (20 B/elem)", sum_slim);
    rf.print(); rs.print(rf.min_ns);

    std::printf("\n  sizeof(Fat)=%zu sizeof(Slim)=%zu -> %zu MB vs %zu MB for %zuM elements\n",
                sizeof(Fat), sizeof(Slim),
                N*sizeof(Fat)/(1u<<20), N*sizeof(Slim)/(1u<<20), N/1000000);

    bench::note(
      "  Scan time is proportional to BYTES TOUCHED, not element count. Halving\n"
      "  your struct roughly halves the traversal cost of every loop over it,\n"
      "  forever, for free.\n"
      "\n"
      "  THE SHRINKING TOOLKIT:\n"
      "   - order members largest-first (kills padding; see 02-memory/07)\n"
      "   - uint32_t indices instead of 8-byte pointers (also survives realloc)\n"
      "   - float instead of double when precision allows\n"
      "   - uint8_t/uint16_t enums and flags\n"
      "   - intern strings into a table; store a 4-byte id\n"
      "   - move rarely-used fields to a side table keyed by index");
}

// ===========================================================================
// 4. POINTER CHASING — why linked structures lose
// ===========================================================================
void demo_pointer_chasing() {
    bench::header("4. Pointer chasing vs a flat array");

    constexpr std::size_t N = 2'000'000;

    struct Node { std::int64_t value; Node* next; };

    // Build a linked list whose nodes are allocated in a SHUFFLED order, which
    // is what a long-running program's heap actually looks like.
    std::vector<std::unique_ptr<Node>> storage;
    storage.reserve(N);
    for (std::size_t i = 0; i < N; ++i)
        storage.push_back(std::make_unique<Node>(Node{static_cast<std::int64_t>(i), nullptr}));

    std::vector<std::size_t> order(N);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), std::mt19937{7});
    for (std::size_t i = 0; i + 1 < N; ++i)
        storage[order[i]]->next = storage[order[i+1]].get();
    Node* head = storage[order[0]].get();

    std::vector<std::int64_t> flat(N);
    std::iota(flat.begin(), flat.end(), 0);

    auto walk_list = [&]{
        std::int64_t s = 0;
        for (Node* p = head; p; p = p->next) s += p->value;   // DEPENDENT loads
        return s;
    };
    auto walk_array = [&]{
        std::int64_t s = 0;
        for (std::int64_t v : flat) s += v;                   // INDEPENDENT loads
        return s;
    };

    auto rl = bench::run_once_per_sample("linked list (dependent loads)", walk_list);
    auto ra = bench::run_once_per_sample("flat array  (prefetchable)   ", walk_array);
    rl.print(); ra.print();
    std::printf("  list is %.1fx slower — same O(n), same element count\n",
                rl.min_ns / ra.min_ns);

    bench::note(
      "  THE MECHANISM: `p = p->next` is a DEPENDENT load. The CPU cannot issue\n"
      "  the fetch for node k+1 until the fetch for node k has RETURNED. So you\n"
      "  serialise ~2M full memory latencies (~80ns each), with zero\n"
      "  memory-level parallelism and zero prefetching.\n"
      "\n"
      "  The array version issues dozens of independent loads concurrently and\n"
      "  the prefetcher runs ahead of the loop. This is why every 'obvious'\n"
      "  linked structure (list, tree of pointers, graph of shared_ptr) is\n"
      "  slower than it looks in the big-O.\n"
      "\n"
      "  THE FIX when you genuinely need links: store nodes in a std::vector and\n"
      "  use 32-bit INDICES instead of pointers. You get: half the memory,\n"
      "  contiguous allocation, stable references across growth, trivial\n"
      "  serialisation, and no allocator traffic. This is the standard technique\n"
      "  in compilers, ECS game engines, and graph databases.");
}

int main() {
    std::printf("⚠️  If you did not run this with `make opt`, every number below is a lie.\n");
    demo_access_patterns();
    demo_aos_vs_soa();
    demo_struct_size();
    demo_pointer_chasing();

    bench::note(
      "=== THE SUMMARY ===\n"
      "  Everything above has IDENTICAL big-O complexity. The differences come\n"
      "  entirely from how many BYTES you touch and in what ORDER.\n"
      "\n"
      "  YOUR OPTIMISATION CHECKLIST, IN ORDER:\n"
      "   1. Fix the algorithm (O(n^2) -> O(n log n)). Nothing else compares.\n"
      "   2. Make the layout contiguous and traverse it in order.\n"
      "   3. Shrink the hot struct. Split hot from cold fields.\n"
      "   4. Remove allocations from the hot path (reserve, arena, pool).\n"
      "   5. Make branches predictable, or remove them.\n"
      "   6. Only now: threads, SIMD, intrinsics.\n"
      "\n"
      "  And measure between every step. On Linux: `perf stat -e cycles,\n"
      "  instructions,cache-misses,LLC-load-misses ./prog`. IPC below ~1.0\n"
      "  means you are stalled on memory, and steps 2-3 are your fix.");
    return 0;
}
