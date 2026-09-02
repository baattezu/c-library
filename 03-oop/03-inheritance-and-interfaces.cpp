// ===========================================================================
//  INHERITANCE, INTERFACES, AND THE PITFALLS
//  run: make run F=03-oop/03-inheritance-and-interfaces.cpp
// ===========================================================================

#include <iostream>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 1. ACCESS SPECIFIERS AND INHERITANCE MODES
// ---------------------------------------------------------------------------
//   MEMBER ACCESS
//     public    — everyone
//     protected — this class and derived classes
//     private   — this class and its friends only (DEFAULT for `class`)
//
//   INHERITANCE MODE (this has no Java equivalent at all)
//     class D : public    B  — "D IS-A B". Public members stay public. ← 99% case
//     class D : protected B  — B's public members become protected in D
//     class D : private   B  — "D is IMPLEMENTED IN TERMS OF B". Not an IS-A.
//                              Outside code cannot convert D* to B*.
//
//   `class D : B` defaults to PRIVATE inheritance (a classic surprise).
//   `struct D : B` defaults to PUBLIC.
//   ⇒ ALWAYS write `: public B` explicitly.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 2. A PURE INTERFACE — the C++ equivalent of a Java interface
// ---------------------------------------------------------------------------
class Codec {                       // "interface Codec"
public:
    virtual ~Codec() = default;                                   // MANDATORY
    virtual std::string encode(const std::string& in) const = 0;  // = 0 -> pure
    virtual std::string decode(const std::string& in) const = 0;
    virtual const char* name() const noexcept = 0;

    // C++ interfaces MAY have implemented methods (like Java 8 default methods).
    // Non-virtual ones are the safest: same behaviour for all implementers.
    std::string round_trip(const std::string& s) const { return decode(encode(s)); }

    // Prevent slicing at the language level:
    Codec(const Codec&)            = delete;
    Codec& operator=(const Codec&) = delete;
protected:
    Codec() = default;              // only derived classes can construct
};

class Rot13 : public Codec {
public:
    std::string encode(const std::string& in) const override {
        std::string out = in;
        for (char& c : out) {
            if (c >= 'a' && c <= 'z') c = char('a' + (c - 'a' + 13) % 26);
            else if (c >= 'A' && c <= 'Z') c = char('A' + (c - 'A' + 13) % 26);
        }
        return out;
    }
    std::string decode(const std::string& in) const override { return encode(in); }
    const char* name() const noexcept override { return "rot13"; }
};

class Reverse final : public Codec {     // `final`: nothing may derive from this
public:
    std::string encode(const std::string& in) const override {
        return std::string(in.rbegin(), in.rend());
    }
    std::string decode(const std::string& in) const override { return encode(in); }
    const char* name() const noexcept override { return "reverse"; }
};

void demo_interfaces() {
    std::cout << "\n=== 1. Interfaces ===\n";
    std::vector<std::unique_ptr<Codec>> codecs;
    codecs.push_back(std::make_unique<Rot13>());
    codecs.push_back(std::make_unique<Reverse>());

    const std::string msg = "Hello C++";
    for (const auto& c : codecs) {
        auto enc = c->encode(msg);
        std::cout << "  " << c->name() << ": '" << msg << "' -> '" << enc
                  << "' -> '" << c->decode(enc) << "'"
                  << "  round_trip ok? " << std::boolalpha
                  << (c->round_trip(msg) == msg) << "\n";
    }

    std::cout <<
      "\n  vs JAVA:\n"
      "   - `= 0` makes a method pure virtual; any class with one is ABSTRACT.\n"
      "   - There is no `implements` keyword; you just inherit publicly.\n"
      "   - C++ allows MULTIPLE inheritance of interfaces (and of state — see\n"
      "     the diamond below), Java allows multiple interfaces only.\n"
      "   - The base destructor MUST be virtual (or protected non-virtual).\n"
      "   - `override` is checked; `final` prevents further overriding and lets\n"
      "     the compiler devirtualise.\n";
}

// ---------------------------------------------------------------------------
// 3. THE DIAMOND PROBLEM AND VIRTUAL INHERITANCE
// ---------------------------------------------------------------------------
void demo_diamond() {
    std::cout << "\n=== 2. The diamond problem (C++ has state in bases) ===\n";

    // NON-virtual: B and C each get their OWN copy of A. `D` has TWO `value`s.
    struct A          { int value = 1; };
    struct B : A      { };
    struct C : A      { };
    struct D : B, C   { };

    D d;
    d.B::value = 10;
    d.C::value = 20;
    // d.value = 5;    // ERROR: ambiguous — which A::value did you mean?
    std::cout << "  non-virtual diamond: D has TWO A subobjects. "
                 "B::value=" << d.B::value << " C::value=" << d.C::value
              << " sizeof(D)=" << sizeof(D) << "\n";

    // VIRTUAL inheritance: B and C SHARE one A subobject.
    struct VA           { int value = 1; };
    struct VB : virtual VA { };
    struct VC : virtual VA { };
    struct VD : VB, VC     { };

    VD vd;
    vd.value = 42;      // unambiguous now — there's only one
    std::cout << "  virtual diamond: ONE shared A. value=" << vd.value
              << " sizeof(VD)=" << sizeof(VD)
              << "  <- bigger! virtual bases need offset pointers\n";

    std::cout <<
      "\n  COST OF VIRTUAL INHERITANCE: each object stores extra offset info\n"
      "  (a vbptr), and accessing a virtual base member requires an extra\n"
      "  indirection. Constructors of virtual bases are called by the MOST\n"
      "  DERIVED class, which is deeply confusing.\n"
      "\n"
      "  PRACTICAL ADVICE: avoid multiple inheritance of STATE entirely.\n"
      "  Multiple inheritance of pure INTERFACES (no data members) is fine and\n"
      "  common — it's exactly Java's `implements A, B`, and it costs one vptr.\n"
      "  Real usage of `virtual` inheritance in the wild: basically only\n"
      "  std::iostream's hierarchy. Java sidestepped all of this by forbidding\n"
      "  multiple inheritance of classes. They were probably right.\n";
}

// ---------------------------------------------------------------------------
// 4. NAME HIDING (a real, common bug)
// ---------------------------------------------------------------------------
void demo_name_hiding() {
    std::cout << "\n=== 3. Name hiding ===\n";

    struct Base {
        void f(int)    { std::cout << "    Base::f(int)\n"; }
        void f(double) { std::cout << "    Base::f(double)\n"; }
    };
    struct Derived : Base {
        void f(std::string) { std::cout << "    Derived::f(string)\n"; }
        // Declaring ANY `f` in Derived HIDES ALL of Base's `f` overloads.
        // Not overrides — HIDES. d.f(1) would not compile without the fix.
    };
    struct Fixed : Base {
        using Base::f;                 // ← THE FIX: pull the base overloads in
        void f(std::string) { std::cout << "    Fixed::f(string)\n"; }
    };

    Derived d;
    // d.f(1);      // ERROR: no viable conversion from int to std::string
    d.f("hi");
    d.Base::f(1);   // works, but ugly

    Fixed fx;
    fx.f(1);        // now resolves to Base::f(int)
    fx.f("hi");

    std::cout <<
      "\n  Java does NOT do this — overloads from a superclass stay visible.\n"
      "  In C++, name lookup finds the FIRST scope containing the name and stops.\n"
      "  Overload resolution then happens only within that scope. Fix with\n"
      "  `using Base::f;`.\n";
}

// ---------------------------------------------------------------------------
// 5. CALLING VIRTUALS FROM CONSTRUCTORS — DIFFERENT FROM JAVA
// ---------------------------------------------------------------------------
void demo_ctor_virtual() {
    std::cout << "\n=== 4. Virtual calls in constructors ===\n";

    struct Base {
        Base() { std::cout << "    Base ctor calls init() -> "; init(); }
        virtual ~Base() = default;
        virtual void init() { std::cout << "Base::init\n"; }
    };
    struct Derived : Base {
        int important = 42;
        Derived() { std::cout << "    Derived ctor body\n"; }
        void init() override { std::cout << "Derived::init (important=" << important << ")\n"; }
    };

    Derived d;
    (void)d;

    std::cout <<
      "\n  C++ called Base::init — because during Base's constructor, the object\n"
      "  IS a Base: the vptr hasn't been updated to Derived's vtable yet, and\n"
      "  Derived's members don't exist.\n"
      "\n"
      "  JAVA calls Derived.init() here, which reads `important` before it is\n"
      "  assigned and observes 0. Java's behaviour is arguably worse (it hands\n"
      "  you a half-built object); C++'s is safer but surprising.\n"
      "\n"
      "  ⚠️ A PURE virtual called from a constructor is UNDEFINED BEHAVIOUR\n"
      "     (there's nothing to call). Usually a link error or a crash.\n"
      "\n"
      "  RULE: never call a virtual function from a constructor or destructor.\n"
      "  Use two-phase init or a factory function that constructs then calls.\n";
}

// ---------------------------------------------------------------------------
// 6. THE NVI IDIOM (Non-Virtual Interface) — good C++ design
// ---------------------------------------------------------------------------
class RequestHandler {
public:
    virtual ~RequestHandler() = default;

    // PUBLIC and NON-virtual: the stable algorithm, shared by everyone. You can
    // add logging, timing, validation, and error handling here ONCE and every
    // implementation gets it. Nobody can accidentally skip it.
    std::string handle(const std::string& req) {
        std::cout << "    [log] request: " << req << "\n";
        if (req.empty()) return "400 Bad Request";
        auto resp = do_handle(req);              // <- the ONE customisation point
        std::cout << "    [log] response: " << resp << "\n";
        return resp;
    }

private:
    // PRIVATE and virtual: subclasses override it but cannot call it directly.
    // Yes — you CAN override a private virtual in C++. Access control and
    // virtual dispatch are independent mechanisms.
    virtual std::string do_handle(const std::string& req) = 0;
};

class EchoHandler : public RequestHandler {
    std::string do_handle(const std::string& req) override { return "200 " + req; }
};

void demo_nvi() {
    std::cout << "\n=== 5. NVI: public non-virtual, private virtual ===\n";
    EchoHandler h;
    h.handle("GET /health");
    std::cout <<
      "\n  WHY: it separates the STABLE interface (what callers see) from the\n"
      "  VARIABLE implementation (what subclasses provide). You can change the\n"
      "  wrapper — add metrics, tracing, retries — without touching any subclass.\n"
      "  This is Spring's AOP/interceptor idea, done at compile time, for free.\n";
}

// ---------------------------------------------------------------------------
void the_rules() {
    std::cout << "\n=== 6. Inheritance rules of thumb ===\n";
    std::cout <<
      "  ✅ DO\n"
      "   - inherit publicly, and write `public` explicitly\n"
      "   - give polymorphic bases a virtual (or protected non-virtual) destructor\n"
      "   - mark every override `override`; mark leaf classes `final`\n"
      "   - inherit from PURE interfaces (no data) when you need multiple bases\n"
      "   - keep hierarchies FLAT (1-2 levels). Deep hierarchies are a cache and\n"
      "     comprehension disaster.\n"
      "   - use NVI: public non-virtual wrapper, private virtual hook\n"
      "\n"
      "  ❌ DON'T\n"
      "   - inherit to reuse code — use composition or a template\n"
      "   - multiply-inherit STATE\n"
      "   - call virtuals from constructors/destructors\n"
      "   - put default arguments on virtual functions (they bind to the STATIC\n"
      "     type, so an override's default is ignored — a silent, real bug)\n"
      "   - store polymorphic objects by value in containers (slicing)\n"
      "\n"
      "  AND THE BIG ONE: in C++, most 'polymorphism' problems are better solved\n"
      "  by std::variant (closed set) or templates (compile-time). Runtime\n"
      "  inheritance is for genuinely open, plugin-style extension points.\n";
}

int main() {
    demo_interfaces();
    demo_diamond();
    demo_name_hiding();
    demo_ctor_virtual();
    demo_nvi();
    the_rules();
    return 0;
}
