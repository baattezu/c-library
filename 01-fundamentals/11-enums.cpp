// ===========================================================================
//  ENUMS, STRUCTS, UNIONS, AND std::variant — modelling data without objects
//  run: make run F=01-fundamentals/11-enums.cpp
// ===========================================================================
//
//  Java's enum is a full class: it can have fields, methods, constructors, and
//  it's a singleton object per constant. Powerful, and it costs a heap object
//  and a pointer dereference.
//
//  C++'s enum is an INTEGER with a type. Zero storage overhead, zero indirection.
//  When you need Java-enum-like behaviour you compose it from free functions,
//  constexpr tables, and (for sum types) std::variant.
//
// ===========================================================================

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>        // std::to_underlying (C++23)
#include <variant>

// ---------------------------------------------------------------------------
// 1. UNSCOPED (C-style) enum — legacy, mostly avoid
// ---------------------------------------------------------------------------
enum Color { RED, GREEN, BLUE };   // values leak into the enclosing scope!
// Problems: RED is now a global name; it implicitly converts to int; two enums
// with the same member name collide. This is why C++11 added enum class.

// ---------------------------------------------------------------------------
// 2. SCOPED enum (enum class) — YOUR DEFAULT
// ---------------------------------------------------------------------------
// Note: the underlying type must actually FIT the values. `: uint8_t` here
// would be a compile error, because 404 doesn't fit in 8 bits — and that error
// is exactly the kind of protection you want in a wire-protocol struct.
enum class Status : std::uint16_t {
    Ok        = 200,
    NotFound  = 404,
    Error     = 500,
};
// - Names are scoped: Status::Ok. No collisions.
// - NO implicit conversion to int (you must static_cast). Type safety.
// - Underlying type is specifiable, so you control the size — critical for
//   struct layout and wire protocols.

enum class LogLevel { Trace, Debug, Info, Warn, Error };   // 0,1,2,3,4

// Attaching behaviour: in Java you'd add a method to the enum. In C++ you write
// a free function — and if it's constexpr, the call vanishes at compile time.
constexpr std::string_view to_string(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}
// A `switch` over an enum class with no `default:` is a FEATURE: add a new
// enumerator and the compiler warns (-Wswitch) at every switch you forgot to
// update. That is exhaustiveness checking, for free. Never write `default:` in
// an enum switch if you want that safety.

// C++ has no "values()" — but you can make one, and it's a compile-time array.
constexpr std::array ALL_LEVELS{LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                                LogLevel::Warn,  LogLevel::Error};

void demo_enums() {
    std::cout << "\n=== 1-2. Enums ===\n";

    Color c = GREEN;
    std::cout << "  unscoped: GREEN implicitly converts to " << c << "\n";

    Status s = Status::NotFound;
    // std::cout << s;                        // ERROR: no implicit conversion
    std::cout << "  scoped Status::NotFound = "
              << static_cast<int>(s)
              << "   sizeof(Status)=" << sizeof(Status)
              << " (we forced uint16_t; default would be int == 4 bytes)\n";

    std::cout << "  levels: ";
    for (auto l : ALL_LEVELS) std::cout << to_string(l) << " ";
    std::cout << "\n";

    // Compile-time evaluation: this string comparison happens during compilation.
    static_assert(to_string(LogLevel::Warn) == "WARN");

    // C++23: std::to_underlying(e) instead of static_cast<int>(e).
    std::cout << "  to_underlying(Status::Error) = "
              << static_cast<int>(std::to_underlying(Status::Error)) << "\n";
}

// ---------------------------------------------------------------------------
// 3. ENUM AS BITFLAGS
// ---------------------------------------------------------------------------
// Java uses EnumSet (a heap object wrapping a long). C++ uses raw bits — one
// integer, zero allocation. This is how every OS API expresses options
// (O_RDONLY|O_CREAT, PROT_READ|PROT_WRITE, EPOLLIN|EPOLLET).
enum class Perm : std::uint32_t {
    None    = 0,
    Read    = 1u << 0,
    Write   = 1u << 1,
    Execute = 1u << 2,
};
// enum class blocks the operators, so we define them. This is boilerplate you
// write once (or macro-ise).
constexpr Perm operator|(Perm a, Perm b) {
    return static_cast<Perm>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr Perm operator&(Perm a, Perm b) {
    return static_cast<Perm>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr bool has(Perm set, Perm flag) { return (set & flag) == flag; }

void demo_flags() {
    std::cout << "\n=== 3. Bit flags ===\n";
    constexpr Perm p = Perm::Read | Perm::Write;
    std::cout << "  bits = " << static_cast<std::uint32_t>(p)
              << "  read=" << std::boolalpha << has(p, Perm::Read)
              << " exec=" << has(p, Perm::Execute) << "\n";
    static_assert(has(p, Perm::Write));   // checked at compile time
    std::cout << "  entire permission set costs 4 bytes and 0 allocations\n";
}

// ---------------------------------------------------------------------------
// 4. STRUCT vs CLASS (they are the same thing)
// ---------------------------------------------------------------------------
// `struct` = class with default PUBLIC members and public inheritance.
// `class`  = class with default PRIVATE members.
// That is the ONLY difference. Convention: `struct` for plain data with no
// invariants; `class` when there are invariants to protect.
struct Point3 { float x{}, y{}, z{}; };   // POD-ish, 12 bytes, no vtable, no header

void demo_structs() {
    std::cout << "\n=== 4. Structs are plain memory ===\n";

    Point3 p{1.0f, 2.0f, 3.0f};                  // aggregate initialization
    Point3 q{.x = 4.0f, .z = 6.0f};              // C++20 designated initializers
    std::cout << "  p=(" << p.x << "," << p.y << "," << p.z << ") "
                 "q=(" << q.x << "," << q.y << "," << q.z << ")\n";
    std::cout << "  sizeof(Point3) = " << sizeof(Point3)
              << " bytes. In Java a Point3 object is ~28-32 bytes (header + fields\n"
                 "  + alignment) and lives behind a pointer.\n";

    // Structured bindings (C++17) decompose aggregates — like destructuring.
    auto [x, y, z] = p;
    std::cout << "  structured binding: x=" << x << " y=" << y << " z=" << z << "\n";

    // std::is_trivially_copyable => you may memcpy it, write it to a socket,
    // mmap it, put it in shared memory. That's a property Java objects can never
    // have, and it is the foundation of zero-copy systems programming.
    std::cout << "  trivially copyable? " << std::boolalpha
              << std::is_trivially_copyable_v<Point3> << "  -> memcpy-safe\n";
}

// ---------------------------------------------------------------------------
// 5. UNION and std::variant — sum types
// ---------------------------------------------------------------------------
// A raw `union` overlays members in the same storage. It does NOT remember which
// member is active — reading the wrong one is UB. Only use raw unions for
// low-level bit tricks or C interop.
union RawBits { float f; std::uint32_t u; };

// std::variant is the type-safe, C++17 version: a tagged union. It DOES know
// which alternative is active and throws if you ask for the wrong one.
// This is C++'s answer to Java 17's sealed interfaces + pattern matching.
using Value = std::variant<int, double, std::string>;

void demo_variant() {
    std::cout << "\n=== 5. union and std::variant ===\n";

    RawBits rb{};
    rb.f = 1.5f;
    std::printf("  union: 1.5f has bit pattern 0x%08X\n", rb.u);
    std::cout << "  (a raw union doesn't track the active member — that's UB waiting)\n";

    Value v = 42;
    std::cout << "  variant holds index " << v.index() << " (0=int)\n";
    v = std::string("now a string");
    std::cout << "  variant holds index " << v.index() << " (2=string)\n";

    // std::visit + an overload set is the C++ equivalent of a `switch` over a
    // sealed hierarchy. It is exhaustive: forget an alternative, get a compile
    // error. And it costs ONE indirect jump — no heap object, no virtual call
    // through a base class pointer.
    auto describe = [](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int>)              std::cout << "  int: " << x << "\n";
        else if constexpr (std::is_same_v<T, double>)      std::cout << "  double: " << x << "\n";
        else if constexpr (std::is_same_v<T, std::string>) std::cout << "  string: " << x << "\n";
    };
    std::visit(describe, v);
    v = 3.14;
    std::visit(describe, v);

    std::cout << "  sizeof(variant<int,double,string>) = " << sizeof(Value)
              << " = biggest alternative + a tag. All inline, no heap.\n";

    // get_if returns a pointer (nullptr on mismatch); get<T> throws.
    if (const double* d = std::get_if<double>(&v)) std::cout << "  get_if<double> -> " << *d << "\n";
}

int main() {
    demo_enums();
    demo_flags();
    demo_structs();
    demo_variant();
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * `enum class` is the default: scoped names, no implicit int conversion,
//    controllable underlying type (`: uint8_t`).
//  * A switch over an enum class with NO default: gives you compiler-checked
//    exhaustiveness (-Wswitch). Use it.
//  * Attach behaviour with free constexpr functions, not methods.
//  * Bit flags = an integer + user-defined |, &. Zero allocation. This is how
//    every OS API works.
//  * struct == class, except for default access (public vs private).
//  * Aggregate init `{a,b,c}`, designated init `{.x=1}` (C++20), and structured
//    bindings `auto [a,b] = p;` (C++17).
//  * `std::is_trivially_copyable` means memcpy-safe — the basis of zero-copy IO.
//  * Raw union: no active-member tracking, UB if you read the wrong one.
//    std::variant: tagged, safe, visit-able, still allocation-free.
// ===========================================================================
