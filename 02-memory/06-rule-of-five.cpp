// ===========================================================================
//  RULE OF ZERO / THREE / FIVE — writing a correct resource-owning class
//  run:  make run  F=02-memory/06-rule-of-five.cpp
//  then: make asan F=02-memory/06-rule-of-five.cpp
// ===========================================================================

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ===========================================================================
//  PART A: THE BUG — what happens with only a destructor (Rule of Three broken)
// ===========================================================================
namespace broken {

class Buffer {
public:
    char*       data_;
    std::size_t size_;

    explicit Buffer(std::size_t n) : data_(new char[n]), size_(n) {
        std::memset(data_, 'x', n);
    }
    ~Buffer() { delete[] data_; }        // ← we wrote a destructor...
    // ...and NOTHING else. The compiler generates a COPY constructor that does
    // a memberwise copy — which copies the POINTER. Now two Buffers think they
    // own the same allocation, and both will delete it.
};

void demonstrate() {
    std::cout << "\n=== A. The bug: destructor without copy control ===\n";
    std::cout <<
      "    Buffer a(16);\n"
      "    Buffer b = a;     // shallow copy: b.data_ == a.data_\n"
      "    // both destructors run -> DOUBLE FREE -> heap corruption\n"
      "\n"
      "  The code is compiled but NOT RUN here (it would crash). Uncomment the\n"
      "  block below and run `make asan F=02-memory/06-rule-of-five.cpp` to see\n"
      "  ASan report 'attempting double-free'.\n";

    // {
    //     Buffer a(16);
    //     Buffer b = a;      // ☠️
    // }
}

} // namespace broken

// ===========================================================================
//  PART B: THE FIX — the full Rule of Five
// ===========================================================================
class Buffer {
    char*       data_ = nullptr;
    std::size_t size_ = 0;

public:
    // ---- 0. Default constructor: a valid EMPTY state --------------------
    Buffer() noexcept = default;

    // ---- 1. Resource-acquiring constructor -------------------------------
    explicit Buffer(std::size_t n)
        : data_(n ? new char[n] : nullptr), size_(n) {
        if (data_) std::memset(data_, 0, n);
        std::cout << "    ctor      (" << size_ << " bytes)\n";
    }

    // ---- 2. Destructor ---------------------------------------------------
    // Must handle the null (moved-from / default) state. delete[] on nullptr
    // is well-defined and does nothing, so this is fine as written.
    ~Buffer() {
        if (data_) std::cout << "    dtor      (" << size_ << " bytes)\n";
        delete[] data_;
    }

    // ---- 3. Copy constructor: DEEP copy ----------------------------------
    Buffer(const Buffer& o)
        : data_(o.size_ ? new char[o.size_] : nullptr), size_(o.size_) {
        if (data_) std::copy(o.data_, o.data_ + o.size_, data_);
        std::cout << "    COPY ctor (" << size_ << " bytes duplicated)\n";
    }

    // ---- 4. Copy assignment ----------------------------------------------
    // Written with COPY-AND-SWAP: take the parameter BY VALUE (the copy already
    // happened, using the copy constructor above), then swap. This gives us:
    //   - self-assignment safety for free (we swap with an independent copy)
    //   - the STRONG exception guarantee (if the copy throws, *this is untouched)
    //   - no code duplication
    // Cost: one extra move on the rvalue path, which is negligible.
    Buffer& operator=(Buffer o) noexcept {
        std::cout << "    assign (copy-and-swap)\n";
        swap(*this, o);
        return *this;
    }   // `o` (holding our OLD data) is destroyed here

    // ---- 5. Move constructor ---------------------------------------------
    // noexcept is MANDATORY for vector to use it during reallocation.
    // std::exchange(o.data_, nullptr) sets o.data_ to nullptr and returns the
    // old value — the cleanest way to write "steal and null out".
    Buffer(Buffer&& o) noexcept
        : data_(std::exchange(o.data_, nullptr)),
          size_(std::exchange(o.size_, 0)) {
        std::cout << "    move ctor (" << size_ << " bytes stolen, O(1))\n";
    }

    // ---- (move assignment is covered by the by-value operator= above) ----
    // If you prefer the explicit form:
    //   Buffer& operator=(Buffer&& o) noexcept {
    //       if (this != &o) {
    //           delete[] data_;
    //           data_ = std::exchange(o.data_, nullptr);
    //           size_ = std::exchange(o.size_, 0);
    //       }
    //       return *this;
    //   }

    // ---- swap: noexcept, member-wise, found by ADL ------------------------
    friend void swap(Buffer& a, Buffer& b) noexcept {
        using std::swap;
        swap(a.data_, b.data_);
        swap(a.size_, b.size_);
    }

    // ---- accessors --------------------------------------------------------
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] char*       data()       noexcept { return data_; }
    [[nodiscard]] const char* data() const noexcept { return data_; }
    explicit operator bool() const noexcept { return data_ != nullptr; }
};

void demo_rule_of_five() {
    std::cout << "\n=== B. The Rule of Five, working ===\n";
    Buffer a(16);
    Buffer b = a;              // copy ctor  — deep copy
    Buffer c = std::move(a);   // move ctor  — steal
    std::cout << "  a.size()=" << a.size() << " (moved-from: empty)\n";
    b = c;                     // copy assign via copy-and-swap
    b = std::move(c);          // move assign via the same operator (moves into `o`)
    std::cout << "  self-assignment safety: ";
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"   // deliberate test
    b = b;                     // safe: copy-and-swap handles it
#pragma clang diagnostic pop
    std::cout << "ok\n";
    std::cout << "  destructors:\n";
}

// ===========================================================================
//  PART C: THE RULE OF ZERO — what you should actually write
// ===========================================================================
class GoodBuffer {
    // std::vector already implements all five operations, correctly, optimally.
    // By composing from it, WE inherit all five for free — and they're right.
    std::vector<char> data_;
public:
    GoodBuffer() = default;
    explicit GoodBuffer(std::size_t n) : data_(n) {}
    // NO destructor. NO copy ctor. NO move ctor. NO assignment operators.
    // The compiler generates all of them, and they are all correct AND noexcept.
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] char* data() noexcept { return data_.data(); }
};

void demo_rule_of_zero() {
    std::cout << "\n=== C. The Rule of Zero — what you SHOULD write ===\n";
    GoodBuffer a(16);
    GoodBuffer b = a;            // correct deep copy, generated
    GoodBuffer c = std::move(a); // correct O(1) move, generated, noexcept

    std::cout << "  b.size()=" << b.size() << " c.size()=" << c.size()
              << " a.size()=" << a.size() << " (moved-from)\n";
    std::cout << "  nothrow-move-constructible? " << std::boolalpha
              << std::is_nothrow_move_constructible_v<GoodBuffer> << "\n";
    std::cout <<
      "\n  ⭐ THIS IS THE ANSWER. 5 lines instead of 60, and it is MORE correct\n"
      "     (exception-safe, self-assignment-safe, noexcept, no manual memory).\n"
      "     Write the Rule of Five ONLY when you are wrapping a raw OS handle\n"
      "     or implementing a container. Otherwise: compose and write nothing.\n";
}

// ===========================================================================
//  PART D: MOVE-ONLY AND NON-COPYABLE TYPES
// ===========================================================================
void demo_deleted_ops() {
    std::cout << "\n=== D. Expressing intent with = delete / = default ===\n";

    // MOVE-ONLY: a unique resource. This is what unique_ptr, thread, and
    // ifstream all are.
    struct MoveOnly {
        std::unique_ptr<int> p = std::make_unique<int>(1);
        MoveOnly()                           = default;
        MoveOnly(const MoveOnly&)            = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&&) noexcept        = default;
        MoveOnly& operator=(MoveOnly&&) noexcept = default;
    };
    // (Actually: just having a unique_ptr member gives you all of this for free
    //  via the Rule of Zero. The explicit version is for documentation.)

    // IMMOVABLE: e.g. a class with a std::mutex member, or something whose
    // address is registered somewhere.
    struct Pinned {
        Pinned()                         = default;
        Pinned(const Pinned&)            = delete;
        Pinned& operator=(const Pinned&) = delete;
        Pinned(Pinned&&)                 = delete;
        Pinned& operator=(Pinned&&)      = delete;
    };

    std::cout << "  MoveOnly: copyable=" << std::boolalpha
              << std::is_copy_constructible_v<MoveOnly>
              << " movable=" << std::is_move_constructible_v<MoveOnly> << "\n";
    std::cout << "  Pinned:   copyable=" << std::is_copy_constructible_v<Pinned>
              << " movable=" << std::is_move_constructible_v<Pinned> << "\n";
}

// ===========================================================================
//  PART E: THE GENERATION RULES TABLE
// ===========================================================================
void the_table() {
    std::cout << "\n=== E. When does the compiler generate what? ===\n";
    std::cout <<
      "  If you declare...          | you still get...\n"
      "  ---------------------------|--------------------------------------------\n"
      "  nothing                    | all six (default, dtor, copy x2, move x2)\n"
      "  any constructor            | no default constructor\n"
      "  a DESTRUCTOR               | copies (deprecated), *** NO MOVES ***\n"
      "  a COPY ctor or copy assign | the other copy op (deprecated), NO MOVES\n"
      "  a MOVE ctor or move assign | copies are DELETED\n"
      "\n"
      "  ⚠️ THE ONE THAT BITES: writing `~Foo() {}` — even an EMPTY destructor —\n"
      "     silently removes the implicit move constructor and move assignment.\n"
      "     Every 'move' of your class then falls back to a deep COPY, with no\n"
      "     diagnostic of any kind. This is one of the most common invisible\n"
      "     performance bugs in real C++ codebases.\n"
      "\n"
      "  CHECK YOUR TYPES IN TESTS:\n"
      "     static_assert(std::is_nothrow_move_constructible_v<MyType>);\n"
      "     static_assert(std::is_nothrow_move_assignable_v<MyType>);\n";
}

int main() {
    broken::demonstrate();
    demo_rule_of_five();
    demo_rule_of_zero();
    demo_deleted_ops();
    the_table();
    return 0;
}
