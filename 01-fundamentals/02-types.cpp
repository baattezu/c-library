// ===========================================================================
//  THE TYPE SYSTEM: integers that lie, floats that lie, and casts that kill
//  run: make run  F=01-fundamentals/02-types.cpp
//  then: make asan F=01-fundamentals/02-types.cpp     <- watch UBSan catch overflow
// ===========================================================================
//
//  Java has exactly one set of integer sizes (8/16/32/64, all signed except char)
//  and defines overflow to wrap. C++ has *implementation-defined* sizes and
//  UNDEFINED signed overflow. This is not pedantry — it is a real source of
//  security vulnerabilities and of surprising optimiser behaviour.
//
// ===========================================================================

#include <cstdint>     // fixed-width types: int32_t, uint64_t, ...
#include <cstddef>     // size_t, ptrdiff_t, byte, nullptr_t
#include <iomanip>     // setprecision, setw, hex manipulators
#include <iostream>
#include <limits>
#include <bit>         // C++20: bit_cast, popcount, countl_zero
#include <cmath>

void demo_integers() {
    std::cout << "\n=== 1. Integer types ===\n";

    // The "fundamental" types have MINIMUM sizes, not exact ones.
    //   char      >= 8 bits   (1 byte BY DEFINITION; sizeof(char)==1 always)
    //   short     >= 16 bits
    //   int       >= 16 bits  (in practice 32 everywhere you care about)
    //   long      >= 32 bits  (64 on macOS/Linux LP64, 32 on Windows LLP64!)
    //   long long >= 64 bits
    //
    // => In portable code, use <cstdint> fixed-width types when the size matters
    //    (wire formats, file formats, bit manipulation, hashing).
    std::int8_t   i8  = -128;
    std::uint8_t  u8  = 255;
    std::int32_t  i32 = -2147483648;
    std::uint64_t u64 = 18446744073709551615ULL;

    // Note: int8_t is a typedef for `signed char`, so ostream prints it as a
    // CHARACTER. Cast to int to see the number. Classic beginner confusion.
    std::cout << "int8_t  min = " << static_cast<int>(i8)  << "\n";
    std::cout << "uint8_t max = " << static_cast<int>(u8)  << "\n";
    std::cout << "int32_t min = " << i32 << "\n";
    std::cout << "uint64_t max= " << u64 << "\n";

    std::cout << "int range: " << std::numeric_limits<int>::min()
              << " .. " << std::numeric_limits<int>::max() << "\n";

    // size_t: the unsigned type big enough to hold any object size. It is what
    // sizeof returns and what every container's .size() returns.
    std::cout << "sizeof(size_t)=" << sizeof(std::size_t)
              << "  (== sizeof(void*) on sane platforms)\n";
}

void demo_signed_unsigned_trap() {
    std::cout << "\n=== 2. THE UNSIGNED TRAP (memorise this) ===\n";

    // In a comparison between signed and unsigned of the same rank, the SIGNED
    // operand is converted to unsigned. -1 becomes 18446744073709551615.
    int          s = -1;
    unsigned int u = 1;
    // The compiler warns here (-Wsign-compare). That warning is the ONLY thing
    // standing between you and this bug. Never ignore it. We suppress it here
    // purely so the demo prints.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
    std::cout << "(-1 < 1u) evaluates to " << std::boolalpha << (s < u)
              << "   <-- FALSE. Yes, really.\n";
#pragma clang diagnostic pop

    // This bites hardest in loops over containers, because .size() is unsigned:
    std::size_t n = 0;
    std::cout << "looping `for (size_t i = n - 1; i >= 0; --i)` would be INFINITE\n"
              << "   because n-1 with n==0 wraps to " << (n - 1) << "\n";

    // Correct reverse loop over indices:
    // for (std::size_t i = v.size(); i-- > 0; ) { ... }   // "goes to" operator
    // or just use reverse iterators / ranges.

    // Java sidesteps all of this by not having unsigned types at all
    // (until the awkward Integer.toUnsignedLong helpers). C++ makes you think.
}

void demo_overflow() {
    std::cout << "\n=== 3. Overflow: signed is UB, unsigned wraps ===\n";

    // UNSIGNED overflow is DEFINED: it wraps modulo 2^N. Perfectly legal, and
    // relied upon by hash functions, checksums, ring buffers.
    std::uint32_t umax = std::numeric_limits<std::uint32_t>::max();
    std::cout << "uint32 max + 1 = " << (umax + 1u) << "   (defined: wraps to 0)\n";

    // SIGNED overflow is UNDEFINED BEHAVIOUR. Not "wraps". Not "implementation
    // defined". UNDEFINED — the compiler assumes it cannot happen and optimises
    // on that assumption. This is why `if (x + 1 < x)` can be optimised to
    // `false` for signed x, silently deleting your overflow check.
    //
    // Uncomment the next two lines and run `make asan F=01-fundamentals/02-types.cpp`
    // to watch UBSan catch it at runtime:
    //
    // int imax = std::numeric_limits<int>::max();
    // std::cout << "int max + 1 = " << (imax + 1) << "\n";  // UB!!!

    std::cout << "signed overflow is UB -> uncomment the lines in the source and\n"
                 "   run `make asan F=01-fundamentals/02-types.cpp` to see UBSan fire.\n";

    // The safe way to check: use the builtin, or check BEFORE the operation.
    int a = std::numeric_limits<int>::max(), b = 1, result;
    if (__builtin_add_overflow(a, b, &result))          // clang/gcc builtin
        std::cout << "checked add: overflow detected, no UB executed\n";
}

void demo_floats() {
    std::cout << "\n=== 4. Floating point: the same IEEE-754 you know from Java ===\n";

    // float = 32-bit (24 bits of mantissa ~ 7 decimal digits)
    // double = 64-bit (53 bits of mantissa ~ 15-16 decimal digits)
    // long double = 80 or 128-bit on x86; on arm64 macOS it's just double.
    std::cout << "sizeof(float)=" << sizeof(float)
              << " double=" << sizeof(double)
              << " long double=" << sizeof(long double) << "\n";

    std::cout << "0.1 + 0.2 == 0.3 ? " << std::boolalpha << (0.1 + 0.2 == 0.3)
              << "   (same answer as Java: false)\n";
    std::cout << "0.1 + 0.2 = " << std::setprecision(20) << (0.1 + 0.2) << "\n";

    // Never compare floats with ==. Compare with a tolerance appropriate to the
    // MAGNITUDE of your values (relative epsilon), not an absolute 1e-9.
    double x = 0.1 + 0.2, y = 0.3;
    bool close = std::fabs(x - y) <= 1e-12 * std::fmax(std::fabs(x), std::fabs(y));
    std::cout << "relative-epsilon compare: " << close << "\n";

    // Special values, exactly as in Java:
    double inf = std::numeric_limits<double>::infinity();
    double nan = std::numeric_limits<double>::quiet_NaN();
    std::cout << "inf=" << inf << " nan=" << nan
              << "  (nan == nan) = " << (nan == nan) << "\n";
    std::cout << std::setprecision(6);

    // PERFORMANCE NOTE for later: float is not "the fast one" and double is not
    // "the slow one" — on modern CPUs both are 1 op. But float is HALF THE BYTES,
    // so you fit twice as many in a cache line and twice as many per SIMD vector.
    // That is usually the real reason to choose float. Memory, again.
}

void demo_casts() {
    std::cout << "\n=== 5. The four casts (and why C-style casts are banned) ===\n";

    double d = 3.99;

    // static_cast: compile-time-checked, "sane" conversions. Truncates toward 0.
    int i = static_cast<int>(d);
    std::cout << "static_cast<int>(3.99) = " << i << "\n";

    // const_cast: add/remove const. If you ever *need* it you probably have a
    // design bug (or are calling an old C API). Writing through a const_cast to
    // an actually-const object is UB.
    const int ci = 5;
    int& ncref = const_cast<int&>(ci);
    (void)ncref;   // don't write through it; that would be UB.

    // reinterpret_cast: "trust me, treat these bytes as that type." Almost always
    // wrong, and reading through the wrong type violates STRICT ALIASING (UB).
    // Modern correct tool for bit reinterpretation:
    float  f  = 1.0f;
    auto   bits = std::bit_cast<std::uint32_t>(f);   // C++20, safe, constexpr
    std::cout << "bit pattern of 1.0f = 0x" << std::hex << bits << std::dec
              << "   (sign 0, exp 127, mantissa 0)\n";
    std::cout << "popcount of that = " << std::popcount(bits) << "\n";

    // dynamic_cast: runtime-checked downcast in a polymorphic hierarchy.
    // This is Java's `(Dog) animal` + `instanceof`. It costs a runtime lookup
    // through RTTI — see 03-oop/. Returns nullptr on failure for pointers,
    // throws std::bad_cast for references.

    // C-style cast `(int)x` silently picks whichever of the above compiles,
    // including reinterpret_cast. It is unsearchable and dangerous. Never use it.
}

void demo_type_deduction_gotcha() {
    std::cout << "\n=== 6. Integral promotion surprises ===\n";

    // Every arithmetic operation first promotes operands to at least `int`.
    // So `char + char` is an int, and `short * short` can overflow into int fine.
    std::uint8_t a = 200, b = 100;
    auto sum = a + b;    // sum is int(300), NOT uint8_t(44)
    std::cout << "uint8(200) + uint8(100) = " << sum
              << " and its type has size " << sizeof(sum) << "\n";
    std::uint8_t truncated = static_cast<std::uint8_t>(a + b);
    std::cout << "but stored back into uint8_t: " << static_cast<int>(truncated) << "\n";

    // Integer division truncates toward zero (same as Java).
    std::cout << "7/2=" << 7/2 << "  -7/2=" << -7/2 << "  7%2=" << 7%2
              << "  -7%2=" << -7%2 << "\n";
    std::cout << "7.0/2=" << 7.0/2 << "  (one operand double -> double division)\n";
}

int main() {
    demo_integers();
    demo_signed_unsigned_trap();
    demo_overflow();
    demo_floats();
    demo_casts();
    demo_type_deduction_gotcha();
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * sizeof(char)==1 always. Everything else is platform-defined; use
//    <cstdint> (int32_t, uint64_t) when the exact width matters.
//  * size_t is unsigned. Mixing it with int in comparisons is a classic bug.
//  * UNSIGNED overflow wraps (defined). SIGNED overflow is UNDEFINED BEHAVIOUR.
//  * Never compare floats with ==; use a relative epsilon.
//  * Four casts: static_cast (normal), dynamic_cast (RTTI downcast),
//    const_cast (remove const), reinterpret_cast (reinterpret bits — avoid;
//    prefer std::bit_cast). NEVER use C-style casts.
//  * Arithmetic promotes to int before operating.
//  * float vs double is usually a MEMORY BANDWIDTH decision, not a speed one.
// ===========================================================================
