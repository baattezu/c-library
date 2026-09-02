// ===========================================================================
//  COMPILE-TIME COMPUTATION — work your server does ZERO times per request
//  run: make run F=04-templates/04-constexpr-metaprogramming.cpp
// ===========================================================================
//
//  This has NO Java equivalent. Java can do some work at class-load time and
//  the JIT can constant-fold, but you cannot ask the *compiler* to run your
//  algorithm and bake the answer into the binary.
//
//  C++ can. Anything computed at compile time is:
//    - free at runtime (it's a literal in .rodata)
//    - free at startup (no static initialisation)
//    - verified at build time (a wrong answer is a compile error)
//
//  For a high-load server this is a genuine architectural tool: protocol
//  tables, dispatch tables, perfect hashes, unit conversions, config
//  validation, and state machine transition tables all move off the hot path.
//
// ===========================================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string_view>
#include <type_traits>

// ---------------------------------------------------------------------------
// 1. THE FIVE KEYWORDS
// ---------------------------------------------------------------------------
//   const      — "I won't modify this" (may still be a runtime value)
//   constexpr  — "CAN be evaluated at compile time, if the inputs allow"
//   consteval  — "MUST be evaluated at compile time" (an 'immediate function')
//   constinit  — "MUST be initialised at compile time" (kills static-init-order
//                 fiasco for globals, without forcing const)
//   static_assert — "check this at compile time or fail the build"

constexpr int factorial(int n) { return n <= 1 ? 1 : n * factorial(n - 1); }

consteval std::uint64_t fnv1a(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (char c : s) { h ^= static_cast<std::uint8_t>(c); h *= 1099511628211ULL; }
    return h;
}

constinit int g_startup_value = factorial(10);   // computed by the COMPILER

void demo_keywords() {
    std::cout << "\n=== 1. The five keywords ===\n";
    static_assert(factorial(5) == 120);
    std::cout << "  factorial(10) baked into the binary: " << g_startup_value << "\n";

    int n = 6;                              // a runtime value
    std::cout << "  factorial(runtime 6) = " << factorial(n)
              << "   (same function, runs at runtime this time)\n";

    constexpr auto h = fnv1a("Content-Length");
    std::cout << "  consteval hash(\"Content-Length\") = " << h << "\n";
    // fnv1a(std::string(argv[1]));   // ERROR: consteval demands a constant
}

// ---------------------------------------------------------------------------
// 2. COMPILE-TIME TABLES
// ---------------------------------------------------------------------------
// Since C++14, constexpr functions can have loops, local variables, and
// branches. Since C++20 they can even allocate (as long as the allocation is
// freed before the constant expression ends). So you can just... write normal
// code, and the compiler runs it.

constexpr std::array<std::uint32_t, 256> make_crc32_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
constexpr auto CRC32_TABLE = make_crc32_table();   // 1 KB, in .rodata, free

constexpr std::uint32_t crc32(std::string_view data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (char ch : data)
        c = CRC32_TABLE[(c ^ static_cast<std::uint8_t>(ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// A sine lookup table, computed at build time. (constexpr math: since C++23
// many <cmath> functions are constexpr; we roll our own Taylor series so this
// works everywhere.)
constexpr double const_sin(double x) {
    double term = x, sum = x;
    for (int i = 1; i < 12; ++i) {
        term *= -x * x / ((2*i) * (2*i + 1));
        sum += term;
    }
    return sum;
}
constexpr std::array<double, 16> make_sin_table() {
    std::array<double, 16> t{};
    for (int i = 0; i < 16; ++i) t[i] = const_sin(3.14159265358979 * i / 16);
    return t;
}
constexpr auto SIN_TABLE = make_sin_table();

void demo_tables() {
    std::cout << "\n=== 2. Tables computed by the compiler ===\n";
    std::printf("  CRC32_TABLE[1]   = 0x%08X   (256 entries, zero runtime cost)\n", CRC32_TABLE[1]);
    static_assert(CRC32_TABLE[1] == 0x77073096u);

    constexpr auto c = crc32("hello world");
    std::printf("  crc32(\"hello world\") = 0x%08X  <- computed at COMPILE time\n", c);
    static_assert(crc32("hello world") == 0x0D4A1185u);

    std::printf("  SIN_TABLE[8] = %.6f (sin(pi/2) should be 1.0)\n", SIN_TABLE[8]);

    std::cout <<
      "\n  Verify it yourself: `make asm F=04-templates/04-constexpr-metaprogramming.cpp`\n"
      "  and grep for the table bytes. There is no code that computes them.\n";
}

// ---------------------------------------------------------------------------
// 3. COMPILE-TIME STRING DISPATCH — a `switch` over strings
// ---------------------------------------------------------------------------
// C++ can't switch on strings. But it can switch on a compile-time hash of one.
// This is a real technique used in HTTP header parsers and JSON deserialisers.
constexpr std::uint64_t operator""_hash(const char* s, std::size_t n) {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < n; ++i) { h ^= static_cast<std::uint8_t>(s[i]); h *= 1099511628211ULL; }
    return h;
}
constexpr std::uint64_t hash_of(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (char c : s) { h ^= static_cast<std::uint8_t>(c); h *= 1099511628211ULL; }
    return h;
}

enum class Method { Get, Post, Put, Delete, Unknown };

constexpr Method parse_method(std::string_view s) {
    switch (hash_of(s)) {                       // a real jump table / binary search
        case "GET"_hash:    return Method::Get;
        case "POST"_hash:   return Method::Post;
        case "PUT"_hash:    return Method::Put;
        case "DELETE"_hash: return Method::Delete;
        default:            return Method::Unknown;
    }
}

void demo_string_switch() {
    std::cout << "\n=== 3. switch over strings, via compile-time hashing ===\n";
    static_assert(parse_method("GET")  == Method::Get);
    static_assert(parse_method("POST") == Method::Post);
    static_assert(parse_method("XYZ")  == Method::Unknown);

    for (std::string_view m : {"GET", "POST", "PATCH"}) {
        auto r = parse_method(m);
        std::printf("  parse_method(\"%.*s\") -> %d\n",
                    int(m.size()), m.data(), static_cast<int>(r));
    }
    std::cout <<
      "\n  ⚠️ HASH COLLISIONS: two different strings could hash the same, and\n"
      "     you'd silently dispatch wrong. In production, either verify the\n"
      "     string after matching the hash, or use a PERFECT hash generated for\n"
      "     your exact key set (gperf, or frozen::unordered_map).\n"
      "     Never ship an unverified hash switch on attacker-controlled input.\n";
}

// ---------------------------------------------------------------------------
// 4. TYPE-LEVEL METAPROGRAMMING (<type_traits>)
// ---------------------------------------------------------------------------
// Templates compute with TYPES the way constexpr computes with VALUES.
// You will read this in library code even if you rarely write it.

// A type-level `if`:
template <bool B, class T, class F> struct if_type          { using type = T; };
template <         class T, class F> struct if_type<false,T,F> { using type = F; };
// (the standard version is std::conditional_t<B, T, F>)

// Choose the smallest integer type that can hold N values.
template <std::size_t N>
using smallest_uint_t =
    std::conditional_t<(N <= 0xFF),       std::uint8_t,
    std::conditional_t<(N <= 0xFFFF),     std::uint16_t,
    std::conditional_t<(N <= 0xFFFFFFFF), std::uint32_t,
                                          std::uint64_t>>>;

// Detect a member at compile time and adapt.
template <class T>
constexpr std::size_t serialized_size() {
    if constexpr (std::is_trivially_copyable_v<T>) return sizeof(T);
    else                                           return 0;   // needs custom logic
}

void demo_type_metaprogramming() {
    std::cout << "\n=== 4. Computing with types ===\n";

    static_assert(std::is_same_v<smallest_uint_t<100>,        std::uint8_t>);
    static_assert(std::is_same_v<smallest_uint_t<1000>,       std::uint16_t>);
    static_assert(std::is_same_v<smallest_uint_t<100000>,     std::uint32_t>);
    std::printf("  smallest type for 100 values:    %zu byte(s)\n",  sizeof(smallest_uint_t<100>));
    std::printf("  smallest type for 100000 values: %zu byte(s)\n", sizeof(smallest_uint_t<100000>));
    std::cout <<
      "  -> a real technique: index types sized to your actual data. An index\n"
      "     array of 1M entries is 1 MB as uint8_t and 8 MB as size_t.\n";

    struct Pod { int a; double b; };
    struct NotPod { std::string s; };
    std::printf("  serialized_size<Pod>()    = %zu (memcpy-able)\n", serialized_size<Pod>());
    std::printf("  serialized_size<NotPod>() = %zu (needs custom serialisation)\n", serialized_size<NotPod>());

    // The traits you'll actually use, day to day:
    std::cout <<
      "\n  THE TRAITS WORTH MEMORISING:\n"
      "    std::is_same_v<A,B>                   are these the same type?\n"
      "    std::decay_t<T>                       strip const/ref/array-ness\n"
      "    std::remove_cvref_t<T>                strip const/volatile/reference\n"
      "    std::conditional_t<B,T,F>             compile-time if\n"
      "    std::is_trivially_copyable_v<T>       can I memcpy it?\n"
      "    std::is_nothrow_move_constructible_v  will vector move or copy it?\n"
      "    std::underlying_type_t<Enum>          the enum's integer type\n"
      "    std::invoke_result_t<F,Args...>       what does calling F return?\n"
      "    std::void_t<...>                      the old SFINAE workhorse\n"
      "\n  In modern code, prefer `if constexpr` + `requires` over trait\n"
      "  gymnastics. The traits are still what the standard library is built on.\n";
}

// ---------------------------------------------------------------------------
// 5. THE LIMITS AND THE COSTS
// ---------------------------------------------------------------------------
void the_limits() {
    std::cout << "\n=== 5. Limits and costs ===\n";
    std::cout <<
      "  WHAT constexpr CAN DO (C++20/23)\n"
      "   loops, branches, local variables, recursion, most of the standard\n"
      "   algorithms, std::vector and std::string (if freed before the constant\n"
      "   expression ends), virtual calls, try/catch (can't throw), std::array.\n"
      "\n"
      "  WHAT IT CANNOT DO\n"
      "   reinterpret_cast, undefined behaviour of ANY kind (this is why constexpr\n"
      "   is a UB detector — if it compiles as constexpr, it's UB-free on that\n"
      "   path), goto, static/thread_local variables, I/O, syscalls, and\n"
      "   allocation that escapes the constant expression.\n"
      "\n"
      "  THE COSTS\n"
      "   - COMPILE TIME. Constant evaluation runs in an interpreter inside the\n"
      "     compiler, ~100-1000x slower than native. A table of a million entries\n"
      "     will add real seconds to your build. Use -ftime-trace to see it.\n"
      "   - There are implementation limits on recursion depth and evaluation\n"
      "     steps (clang: -fconstexpr-steps, -fconstexpr-depth).\n"
      "   - BINARY SIZE: baked tables live in .rodata. That's usually a good\n"
      "     trade, but a 100 MB table is still 100 MB.\n"
      "\n"
      "  ⭐ THE HIGH-LOAD PRINCIPLE\n"
      "     Move work as early as possible in this chain:\n"
      "         per-request > per-connection > startup > COMPILE TIME\n"
      "     Every step left is a multiplier on your throughput. C++ is one of\n"
      "     very few languages where the last step is fully available to you.\n";
}

int main() {
    demo_keywords();
    demo_tables();
    demo_string_switch();
    demo_type_metaprogramming();
    the_limits();
    return 0;
}
