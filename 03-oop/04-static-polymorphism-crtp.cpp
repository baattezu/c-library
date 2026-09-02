// ===========================================================================
//  STATIC POLYMORPHISM — polymorphism with ZERO runtime cost
//  run: make opt F=03-oop/04-static-polymorphism-crtp.cpp
// ===========================================================================
//
//  Runtime polymorphism (virtual) costs: 8 bytes/object, an indirect call, and
//  — the expensive part — an OPTIMISATION BARRIER that blocks inlining.
//
//  If the set of types is known at COMPILE TIME, you don't need any of that.
//  C++ gives you four ways to be polymorphic without a vtable:
//
//    1. Plain templates ("duck typing")   — simplest, most common
//    2. Concepts (C++20)                  — templates with checked requirements
//    3. CRTP                              — static "inheritance"
//    4. std::variant + std::visit         — closed set, one predictable jump
//
//  This is what people mean by "zero-cost abstraction": the abstraction exists
//  in the source and is COMPLETELY GONE in the binary.
//
// ===========================================================================

#include <chrono>
#include <concepts>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// ===========================================================================
// APPROACH 0 (the baseline): virtual
// ===========================================================================
struct IShape {
    virtual ~IShape() = default;
    virtual double area() const = 0;
};
struct VCircle : IShape {
    double r;
    explicit VCircle(double x) : r(x) {}
    double area() const override { return 3.14159265358979 * r * r; }
};

// ===========================================================================
// APPROACH 1: PLAIN TEMPLATES (structural / "duck" typing)
// ===========================================================================
// This function works with ANY type that has an .area() method. There is no
// interface, no inheritance, no declaration of intent. The compiler generates a
// separate, fully inlined version for each type you use it with.
template <class Shape>
double total_area(const std::vector<Shape>& shapes) {
    double sum = 0;
    for (const auto& s : shapes) sum += s.area();   // resolved at COMPILE time
    return sum;
}

struct TCircle {
    double r;
    double area() const { return 3.14159265358979 * r * r; }   // no virtual, no vptr
};
static_assert(sizeof(TCircle) == sizeof(double), "no vptr overhead");

// ===========================================================================
// APPROACH 2: CONCEPTS (C++20) — the fix for template error messages
// ===========================================================================
// A concept is a NAMED, CHECKED set of requirements. It is C++'s answer to
// `<T extends Shape>` — except it constrains SYNTAX, not a base class, so
// unrelated types can satisfy it without knowing it exists.
template <class T>
concept HasArea = requires(const T& t) {
    { t.area() } -> std::convertible_to<double>;
};

template <HasArea Shape>
double total_area_checked(const std::vector<Shape>& shapes) {
    double sum = 0;
    for (const auto& s : shapes) sum += s.area();
    return sum;
}
// If you pass a type without .area(), you get:
//   "constraints not satisfied: the required expression 't.area()' is invalid"
// instead of 200 lines of instantiation backtrace. This alone justifies C++20.

// ===========================================================================
// APPROACH 3: CRTP — Curiously Recurring Template Pattern
// ===========================================================================
// "A base class templated on its own derived class." This lets the BASE call
// into the DERIVED, statically. It's inheritance-shaped, but every call is
// resolved and inlined at compile time.
//
// Use it when you want to share implementation in a base class (the way you'd
// use an abstract class) without paying for virtual dispatch.
template <class Derived>
class ShapeBase {
    // The magic: cast `this` to the derived type. Safe because Derived really
    // IS the derived type — that's the contract of the pattern.
    const Derived& self() const { return static_cast<const Derived&>(*this); }

public:
    // Shared algorithms that call the derived implementation. In a virtual
    // hierarchy these would each cost a vtable lookup. Here they inline away.
    double area()      const { return self().compute_area(); }
    double half_area() const { return area() / 2; }
    bool bigger_than(double x) const { return area() > x; }
    void describe() const {
        std::printf("    %s: area=%.3f\n", self().name(), area());
    }
};

class CCircle : public ShapeBase<CCircle> {
    double r_;
public:
    explicit CCircle(double r) : r_(r) {}
    // These are found by the base via static_cast. NOT virtual.
    double compute_area() const { return 3.14159265358979 * r_ * r_; }
    const char* name() const { return "CRTP circle"; }
};

class CSquare : public ShapeBase<CSquare> {
    double s_;
public:
    explicit CSquare(double s) : s_(s) {}
    double compute_area() const { return s_ * s_; }
    const char* name() const { return "CRTP square"; }
};

static_assert(sizeof(CCircle) == sizeof(double), "CRTP adds ZERO bytes");

// CRTP's other classic uses:
//  - std::enable_shared_from_this<T> (you already used it!)
//  - "mixin" behaviour: give a type ==, !=, <, >, <=, >= from one operator<
//  - static counters / registries per derived type
//  - expression templates (Eigen, Blaze) — the technique that makes C++ linear
//    algebra libraries beat hand-written C
template <class D>
struct Comparable {                       // a mixin
    friend bool operator!=(const D& a, const D& b) { return !(a == b); }
    friend bool operator> (const D& a, const D& b) { return b < a; }
    friend bool operator<=(const D& a, const D& b) { return !(b < a); }
    friend bool operator>=(const D& a, const D& b) { return !(a < b); }
};
struct Version : Comparable<Version> {
    int major, minor;
    // NOTE: because Version has a base class, it is no longer an aggregate in
    // the simple sense — `Version{1,2}` would need `Version{{}, 1, 2}`. Writing
    // a constructor is clearer. (A small, real cost of inheriting a mixin.)
    constexpr Version(int ma, int mi) : major(ma), minor(mi) {}

    friend bool operator==(const Version& a, const Version& b) {
        return a.major == b.major && a.minor == b.minor;
    }
    friend bool operator<(const Version& a, const Version& b) {
        return a.major != b.major ? a.major < b.major : a.minor < b.minor;
    }
    // != > <= >= all come from the mixin, at zero cost.
};

// ===========================================================================
// APPROACH 4: std::variant — closed-set polymorphism, no heap, no vtable
// ===========================================================================
struct VarCircle { double r; double area() const { return 3.14159265358979*r*r; } };
struct VarSquare { double s; double area() const { return s*s; } };
using AnyShape = std::variant<VarCircle, VarSquare>;

double area_of(const AnyShape& s) {
    // std::visit compiles to a jump table over the variant's index.
    // One well-predicted indirect jump, and each branch is fully inlined.
    return std::visit([](const auto& x) { return x.area(); }, s);
}

// ===========================================================================
void demo_all() {
    std::printf("\n=== 1. All four approaches, side by side ===\n");

    // templates
    std::vector<TCircle> tc{{1.0}, {2.0}};
    std::printf("  template:  total_area = %.4f  (sizeof element = %zu)\n",
                total_area(tc), sizeof(TCircle));
    std::printf("  concept:   total_area = %.4f\n", total_area_checked(tc));

    // CRTP
    CCircle cc(1.0); CSquare cs(2.0);
    cc.describe(); cs.describe();
    std::printf("  CRTP half_area from the BASE class: %.4f (inlined, no vtable)\n",
                cc.half_area());

    // mixin
    Version v1{1, 2}, v2{1, 3};
    std::printf("  mixin: v1<v2=%d v1>=v2=%d v1!=v2=%d  (from operator< and == only)\n",
                v1 < v2, v1 >= v2, v1 != v2);

    // variant
    std::vector<AnyShape> shapes{VarCircle{1.0}, VarSquare{2.0}, VarCircle{3.0}};
    double sum = 0; for (const auto& s : shapes) sum += area_of(s);
    std::printf("  variant:   total = %.4f, sizeof(AnyShape) = %zu (inline, no heap)\n",
                sum, sizeof(AnyShape));
}

// ===========================================================================
//  BENCHMARK
//
//  ⚠️ NOTE ON BENCHMARK METHODOLOGY — this matters more than the result:
//   1. Build ALL the data BEFORE timing anything. If construction lands inside
//      a timed region, you are measuring malloc, not dispatch. (The first draft
//      of this file made exactly that mistake and reported a nonsense result.)
//   2. WARM UP: the first traversal pays page faults for freshly-mapped memory.
//   3. Repeat and take the BEST time — you are measuring a lower bound, and
//      noise only ever makes things slower.
//   4. Consume the result so the optimiser can't delete the loop.
//   5. Test the MEGAMORPHIC case too. A virtual call where the target is always
//      the same class is the best case for the branch predictor and lets the
//      compiler speculatively devirtualise. Real code isn't like that.
// ===========================================================================
struct VSquare : IShape {
    double s;
    explicit VSquare(double x) : s(x) {}
    double area() const override { return s * s; }
};

void benchmark() {
    std::printf("\n=== 2. The cost, measured ===\n");
    constexpr int N      = 2'000'000;
    constexpr int REPS   = 5;

    // ---- build everything up front, OUTSIDE any timing ----
    std::vector<std::unique_ptr<IShape>> mono;    // all VCircle
    std::vector<std::unique_ptr<IShape>> poly;    // alternating types
    std::vector<TCircle>                 vals;    // contiguous values
    std::vector<AnyShape>                vars;    // contiguous variants
    mono.reserve(N); poly.reserve(N); vals.reserve(N); vars.reserve(N);
    for (int i = 0; i < N; ++i) {
        double d = 1.0 + i % 10;
        mono.push_back(std::make_unique<VCircle>(d));
        if (i % 2) poly.push_back(std::make_unique<VCircle>(d));
        else       poly.push_back(std::make_unique<VSquare>(d));
        vals.push_back(TCircle{d});
        vars.push_back(i % 2 ? AnyShape{VarCircle{d}} : AnyShape{VarSquare{d}});
    }

    // Time a callable REPS times and return the BEST (lowest) result.
    auto best_ms = [](auto&& fn) {
        double best = 1e18;
        fn();                                   // warm-up: page faults, caches
        for (int r = 0; r < REPS; ++r) {
            auto t0 = std::chrono::steady_clock::now();
            volatile double sink = fn();        // volatile: can't be optimised away
            auto t1 = std::chrono::steady_clock::now();
            (void)sink;
            double ms = std::chrono::duration<double,std::milli>(t1-t0).count();
            if (ms < best) best = ms;
        }
        return best;
    };

    double t_mono = best_ms([&]{ double s=0; for (const auto& p : mono) s += p->area(); return s; });
    double t_poly = best_ms([&]{ double s=0; for (const auto& p : poly) s += p->area(); return s; });
    double t_vals = best_ms([&]{ return total_area(vals); });
    double t_vars = best_ms([&]{ double s=0; for (const auto& v : vars) s += area_of(v); return s; });

    std::printf("  virtual, MONOmorphic (all one type):  %7.2f ms\n", t_mono);
    std::printf("  virtual, POLYmorphic (mixed types):   %7.2f ms\n", t_poly);
    std::printf("  template on contiguous values:        %7.2f ms\n", t_vals);
    std::printf("  variant  on contiguous values:        %7.2f ms\n", t_vars);
    std::printf("  template vs polymorphic-virtual: %.1fx faster\n", t_poly / (t_vals + 1e-9));
    std::printf("\n  memory: virtual = %zu MB of POINTERS + %d separate heap allocations\n",
                N*sizeof(void*)/(1u<<20), N);
    std::printf("          values  = %zu MB total, ONE allocation, zero indirection\n",
                N*sizeof(TCircle)/(1u<<20));

    std::printf(
      "\n  HOW TO READ THIS HONESTLY:\n"
      "   - The monomorphic virtual case is close to the template case. That is\n"
      "     REAL: the branch predictor always guesses right, and clang can\n"
      "     speculatively devirtualise when it proves only one type is used.\n"
      "     Don't let anyone tell you virtual is 'always slow'.\n"
      "   - The polymorphic case is where the gap opens: unpredictable indirect\n"
      "     branch, no inlining, no vectorisation.\n"
      "   - In this microbenchmark the objects were all allocated in one burst,\n"
      "     so even the heap ones ended up fairly contiguous. In a long-running\n"
      "     server with mixed allocation and frees, the pointer-chasing version\n"
      "     degrades much further and the gap widens a lot.\n"
      "   - The MEMORY difference is the part that is unconditionally true, and\n"
      "     it's usually what dominates at scale.\n");
}

// ===========================================================================
void the_tradeoffs() {
    std::printf("\n=== 3. When to use which ===\n");
    std::printf(
      "  TEMPLATES / CONCEPTS\n"
      "    ✅ zero cost, full inlining, works with unrelated types\n"
      "    ❌ type must be known at compile time; code bloat; slow builds;\n"
      "       must live in headers; every user recompiles when you change it\n"
      "    USE FOR: algorithms, containers, hot paths, library internals\n"
      "\n"
      "  CRTP\n"
      "    ✅ zero cost, lets a base share implementation with derived classes\n"
      "    ❌ no common base type -> you cannot make a vector<ShapeBase>;\n"
      "       error messages are rough; the pattern confuses newcomers\n"
      "    USE FOR: mixins, static interfaces, expression templates\n"
      "\n"
      "  std::variant\n"
      "    ✅ closed set, value semantics, no heap, exhaustive (compile error if\n"
      "       you forget a case), one predictable jump\n"
      "    ❌ every alternative must be known up front; sizeof = largest member\n"
      "    USE FOR: AST nodes, protocol messages, state machines, results\n"
      "\n"
      "  VIRTUAL\n"
      "    ✅ open set (plugins!), one binary, fast compiles, easy to read,\n"
      "       heterogeneous containers\n"
      "    ❌ vptr, indirect call, inlining barrier, requires heap + indirection\n"
      "    USE FOR: plugin boundaries, coarse-grained calls, genuinely open\n"
      "             extension points\n"
      "\n"
      "  ⭐ THE HIGH-PERFORMANCE ANSWER: virtual at the COARSE level (one call per\n"
      "     request / per batch / per frame), templates INSIDE the hot loop.\n"
      "     ClickHouse, DuckDB, and every vectorised database do exactly this:\n"
      "     one virtual call dispatches a BATCH of 100k rows to a fully inlined,\n"
      "     vectorised, templated kernel. Amortised dispatch cost: ~0.\n");
}

int main() {
    demo_all();
    benchmark();
    the_tradeoffs();
    std::printf("\n>>> Run with `make opt` — this file is meaningless at -O0,\n"
                "    because -O0 doesn't inline and the whole point is inlining.\n");
    return 0;
}
