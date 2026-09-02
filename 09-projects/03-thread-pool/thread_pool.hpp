// ===========================================================================
//  PROJECT 3: A THREAD POOL — building Java's ExecutorService by hand
// ===========================================================================
//
//  Java hands you Executors.newFixedThreadPool(n) and CompletableFuture.
//  C++ hands you std::thread and a memory model. This file is the gap.
//
//  What we build:
//    - a fixed pool of worker threads
//    - submit(f, args...) returning a std::future<R>, like Java's Future
//    - graceful shutdown (drain the queue) and immediate shutdown
//    - per-worker queues with WORK STEALING, which is what makes real pools
//      (Java's ForkJoinPool, Intel TBB, Go's scheduler) scale
//
//  ⚠️ THE THREE RULES OF THREAD POOLS, learned the hard way by everyone:
//    1. NEVER submit a task that blocks waiting on another task in the SAME
//       pool. That is a deadlock: all N workers wait, nobody runs the thing
//       they're waiting for. (Java calls this thread starvation deadlock.)
//    2. Size CPU-bound pools to hardware_concurrency(). Size I/O-bound pools
//       larger — but better, don't block at all; use an event loop.
//    3. Exceptions escaping a task must not kill the worker. Capture them into
//       the future.
//
// ===========================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <deque>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace pool {

// ===========================================================================
//  A per-worker task queue supporting steal() from the other end.
//
//  The owner pushes and pops at the FRONT (LIFO — better cache locality: the
//  task you just created is likely to touch data still in your cache).
//  Thieves steal from the BACK (FIFO — they take the OLDEST task, which is
//  likely the largest remaining unit of work, and minimises contention with
//  the owner).
//
//  This front/back split is the key insight of work stealing, and it's exactly
//  what Java's ForkJoinPool WorkQueue does.
// ===========================================================================
class TaskQueue {
    using Task = std::function<void()>;
    std::deque<Task>        q_;
    mutable std::mutex      m_;
    std::condition_variable cv_;
    bool                    done_ = false;

public:
    void push(Task t) {
        { std::lock_guard lk(m_); q_.push_front(std::move(t)); }
        cv_.notify_one();
    }

    // Non-blocking: used by the owner's fast path and by thieves.
    bool try_pop(Task& out) {
        std::unique_lock lk(m_, std::try_to_lock);
        if (!lk || q_.empty()) return false;       // don't even wait for the lock
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    bool try_steal(Task& out) {
        std::unique_lock lk(m_, std::try_to_lock);
        if (!lk || q_.empty()) return false;
        out = std::move(q_.back());                // steal the OLDEST
        q_.pop_back();
        return true;
    }

    // Blocking: only used when a worker has found nothing anywhere.
    bool pop_wait(Task& out) {
        std::unique_lock lk(m_);
        cv_.wait(lk, [this]{ return !q_.empty() || done_; });
        if (q_.empty()) return false;              // done and drained
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    void close() {
        { std::lock_guard lk(m_); done_ = true; }
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t size() const { std::lock_guard lk(m_); return q_.size(); }
};

// ===========================================================================
//  ThreadPool
// ===========================================================================
class ThreadPool {
    std::vector<std::thread>                    workers_;
    std::vector<std::unique_ptr<TaskQueue>>     queues_;
    std::atomic<std::size_t>                    round_robin_{0};
    std::atomic<bool>                           stopping_{false};
    std::atomic<std::size_t>                    pending_{0};
    std::atomic<std::size_t>                    completed_{0};
    std::atomic<std::size_t>                    steals_{0};

    // How many times to spin over all queues before blocking. Spinning avoids
    // the ~1-3µs cost of parking and waking a thread when work is imminent.
    static constexpr unsigned SPIN_ROUNDS = 4;

    void worker_loop(std::size_t index) {
        const std::size_t n = queues_.size();
        while (true) {
            std::function<void()> task;
            bool got = false;

            // 1. FAST PATH: my own queue, no blocking.
            if (queues_[index]->try_pop(task)) got = true;

            // 2. STEAL: scan the other queues. We start at a rotating offset so
            //    every worker doesn't hammer worker 0 first.
            if (!got) {
                for (unsigned round = 0; round < SPIN_ROUNDS && !got; ++round) {
                    for (std::size_t k = 1; k < n; ++k) {
                        std::size_t victim = (index + round + k) % n;
                        if (queues_[victim]->try_steal(task)) {
                            got = true;
                            steals_.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                    if (!got) std::this_thread::yield();
                }
            }

            // 3. SLEEP: nothing anywhere. Block on our own queue so we consume
            //    no CPU. (A production pool would use a shared "sleeping
            //    workers" count so a push can wake exactly one.)
            if (!got) {
                if (stopping_.load(std::memory_order_acquire) &&
                    pending_.load(std::memory_order_acquire) == 0) return;
                if (!queues_[index]->pop_wait(task)) {
                    if (stopping_.load(std::memory_order_acquire)) return;
                    continue;
                }
                got = true;
            }

            if (got) {
                // ⚠️ A TASK MUST NEVER LET AN EXCEPTION ESCAPE INTO THE WORKER
                // LOOP. If it did, std::thread would call std::terminate and
                // take the whole process down. The packaged_task in submit()
                // already captures exceptions into the future; this is defence
                // in depth for raw post() tasks.
                try { task(); } catch (...) { /* swallowed: see submit() */ }
                pending_.fetch_sub(1, std::memory_order_release);
                completed_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

public:
    explicit ThreadPool(std::size_t n = 0) {
        if (n == 0) {
            n = std::thread::hardware_concurrency();
            if (n == 0) n = 4;                     // hardware_concurrency may return 0
        }
        queues_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) queues_.push_back(std::make_unique<TaskQueue>());
        workers_.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            workers_.emplace_back([this, i]{ worker_loop(i); });
    }

    // ⭐ RAII: the destructor drains and joins. You cannot forget to shut down,
    // and you cannot destroy the pool while tasks still reference it.
    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // -----------------------------------------------------------------------
    //  submit(): the ExecutorService.submit() equivalent.
    //
    //  Returns std::future<R>. If the task throws, the exception is STORED in
    //  the future and rethrown when you call .get() — exactly like Java's
    //  ExecutionException.
    //
    //  std::packaged_task wraps a callable so its result (or exception) is
    //  delivered to a std::future. We wrap it in a shared_ptr because
    //  std::function requires its target to be COPYABLE and packaged_task is
    //  move-only. (C++23's std::move_only_function removes this workaround.)
    // -----------------------------------------------------------------------
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;

        if (stopping_.load(std::memory_order_acquire))
            throw std::runtime_error("submit() on a stopping pool");

        auto task = std::make_shared<std::packaged_task<R()>>(
            [f = std::forward<F>(f),
             tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
                return std::apply(std::move(f), std::move(tup));
            });

        std::future<R> fut = task->get_future();
        pending_.fetch_add(1, std::memory_order_release);

        // Round-robin the initial placement; work stealing fixes any imbalance.
        std::size_t idx = round_robin_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
        queues_[idx]->push([task]{ (*task)(); });
        return fut;
    }

    // Fire-and-forget: no future, so no allocation for shared state.
    // Cheaper than submit() when you don't need the result.
    template <class F>
    void post(F&& f) {
        if (stopping_.load(std::memory_order_acquire)) return;
        pending_.fetch_add(1, std::memory_order_release);
        std::size_t idx = round_robin_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
        queues_[idx]->push(std::forward<F>(f));
    }

    // Block until every submitted task has completed.
    void wait_idle() {
        while (pending_.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();
    }

    void shutdown() {
        if (stopping_.exchange(true)) return;      // idempotent
        wait_idle();
        for (auto& q : queues_) q->close();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    [[nodiscard]] std::size_t size()      const noexcept { return workers_.size(); }
    [[nodiscard]] std::size_t completed() const noexcept { return completed_.load(); }
    [[nodiscard]] std::size_t steals()    const noexcept { return steals_.load(); }
};

// ===========================================================================
//  parallel_for — the thing you actually want 90% of the time
//
//  ⚠️ NOTE THE CHUNKING. We give each worker a CONTIGUOUS BLOCK, not a strided
//  slice. Two reasons:
//    1. contiguous = cache-friendly and prefetchable
//    2. strided/interleaved indices make adjacent threads write adjacent
//       memory -> FALSE SHARING (see 07-performance/02).
//  This one design choice is often the difference between 8x and 1x scaling.
// ===========================================================================
template <class F>
void parallel_for(ThreadPool& p, std::size_t begin, std::size_t end, F&& body,
                  std::size_t min_chunk = 1024) {
    if (end <= begin) return;
    const std::size_t total  = end - begin;
    const std::size_t nchunk = std::min(p.size() * 4, std::max<std::size_t>(1, total / min_chunk));
    const std::size_t chunk  = (total + nchunk - 1) / nchunk;

    std::vector<std::future<void>> futures;
    futures.reserve(nchunk);
    for (std::size_t c = 0; c < nchunk; ++c) {
        std::size_t lo = begin + c * chunk;
        std::size_t hi = std::min(lo + chunk, end);
        if (lo >= hi) break;
        futures.push_back(p.submit([lo, hi, &body]{
            for (std::size_t i = lo; i < hi; ++i) body(i);
        }));
    }
    // .get() rethrows any exception from the chunk — so errors propagate.
    for (auto& f : futures) f.get();
}

} // namespace pool
