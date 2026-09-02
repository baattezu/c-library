// ===========================================================================
//  RAII — the single best idea in C++
//  run: make run F=02-memory/03-raii.cpp
// ===========================================================================
//
//  Resource Acquisition Is Initialization:
//      acquire in the constructor, release in the destructor.
//
//  Because C++ guarantees destructors run at scope exit — on the normal path,
//  on `return`, on `break`, and during exception unwinding — you literally
//  cannot forget to clean up. There is no code path out of a scope that skips
//  destructors.
//
//  Java's try-with-resources is RAII with training wheels: it only works for
//  AutoCloseable, only inside a try block, and only when you remember. C++
//  applies it to everything, everywhere, automatically.
//
// ===========================================================================

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. THE ANATOMY OF DESTRUCTION
// ---------------------------------------------------------------------------
struct Noisy {
    std::string name;
    explicit Noisy(std::string n) : name(std::move(n)) {
        std::cout << "    + ctor " << name << "\n";
    }
    ~Noisy() { std::cout << "    - dtor " << name << "\n"; }
};

void demo_destruction_order() {
    std::cout << "\n=== 1. Destruction order is exactly reverse of construction ===\n";
    {
        Noisy a("a");
        Noisy b("b");
        Noisy c("c");
        std::cout << "  (leaving scope now)\n";
    }   // c, then b, then a. Guaranteed. Always.

    std::cout << "\n  Even on the exception path:\n";
    try {
        Noisy x("x");
        Noisy y("y");
        throw std::runtime_error("boom");
    } catch (const std::exception& e) {
        std::cout << "    caught '" << e.what() << "' — y and x already destroyed\n";
    }

    std::cout << "\n  And for members, in reverse DECLARATION order:\n";
    struct Composite {
        Noisy first{"member-first"};
        Noisy second{"member-second"};
        ~Composite() { std::cout << "    (Composite body runs BEFORE members)\n"; }
    };
    { Composite c; }
}

// ---------------------------------------------------------------------------
// 2. A REAL RAII WRAPPER — a C FILE* handle
//    This is the template for every RAII class you will ever write.
// ---------------------------------------------------------------------------
class File {
    std::FILE* f_ = nullptr;    // the "null state" — essential for move support

public:
    File() = default;

    File(const char* path, const char* mode) : f_(std::fopen(path, mode)) {
        if (!f_) throw std::runtime_error(std::string("fopen failed: ") + path
                                          + ": " + std::strerror(errno));
        std::cout << "    opened " << path << "\n";
    }

    // ---- destructor: RELEASE ----
    // Must be noexcept (it implicitly is). Must handle the null state.
    ~File() { if (f_) { std::fclose(f_); std::cout << "    closed\n"; } }

    // ---- copies deleted: a file handle has exactly one owner ----
    File(const File&)            = delete;
    File& operator=(const File&) = delete;

    // ---- moves: transfer ownership, NULL THE SOURCE ----
    File(File&& o) noexcept : f_(std::exchange(o.f_, nullptr)) {}
    //                             ^^^^^^^^^^^^ std::exchange(obj, new) sets obj
    //                             to new and returns the OLD value. The cleanest
    //                             way to write a move constructor.

    File& operator=(File&& o) noexcept {
        if (this != &o) {                       // self-assignment guard
            if (f_) std::fclose(f_);            // release what we currently hold
            f_ = std::exchange(o.f_, nullptr);
        }
        return *this;
    }

    // ---- the interface ----
    void write(std::string_view s) {
        if (std::fwrite(s.data(), 1, s.size(), f_) != s.size())
            throw std::runtime_error("short write");
    }
    [[nodiscard]] std::FILE* get() const noexcept { return f_; }
    explicit operator bool() const noexcept { return f_ != nullptr; }

    // ---- release: give ownership away to a C API ----
    [[nodiscard]] std::FILE* release() noexcept { return std::exchange(f_, nullptr); }
};

void demo_file_raii() {
    std::cout << "\n=== 2. A real RAII handle ===\n";
    const char* path = "/tmp/cpp_raii_demo.txt";
    {
        File f(path, "w");
        f.write("hello from RAII\n");
        // no close() call anywhere. It cannot leak.
    }
    {
        File in(path, "r");
        char buf[64]{};
        std::fgets(buf, sizeof(buf), in.get());
        std::cout << "    read back: " << buf;
    }
    std::remove(path);

    // Moving works and is safe:
    File a("/tmp/cpp_raii_move.txt", "w");
    File b = std::move(a);           // a is now empty (f_ == nullptr)
    std::cout << "    after move: a is " << (a ? "open" : "empty")
              << ", b is " << (b ? "open" : "empty") << "\n";
    std::remove("/tmp/cpp_raii_move.txt");
}

// ---------------------------------------------------------------------------
// 3. RAII IS NOT JUST FOR MEMORY
// ---------------------------------------------------------------------------
std::mutex g_mtx;
int        g_shared = 0;

void demo_non_memory_raii() {
    std::cout << "\n=== 3. RAII for locks, timers, and transactions ===\n";

    // (a) LOCKS — this is `synchronized` in Java, but scoped and exception-safe.
    {
        std::lock_guard<std::mutex> lock(g_mtx);   // lock()
        ++g_shared;
        // even `throw` here would unlock correctly
    }   // unlock()
    std::cout << "  lock_guard: g_shared = " << g_shared << "\n";
    // std::scoped_lock (C++17) locks MULTIPLE mutexes deadlock-free. Prefer it.
    // std::unique_lock is the flexible one (deferred/timed/movable; needed by
    // condition_variable). std::shared_lock for reader-writer locks.

    // (b) A SCOPED TIMER — this is how you instrument code with zero effort.
    struct ScopedTimer {
        const char* label;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~ScopedTimer() {
            auto us = std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - start).count();
            std::printf("  [timer] %s: %.1f us\n", label, us);
        }
    };
    {
        ScopedTimer t{"some work"};
        volatile long x = 0;
        for (int i = 0; i < 200000; ++i) x += i;
    }

    // (c) TRANSACTION / ROLLBACK — commit-or-undo, without a `finally`.
    struct Transaction {
        std::vector<int>& data;
        std::size_t       mark;
        bool              committed = false;
        explicit Transaction(std::vector<int>& d) : data(d), mark(d.size()) {}
        void commit() { committed = true; }
        ~Transaction() {
            if (!committed) {
                data.resize(mark);
                std::cout << "  [txn] ROLLED BACK to size " << mark << "\n";
            } else {
                std::cout << "  [txn] committed\n";
            }
        }
    };
    std::vector<int> db{1,2,3};
    try {
        Transaction txn(db);
        db.push_back(4); db.push_back(5);
        throw std::runtime_error("validation failed");
        // txn.commit();      // never reached
    } catch (...) {}
    std::cout << "  db.size() after rollback = " << db.size() << "\n";
}

// ---------------------------------------------------------------------------
// 4. A GENERIC SCOPE GUARD — the "finally" you don't have
// ---------------------------------------------------------------------------
template <class F>
class ScopeExit {
    F    fn_;
    bool active_ = true;
public:
    explicit ScopeExit(F f) : fn_(std::move(f)) {}
    ~ScopeExit() noexcept { if (active_) { try { fn_(); } catch (...) {} } }
    void dismiss() noexcept { active_ = false; }

    ScopeExit(const ScopeExit&)            = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&& o) noexcept : fn_(std::move(o.fn_)), active_(o.active_) {
        o.active_ = false;
    }
};
template <class F> ScopeExit(F) -> ScopeExit<F>;   // CTAD deduction guide

void demo_scope_guard() {
    std::cout << "\n=== 4. Generic scope guard ===\n";
    {
        auto guard = ScopeExit{[]{ std::cout << "  guard fired (always runs)\n"; }};
        std::cout << "  doing work...\n";
    }

    // Dismissed guard: the "commit" pattern.
    {
        auto rollback = ScopeExit{[]{ std::cout << "  ROLLBACK\n"; }};
        std::cout << "  work succeeded, dismissing rollback\n";
        rollback.dismiss();
    }
    // (Boost has boost::scope_exit; C++26 is standardising std::scope_exit,
    //  std::scope_fail, std::scope_success. Until then, 12 lines like the above.)
}

// ---------------------------------------------------------------------------
// 5. THE RULES AND THE TRAPS
// ---------------------------------------------------------------------------
void demo_rules() {
    std::cout << "\n=== 5. Rules and traps ===\n";
    std::cout <<
      "  RULES\n"
      "   1. Every resource gets an owner object. No exceptions.\n"
      "   2. Acquire in the constructor; if acquisition fails, THROW (so the\n"
      "      object never exists in a broken state).\n"
      "   3. Release in the destructor. The destructor must NEVER throw.\n"
      "   4. Delete the copy operations unless copying the resource is meaningful.\n"
      "   5. Provide noexcept moves that null out the source.\n"
      "   6. Handle the null (moved-from) state in the destructor.\n"
      "\n"
      "  TRAPS\n"
      "   a. A DESTRUCTOR THAT THROWS during stack unwinding calls\n"
      "      std::terminate(). Destructors are implicitly noexcept. If cleanup\n"
      "      can fail (fclose! close()!), either swallow the error or offer an\n"
      "      explicit close() method that the user can call to check.\n"
      "   b. If a CONSTRUCTOR throws, the destructor is NOT called (the object\n"
      "      never existed) — but already-constructed MEMBERS and BASES are\n"
      "      destroyed correctly. So member-wise RAII is still safe.\n"
      "   c. `Widget w();` declares a FUNCTION, not an object (\"most vexing\n"
      "      parse\"). Write `Widget w;` or `Widget w{};`.\n"
      "   d. A base class used polymorphically needs a VIRTUAL destructor, or\n"
      "      `delete base_ptr` is UB and the derived destructor never runs.\n"
      "   e. RAII objects must not be created with `new` and forgotten. The\n"
      "      whole point is that they live on the stack (or inside another\n"
      "      RAII object).\n";
}

int main() {
    demo_destruction_order();
    demo_file_raii();
    demo_non_memory_raii();
    demo_scope_guard();
    demo_rules();

    std::cout << "\n>>> This is the file to re-read. RAII is not a technique in\n"
                 "    C++ — it IS C++. Every good C++ program is a tree of\n"
                 "    stack-rooted owners whose destructors unwind in order.\n";
    return 0;
}
