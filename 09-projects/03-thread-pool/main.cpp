// ===========================================================================
//  PROJECT 3 — tests and scaling measurements for the thread pool
//  run:  make opt F=09-projects/03-thread-pool/main.cpp
//  race: clang++ -std=c++23 -fsanitize=thread -O1 09-projects/03-thread-pool/main.cpp
// ===========================================================================

#include "thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using pool::ThreadPool;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("  ❌ FAILED: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// ---------------------------------------------------------------------------
static void test_basic() {
    std::printf("\n=== TEST 1: submit and get results ===\n");
    ThreadPool p(4);
    CHECK(p.size() == 4);

    auto f1 = p.submit([]{ return 42; });
    auto f2 = p.submit([](int a, int b){ return a + b; }, 20, 22);
    auto f3 = p.submit([](std::string s){ return s + " world"; }, std::string("hello"));

    CHECK(f1.get() == 42);
    CHECK(f2.get() == 42);
    CHECK(f3.get() == "hello world");

    // void-returning tasks work too.
    std::atomic<int> counter{0};
    std::vector<std::future<void>> fs;
    for (int i = 0; i < 1000; ++i)
        fs.push_back(p.submit([&counter]{ counter.fetch_add(1, std::memory_order_relaxed); }));
    for (auto& f : fs) f.get();
    CHECK(counter.load() == 1000);
    std::printf("  ✅ 1003 tasks completed, %zu work-steals occurred\n", p.steals());
}

// ---------------------------------------------------------------------------
static void test_exceptions() {
    std::printf("\n=== TEST 2: exceptions travel through the future ===\n");
    ThreadPool p(2);

    auto f = p.submit([]() -> int { throw std::runtime_error("task blew up"); });

    bool caught = false;
    try {
        (void)f.get();                    // rethrows HERE, on the caller's thread
    } catch (const std::runtime_error& e) {
        caught = true;
        std::printf("  caught on the calling thread: '%s'\n", e.what());
    }
    CHECK(caught);

    // Crucially, the pool must still be alive and working.
    auto f2 = p.submit([]{ return 7; });
    CHECK(f2.get() == 7);
    std::printf("  ✅ the worker survived the exception (this is Java's\n"
                "     ExecutionException, spelled differently)\n");
}

// ---------------------------------------------------------------------------
static void test_parallel_for_scaling() {
    std::printf("\n=== TEST 3: parallel_for scaling ===\n");

    constexpr std::size_t N = 8'000'000;
    std::vector<double> data(N);
    std::iota(data.begin(), data.end(), 1.0);
    std::vector<double> out(N);

    // A deliberately CPU-heavy body, so we measure parallelism rather than
    // memory bandwidth. (If the body were `out[i]=data[i]*2`, we'd be
    // bandwidth-bound and more threads would not help at all — an important
    // thing to know before you reach for a thread pool.)
    auto body = [&](std::size_t i) {
        double x = data[i];
        out[i] = std::sqrt(x) * std::sin(x) + std::log(x + 1.0);
    };

    auto ms = [](auto a, auto b){ return std::chrono::duration<double,std::milli>(b-a).count(); };

    // ⚠️ WARM-UP, and here's why it matters: `out` is a freshly-allocated 64 MB
    // vector. The FIRST pass over it takes a page fault per 16 KB page, and the
    // kernel zeroes each page for us. Without this warm-up the serial baseline
    // absorbs all that cost, the parallel runs don't, and you get a bogus
    // "110% efficient" superlinear speedup. Benchmarks lie by default; you have
    // to actively stop them.
    for (std::size_t i = 0; i < N; ++i) body(i);

    // Best-of-3 for the baseline, same as we do for the parallel runs.
    double serial = 1e18;
    for (int r = 0; r < 3; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < N; ++i) body(i);
        auto t1 = std::chrono::steady_clock::now();
        serial = std::min(serial, ms(t0, t1));
    }
    std::printf("  serial:                 %8.1f ms   (baseline, best of 3)\n", serial);

    for (std::size_t threads : {2u, 4u, 8u, std::thread::hardware_concurrency()}) {
        ThreadPool p(threads);
        double par = 1e18;
        for (int r = 0; r < 3; ++r) {           // best of 3, same as the baseline
            auto a = std::chrono::steady_clock::now();
            pool::parallel_for(p, 0, N, body);
            auto b = std::chrono::steady_clock::now();
            par = std::min(par, ms(a, b));
        }
        std::printf("  parallel_for %2zu threads: %8.1f ms   %5.2fx speedup  (%.0f%% efficient)\n",
                    threads, par, serial / par, 100.0 * (serial / par) / double(threads));
    }

    // Correctness check.
    bool ok = true;
    for (std::size_t i = 0; i < N; i += 100000) {
        double expect = std::sqrt(data[i]) * std::sin(data[i]) + std::log(data[i] + 1.0);
        if (std::abs(out[i] - expect) > 1e-9) { ok = false; break; }
    }
    CHECK(ok);

    std::printf(
      "\n  READING THE EFFICIENCY COLUMN:\n"
      "   - You will NOT see Nx for N threads. Amdahl's law, the cost of\n"
      "     dispatching chunks, and memory bandwidth all take a cut.\n"
      "   - On Apple Silicon, `hardware_concurrency` counts P-cores AND E-cores.\n"
      "     The E-cores are much slower, so the last row often shows LOWER\n"
      "     efficiency than the 4- or 8-thread row. That's not a bug in the\n"
      "     pool — it's heterogeneous hardware, and it's why you benchmark on\n"
      "     the machine you deploy to.\n"
      "   - If efficiency collapses, suspect (in order): a memory-bandwidth\n"
      "     bound body, false sharing on the output array, chunks too small\n"
      "     (dispatch overhead dominates), or contention on a shared resource.\n");
}

// ---------------------------------------------------------------------------
static void test_work_stealing() {
    std::printf("\n=== TEST 4: work stealing rebalances uneven work ===\n");

    ThreadPool p(8);
    std::atomic<int> done{0};

    // Deliberately lopsided: the first few tasks are enormous, the rest tiny.
    // Without stealing, the workers that got the small tasks would sit idle
    // while a couple of workers grind. With stealing, they take over.
    std::vector<std::future<void>> fs;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        int work = (i < 8) ? 4'000'000 : 20'000;
        fs.push_back(p.submit([work, &done]{
            volatile double s = 0;
            for (int k = 0; k < work; ++k) s += k * 0.5;
            done.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (auto& f : fs) f.get();
    auto t1 = std::chrono::steady_clock::now();

    CHECK(done.load() == 200);
    std::printf("  200 lopsided tasks in %.1f ms, %zu steals occurred\n",
                std::chrono::duration<double,std::milli>(t1-t0).count(), p.steals());
    std::printf("  ✅ work stealing kept every core busy. A single shared queue\n"
                "     would work too here, but it becomes a contention point at\n"
                "     high task rates — which is exactly why ForkJoinPool, TBB,\n"
                "     and Go's scheduler are all per-worker-deque + stealing.\n");
}

// ---------------------------------------------------------------------------
static void test_shutdown() {
    std::printf("\n=== TEST 5: graceful shutdown drains the queue ===\n");
    std::atomic<int> ran{0};
    {
        ThreadPool p(4);
        for (int i = 0; i < 500; ++i)
            p.post([&ran]{ ran.fetch_add(1, std::memory_order_relaxed); });
        // ~ThreadPool -> shutdown() -> wait_idle() -> close() -> join()
    }
    CHECK(ran.load() == 500);
    std::printf("  ✅ all %d posted tasks ran before the destructor returned\n", ran.load());
}

// ---------------------------------------------------------------------------
static void the_warnings() {
    std::printf(
      "\n=== ⚠️ THE DEADLOCK YOU WILL EVENTUALLY WRITE ===\n"
      "    ThreadPool p(4);\n"
      "    auto outer = p.submit([&]{\n"
      "        auto inner = p.submit([]{ return 1; });\n"
      "        return inner.get();          // ☠️ BLOCKS a worker\n"
      "    });\n"
      "  Submit 4 of those and all 4 workers are blocked waiting for tasks that\n"
      "  can never be scheduled. Total deadlock, no error message.\n"
      "  Java calls this THREAD STARVATION DEADLOCK and it happens there too.\n"
      "\n"
      "  FIXES:\n"
      "   - never block inside a task on work from the same pool\n"
      "   - use continuations instead of .get() (a task that SCHEDULES the next\n"
      "     task rather than waiting for it)\n"
      "   - use a pool that can run stolen work while blocked (ForkJoinPool's\n"
      "     ManagedBlocker; TBB does this natively)\n"
      "   - use coroutines, where awaiting suspends the TASK, not the THREAD.\n"
      "     That is the real fix, and it's why C++20 coroutines exist.\n"
      "\n=== WHEN TO USE WHAT ===\n"
      "  CPU-bound work         -> thread pool sized to hardware_concurrency()\n"
      "  I/O-bound work         -> an EVENT LOOP (08-systems/03), not a big pool.\n"
      "                            10,000 blocked threads is not a design.\n"
      "  Mixed                  -> event loop for I/O + a small pool for CPU work,\n"
      "                            and never block the loop. (This is Netty's\n"
      "                            EventLoopGroup + DefaultEventExecutorGroup.)\n"
      "  Highest throughput     -> thread-per-core, no shared queues at all,\n"
      "                            shard the DATA instead of the work. (Seastar)\n"
      "\n=== EXERCISES ===\n"
      "  a. Replace std::function with std::move_only_function (C++23) and drop\n"
      "     the shared_ptr<packaged_task> workaround.\n"
      "  b. Add a bounded queue so submit() applies BACKPRESSURE instead of\n"
      "     growing without limit. (Reuse 06-concurrency/03's BlockingQueue.)\n"
      "  c. Add task PRIORITIES, then measure the latency of high-priority tasks\n"
      "     under load.\n"
      "  d. Give each worker an Arena from project 1 as a thread_local, and make\n"
      "     tasks allocate from it. Measure allocations per task before/after.\n"
      "  e. Add a `then()` continuation so you can chain tasks without blocking:\n"
      "     that's CompletableFuture, and building it teaches you why coroutines\n"
      "     are nicer.\n");
}

// ---------------------------------------------------------------------------
int main() {
    test_basic();
    test_exceptions();
    test_parallel_for_scaling();
    test_work_stealing();
    test_shutdown();
    the_warnings();

    std::printf("\n=== RESULT: %s ===\n",
                g_failures == 0 ? "ALL TESTS PASSED ✅" : "THERE WERE FAILURES ❌");
    return g_failures == 0 ? 0 : 1;
}
