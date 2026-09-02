// ===========================================================================
//  THREADS AND MUTEXES — bare metal after Java's ExecutorService
//  run:  make opt  F=06-concurrency/01-threads-and-mutexes.cpp
//  race: clang++ -std=c++23 -fsanitize=thread -O1 06-concurrency/01-threads-and-mutexes.cpp
// ===========================================================================
//
//  Java gives you: Thread, ExecutorService, CompletableFuture, synchronized,
//  ConcurrentHashMap, AtomicInteger, java.util.concurrent — a whole toolkit.
//
//  C++ gives you: std::thread, std::mutex, std::condition_variable,
//  std::atomic, and a memory model. Everything above that, you build.
//  (That's stage 9.)
//
//  ⚠️ AND THE BIG DIFFERENCE: in Java a data race gives you a stale or torn
//     value — bad, but bounded. In C++ a data race is UNDEFINED BEHAVIOUR.
//     The optimiser is allowed to assume it never happens and can transform
//     your code in ways that produce nonsense. There is no "benign" data race.
//
// ===========================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <numeric>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// The RAII wrapper you should use until std::jthread is available in your
// standard library. (Must be at namespace scope: a LOCAL class cannot have a
// member template — a small C++ rule worth knowing.)
class JoiningThread {
    std::thread t_;
public:
    template <class... A>
    explicit JoiningThread(A&&... a) : t_(std::forward<A>(a)...) {}
    ~JoiningThread() { if (t_.joinable()) t_.join(); }

    JoiningThread(const JoiningThread&)            = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;
    JoiningThread(JoiningThread&&) noexcept        = default;
    JoiningThread& operator=(JoiningThread&&) noexcept = default;

    std::thread& get() noexcept { return t_; }
};

// ---------------------------------------------------------------------------
void demo_basics() {
    std::cout << "\n=== 1. std::thread basics ===\n";

    std::printf("  hardware_concurrency() = %u\n", std::thread::hardware_concurrency());
    std::printf("  (on Apple Silicon this counts P-cores AND E-cores — your\n"
                "   threads may land on the slow ones, which makes benchmarks noisy)\n");

    // A thread starts running IMMEDIATELY on construction.
    std::thread t([]{
        std::printf("  worker thread id=%zu running\n",
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
    });

    // ⚠️ YOU MUST join() OR detach() BEFORE THE THREAD OBJECT IS DESTROYED.
    // Destroying a *joinable* std::thread calls std::terminate() — your whole
    // process aborts. This is deliberate: the alternatives (silently detaching,
    // or silently blocking) are both worse.
    t.join();
    std::printf("  joined.\n");

    // Arguments are COPIED (actually, decay-copied) into the thread. To pass a
    // reference you must be explicit — and then you own the lifetime problem.
    int value = 0;
    std::thread t2([&value]{ value = 42; });        // capture by reference
    t2.join();                                       // ...and join before `value` dies
    std::printf("  captured by reference: value=%d\n", value);

    std::thread t3(
        [](int by_val, int& by_ref){ by_ref = by_val * 2; },
        21, std::ref(value));                        // std::ref to pass a reference
    t3.join();
    std::printf("  std::ref: value=%d\n", value);

    std::cout <<
      "\n  ⚠️ std::jthread (C++20) joins automatically in its destructor and\n"
      "     supports std::stop_token for cooperative cancellation. It is NOT\n"
      "     available in Apple clang 16's libc++ — so on this machine, always\n"
      "     join() explicitly, ideally via an RAII wrapper (below).\n";

    // The RAII wrapper (defined above main's helpers) auto-joins:
    { JoiningThread jt([]{ std::printf("  JoiningThread ran and auto-joins\n"); }); }
}

// ---------------------------------------------------------------------------
// 2. THE DATA RACE
// ---------------------------------------------------------------------------
void demo_data_race() {
    std::cout << "\n=== 2. The data race ===\n";

    constexpr int THREADS = 8, ITERS = 100'000;

    // (a) UNSYNCHRONISED — this is UNDEFINED BEHAVIOUR, not just "wrong count".
    //     `++counter` is load, add, store: three steps that interleave.
    {
        long long counter = 0;                   // deliberately NOT atomic
        std::vector<std::thread> ts;
        for (int i = 0; i < THREADS; ++i)
            ts.emplace_back([&counter]{ for (int j = 0; j < ITERS; ++j) ++counter; });
        for (auto& t : ts) t.join();
        std::printf("  UNSYNCHRONISED: got %lld, expected %d\n", counter, THREADS * ITERS);
        if (counter == THREADS * ITERS) {
            std::printf(
              "     ⚠️⚠️ IT GOT THE RIGHT ANSWER — AND THAT IS THE SCARY PART.\n"
              "     At -O2 the compiler is ALLOWED to assume no data race exists,\n"
              "     so it hoists `counter` into a REGISTER for the whole loop and\n"
              "     writes back once. Each thread then does one store, and they\n"
              "     happen not to collide. The race is still UNDEFINED BEHAVIOUR.\n"
              "     It will 'work' in testing and corrupt data in production under\n"
              "     different timing, a different compiler, or a different -O level.\n"
              "     Run `clang++ -fsanitize=thread -O1` on this file: TSan reports\n"
              "     the race regardless of whether the output looks correct.\n"
              "     THIS is why 'I tested it and it worked' proves nothing about\n"
              "     concurrency.\n");
        } else {
            std::printf("     ^ lost updates: the classic load-add-store interleaving.\n");
        }
    }

    // (b) MUTEX — correct, but every increment is a lock/unlock.
    {
        long long counter = 0;
        std::mutex m;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> ts;
        for (int i = 0; i < THREADS; ++i)
            ts.emplace_back([&]{
                for (int j = 0; j < ITERS; ++j) {
                    std::lock_guard<std::mutex> lock(m);   // RAII lock
                    ++counter;
                }
            });
        for (auto& t : ts) t.join();
        auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
        std::printf("  MUTEX:          got %lld  in %.1f ms\n", counter, ms);
    }

    // (c) ATOMIC — correct and much faster for a single word.
    {
        std::atomic<long long> counter{0};
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> ts;
        for (int i = 0; i < THREADS; ++i)
            ts.emplace_back([&]{ for (int j = 0; j < ITERS; ++j) counter.fetch_add(1, std::memory_order_relaxed); });
        for (auto& t : ts) t.join();
        auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
        std::printf("  ATOMIC:         got %lld  in %.1f ms\n", counter.load(), ms);
    }

    // (d) SHARDED / THREAD-LOCAL — the FASTEST, by a mile.
    //     No sharing at all during the work; combine once at the end.
    {
        std::vector<long long> partials(THREADS, 0);
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> ts;
        for (int i = 0; i < THREADS; ++i)
            ts.emplace_back([&partials, i]{
                long long local = 0;                       // a REGISTER, not memory
                for (int j = 0; j < ITERS; ++j) ++local;
                partials[static_cast<std::size_t>(i)] = local;   // one write at the end
            });
        for (auto& t : ts) t.join();
        long long total = std::accumulate(partials.begin(), partials.end(), 0LL);
        auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
        std::printf("  SHARDED:        got %lld  in %.1f ms\n", total, ms);
        std::printf("     (this one is almost free partly because the optimiser can\n"
                    "      collapse the private loop entirely — which is exactly the\n"
                    "      point: private data can be optimised, shared data cannot.)\n");
    }

    std::cout <<
      "\n  ⭐ THE LESSON OF HIGH-LOAD CONCURRENCY, IN ONE LINE:\n"
      "     THE FASTEST SYNCHRONISATION IS NO SHARING.\n"
      "\n"
      "  Even a lock-free atomic increment forces the cache line holding the\n"
      "  counter to bounce between cores (~100ns each time). 8 cores hammering\n"
      "  one atomic is 8 cores taking turns. That's not parallelism.\n"
      "\n"
      "  This is why the winning architecture is THREAD-PER-CORE with private\n"
      "  state, combined rarely — Seastar, ScyllaDB, DPDK, Redis, per-core\n"
      "  sharding in every real high-throughput system. It's also why\n"
      "  LongAdder exists in Java: it's this exact sharding trick.\n";
}

// ---------------------------------------------------------------------------
// 3. MUTEXES IN DETAIL
// ---------------------------------------------------------------------------
class ThreadSafeCounter {
    // `mutable` so we can lock inside a const member function. This is the
    // canonical, legitimate use of `mutable`.
    mutable std::shared_mutex mtx_;
    std::vector<int>          data_;

public:
    void add(int v) {
        std::unique_lock lock(mtx_);       // EXCLUSIVE (writer) lock
        data_.push_back(v);
    }
    // Many readers can hold a shared lock simultaneously; a writer excludes all.
    // (Java: ReentrantReadWriteLock. Only worth it when reads vastly outnumber
    //  writes AND the critical section is long — otherwise the extra bookkeeping
    //  makes it SLOWER than a plain mutex. Measure.)
    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock(mtx_);       // SHARED (reader) lock
        return data_.size();
    }
    [[nodiscard]] long long sum() const {
        std::shared_lock lock(mtx_);
        return std::accumulate(data_.begin(), data_.end(), 0LL);
    }
};

void demo_mutex_types() {
    std::cout << "\n=== 3. The lock types ===\n";

    ThreadSafeCounter c;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&c, i]{ for (int j = 0; j < 1000; ++j) c.add(i); });
    for (auto& t : ts) t.join();
    std::printf("  shared_mutex counter: size=%zu sum=%lld\n", c.size(), c.sum());

    std::cout <<
      "\n  THE LOCK VOCABULARY\n"
      "    std::mutex               basic, non-recursive, non-timed\n"
      "    std::recursive_mutex     same thread may lock it N times. Its need is\n"
      "                             almost always a design smell (Java's\n"
      "                             `synchronized` is recursive by default,\n"
      "                             which hides the smell).\n"
      "    std::timed_mutex         try_lock_for / try_lock_until\n"
      "    std::shared_mutex        readers/writer (C++17)\n"
      "\n"
      "    std::lock_guard<M>       simplest RAII lock. Cannot unlock early.\n"
      "    std::unique_lock<M>      movable, deferrable, timed, unlockable.\n"
      "                             REQUIRED by condition_variable.\n"
      "    std::shared_lock<M>      the reader half of shared_mutex\n"
      "    std::scoped_lock         locks N mutexes at once, DEADLOCK-FREE\n"
      "                             (C++17). Prefer it over lock_guard.\n"
      "\n"
      "    std::call_once + std::once_flag   run an initialiser exactly once\n"
      "                             (though a function-local static is simpler\n"
      "                              and does the same thing).\n";
}

// ---------------------------------------------------------------------------
// 4. DEADLOCK
// ---------------------------------------------------------------------------
void demo_deadlock() {
    std::cout << "\n=== 4. Deadlock and how to not have it ===\n";

    std::mutex a, b;

    std::cout <<
      "  THE CLASSIC:\n"
      "    thread 1: lock(a); lock(b);\n"
      "    thread 2: lock(b); lock(a);      <- opposite order = deadlock\n"
      "\n"
      "  Deadlock needs all FOUR of Coffman's conditions. Break any one:\n"
      "    1. mutual exclusion   -> use lock-free / immutable data\n"
      "    2. hold and wait      -> acquire everything at once (std::scoped_lock)\n"
      "    3. no preemption      -> use try_lock with a timeout and back off\n"
      "    4. circular wait      -> IMPOSE A GLOBAL LOCK ORDER  <- the usual fix\n";

    // FIX 1: std::scoped_lock takes all mutexes at once, using a deadlock-
    // avoidance algorithm (try/back-off). This is the modern default.
    {
        std::scoped_lock lock(a, b);        // order doesn't matter here
        std::printf("  scoped_lock(a, b) acquired both safely\n");
    }
    {
        std::scoped_lock lock(b, a);        // reversed — still safe
        std::printf("  scoped_lock(b, a) also safe — no deadlock possible\n");
    }

    std::cout <<
      "\n  PRACTICAL DEADLOCK RULES\n"
      "   1. Prefer ONE lock. Two locks is where the trouble starts.\n"
      "   2. If you need several, take them with std::scoped_lock, or always in\n"
      "      a documented global order (e.g. by address, or by a level number).\n"
      "   3. NEVER call unknown code (a callback, a virtual function, a user\n"
      "      lambda) while holding a lock. It might lock something else, or\n"
      "      re-enter you. This is the #1 real-world deadlock cause.\n"
      "   4. Keep critical sections TINY. Compute outside the lock; only the\n"
      "      mutation goes inside.\n"
      "   5. Don't do I/O or allocate while holding a lock if you can help it.\n"
      "   6. Use ThreadSanitizer in CI: it detects both races AND lock-order\n"
      "      inversions, before they deadlock in production.\n"
      "        clang++ -fsanitize=thread -O1 file.cpp\n";
}

// ---------------------------------------------------------------------------
void the_summary() {
    std::cout << "\n=== 5. Summary ===\n";
    std::cout <<
      "  - std::thread must be joined or detached before destruction, or\n"
      "    std::terminate. Wrap it in RAII (jthread when your libc++ has it).\n"
      "  - A data race is UNDEFINED BEHAVIOUR in C++, not just a wrong value.\n"
      "  - lock_guard for one mutex, scoped_lock for several, unique_lock when\n"
      "    you need flexibility or a condition_variable.\n"
      "  - `mutable std::mutex` is the right way to lock in a const method.\n"
      "  - shared_mutex only pays off with long, read-dominated critical sections.\n"
      "  - Impose a lock ORDER. Never call out to unknown code under a lock.\n"
      "  - Run your tests under -fsanitize=thread.\n"
      "  - AND ABOVE ALL: design so threads don't share mutable state. Shard by\n"
      "    core, pass messages, or use immutable snapshots.\n";
}

int main() {
    demo_basics();
    demo_data_race();
    demo_mutex_types();
    demo_deadlock();
    the_summary();
    return 0;
}
