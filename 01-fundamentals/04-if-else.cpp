// ===========================================================================
//  BRANCHING — and why branches are a PERFORMANCE topic, not a syntax topic
//  run:  make run F=01-fundamentals/04-if-else.cpp
//  then: make asm F=01-fundamentals/04-if-else.cpp   <- see branchless code emitted
// ===========================================================================
//
//  The syntax here is 95% identical to Java, so we spend 10 lines on syntax and
//  the rest on the thing that actually matters at high load: a modern CPU is a
//  15-20 stage pipeline that SPECULATIVELY EXECUTES past every branch. A
//  mispredicted branch costs ~15-20 cycles of thrown-away work. In a hot loop,
//  an unpredictable `if` can be slower than doing BOTH sides of it.
//
// ===========================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

void demo_syntax() {
    std::cout << "\n=== 1. Syntax (basically Java) ===\n";

    int x = 7;
    if (x > 10)       std::cout << "big\n";
    else if (x > 5)   std::cout << "medium\n";
    else              std::cout << "small\n";

    // Ternary, same as Java.
    std::cout << "parity: " << (x % 2 == 0 ? "even" : "odd") << "\n";

    // ⚠️ DIFFERENCE FROM JAVA: the condition is contextually converted to bool,
    // so ANY integer, pointer, or type with `explicit operator bool` works.
    // 0 / nullptr / empty optional are false; everything else is true.
    int* p = nullptr;
    if (!p) std::cout << "null pointer is falsy\n";
    if (x)  std::cout << "nonzero int is truthy (Java would reject this)\n";

    // ⚠️ THE CLASSIC BUG: `if (x = 5)` assigns and is always true.
    // Modern compilers warn (-Wparentheses). Some people write `if (5 == x)`
    // ("Yoda conditions") to make it a compile error. Just enable warnings.
}

void demo_init_statements() {
    std::cout << "\n=== 2. if with initializer (C++17) — scope hygiene ===\n";

    std::vector<int> v{1, 2, 3, 4, 5};

    // The variable `it` exists ONLY inside the if/else. This is the single best
    // small feature of modern C++: it keeps your scopes tight, which means fewer
    // dangling references and better register allocation.
    if (auto it = std::find(v.begin(), v.end(), 3); it != v.end()) {
        std::cout << "found " << *it << " at index " << (it - v.begin()) << "\n";
    } else {
        std::cout << "not found\n";
    }
    // `it` is gone here.

    // Same for switch:
    if (auto n = v.size(); n > 3) std::cout << "vector has " << n << " elements\n";
}

// C++17 structured bindings + optional make error handling readable without
// exceptions. (Full treatment in 05-stl/08-error-handling-optional-expected.cpp)
std::optional<int> parse_port(const std::string& s) {
    try {
        int v = std::stoi(s);
        if (v < 1 || v > 65535) return std::nullopt;
        return v;
    } catch (...) { return std::nullopt; }
}

void demo_optional_branching() {
    std::cout << "\n=== 3. Branching on optional ===\n";
    for (const std::string& in : {"8080", "99999", "abc"}) {
        if (auto port = parse_port(in)) {           // optional is contextually bool
            std::cout << "  '" << in << "' -> port " << *port << "\n";
        } else {
            std::cout << "  '" << in << "' -> invalid\n";
        }
    }
}

void demo_switch() {
    std::cout << "\n=== 4. switch — and what the compiler does with it ===\n";

    // switch works on INTEGRAL and ENUM types only. NOT on strings (unlike Java 7+).
    // Workaround: hash the string at compile time (see const.cpp) or use a
    // std::unordered_map<std::string_view, Handler>.
    enum class HttpMethod { Get, Post, Put, Delete_ };
    auto name = [](HttpMethod m) -> const char* {
        switch (m) {
            case HttpMethod::Get:     return "GET";
            case HttpMethod::Post:    return "POST";
            case HttpMethod::Put:     return "PUT";
            case HttpMethod::Delete_: return "DELETE";
        }
        return "?";   // unreachable, but silences -Wreturn-type
    };
    std::cout << name(HttpMethod::Post) << " " << name(HttpMethod::Delete_) << "\n";

    // ⭐ COMPILER INSIGHT: for a dense set of cases the compiler emits a JUMP
    // TABLE — one indirect jump, O(1), no comparisons. For a sparse set it emits
    // a binary search tree of comparisons. Look at `make asm` output to see which.
    // This is exactly what the JVM does with tableswitch vs lookupswitch.

    // Fallthrough is implicit (C legacy) and a famous bug source. Since C++17
    // you annotate deliberate fallthrough so the compiler stops warning:
    int code = 302, redirects = 0;
    switch (code) {
        case 301:
        case 302:
            ++redirects;
            [[fallthrough]];      // "I meant to do that"
        case 200:
            std::cout << "handled code " << code << ", redirects=" << redirects << "\n";
            break;
        default:
            std::cout << "unhandled\n";
    }
}

// ---------------------------------------------------------------------------
//  THE REAL LESSON: BRANCH PREDICTION
// ---------------------------------------------------------------------------
// We sum only elements >= 128 in a big array, twice: once with random data
// (unpredictable branch) and once with sorted data (perfectly predictable).
// SAME instructions, SAME memory access pattern, SAME cache behaviour.
// The only difference is whether the CPU can guess the branch. Run with
// `make opt F=01-fundamentals/04-if-else.cpp` — at -O0 the noise hides the effect.
// ---------------------------------------------------------------------------
void demo_branch_prediction() {
    std::cout << "\n=== 5. Branch prediction: same work, 3-6x time difference ===\n";

    constexpr std::size_t N = 1u << 22;   // 4M elements
    std::vector<int> data(N);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& x : data) x = dist(rng);

    auto bench = [](const std::vector<int>& v, const char* label) {
        auto t0 = std::chrono::steady_clock::now();
        std::int64_t sum = 0;
        for (int rep = 0; rep < 4; ++rep)
            for (int x : v)
                if (x >= 128) sum += x;       // <-- THE BRANCH
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  " << label << ": " << ms << " ms  (sum=" << sum << ")\n";
        return ms;
    };

    double unsorted = bench(data, "unsorted (unpredictable)");
    std::sort(data.begin(), data.end());
    double sorted   = bench(data, "sorted   (predictable)  ");
    std::cout << "  speedup from sorting alone: " << (unsorted / sorted) << "x\n";

    // BRANCHLESS version: no `if` at all. We compute a mask and multiply.
    // The CPU cannot mispredict what it never branches on.
    {
        std::vector<int> rnd(N);
        std::mt19937 r2(42);
        for (auto& x : rnd) x = dist(r2);
        auto t0 = std::chrono::steady_clock::now();
        std::int64_t sum = 0;
        for (int rep = 0; rep < 4; ++rep)
            for (int x : rnd) {
                // -(x >= 128) is 0 or -1 (all bits set). AND-ing gives x or 0.
                std::int64_t mask = -static_cast<std::int64_t>(x >= 128);
                sum += x & mask;
            }
        auto t1 = std::chrono::steady_clock::now();
        std::cout << "  branchless on UNSORTED data: "
                  << std::chrono::duration<double, std::milli>(t1 - t0).count()
                  << " ms (sum=" << sum << ")\n";
    }

    std::cout << "\n  WHY: the CPU pipeline is ~15-20 stages deep. It guesses which\n"
                 "  way a branch goes and speculatively executes ahead. A wrong guess\n"
                 "  flushes the pipeline: ~15-20 wasted cycles. With random data the\n"
                 "  predictor is right ~50% of the time. With sorted data, ~100%.\n"
                 "  This is why `sort then process` sometimes beats `process directly`\n"
                 "  even counting the sort. This is real, and it is why hot-path code\n"
                 "  in databases and network stacks is written branch-free.\n";
}

int main() {
    demo_syntax();
    demo_init_statements();
    demo_optional_branching();
    demo_switch();
    demo_branch_prediction();

    std::cout << "\n>>> RUN THIS WITH `make opt F=01-fundamentals/04-if-else.cpp`.\n"
                 "    At -O0 the measurements are meaningless.\n";
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * Conditions are contextually converted to bool: ints, pointers, optional,
//    and any type with `explicit operator bool()`.
//  * `if (init; cond)` (C++17) keeps variables scoped to the branch. Use it.
//  * switch is integral/enum only — no strings. Compiler emits a jump table for
//    dense cases, a comparison tree for sparse ones.
//  * `[[fallthrough]];` documents deliberate fallthrough.
//  * A mispredicted branch costs ~15-20 cycles. Predictable branches are ~free.
//    In hot loops, prefer branchless arithmetic, `std::min/max`, or sorting the
//    data so the predictor wins.
//  * `[[likely]]` / `[[unlikely]]` attributes hint the compiler about layout —
//    use only with profile data, never on a hunch.
// ===========================================================================
