// ===========================================================================
//  const, constexpr, consteval — C++'s answer to "when does this happen?"
//  run: make run F=01-fundamentals/03-const.cpp
// ===========================================================================
//
//  Java has `final`, which means "this reference can't be reassigned" — it says
//  nothing about the object. C++'s `const` is deep, viral, part of the type
//  system, and enforced by the compiler through pointers and references.
//
//  Then C++ goes further: `constexpr` moves computation from RUNTIME to COMPILE
//  TIME. This has no Java equivalent at all (annotation processors are a distant,
//  sad cousin). It's how you get zero-cost abstractions and compile-time tables.
//
// ===========================================================================

#include <array>
#include <iostream>
#include <string_view>

// ---------------------------------------------------------------------------
// 1. const on VALUES
// ---------------------------------------------------------------------------
void demo_const_values() {
    std::cout << "\n=== 1. const values ===\n";

    const int max_retries = 3;
    // max_retries = 4;   // compile error, exactly like Java's final

    // But unlike Java's `final Foo f`, const on a class type makes the OBJECT
    // const: you can only call methods marked `const` on it.
    const std::string s = "hello";
    std::cout << s.size() << "\n";   // ok: size() is const
    // s.push_back('!');             // ERROR: push_back is non-const

    std::cout << "max_retries=" << max_retries << " s=" << s << "\n";
}

// ---------------------------------------------------------------------------
// 2. const with POINTERS — read declarations RIGHT TO LEFT
// ---------------------------------------------------------------------------
void demo_const_pointers() {
    std::cout << "\n=== 2. const and pointers: read right-to-left ===\n";

    int a = 1, b = 2;

    //  int*             p   -> "p is a pointer to int"                 (both mutable)
    //  const int*       p   -> "p is a pointer to const int"           (can't write *p)
    //  int const*       p   -> identical to above (const binds left)
    //  int* const       p   -> "p is a const pointer to int"           (can't reseat p)
    //  const int* const p   -> "const pointer to const int"            (neither)

    int* p1 = &a;                 p1 = &b;  *p1 = 20;   // both fine
    const int* p2 = &a;           p2 = &b;              // reseat ok
    //                            *p2 = 5;              // ERROR: can't write
    int* const p3 = &a;                     *p3 = 10;   // write ok
    //                            p3 = &b;              // ERROR: can't reseat
    const int* const p4 = &a;
    // neither allowed on p4.

    std::cout << "a=" << a << " b=" << b << " *p2=" << *p2 << " *p4=" << *p4 << "\n";

    // ⭐ THE MOST IMPORTANT USE: `const T&` as a function parameter.
    // It means "I will read your object and not copy it and not modify it."
    // In Java, passing an object is always a cheap reference copy. In C++,
    // passing by value COPIES (possibly a whole vector!). So:
    //     void f(std::vector<int> v)         // COPIES the entire vector
    //     void f(const std::vector<int>& v)  // no copy, read-only  <- default choice
}

// ---------------------------------------------------------------------------
// 3. const MEMBER FUNCTIONS — the interface contract
// ---------------------------------------------------------------------------
class Counter {
    int count_ = 0;
    mutable int reads_ = 0;   // `mutable` = "modifiable even in a const object".
                              // Legitimate uses: caches, mutexes, stats counters.
public:
    void increment() { ++count_; }             // non-const: mutates
    int  value() const { ++reads_; return count_; }  // const: promises not to mutate
    int  reads() const { return reads_; }

    // "const correctness" is transitive through the interface: a const Counter&
    // can only call value()/reads(). This is compile-time-checked immutability,
    // which Java simply cannot express for objects.
};

void print_counter(const Counter& c) {   // takes const ref: no copy, no mutation
    std::cout << "counter=" << c.value() << "\n";
    // c.increment();   // ERROR — and that error is the whole point.
}

void demo_const_methods() {
    std::cout << "\n=== 3. const member functions ===\n";
    Counter c;
    c.increment(); c.increment(); c.increment();
    print_counter(c);
    std::cout << "reads so far: " << c.reads() << " (mutable field still worked)\n";
}

// ---------------------------------------------------------------------------
// 4. constexpr — computation at COMPILE TIME
// ---------------------------------------------------------------------------

// constexpr function: "this CAN run at compile time if its inputs are known."
// If they're not, it just runs at runtime. Best of both.
constexpr std::uint64_t factorial(int n) {
    std::uint64_t r = 1;
    for (int i = 2; i <= n; ++i) r *= static_cast<std::uint64_t>(i);
    return r;
}

// consteval (C++20): "this MUST run at compile time." A runtime call is an error.
consteval std::uint32_t fnv1a(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (char c : s) { h ^= static_cast<std::uint8_t>(c); h *= 16777619u; }
    return h;
}

// A lookup table computed entirely by the compiler and baked into the binary's
// read-only data section. Zero runtime cost, zero startup cost.
constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
constexpr auto CRC_TABLE = make_crc_table();

void demo_constexpr() {
    std::cout << "\n=== 4. constexpr: work the compiler does so the CPU doesn't ===\n";

    // static_assert is a COMPILE-TIME assertion. If this were wrong, the build
    // would fail. Notice: factorial(20) is never computed at runtime.
    static_assert(factorial(5) == 120);
    static_assert(factorial(20) == 2432902008176640000ULL);
    std::cout << "factorial(20) = " << factorial(20)
              << "   <- this literal was in the binary before you ran it\n";

    // Same function, runtime argument -> runs at runtime. One implementation.
    int n = 10;
    std::cout << "factorial(runtime 10) = " << factorial(n) << "\n";

    // consteval string hashing: turn strings into integers at compile time.
    // This is how you write a `switch` over strings, which C++ otherwise lacks.
    constexpr std::uint32_t h_get  = fnv1a("GET");
    constexpr std::uint32_t h_post = fnv1a("POST");
    static_assert(h_get != h_post);
    std::cout << "compile-time hash(\"GET\")=" << h_get
              << " hash(\"POST\")=" << h_post << "\n";

    std::cout << "CRC_TABLE[1]=0x" << std::hex << CRC_TABLE[1]
              << " CRC_TABLE[255]=0x" << CRC_TABLE[255] << std::dec
              << "  (256 entries, computed by the compiler)\n";

    // ⭐ THE HIGH-LOAD ANGLE: any work you can push to compile time is work your
    // server does zero times per request. Protocol tables, dispatch tables,
    // perfect hashes, unit conversions, config validation. Java can't do this;
    // it does it at class-load time at best, and via reflection at worst.
}

// ---------------------------------------------------------------------------
// 5. `const` vs `constexpr` vs `#define`
// ---------------------------------------------------------------------------
void demo_comparison() {
    std::cout << "\n=== 5. const vs constexpr vs macro ===\n";
    // #define MAX 100        <- preprocessor text substitution. No type, no scope,
    //                           no debugger visibility. AVOID in C++.
    // const int MAX = 100;   <- typed, scoped. Usable as an array size if the
    //                           initialiser is a constant expression.
    // constexpr int MAX = 100; <- guaranteed usable in constant expressions.
    //                             THIS IS THE DEFAULT CHOICE for constants.
    // inline constexpr int MAX = 100;  <- at namespace scope in a header, so all
    //                                     translation units share one entity (C++17)
    constexpr int MAX = 100;
    int arr[MAX]{};          // array size must be a compile-time constant
    std::cout << "array of " << std::size(arr) << " ints, "
              << sizeof(arr) << " bytes on the stack\n";
}

int main() {
    demo_const_values();
    demo_const_pointers();
    demo_const_methods();
    demo_constexpr();
    demo_comparison();

    std::cout << "\n>>> TRY IT: change `consteval` to `constexpr` on fnv1a and call it\n"
                 "    with a runtime std::string. Then change it back and watch the\n"
                 "    compiler refuse. That's the difference between CAN and MUST.\n";
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * Java `final` == "reference not reassignable". C++ `const` == deep, viral,
//    type-system-level immutability that propagates through refs and pointers.
//  * Read pointer declarations RIGHT TO LEFT. `const int*` = pointer to const
//    int; `int* const` = const pointer to int.
//  * `const T&` parameters are the default way to pass anything bigger than a
//    couple of words. It avoids a copy AND documents intent.
//  * `const` member functions are the read-only half of your class's interface.
//    `mutable` fields (caches, mutexes) are the sanctioned escape hatch.
//  * constexpr = "may be evaluated at compile time". consteval = "must be".
//    constinit = "must be initialised at compile time" (kills static-init-order
//    fiasco for globals).
//  * Compile-time work is free work. Use it for tables, hashes, and validation.
// ===========================================================================
