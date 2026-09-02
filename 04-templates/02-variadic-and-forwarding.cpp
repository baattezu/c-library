// ===========================================================================
//  VARIADIC TEMPLATES AND PERFECT FORWARDING
//  run: make run F=04-templates/02-variadic-and-forwarding.cpp
// ===========================================================================
//
//  This is how make_unique, emplace_back, std::format, std::thread, and every
//  wrapper in the standard library is built. Two ideas:
//
//    VARIADIC TEMPLATES  — a function/class taking any number of arguments of
//                          any types, resolved at compile time, fully inlined.
//                          (Java's `Object... args` allocates an array and boxes
//                           every primitive. This does neither.)
//
//    PERFECT FORWARDING  — passing arguments through a wrapper while preserving
//                          their exact value category, so nothing is copied that
//                          didn't need to be.
//
// ===========================================================================

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. VARIADIC BASICS — parameter packs
// ---------------------------------------------------------------------------
//   typename... Ts   declares a TYPE pack
//   Ts... args       declares a FUNCTION PARAMETER pack
//   args...          EXPANDS the pack
//   sizeof...(args)  the number of elements (a compile-time constant)

// The C++11/14 way: recursion with a base case.
void print_recursive() { std::cout << "\n"; }              // base case

template <typename First, typename... Rest>
void print_recursive(const First& first, const Rest&... rest) {
    std::cout << first;
    if constexpr (sizeof...(rest) > 0) std::cout << ", ";
    print_recursive(rest...);                              // recurse with one fewer
}

// The C++17 way: FOLD EXPRESSIONS. Much better — no recursion, no base case,
// and the compiler generates a flat sequence of operations.
//   (pack op ...)         unary right fold:  a op (b op c)
//   (... op pack)         unary left fold:   (a op b) op c
//   (init op ... op pack) binary fold
template <typename... Ts>
void print_fold(const Ts&... args) {
    ((std::cout << args << " "), ...);      // fold over the comma operator
    std::cout << "\n";
}

template <typename... Ts>
auto sum(const Ts&... args) { return (args + ... + 0); }   // binary left fold

template <typename... Ts>
bool all_positive(const Ts&... args) { return ((args > 0) && ...); }

void demo_variadic() {
    std::cout << "\n=== 1. Variadic templates ===\n";
    print_recursive(1, "two", 3.0, 'x');
    print_fold(1, "two", 3.0, 'x');
    std::cout << "  sum(1,2,3,4,5) = " << sum(1,2,3,4,5) << "\n";
    std::cout << "  sum(1.5, 2.5)  = " << sum(1.5, 2.5) << "\n";
    std::cout << "  all_positive(1,2,3) = " << std::boolalpha << all_positive(1,2,3)
              << ", all_positive(1,-2,3) = " << all_positive(1,-2,3) << "\n";

    std::cout <<
      "\n  All of that is resolved at COMPILE TIME and inlines to straight-line\n"
      "  code. Java's varargs allocates an Object[] and boxes every primitive.\n";
}

// ---------------------------------------------------------------------------
// 2. PERFECT FORWARDING
// ---------------------------------------------------------------------------
struct Probe {
    std::string data;
    Probe(std::string d) : data(std::move(d)) { std::cout << "    ctor\n"; }
    Probe(const Probe& o) : data(o.data)      { std::cout << "    COPY ctor\n"; }
    Probe(Probe&& o) noexcept : data(std::move(o.data)) { std::cout << "    move ctor\n"; }
};

struct Consumer {
    Probe p;
    template <typename... Args>
    explicit Consumer(Args&&... args) : p(std::forward<Args>(args)...) {}
};

// ❌ A wrapper that takes by value: one extra move on every call.
template <typename T, typename Arg>
std::unique_ptr<T> make_by_value(Arg arg) { return std::make_unique<T>(std::move(arg)); }

// ✅ PERFECT FORWARDING: `Args&&...` in a deduced context is a FORWARDING
//    reference. It binds to lvalues AND rvalues, and std::forward re-casts each
//    argument back to exactly what the caller passed. Zero extra operations.
template <typename T, typename... Args>
std::unique_ptr<T> my_make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

void demo_forwarding() {
    std::cout << "\n=== 2. Perfect forwarding ===\n";

    Probe lvalue{"an lvalue"};

    std::cout << "  my_make_unique<Consumer>(lvalue):        ";
    std::cout << "\n";
    auto a = my_make_unique<Consumer>(lvalue);              // -> COPY (correct!)

    std::cout << "  my_make_unique<Consumer>(std::move(lv)): \n";
    auto b = my_make_unique<Consumer>(std::move(lvalue));   // -> move (correct!)

    std::cout << "  my_make_unique<Consumer>(Probe{...}):    \n";
    auto c = my_make_unique<Consumer>(Probe{"a temporary"}); // -> move (correct!)

    std::cout << "  make_by_value<Consumer>(Probe{...}):     \n";
    auto d = make_by_value<Consumer>(Probe{"a temporary"});  // -> move + EXTRA move

    (void)a; (void)b; (void)c; (void)d;

    std::cout <<
      "\n  THE MECHANISM (reference collapsing):\n"
      "    template <class T> void f(T&& x);\n"
      "      called with an LVALUE  -> T = Probe&,  so T&& = Probe& && = Probe&\n"
      "      called with an RVALUE  -> T = Probe,   so T&& = Probe&&\n"
      "    Collapsing rules: & &  -> &,  & && -> &,  && & -> &,  && && -> &&\n"
      "    (\"an lvalue reference always wins\")\n"
      "\n"
      "    std::forward<T>(x) is a CONDITIONAL cast: it casts to T&& only when T\n"
      "    was deduced as a non-reference. That's what makes it 'perfect'.\n"
      "\n"
      "  ⚠️ THE RULE:  std::move on a real rvalue reference (T&&, T not deduced).\n"
      "                std::forward<T> on a FORWARDING reference (deduced T&&).\n"
      "     Using std::move on a forwarding reference will silently STEAL from an\n"
      "     lvalue the caller still owns. That is a genuinely nasty bug.\n";
}

// ---------------------------------------------------------------------------
// 3. FORWARDING REFERENCE PITFALLS
// ---------------------------------------------------------------------------
void demo_pitfalls() {
    std::cout << "\n=== 3. Forwarding reference pitfalls ===\n";
    std::cout <<
      "  (a) A FORWARDING REFERENCE CONSTRUCTOR HIJACKS THE COPY CONSTRUCTOR.\n"
      "        template <class T> Person(T&& name);   // greedier than\n"
      "        Person(const Person&);                 // this one, for Person&\n"
      "      Passing a non-const Person& picks the TEMPLATE (exact match beats\n"
      "      the const-qualified copy ctor). Fix: constrain it.\n"
      "        template <class T>\n"
      "          requires (!std::same_as<std::decay_t<T>, Person>)\n"
      "        Person(T&& name);\n"
      "\n"
      "  (b) `auto&&` IS a forwarding reference. `const auto&` is not.\n"
      "        for (auto&& x : range)   // binds to proxies (vector<bool>, views)\n"
      "\n"
      "  (c) `T&&` where T is NOT deduced is a plain rvalue reference:\n"
      "        template <class T> void f(std::vector<T>&& v);   // rvalue ref ONLY\n"
      "\n"
      "  (d) DON'T forward the same argument twice. After the first forward\n"
      "      moves from it, the second gets a gutted object.\n";
}

// ---------------------------------------------------------------------------
// 4. std::tuple AND PACK ALGORITHMS
// ---------------------------------------------------------------------------
void demo_tuple() {
    std::cout << "\n=== 4. tuple, apply, index sequences ===\n";

    std::tuple<int, std::string, double> t{42, "hello", 3.14};
    std::cout << "  get<0>=" << std::get<0>(t)
              << " get<1>=" << std::get<1>(t)
              << " get<2>=" << std::get<2>(t) << "\n";

    // Structured bindings destructure a tuple (C++17). Much nicer than get<N>.
    auto& [n, s, d] = t;
    std::cout << "  structured binding: " << n << " " << s << " " << d << "\n";

    // std::apply unpacks a tuple into a function's arguments.
    auto combine = [](int i, const std::string& str, double dd) {
        return str + ":" + std::to_string(i) + ":" + std::to_string(dd);
    };
    std::cout << "  std::apply -> " << std::apply(combine, t) << "\n";

    // Iterating a tuple: there's no runtime loop (the types differ per element),
    // so you expand a compile-time index sequence.
    auto print_tuple = [](const auto& tup) {
        std::apply([](const auto&... elems) { ((std::cout << elems << " "), ...); }, tup);
        std::cout << "\n";
    };
    std::cout << "  fold-printed tuple: "; print_tuple(t);

    std::cout <<
      "\n  std::index_sequence<0,1,2> + std::make_index_sequence<N> is the classic\n"
      "  tool for 'do something for each element of a pack, with its index'.\n"
      "  Fold expressions replaced most uses of it in C++17, thankfully.\n";
}

// ---------------------------------------------------------------------------
// 5. A REAL EXAMPLE: a zero-allocation typed logger
// ---------------------------------------------------------------------------
enum class Level { Debug, Info, Warn, Error };

template <typename... Args>
void log(Level lvl, const char* fmt_label, Args&&... args) {
    static constexpr const char* names[] = {"DEBUG","INFO","WARN","ERROR"};
    std::cout << "  [" << names[static_cast<int>(lvl)] << "] " << fmt_label << ": ";
    ((std::cout << args << " "), ...);     // fold, fully inlined, no allocation
    std::cout << "\n";
}

// A perfectly-forwarding "call and time it" wrapper — the shape of every
// middleware/interceptor you'd write in Spring, but with zero runtime cost.
template <typename F, typename... Args>
decltype(auto) timed(const char* label, F&& f, Args&&... args) {
    auto t0 = std::chrono::steady_clock::now();
    // decltype(auto) preserves references in the return type — important for
    // a transparent wrapper. `auto` alone would copy.
    if constexpr (std::is_void_v<std::invoke_result_t<F, Args...>>) {
        std::forward<F>(f)(std::forward<Args>(args)...);
        auto us = std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count();
        std::printf("  [timed] %s: %.1f us (void)\n", label, us);
    } else {
        decltype(auto) result = std::forward<F>(f)(std::forward<Args>(args)...);
        auto us = std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count();
        std::printf("  [timed] %s: %.1f us\n", label, us);
        return result;
    }
}

void demo_real_use() {
    std::cout << "\n=== 5. Real uses ===\n";
    log(Level::Info,  "request", "GET", "/api/users", 200, 1.5);
    log(Level::Error, "failed",  "code=", 500);

    auto result = timed("sum to 1e6", [](int n) {
        long long s = 0; for (int i = 0; i < n; ++i) s += i; return s;
    }, 1'000'000);
    std::cout << "  result = " << result << "\n";
}

int main() {
    demo_variadic();
    demo_forwarding();
    demo_pitfalls();
    demo_tuple();
    demo_real_use();
    return 0;
}
