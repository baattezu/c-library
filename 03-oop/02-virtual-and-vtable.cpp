// ===========================================================================
//  VIRTUAL FUNCTIONS — the vtable, laid bare
//  run: make run F=03-oop/02-virtual-and-vtable.cpp
//  then: make asm F=03-oop/02-virtual-and-vtable.cpp   <- see the indirect call
// ===========================================================================
//
//  In Java, EVERY non-final, non-private method is virtual by default, and the
//  JIT devirtualises them at runtime using profile data.
//
//  In C++, methods are NON-virtual by default, and you opt in. There is no JIT,
//  so a virtual call stays an indirect call forever. That means you pay for
//  virtual only where you asked for it — and it means you must know what it costs.
//
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

// ---------------------------------------------------------------------------
// 1. THE OBJECT MODEL: what `virtual` does to your memory
// ---------------------------------------------------------------------------
struct NonVirtual {
    int  x = 1;
    void f() { }                 // ordinary member function
};

struct Virtual {
    int  x = 1;
    virtual ~Virtual() = default;
    virtual void f() { }
};

void demo_layout() {
    std::cout << "\n=== 1. What `virtual` costs in bytes ===\n";
    std::printf("  sizeof(NonVirtual) = %zu   (just the int)\n", sizeof(NonVirtual));
    std::printf("  sizeof(Virtual)    = %zu   (hidden vptr 8 + int 4 + pad 4)\n", sizeof(Virtual));

    Virtual v;
    // The vptr is the FIRST 8 bytes of the object (on the Itanium ABI, which
    // macOS/Linux both use). Let's look at it.
    auto raw = *reinterpret_cast<std::uintptr_t*>(&v);
    std::printf("  first 8 bytes of a Virtual object = %p  <- that's the vptr,\n"
                "     pointing into .rodata where the vtable lives\n", (void*)raw);

    std::cout <<
      "\n  A ONE-INT class went from 4 bytes to 16 — a 4x size increase — because\n"
      "  of one `virtual`. In an array of 10 million, that's 40 MB vs 160 MB, and\n"
      "  memory traffic is what you actually pay for. THIS is why you don't make\n"
      "  small, numerous objects polymorphic.\n";
}

// ---------------------------------------------------------------------------
// 2. HOW DISPATCH WORKS
// ---------------------------------------------------------------------------
//
//   Shape* s = new Circle;
//   s->area();
//
//   compiles to roughly:
//     ldr  x8, [x0]        // load the vptr from the object (offset 0)
//     ldr  x9, [x8, #16]   // load the function pointer at slot 2 of the vtable
//     blr  x9              // indirect call
//
//   Two dependent loads and an indirect branch. Both loads are usually cache
//   hits (vtables are hot), and the branch predictor learns the target if it's
//   stable. Cost: ~2-3ns if predicted, ~20ns if not.
//
//   THE VTABLE ITSELF (one per CLASS, in .rodata, not per object):
//     Circle's vtable: [ offset-to-top | typeinfo* | ~Circle | ~Circle(del) |
//                        area() | name() ]
//
// ---------------------------------------------------------------------------
class Shape {
public:
    virtual ~Shape() = default;                 // ⚠️ MANDATORY. See below.
    virtual double area() const = 0;            // pure virtual -> abstract class
    virtual std::string name() const = 0;

    // NON-virtual: resolved at compile time, inlinable, same for all subclasses.
    // This is the "Template Method" pattern done right — the algorithm is fixed,
    // only the primitives vary.
    void describe() const {
        std::cout << "    " << name() << " with area " << area() << "\n";
    }
};

class Circle : public Shape {
    double r_;
public:
    explicit Circle(double r) : r_(r) {}
    // `override` is not decoration: it makes the compiler VERIFY you're actually
    // overriding something. Typo the signature and you get an error instead of a
    // silently-never-called function. ALWAYS write it.
    double      area() const override { return 3.14159265358979 * r_ * r_; }
    std::string name() const override { return "Circle"; }
};

class Square : public Shape {
    double s_;
public:
    explicit Square(double s) : s_(s) {}
    double      area() const override { return s_ * s_; }
    std::string name() const override { return "Square"; }
};

// `final` on a class or method lets the compiler DEVIRTUALISE: if it knows no
// further override can exist, it can call directly and even inline.
class Triangle final : public Shape {
    double b_, h_;
public:
    Triangle(double b, double h) : b_(b), h_(h) {}
    double      area() const override { return 0.5 * b_ * h_; }
    std::string name() const override { return "Triangle"; }
};

void demo_dispatch() {
    std::cout << "\n=== 2. Runtime polymorphism ===\n";

    // Polymorphism requires INDIRECTION: a pointer or a reference. A Shape by
    // value cannot be a Circle — it isn't big enough. (See slicing, below.)
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Circle>(1.0));
    shapes.push_back(std::make_unique<Square>(2.0));
    shapes.push_back(std::make_unique<Triangle>(3.0, 4.0));

    for (const auto& s : shapes) s->describe();

    double total = 0;
    for (const auto& s : shapes) total += s->area();
    std::cout << "  total area = " << total << "\n";
}

// ---------------------------------------------------------------------------
// 3. THE VIRTUAL DESTRUCTOR RULE (this WILL bite you)
// ---------------------------------------------------------------------------
void demo_virtual_destructor() {
    std::cout << "\n=== 3. The virtual destructor rule ===\n";

    struct BadBase    { ~BadBase()  { std::cout << "    ~BadBase\n"; } };  // NOT virtual
    struct BadDerived : BadBase {
        std::vector<int> data{1,2,3};
        ~BadDerived() { std::cout << "    ~BadDerived (never runs!)\n"; }
    };

    std::cout <<
      "    BadBase* p = new BadDerived;\n"
      "    delete p;   // ☠️ UNDEFINED BEHAVIOUR. ~BadDerived never runs.\n"
      "                //    Its vector<int> leaks. Silently.\n"
      "\n"
      "  THE RULE, stated three ways (they're equivalent):\n"
      "   - If a class has ANY virtual function, give it a virtual destructor.\n"
      "   - If you might `delete` through a base pointer, the base destructor\n"
      "     must be virtual.\n"
      "   - Otherwise, make the base destructor PROTECTED and non-virtual, so\n"
      "     deleting through the base won't compile at all.\n"
      "\n"
      "  The compiler flag -Wnon-virtual-dtor (in this repo's Makefile) catches\n"
      "  it. Java has no equivalent problem — finalization is the GC's job.\n";

    // Correct version:
    struct GoodBase    { virtual ~GoodBase() { std::cout << "    ~GoodBase\n"; } };
    struct GoodDerived : GoodBase { ~GoodDerived() override { std::cout << "    ~GoodDerived\n"; } };
    std::cout << "  correct (virtual dtor):\n";
    { std::unique_ptr<GoodBase> p = std::make_unique<GoodDerived>(); }
}

// ---------------------------------------------------------------------------
// 4. SLICING — the bug that cannot happen in Java
// ---------------------------------------------------------------------------
void demo_slicing() {
    std::cout << "\n=== 4. Object slicing ===\n";

    struct Base    { int a = 1; virtual ~Base() = default; virtual const char* who() const { return "Base"; } };
    struct Derived : Base { int b = 2; const char* who() const override { return "Derived"; } };

    Derived d;
    Base    sliced = d;      // ☠️ COPIES ONLY THE BASE PART. `b` is gone.
                             //    The vptr is the Base's. who() returns "Base".
    Base&   ref    = d;      // ✅ no copy, still polymorphic

    std::cout << "  Derived d          -> who() = " << d.who() << "\n";
    std::cout << "  Base sliced = d    -> who() = " << sliced.who()
              << "   <-- SLICED! sizeof " << sizeof(sliced) << " vs " << sizeof(d) << "\n";
    std::cout << "  Base& ref = d      -> who() = " << ref.who() << "   (correct)\n";

    std::cout <<
      "\n  This happens SILENTLY whenever you pass a derived object by value to a\n"
      "  function taking the base by value, or push one into a vector<Base>.\n"
      "  Java cannot do this because variables are always references.\n"
      "\n"
      "  DEFENCES:\n"
      "   - Store polymorphic objects as unique_ptr<Base>, never as Base.\n"
      "   - Pass by `const Base&`, never by `Base`.\n"
      "   - Delete the copy operations in your abstract base:\n"
      "       Base(const Base&) = delete;  Base& operator=(const Base&) = delete;\n";
}

// ---------------------------------------------------------------------------
// 5. RTTI: dynamic_cast and typeid
// ---------------------------------------------------------------------------
void demo_rtti() {
    std::cout << "\n=== 5. RTTI — dynamic_cast and typeid ===\n";

    std::unique_ptr<Shape> s = std::make_unique<Circle>(2.0);

    // This is Java's `instanceof` + cast, in one operation.
    if (auto* c = dynamic_cast<Circle*>(s.get()))
        std::cout << "  dynamic_cast<Circle*> succeeded, area=" << c->area() << "\n";
    if (auto* sq = dynamic_cast<Square*>(s.get()); !sq)
        std::cout << "  dynamic_cast<Square*> returned nullptr (the failure signal)\n";

    // For REFERENCES it throws instead of returning null:
    try { [[maybe_unused]] auto& bad = dynamic_cast<Square&>(*s); }
    catch (const std::bad_cast&) { std::cout << "  dynamic_cast<Square&> threw std::bad_cast\n"; }

    // typeid gives you the runtime type. The name is MANGLED — demangle with
    // `c++filt` or abi::__cxa_demangle.
    // typeid on a polymorphic glvalue is EVALUATED at runtime (that's the point
    // — it reads the vptr). Clang warns about the side effect; here it's intended.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpotentially-evaluated-expression"
    std::cout << "  typeid(*s).name() = " << typeid(*s).name()
              << "  (mangled; pipe through c++filt)\n";
#pragma clang diagnostic pop

    std::cout <<
      "\n  COST: dynamic_cast walks the class hierarchy at runtime comparing\n"
      "  type_info pointers. For a deep or multiply-inherited hierarchy it can\n"
      "  cost HUNDREDS of nanoseconds — far more than a virtual call. It is\n"
      "  not something you put in a hot loop.\n"
      "\n"
      "  ⭐ AND: needing dynamic_cast is usually a DESIGN SMELL. If you're asking\n"
      "  'what type is this really?', the behaviour you want probably belongs as\n"
      "  a virtual function on the base — or the whole thing should be a\n"
      "  std::variant + std::visit, which is exhaustive, faster, and allocation-free.\n"
      "\n"
      "  Many codebases compile with -fno-rtti to save binary size, which\n"
      "  disables dynamic_cast and typeid entirely.\n";
}

// ---------------------------------------------------------------------------
// 6. THE COST, MEASURED
// ---------------------------------------------------------------------------
struct IOp     { virtual ~IOp() = default; virtual int apply(int) const = 0; };
struct AddOp   : IOp { int apply(int x) const override { return x + 1; } };
struct DirectOp{        int apply(int x) const          { return x + 1; } };

void demo_cost() {
    std::cout << "\n=== 6. Measured cost of virtual dispatch ===\n";

    constexpr int N = 20'000'000;

    // Direct (non-virtual): the compiler INLINES apply() into the loop and may
    // even vectorise or fold the whole thing.
    DirectOp d;
    auto t0 = std::chrono::steady_clock::now();
    long long s1 = 0;
    for (int i = 0; i < N; ++i) s1 += d.apply(i);
    auto t1 = std::chrono::steady_clock::now();

    // Virtual through a base pointer: an indirect call every iteration.
    // The compiler cannot inline it and cannot vectorise across it.
    std::unique_ptr<IOp> v = std::make_unique<AddOp>();
    long long s2 = 0;
    for (int i = 0; i < N; ++i) s2 += v->apply(i);
    auto t2 = std::chrono::steady_clock::now();

    auto ms = [](auto a, auto b) { return std::chrono::duration<double,std::milli>(b-a).count(); };
    std::printf("  direct   (inlinable): %7.2f ms  (sum=%lld)\n", ms(t0,t1), s1);
    std::printf("  virtual  (indirect):  %7.2f ms  (sum=%lld)\n", ms(t1,t2), s2);
    std::printf("  ratio: %.1fx\n", ms(t1,t2) / (ms(t0,t1) + 0.001));

    std::cout <<
      "\n  The gap is NOT mainly the indirect call itself (~2ns, well predicted\n"
      "  here since it's always AddOp). It is that the virtual call is an\n"
      "  OPTIMISATION BARRIER: no inlining, no constant folding, no vectorising,\n"
      "  no loop transformation across it.\n"
      "\n"
      "  ⚠️ Run this with `make opt`. At -O0 both are slow and the lesson is lost.\n";
}

void when_to_use_virtual() {
    std::cout << "\n=== 7. When should you use virtual? ===\n";
    std::cout <<
      "  ✅ YES when:\n"
      "     - the set of types is open (plugins, user-extensible)\n"
      "     - the call is COARSE-GRAINED (one call handles a whole request, a\n"
      "       whole frame, a whole file) so ~3ns of dispatch is irrelevant\n"
      "     - you need heterogeneous containers of unrelated implementations\n"
      "\n"
      "  ❌ NO when:\n"
      "     - the call is in a hot inner loop (millions/sec)\n"
      "     - the objects are small and numerous (vptr doubles their size)\n"
      "     - the type set is CLOSED and known at compile time\n"
      "\n"
      "  ALTERNATIVES, in order of preference for hot paths:\n"
      "     1. Don't be polymorphic. Just call the function.\n"
      "     2. std::variant + std::visit — closed set, exhaustive, no allocation,\n"
      "        one predictable jump. (see 01-fundamentals/11-enums.cpp)\n"
      "     3. Templates / CRTP — static polymorphism, fully inlined, zero cost.\n"
      "        (see 03-oop/04-static-polymorphism-crtp.cpp)\n"
      "     4. Type erasure with a small-buffer optimisation (std::function-like).\n"
      "     5. Data-oriented design: sort work BY TYPE and process each batch\n"
      "        with one direct call. This is what game engines and columnar\n"
      "        databases (ClickHouse, DuckDB) do — it turns 1M virtual calls into\n"
      "        10 virtual calls over batches of 100k.\n";
}

int main() {
    demo_layout();
    demo_dispatch();
    demo_virtual_destructor();
    demo_slicing();
    demo_rtti();
    demo_cost();
    when_to_use_virtual();
    std::cout << "\n>>> Run `make opt F=03-oop/02-virtual-and-vtable.cpp` for the\n"
                 "    real numbers, and `make asm` to see the two-load-plus-blr\n"
                 "    sequence with your own eyes.\n";
    return 0;
}
