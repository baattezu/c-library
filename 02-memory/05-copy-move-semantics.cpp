// ===========================================================================
//  COPY vs MOVE — watch every single operation happen
//  run: make run F=02-memory/05-copy-move-semantics.cpp
// ===========================================================================
//
//  This file instruments a class so you can SEE every construction, copy, move,
//  and destruction. Run it and read the output alongside the code. This is the
//  fastest way to build correct intuition about C++'s value semantics.
//
// ===========================================================================

#include <iostream>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// A class that narrates its own life.
// ---------------------------------------------------------------------------
struct Tracer {
    std::string name;
    int         id;
    static inline int next_id = 0;
    static inline int copies = 0, moves = 0;

    explicit Tracer(std::string n = "anon") : name(std::move(n)), id(++next_id) {
        std::cout << "  ctor      #" << id << " (" << name << ")\n";
    }
    Tracer(const Tracer& o) : name(o.name), id(++next_id) {
        ++copies;
        std::cout << "  COPY ctor #" << id << " <- #" << o.id << " (" << name << ")\n";
    }
    Tracer(Tracer&& o) noexcept : name(std::move(o.name)), id(++next_id) {
        ++moves;
        std::cout << "  move ctor #" << id << " <- #" << o.id << "\n";
    }
    Tracer& operator=(const Tracer& o) {
        ++copies;
        std::cout << "  COPY asgn #" << id << " <- #" << o.id << "\n";
        name = o.name;
        return *this;
    }
    Tracer& operator=(Tracer&& o) noexcept {
        ++moves;
        std::cout << "  move asgn #" << id << " <- #" << o.id << "\n";
        name = std::move(o.name);
        return *this;
    }
    ~Tracer() { std::cout << "  dtor      #" << id << "\n"; }

    static void reset_counts() { copies = moves = 0; }
    static void report(const char* what) {
        std::cout << "  >>> " << what << ": " << copies << " copies, "
                  << moves << " moves\n";
        reset_counts();
    }
};

// ---------------------------------------------------------------------------
void demo_basics() {
    std::cout << "\n=== 1. The four operations ===\n";
    Tracer::next_id = 0; Tracer::reset_counts();

    std::cout << " Tracer a{\"a\"};\n";        Tracer a{"a"};
    std::cout << " Tracer b = a;              // copy ctor\n";  Tracer b = a;
    std::cout << " Tracer c = std::move(a);   // move ctor\n";  Tracer c = std::move(a);
    std::cout << " b = c;                     // copy assign\n"; b = c;
    std::cout << " b = std::move(c);          // move assign\n"; b = std::move(c);
    std::cout << " (end of scope)\n";
}

// ---------------------------------------------------------------------------
// 2. PARAMETER PASSING — count the operations for each style
// ---------------------------------------------------------------------------
void by_value(Tracer t)        { (void)t; }
void by_cref (const Tracer& t) { (void)t; }
void by_rref (Tracer&& t)      { Tracer local = std::move(t); (void)local; }

void demo_parameters() {
    std::cout << "\n=== 2. Parameter passing costs ===\n";
    Tracer src{"src"};
    Tracer::reset_counts();

    std::cout << " by_value(src):        ";  std::cout << "\n"; by_value(src);
    Tracer::report("by_value with lvalue");

    std::cout << " by_value(Tracer{}):   \n"; by_value(Tracer{"tmp"});
    Tracer::report("by_value with prvalue (elided!)");

    std::cout << " by_cref(src):         \n"; by_cref(src);
    Tracer::report("by_cref");

    std::cout << " by_rref(std::move(src)):\n"; by_rref(std::move(src));
    Tracer::report("by_rref + move into local");
}

// ---------------------------------------------------------------------------
// 3. RETURN VALUES — copy elision in action
// ---------------------------------------------------------------------------
Tracer make_prvalue()  { return Tracer{"prvalue"};        }   // guaranteed elision
Tracer make_named()    { Tracer t{"named"}; return t;     }   // NRVO
// ❌ defeats NRVO. Note clang catches this with -Wpessimizing-move — we suppress
// the warning only so the demo compiles and you can see the extra move happen.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpessimizing-move"
Tracer make_bad()      { Tracer t{"bad"}; return std::move(t); }
#pragma clang diagnostic pop

void demo_returns() {
    std::cout << "\n=== 3. Return values ===\n";
    Tracer::reset_counts();

    std::cout << " auto a = make_prvalue();\n";
    { auto a = make_prvalue(); (void)a; }
    Tracer::report("return prvalue (C++17 GUARANTEED elision)");

    std::cout << " auto b = make_named();\n";
    { auto b = make_named(); (void)b; }
    Tracer::report("return named local (NRVO)");

    std::cout << " auto c = make_bad();   // return std::move(t)\n";
    { auto c = make_bad(); (void)c; }
    Tracer::report("return std::move(local)  <-- NEVER DO THIS");

    std::cout << "\n  ^ notice: `return std::move(t)` turns a FREE elision into a\n"
                 "    real move constructor call. It is strictly worse. Just\n"
                 "    `return t;`.\n";
}

// ---------------------------------------------------------------------------
// 4. VECTORS — reallocation, reserve, emplace, and the noexcept rule
// ---------------------------------------------------------------------------
void demo_vector_growth() {
    std::cout << "\n=== 4. Vector growth: watch the moves ===\n";

    std::cout << " Without reserve (4 push_backs):\n";
    Tracer::reset_counts();
    {
        std::vector<Tracer> v;
        for (int i = 0; i < 4; ++i) v.emplace_back("v" + std::to_string(i));
    }
    Tracer::report("no reserve");

    std::cout << "\n With reserve(4):\n";
    Tracer::reset_counts();
    {
        std::vector<Tracer> v;
        v.reserve(4);                    // ONE allocation, no reallocation
        for (int i = 0; i < 4; ++i) v.emplace_back("r" + std::to_string(i));
    }
    Tracer::report("with reserve");

    std::cout << "\n  ⭐ reserve() eliminated ALL the reallocation moves. On a real\n"
                 "     workload with expensive elements this is often 2-5x.\n";

    std::cout << "\n push_back(Tracer{...}) vs emplace_back(...):\n";
    Tracer::reset_counts();
    {
        std::vector<Tracer> v; v.reserve(2);
        std::cout << "  push_back(Tracer{\"p\"}):\n";
        v.push_back(Tracer{"p"});         // construct temp, MOVE into vector
        std::cout << "  emplace_back(\"e\"):\n";
        v.emplace_back("e");              // construct IN PLACE — no temp at all
    }
    Tracer::report("push vs emplace");
}

// ---------------------------------------------------------------------------
// 5. THE noexcept RULE — the silent performance killer
// ---------------------------------------------------------------------------
struct ThrowingMove {
    std::vector<int> data{1,2,3};
    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove& o) : data(o.data) { std::cout << "    COPY\n"; }
    ThrowingMove(ThrowingMove&& o) /* NOT noexcept! */ : data(std::move(o.data)) {
        std::cout << "    move\n";
    }
};
struct NoexceptMove {
    std::vector<int> data{1,2,3};
    NoexceptMove() = default;
    NoexceptMove(const NoexceptMove& o) : data(o.data) { std::cout << "    COPY\n"; }
    NoexceptMove(NoexceptMove&& o) noexcept : data(std::move(o.data)) {
        std::cout << "    move\n";
    }
};

void demo_noexcept_rule() {
    std::cout << "\n=== 5. Why move constructors MUST be noexcept ===\n";

    std::cout << "  Growing a vector<ThrowingMove> (move NOT noexcept):\n";
    { std::vector<ThrowingMove> v; for (int i = 0; i < 3; ++i) v.emplace_back(); }

    std::cout << "  Growing a vector<NoexceptMove> (move IS noexcept):\n";
    { std::vector<NoexceptMove> v; for (int i = 0; i < 3; ++i) v.emplace_back(); }

    std::cout <<
      "\n  vector uses std::move_if_noexcept when reallocating. If your move can\n"
      "  throw, vector cannot roll back a half-finished reallocation, so it\n"
      "  COPIES instead — silently, with no warning, forever.\n"
      "  ALWAYS write `noexcept` on move ctor and move assignment.\n";

    std::cout << "  is_nothrow_move_constructible<ThrowingMove> = " << std::boolalpha
              << std::is_nothrow_move_constructible_v<ThrowingMove> << "\n";
    std::cout << "  is_nothrow_move_constructible<NoexceptMove>  = "
              << std::is_nothrow_move_constructible_v<NoexceptMove> << "\n";
}

// ---------------------------------------------------------------------------
// 6. THE SILENT KILLERS
// ---------------------------------------------------------------------------
void demo_silent_pessimisations() {
    std::cout << "\n=== 6. Silent pessimisations ===\n";

    // (a) std::move on a const object does NOTHING.
    //     const Tracer&& binds to const Tracer&, which selects the COPY ctor.
    std::cout << "  (a) std::move on a const object:\n";
    const Tracer c{"const-src"};
    Tracer::reset_counts();
    Tracer copy = std::move(c);          // ← this is a COPY. Silently.
    Tracer::report("std::move(const Tracer)");
    std::cout << "      ^ const killed the move. No warning. Search your code for\n"
                 "        `std::move(` applied to const things.\n";

    // (b) A user-declared DESTRUCTOR suppresses the implicit move operations.
    std::cout << "\n  (b) Declaring a destructor kills your implicit moves:\n";
    struct HasDtor {
        std::vector<int> v{1,2,3};
        ~HasDtor() {}                    // ← this one line makes the class copy-only
    };
    struct NoDtor { std::vector<int> v{1,2,3}; };
    std::cout << "      HasDtor movable? " << std::boolalpha
              << std::is_move_constructible_v<HasDtor>
              << " but nothrow-movable? " << std::is_nothrow_move_constructible_v<HasDtor>
              << "\n";
    std::cout << "      NoDtor  nothrow-movable? "
              << std::is_nothrow_move_constructible_v<NoDtor> << "\n";
    std::cout << "      (HasDtor 'moves' by falling back to COPY — every element copied.)\n";
    std::cout << "      FIX: if you must declare a destructor, also write\n"
                 "           X(X&&) = default; X& operator=(X&&) = default;\n"
                 "      BETTER FIX: don't declare a destructor. Rule of Zero.\n";

    // (c) auto drops const and references
    std::cout << "\n  (c) `auto x = v[i]` copies. `auto& x = v[i]` doesn't.\n";
}

int main() {
    demo_basics();
    demo_parameters();
    demo_returns();
    demo_vector_growth();
    demo_noexcept_rule();
    demo_silent_pessimisations();

    std::cout << "\n>>> EXERCISE: add `std::cout` to your own classes' copy\n"
                 "    constructors when investigating a performance problem.\n"
                 "    Counting copies is how you find them.\n";
    return 0;
}
