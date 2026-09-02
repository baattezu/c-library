// ===========================================================================
//  CLASSES — and why idiomatic C++ OOP looks nothing like idiomatic Java OOP
//  run: make run F=03-oop/01-classes-and-encapsulation.cpp
// ===========================================================================
//
//  Java's design centre: everything is a class, deep hierarchies, interfaces,
//  DI containers, getters/setters, `Object` at the root.
//
//  C++'s design centre: VALUE TYPES with invariants, free functions, templates,
//  composition. Inheritance is a tool for one specific job (runtime
//  polymorphism), not the organising principle of your program.
//
//  If you write Java-shaped C++, you will get Java-shaped performance without
//  Java's JIT to save you. This file is about writing C++-shaped C++.
//
// ===========================================================================

#include <cmath>
#include <compare>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. A CLASS WITH AN INVARIANT
//    Encapsulation exists to protect an INVARIANT — a statement that is always
//    true about the object. If there's no invariant, use a plain struct.
// ---------------------------------------------------------------------------
class Temperature {
    double celsius_;         // INVARIANT: always >= -273.15

public:
    // `explicit` prevents implicit conversion: `f(42.0)` won't silently become
    // `f(Temperature{42.0})`. Mark single-argument constructors explicit unless
    // you specifically want the conversion.
    explicit Temperature(double c) : celsius_(c) {
        if (c < -273.15) throw std::invalid_argument("below absolute zero");
    }

    // Named constructors — clearer than overloading on a double.
    static Temperature from_fahrenheit(double f) { return Temperature((f - 32) * 5 / 9); }
    static Temperature from_kelvin(double k)     { return Temperature(k - 273.15); }

    // const member functions = the read-only interface.
    // [[nodiscard]] makes ignoring the result a warning — use it on anything
    // pure. It catches real bugs (`v.empty();` instead of `v.clear();`).
    [[nodiscard]] double celsius()    const noexcept { return celsius_; }
    [[nodiscard]] double fahrenheit() const noexcept { return celsius_ * 9 / 5 + 32; }
    [[nodiscard]] double kelvin()     const noexcept { return celsius_ + 273.15; }

    // Mutators re-check the invariant.
    void set_celsius(double c) {
        if (c < -273.15) throw std::invalid_argument("below absolute zero");
        celsius_ = c;
    }

    // C++20 THREE-WAY COMPARISON ("spaceship"). This ONE line generates
    // <, <=, >, >=, ==, and != for you. Java needs Comparable + equals +
    // hashCode + a lot of ceremony.
    auto operator<=>(const Temperature&) const = default;
    bool operator==(const Temperature&)  const = default;
};

// Stream output as a FREE function (Java would make this toString()).
// Free functions keep the class small and let you extend types you don't own.
std::ostream& operator<<(std::ostream& os, const Temperature& t) {
    return os << t.celsius() << "°C";
}

void demo_invariants() {
    std::cout << "\n=== 1. Encapsulation protects an invariant ===\n";
    Temperature room(21.5);
    std::cout << "  " << room << " = " << room.fahrenheit() << "°F = "
              << room.kelvin() << "K\n";

    auto boiling = Temperature::from_fahrenheit(212);
    std::cout << "  from_fahrenheit(212) = " << boiling << "\n";
    std::cout << "  room < boiling? " << std::boolalpha << (room < boiling)
              << "   (from one `operator<=>` line)\n";

    try { Temperature bad(-300); }
    catch (const std::invalid_argument& e) { std::cout << "  rejected: " << e.what() << "\n"; }
}

// ---------------------------------------------------------------------------
// 2. WHEN *NOT* TO WRITE A CLASS
// ---------------------------------------------------------------------------
void demo_no_invariant_no_class() {
    std::cout << "\n=== 2. No invariant? Use a struct. ===\n";

    // ❌ Java-brain version: private fields + trivial getters/setters.
    class PointJavaStyle {
        double x_, y_;
    public:
        PointJavaStyle(double x, double y) : x_(x), y_(y) {}
        double getX() const { return x_; }
        double getY() const { return y_; }
        void setX(double x) { x_ = x; }
        void setY(double y) { y_ = y; }
    };

    // ✅ C++ version. Any (x,y) is valid, so there is nothing to protect.
    // Trivial getters/setters provide zero encapsulation — they're just typing.
    struct Point { double x{}, y{}; };

    Point p{3, 4};
    p.x = 5;
    std::cout << "  struct Point: " << sizeof(Point) << " bytes, aggregate-init,\n"
                 "    structured-bindable, trivially copyable, memcpy-safe.\n";

    // Behaviour goes in FREE FUNCTIONS. This is not a compromise — it is better:
    //  - the class stays minimal and its invariant obvious
    //  - you can add operations without touching (or owning) the type
    //  - free functions found by ADL work in generic code
    auto length = [](Point q) { return std::hypot(q.x, q.y); };
    std::cout << "  length via a free function: " << length(p) << "\n";

    (void)sizeof(PointJavaStyle);
    std::cout <<
      "\n  RULE: `class` when there is an invariant to protect.\n"
      "        `struct` (all public) when the data is just data.\n"
      "        Never write a getter/setter pair that does nothing.\n";
}

// ---------------------------------------------------------------------------
// 3. CONSTRUCTORS IN DETAIL
// ---------------------------------------------------------------------------
class Connection {
    std::string host_;         // ⚠️ declaration order == INITIALISATION order
    int         port_;
    std::string url_;          // depends on host_ and port_, so declared AFTER

public:
    // Member-init list: constructs each member ONCE, directly.
    // Assigning in the body would default-construct then assign — two ops.
    Connection(std::string host, int port)
        : host_(std::move(host)),                     // sink param + move
          port_(port),
          url_(host_ + ":" + std::to_string(port_))   // safe: host_ is already built
    {}

    // DELEGATING constructor (C++11) — avoid duplicating logic.
    explicit Connection(std::string host) : Connection(std::move(host), 80) {}

    [[nodiscard]] const std::string& url() const noexcept { return url_; }
};

void demo_constructors() {
    std::cout << "\n=== 3. Constructors ===\n";
    Connection a("example.com", 8080);
    Connection b("example.com");
    std::cout << "  " << a.url() << "  and  " << b.url() << " (delegating ctor)\n";

    std::cout <<
      "\n  KEY POINTS\n"
      "   - Members initialise in DECLARATION order, not member-init-list order.\n"
      "     Get them out of sync and clang warns (-Wreorder). Fix the order.\n"
      "   - Prefer the init list over assignment in the body.\n"
      "   - `explicit` on 1-arg constructors, unless you WANT implicit conversion.\n"
      "   - If the constructor throws, the object never existed: its destructor\n"
      "     is not called, but already-built members ARE destroyed correctly.\n"
      "   - MOST VEXING PARSE: `Connection c();` declares a FUNCTION returning\n"
      "     Connection. Write `Connection c;` or `Connection c{};`.\n";

    // Default member initialisers (C++11) — usually the cleanest option:
    struct Config {
        int         timeout_ms = 5000;
        int         retries    = 3;
        bool        verbose    = false;
        std::string name       = "default";
    };
    Config cfg;                                  // all defaults
    Config cfg2{.timeout_ms = 100, .verbose = true};   // C++20 designated init
    std::cout << "  designated init: timeout=" << cfg2.timeout_ms
              << " retries=" << cfg2.retries << " verbose=" << cfg2.verbose << "\n";
}

// ---------------------------------------------------------------------------
// 4. STATIC MEMBERS, and the thread-safe singleton
// ---------------------------------------------------------------------------
class Counter {
    static inline int instances_ = 0;   // C++17 `inline` = define it right here,
                                        // no separate definition in a .cpp needed
public:
    Counter()  { ++instances_; }
    ~Counter() { --instances_; }
    static int instances() noexcept { return instances_; }
};

class Registry {
    Registry() { std::cout << "  Registry constructed (lazily, once)\n"; }
public:
    // The Meyers singleton. Function-local statics are initialised on FIRST CALL
    // and C++11 GUARANTEES that initialisation is thread-safe ("magic statics").
    // This is the correct C++ singleton — no double-checked locking, no volatile,
    // no static-init-order fiasco.
    static Registry& instance() {
        static Registry r;
        return r;
    }
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    void hello() const { std::cout << "  registry says hi\n"; }
};

void demo_statics() {
    std::cout << "\n=== 4. Static members and singletons ===\n";
    { Counter a, b, c; std::cout << "  live counters: " << Counter::instances() << "\n"; }
    std::cout << "  after scope: " << Counter::instances() << "\n";

    Registry::instance().hello();
    Registry::instance().hello();    // constructed only once

    std::cout <<
      "\n  ⚠️ NAMESPACE-SCOPE globals in DIFFERENT translation units have an\n"
      "     UNSPECIFIED initialisation order (the 'static initialisation order\n"
      "     fiasco'). The function-local static above sidesteps it entirely.\n"
      "     For compile-time-initialised globals, use `constinit` (C++20).\n";
}

// ---------------------------------------------------------------------------
// 5. COMPOSITION OVER INHERITANCE — the C++ default
// ---------------------------------------------------------------------------
void demo_composition() {
    std::cout << "\n=== 5. Composition is the default ===\n";

    // Java reflex:  class HttpServer extends AbstractServer implements Lifecycle
    // C++ reflex:   class HttpServer { Socket s_; ThreadPool p_; Router r_; };
    struct Socket     { int fd = -1; };
    struct ThreadPool { int threads = 4; };
    struct Router     { std::vector<std::string> routes; };

    class HttpServer {
        Socket     socket_;      // HAS-A, stored INLINE — no allocation, no indirection
        ThreadPool pool_;
        Router     router_;
    public:
        void add_route(std::string r) { router_.routes.push_back(std::move(r)); }
        [[nodiscard]] std::size_t route_count() const { return router_.routes.size(); }
    };

    HttpServer s;
    s.add_route("/api/health");
    s.add_route("/api/users");
    std::cout << "  HttpServer composed of 3 members, " << sizeof(HttpServer)
              << " bytes, all inline. " << s.route_count() << " routes.\n";

    std::cout <<
      "\n  WHY COMPOSITION WINS IN C++:\n"
      "   - members live INLINE: no allocation, no pointer chase, one cache line\n"
      "     if they're small\n"
      "   - no vptr, so the objects stay small\n"
      "   - no fragile base class problem; changing a member doesn't reshape a\n"
      "     hierarchy\n"
      "   - the compiler can inline right through it\n"
      "\n  Use INHERITANCE only when you genuinely need runtime substitutability\n"
      "  through a base pointer. That's it. That's the rule.\n"
      "\n  And note: you don't need Spring. Dependency injection in C++ is\n"
      "  'pass the dependency to the constructor'. That was always all it was.\n";
}

int main() {
    demo_invariants();
    demo_no_invariant_no_class();
    demo_constructors();
    demo_statics();
    demo_composition();
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * class vs struct: only default access differs. class = has an invariant.
//  * Members initialise in DECLARATION order. Use the member-init list.
//  * `explicit` on single-argument constructors.
//  * `[[nodiscard]]` on pure functions; `noexcept` on things that can't throw.
//  * `auto operator<=>(const T&) const = default;` gives you all six comparisons.
//  * Free functions > member functions for anything that doesn't need private
//    access. Smaller interface, extensible, works with ADL.
//  * Function-local `static` = thread-safe lazy init (the correct singleton).
//  * Most vexing parse: `T t();` is a function declaration.
//  * COMPOSITION by default; inheritance only for runtime polymorphism.
// ===========================================================================
