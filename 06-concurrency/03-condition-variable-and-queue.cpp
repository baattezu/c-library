// ===========================================================================
//  CONDITION VARIABLES AND A BLOCKING QUEUE
//  run: make opt F=06-concurrency/03-condition-variable-and-queue.cpp
// ===========================================================================
//
//  A mutex answers "who may touch this?". A condition variable answers
//  "how do I WAIT until something becomes true, without burning a core?".
//
//  Java equivalent: Object.wait()/notify(), or Condition.await()/signal().
//  The semantics are nearly identical — including SPURIOUS WAKEUPS, which is
//  why you must always wait in a loop (or use the predicate overload).
//
//  This file builds a bounded blocking queue, which is the single most useful
//  concurrency primitive there is — it's the heart of every thread pool,
//  producer/consumer pipeline, and log writer.
//
// ===========================================================================

#include <atomic>
#include <chrono>
#include <barrier>
#include <condition_variable>
#include <latch>
#include <semaphore>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
//  A BOUNDED BLOCKING QUEUE (Java: ArrayBlockingQueue)
// ===========================================================================
template <class T>
class BlockingQueue {
    mutable std::mutex      mtx_;
    std::condition_variable not_empty_;     // consumers wait here
    std::condition_variable not_full_;      // producers wait here
    std::queue<T>           q_;
    std::size_t             capacity_;
    bool                    closed_ = false;

public:
    explicit BlockingQueue(std::size_t cap) : capacity_(cap) {}

    // Returns false if the queue was closed before we could push.
    bool push(T item) {
        std::unique_lock lock(mtx_);        // unique_lock, not lock_guard —
                                            // the cv needs to unlock/relock it
        // ⭐ ALWAYS use the PREDICATE overload of wait(). It expands to:
        //      while (!pred()) cv.wait(lock);
        // The loop is MANDATORY because of:
        //   (a) SPURIOUS WAKEUPS — the OS may wake you for no reason
        //   (b) STOLEN WAKEUPS — another thread may grab the item first
        // Writing `if (empty) cv.wait(lock);` is a real, classic bug.
        not_full_.wait(lock, [this]{ return q_.size() < capacity_ || closed_; });

        if (closed_) return false;
        q_.push(std::move(item));
        lock.unlock();                       // ⭐ unlock BEFORE notifying, so the
                                             // woken thread doesn't immediately
                                             // block on the mutex we still hold
        not_empty_.notify_one();
        return true;
    }

    // Blocks until an item is available, or the queue is closed and drained.
    std::optional<T> pop() {
        std::unique_lock lock(mtx_);
        not_empty_.wait(lock, [this]{ return !q_.empty() || closed_; });

        if (q_.empty()) return std::nullopt;   // closed and drained
        T item = std::move(q_.front());
        q_.pop();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    // Non-blocking variant.
    std::optional<T> try_pop() {
        std::lock_guard lock(mtx_);
        if (q_.empty()) return std::nullopt;
        T item = std::move(q_.front());
        q_.pop();
        not_full_.notify_one();
        return item;
    }

    // Timed variant — essential for graceful shutdown and health checks.
    std::optional<T> pop_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mtx_);
        if (!not_empty_.wait_for(lock, timeout, [this]{ return !q_.empty() || closed_; }))
            return std::nullopt;               // timed out
        if (q_.empty()) return std::nullopt;
        T item = std::move(q_.front());
        q_.pop();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    // Closing is how you shut a pipeline down cleanly: no more pushes, and
    // every blocked consumer wakes up and drains what's left.
    void close() {
        { std::lock_guard lock(mtx_); closed_ = true; }
        not_empty_.notify_all();               // notify_ALL: everyone must wake
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mtx_);
        return q_.size();
    }
};

// ---------------------------------------------------------------------------
void demo_producer_consumer() {
    std::cout << "\n=== 1. Producer/consumer with a bounded queue ===\n";

    BlockingQueue<int>      queue(8);          // small capacity -> backpressure
    std::atomic<long long>  consumed_sum{0};
    std::atomic<int>        consumed_count{0};

    constexpr int PRODUCERS = 3, CONSUMERS = 4, PER_PRODUCER = 2000;

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p)
        producers.emplace_back([&queue, p]{
            for (int i = 0; i < PER_PRODUCER; ++i) queue.push(p * PER_PRODUCER + i);
        });

    std::vector<std::thread> consumers;
    for (int c = 0; c < CONSUMERS; ++c)
        consumers.emplace_back([&]{
            while (auto item = queue.pop()) {           // stops when closed+drained
                consumed_sum.fetch_add(*item, std::memory_order_relaxed);
                consumed_count.fetch_add(1, std::memory_order_relaxed);
            }
        });

    for (auto& t : producers) t.join();
    queue.close();                                      // signal end of stream
    for (auto& t : consumers) t.join();

    long long expected = 0;
    for (int i = 0; i < PRODUCERS * PER_PRODUCER; ++i) expected += i;
    std::printf("  consumed %d items, sum=%lld (expected %d items, sum=%lld) %s\n",
                consumed_count.load(), consumed_sum.load(),
                PRODUCERS * PER_PRODUCER, expected,
                (consumed_count == PRODUCERS*PER_PRODUCER && consumed_sum == expected)
                    ? "✅" : "❌");

    std::cout <<
      "\n  WHY BOUNDED? The capacity IS your BACKPRESSURE mechanism. An unbounded\n"
      "  queue turns a slow consumer into unbounded memory growth and then an\n"
      "  OOM kill — the single most common way async systems die in production.\n"
      "  Bounding it makes the producer block (or fail fast), which propagates\n"
      "  the pressure back to the source where it can actually be handled.\n"
      "  Same lesson as Java: use ArrayBlockingQueue, not LinkedBlockingQueue\n"
      "  with the default Integer.MAX_VALUE capacity.\n";
}

// ---------------------------------------------------------------------------
void demo_cv_rules() {
    std::cout << "\n=== 2. Condition variable rules ===\n";
    std::cout <<
      "  1. ALWAYS use the predicate overload:\n"
      "        cv.wait(lock, []{ return condition; });\n"
      "     which is exactly `while (!condition) cv.wait(lock);`\n"
      "     SPURIOUS WAKEUPS are real (they come from the OS futex layer), and\n"
      "     STOLEN wakeups are guaranteed under contention. An `if` is a bug.\n"
      "\n"
      "  2. The predicate must be checked WHILE HOLDING THE MUTEX. cv.wait\n"
      "     atomically unlocks + sleeps, then re-locks on wake. That atomicity\n"
      "     is the entire reason condition variables exist — it closes the race\n"
      "     between 'check the condition' and 'go to sleep'.\n"
      "\n"
      "  3. Modify the shared state UNDER the mutex, then notify.\n"
      "     Notifying without holding the mutex is allowed, but the state change\n"
      "     itself must be protected.\n"
      "\n"
      "  4. Prefer to UNLOCK BEFORE NOTIFYING. Otherwise the woken thread wakes\n"
      "     up, tries to take the mutex you still hold, and goes back to sleep.\n"
      "     (This is the 'hurry up and wait' pathology. Some implementations\n"
      "      optimise it away; don't rely on that.)\n"
      "\n"
      "  5. notify_one vs notify_all:\n"
      "       notify_one  — any ONE waiter can handle it. Cheaper. Default choice.\n"
      "       notify_all  — the state change matters to everyone (shutdown,\n"
      "                     a barrier releasing, capacity increasing for many).\n"
      "     Using notify_one when waiters are waiting for DIFFERENT conditions on\n"
      "     the same cv can lose a wakeup. Either use notify_all or use separate\n"
      "     condition variables (as this queue does: not_empty_ and not_full_).\n"
      "\n"
      "  6. There is no way to 'signal in advance'. A notify with no waiter is\n"
      "     lost. That's why the condition lives in shared state, not in the cv.\n";
}

// ---------------------------------------------------------------------------
void demo_other_primitives() {
    std::cout << "\n=== 3. The other C++20 synchronisation primitives ===\n";

    // std::latch — a one-shot countdown. Java: CountDownLatch.
    // "Wait until N things have happened."
    {
        constexpr int N = 4;
        std::latch done(N);
        std::vector<std::thread> ts;
        for (int i = 0; i < N; ++i)
            ts.emplace_back([&done, i]{
                std::this_thread::sleep_for(std::chrono::milliseconds(10 * (i+1)));
                done.count_down();
            });
        done.wait();                        // blocks until the count hits 0
        std::printf("  std::latch: all %d workers finished\n", N);
        for (auto& t : ts) t.join();
    }

    // std::barrier — a REUSABLE rendezvous point. Java: CyclicBarrier.
    // "All N threads wait here, then all proceed. Repeat."
    // The completion function runs once per phase, on the last arriving thread.
    {
        constexpr int N = 3;
        std::atomic<int> phase{0};
        std::barrier sync(N, [&phase]() noexcept {
            std::printf("  std::barrier: phase %d complete, releasing all\n", phase.fetch_add(1));
        });
        std::vector<std::thread> ts;
        for (int i = 0; i < N; ++i)
            ts.emplace_back([&sync, i]{
                for (int p = 0; p < 3; ++p) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5 * (i+1)));
                    sync.arrive_and_wait();       // all N must arrive
                }
            });
        for (auto& t : ts) t.join();
    }

    // std::counting_semaphore — permits. Java: Semaphore.
    // The right tool for "at most N concurrent X" (connections, file handles,
    // outbound requests to a rate-limited API).
    {
        std::counting_semaphore<4> sem(2);   // max 4 permits, 2 available now
        std::atomic<int> concurrent{0}, max_concurrent{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 6; ++i)
            ts.emplace_back([&]{
                sem.acquire();
                int c = concurrent.fetch_add(1) + 1;
                int m = max_concurrent.load();
                while (c > m && !max_concurrent.compare_exchange_weak(m, c)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                concurrent.fetch_sub(1);
                sem.release();
            });
        for (auto& t : ts) t.join();
        std::printf("  std::counting_semaphore(2): max observed concurrency = %d\n",
                    max_concurrent.load());
    }

    // std::atomic<T>::wait / notify (C++20) — futex-style blocking with no
    // mutex and no condition variable. Great for a single flag.
    {
        std::atomic<bool> flag{false};
        std::thread waiter([&]{
            flag.wait(false);                // sleeps until flag != false
            std::printf("  atomic::wait/notify: woke up, flag=%d\n", flag.load());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        flag.store(true);
        flag.notify_one();
        waiter.join();
    }

    std::cout <<
      "\n  ⚠️ Missing on this platform: std::jthread and std::stop_token.\n"
      "     For cancellation, pass a `std::atomic<bool>& stop` (checked in the\n"
      "     loop) plus a close()-able queue, exactly as in demo 1.\n"
      "\n"
      "  ⚠️ std::async / std::future: C++'s futures are WEAK compared to Java's\n"
      "     CompletableFuture. There is no .then(), no combinators, and the\n"
      "     future returned by std::async has a destructor that BLOCKS until the\n"
      "     task completes — a famous footgun. For real async work use a\n"
      "     library (folly::Future, Boost.Asio, cppcoro, stdexec) or coroutines.\n";
}

int main() {
    demo_producer_consumer();
    demo_cv_rules();
    demo_other_primitives();
    std::cout << "\n>>> NEXT: 09-projects/03-thread-pool builds an ExecutorService\n"
                 "    on top of exactly this queue.\n";
    return 0;
}
