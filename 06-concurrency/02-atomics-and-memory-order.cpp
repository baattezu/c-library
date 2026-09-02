// ===========================================================================
//  ATOMICS AND THE MEMORY MODEL — the hardest topic in the language
//  run: make opt F=06-concurrency/02-atomics-and-memory-order.cpp
// ===========================================================================
//
//  You already know this material from Java — you just know it by other names.
//  The C++11 and Java 5 memory models are close cousins (both descend from the
//  same research), so here is the dictionary:
//
//    Java                          C++
//    ------------------------      ---------------------------------
//    volatile field                std::atomic<T> with seq_cst (the default)
//    happens-before                the same concept, spelled the same
//    AtomicInteger.incrementAndGet fetch_add(1)
//    compareAndSet                 compare_exchange_strong / _weak
//    VarHandle.setRelease          store(v, memory_order_release)
//    VarHandle.getAcquire          load(memory_order_acquire)
//    VarHandle.setOpaque           store(v, memory_order_relaxed)
//    (no equivalent)               memory_order_consume (deprecated, avoid)
//    final field freeze            (roughly) release on construction
//
//  ⚠️ C++'s `volatile` is NOT Java's volatile. C++ `volatile` means
//     "this memory may change outside the program" — it is for
//     MEMORY-MAPPED HARDWARE REGISTERS and signal handlers. It provides NO
//     atomicity and NO ordering guarantees between threads. Using it for
//     threading is a bug. Use std::atomic.
//
// ===========================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
void demo_atomic_basics() {
    std::cout << "\n=== 1. What an atomic guarantees ===\n";

    std::atomic<int> a{0};

    // Every one of these is a single, indivisible operation. No other thread can
    // observe a half-completed state, and no torn read/write is possible.
    a.store(10);
    int x = a.load();
    int old = a.exchange(20);                       // set and return the old value
    a.fetch_add(5);                                 // atomic +=, returns old
    a.fetch_sub(2);
    a.fetch_or(0x100);

    std::printf("  load=%d exchange returned %d, now %d\n", x, old, a.load());

    // COMPARE-AND-SWAP: the primitive that everything lock-free is built on.
    // "If the value is still `expected`, set it to `desired`. Otherwise, write
    //  the actual value back into `expected` and fail."
    int expected = a.load();
    bool ok = a.compare_exchange_strong(expected, 999);
    std::printf("  CAS from %d -> 999: %s (a is now %d)\n",
                expected, ok ? "succeeded" : "failed", a.load());

    // The canonical CAS loop — how you do ANY atomic read-modify-write:
    std::atomic<int> max_seen{0};
    auto update_max = [&max_seen](int candidate) {
        int cur = max_seen.load(std::memory_order_relaxed);
        // compare_exchange_WEAK may fail SPURIOUSLY on ARM (it maps to
        // LDXR/STXR, which can fail from an interrupt). That's fine and even
        // preferred INSIDE A LOOP, because it's cheaper. Use _strong when you
        // are NOT looping.
        while (candidate > cur &&
               !max_seen.compare_exchange_weak(cur, candidate,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
            // on failure, `cur` was updated with the actual value — retry
        }
    };
    update_max(5); update_max(3); update_max(9);
    std::printf("  CAS-loop max: %d\n", max_seen.load());

    // is_lock_free: is this atomic implemented with real instructions, or does
    // the library fall back to a hidden mutex? Anything > 16 bytes usually
    // falls back — and then it isn't lock-free or signal-safe at all.
    std::printf("  atomic<int>       lock-free? %d\n", std::atomic<int>{}.is_lock_free());
    std::printf("  atomic<long long> lock-free? %d\n", std::atomic<long long>{}.is_lock_free());
    struct Big { double a,b,c,d; };
    std::printf("  atomic<32-byte struct> lock-free? %d  <- uses a hidden mutex!\n",
                std::atomic<Big>{}.is_lock_free());
    std::cout << "  ALWAYS check is_lock_free() (or is_always_lock_free) before\n"
                 "  assuming your atomic is fast.\n";
}

// ---------------------------------------------------------------------------
// 2. THE MEMORY ORDERINGS
// ---------------------------------------------------------------------------
void explain_memory_orders() {
    std::cout << "\n=== 2. The six memory orderings ===\n";
    std::cout <<
      "  WHY THIS EXISTS: the compiler AND the CPU both reorder memory\n"
      "  operations to go faster. Single-threaded, you never notice (they\n"
      "  preserve the illusion). Multi-threaded, you absolutely do.\n"
      "  A memory order tells them how much reordering you can tolerate.\n"
      "\n"
      "  ── memory_order_relaxed ──────────────────────────────────────────\n"
      "     ATOMICITY ONLY. No ordering with respect to other memory ops.\n"
      "     Fastest. Correct for: counters/statistics you only read at the end,\n"
      "     and reference counts being INCREMENTED.\n"
      "     Java: VarHandle.getOpaque/setOpaque.\n"
      "\n"
      "  ── memory_order_acquire (on loads) ───────────────────────────────\n"
      "     Nothing after this load can be reordered before it.\n"
      "     Everything a releasing thread wrote BEFORE its release store is\n"
      "     visible to you after this load. \"Acquire the data.\"\n"
      "     Java: VarHandle.getAcquire, or reading a volatile.\n"
      "\n"
      "  ── memory_order_release (on stores) ──────────────────────────────\n"
      "     Nothing before this store can be reordered after it.\n"
      "     All your prior writes become visible to an acquiring reader.\n"
      "     \"Publish the data.\"  Java: VarHandle.setRelease / volatile write.\n"
      "\n"
      "  ── memory_order_acq_rel (on read-modify-write) ───────────────────\n"
      "     Both, for operations like fetch_add / compare_exchange.\n"
      "\n"
      "  ── memory_order_seq_cst  ← THE DEFAULT ───────────────────────────\n"
      "     Acquire+release PLUS a single global total order that all threads\n"
      "     agree on. The easiest to reason about. Costs a full barrier\n"
      "     (DMB ISH on arm64; on x86 stores become XCHG).\n"
      "     Java: a plain `volatile` field is seq_cst.\n"
      "\n"
      "  ── memory_order_consume ──────────────────────────────────────────\n"
      "     A weaker acquire for dependency chains. Every compiler promotes it\n"
      "     to acquire. Its specification is broken and it is being reworked.\n"
      "     DO NOT USE IT.\n"
      "\n"
      "  ⭐ THE RULE FOR HUMANS:\n"
      "     Use the DEFAULT (seq_cst) until you have measured that it matters.\n"
      "     Then use acquire/release, which is the natural fit for 99% of real\n"
      "     patterns (publish a value / consume a value).\n"
      "     Use relaxed ONLY for independent counters.\n"
      "     Getting this wrong produces bugs that appear once a month on one\n"
      "     machine under load. It is not worth guessing.\n";
}

// ---------------------------------------------------------------------------
// 3. ACQUIRE/RELEASE IN ACTION — the publish/consume pattern
// ---------------------------------------------------------------------------
void demo_acquire_release() {
    std::cout << "\n=== 3. Publishing data with release/acquire ===\n";

    struct Payload { int a{}, b{}, c{}; };
    Payload            data;              // ORDINARY, non-atomic data
    std::atomic<bool>  ready{false};      // the FLAG that publishes it

    std::thread producer([&]{
        // These are plain, non-atomic writes.
        data.a = 1; data.b = 2; data.c = 3;
        // The release store is a ONE-WAY BARRIER: everything above is
        // guaranteed to be visible to anyone who ACQUIRES this flag.
        ready.store(true, std::memory_order_release);
    });

    std::thread consumer([&]{
        // Spin until the flag is set, with an acquire load. Once we see `true`,
        // we are guaranteed to see a==1, b==2, c==3.
        while (!ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::printf("  consumer sees a=%d b=%d c=%d  (guaranteed by release/acquire)\n",
                    data.a, data.b, data.c);
    });

    producer.join(); consumer.join();

    std::cout <<
      "\n  ⭐ THIS IS THE MOST IMPORTANT PATTERN IN LOCK-FREE PROGRAMMING.\n"
      "     Note: `data` is NOT atomic and does NOT need to be. The single\n"
      "     atomic flag carries all the ordering. That's the whole trick, and\n"
      "     it is exactly what a mutex does internally (lock = acquire,\n"
      "     unlock = release).\n"
      "\n"
      "     WITHOUT the orderings (using relaxed on both), the consumer could\n"
      "     legally see ready==true but a==0 — the CPU is free to make the flag\n"
      "     store visible before the data stores. On x86 you'd probably never\n"
      "     see it; on ARM (this machine) you would, eventually, under load.\n"
      "     THAT is why 'it works on my laptop' is not a concurrency test.\n";
}

// ---------------------------------------------------------------------------
// 4. THE COST OF EACH ORDERING
// ---------------------------------------------------------------------------
void benchmark_orderings() {
    std::cout << "\n=== 4. What each ordering costs ===\n";

    constexpr int N = 20'000'000;
    std::atomic<long long> counter{0};

    auto bench = [&](const char* label, std::memory_order mo) {
        counter.store(0);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) counter.fetch_add(1, mo);
        auto ns = std::chrono::duration<double,std::nano>(
                      std::chrono::steady_clock::now()-t0).count() / N;
        std::printf("  %-32s %6.2f ns/op\n", label, ns);
    };

    // Single-threaded, so there's no contention — this measures the BARRIER
    // cost alone, not the cache-coherence cost.
    bench("fetch_add relaxed",   std::memory_order_relaxed);
    bench("fetch_add acq_rel",   std::memory_order_acq_rel);
    bench("fetch_add seq_cst",   std::memory_order_seq_cst);

    // Non-atomic baseline for scale:
    {
        volatile long long plain = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) plain = plain + 1;
        auto ns = std::chrono::duration<double,std::nano>(
                      std::chrono::steady_clock::now()-t0).count() / N;
        std::printf("  %-32s %6.2f ns/op\n", "volatile non-atomic ++", ns);
    }

    std::cout <<
      "\n  ⚠️ THIS IS THE UNCONTENDED COST. It is the SMALL part.\n"
      "     Under contention, the dominant cost is the CACHE LINE bouncing\n"
      "     between cores (~100ns per transfer). See 07-performance/\n"
      "     04-false-sharing.cpp — that's where the real numbers are.\n"
      "\n"
      "     On arm64 (this machine) the orderings differ meaningfully because\n"
      "     ARM has a WEAK memory model and needs explicit DMB barriers.\n"
      "     On x86-64, which is strongly ordered, acquire/release loads and\n"
      "     stores are FREE (plain movs) and only seq_cst stores cost anything.\n"
      "     So: code that seems correct on x86 can break on ARM. Both your\n"
      "     laptop and modern servers (Graviton, Ampere) are ARM. Test there.\n";
}

// ---------------------------------------------------------------------------
// 5. THE PATTERNS
// ---------------------------------------------------------------------------
void the_patterns() {
    std::cout << "\n=== 5. The patterns worth memorising ===\n";
    std::cout <<
      "  (a) STATISTICS COUNTER — relaxed is correct and fastest.\n"
      "        requests.fetch_add(1, std::memory_order_relaxed);\n"
      "      You only need the total to be right when you finally read it.\n"
      "      (Better still: per-thread counters, summed at read time.)\n"
      "\n"
      "  (b) PUBLISH / SUBSCRIBE — release store, acquire load. (demo 3 above)\n"
      "\n"
      "  (c) REFERENCE COUNTING — the shared_ptr recipe:\n"
      "        increment: fetch_add(1, relaxed)      // just need atomicity\n"
      "        decrement: if (fetch_sub(1, acq_rel) == 1) delete;\n"
      "      The release on decrement ensures all your writes are visible to\n"
      "      whoever destroys the object; the acquire ensures you see everyone\n"
      "      else's writes before you destroy it.\n"
      "\n"
      "  (d) SPINLOCK — for critical sections of a few nanoseconds ONLY:\n"
      "        std::atomic_flag f = ATOMIC_FLAG_INIT;\n"
      "        while (f.test_and_set(std::memory_order_acquire))\n"
      "            ;                             // spin (add a pause/yield!)\n"
      "        ... critical section ...\n"
      "        f.clear(std::memory_order_release);\n"
      "      ⚠️ Spinlocks are usually the WRONG answer in userspace: if the\n"
      "         holder gets descheduled, you burn a whole timeslice spinning.\n"
      "         std::mutex on macOS/Linux already spins briefly before parking,\n"
      "         which gives you the best of both. Use std::mutex.\n"
      "\n"
      "  (e) atomic<T>::wait/notify (C++20) — futex-based blocking without a\n"
      "      mutex. `a.wait(old)` sleeps until the value changes.\n"
      "\n"
      "  (f) THE ABA PROBLEM — lock-free code's signature hazard:\n"
      "        thread 1 reads A, is preempted\n"
      "        thread 2 changes A -> B -> A (freeing and reusing a node)\n"
      "        thread 1's CAS succeeds because it still sees A — but the world\n"
      "        changed underneath it.\n"
      "      Fixes: tagged pointers (a counter in the unused high bits),\n"
      "      hazard pointers, epoch-based reclamation, or RCU.\n"
      "      MEMORY RECLAMATION is the genuinely hard part of lock-free data\n"
      "      structures — a GC language gets it free, which is why lock-free\n"
      "      queues are much easier to write in Java than in C++.\n";
}

int main() {
    demo_atomic_basics();
    explain_memory_orders();
    demo_acquire_release();
    benchmark_orderings();
    the_patterns();
    std::cout <<
      "\n>>> HONEST ADVICE: do not write lock-free data structures in production\n"
      "    until you have read Herlihy & Shavit and can prove your algorithm.\n"
      "    Use a mutex, or a proven library (folly, moodycamel, boost::lockfree).\n"
      "    Understand this material so you can READ and DEBUG such code, and so\n"
      "    you know what your mutex is actually doing. That is the real payoff.\n";
    return 0;
}
