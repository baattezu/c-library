// ===========================================================================
//  CONCEPTS (C++20) — the feature that makes templates usable
//  run: make run F=04-templates/03-concepts.cpp
// ===========================================================================
//
//  For 20 years, C++ templates had no way to say "T must support X". You wrote
//  the template, and if T didn't fit, you got 300 lines of instantiation
//  backtrace pointing into <algorithm>.
//
//  Concepts fix this. A concept is a named, compile-time-checked predicate on
//  types. Closest Java analogue: `<T extends Comparable<T>>` — except a concept
//  constrains SYNTAX (what you can DO with T), not a base class. So unrelated
//  types satisfy it without knowing it exists. That is far more powerful.
//
// ===========================================================================

#include <concepts>
#include <cstdio>
#include <iostream>
#include <list>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// 1. DEFINING AND USING A CONCEPT
// ---------------------------------------------------------------------------

// A concept is just a compile-time bool expression over types.
template <typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

// A `requires` expression lists things that must COMPILE. It never runs.
template <typename T>
concept Drawable = requires(const T& t) {
    t.draw();                                  // must have a draw() member
    { t.area() } -> std::convertible_to<double>;   // returning something double-ish
    typename T::unit_type;                     // must have a nested type
};

// A concept with more requirements, showing all four kinds of requirement:
template <typename T>
concept Container = requires(T c, const T cc) {
    typename T::value_type;                    // TYPE requirement
    typename T::iterator;
    c.begin();                                 // SIMPLE requirement (must compile)
    c.end();
    { cc.size() } -> std::convertible_to<std::size_t>;   // COMPOUND (type-checked)
    { cc.empty() } -> std::same_as<bool>;
    requires !std::is_void_v<typename T::value_type>;    // NESTED (a bool expr)
};

// Four equivalent ways to constrain a template — pick whichever reads best:
template <Numeric T>                                   // (1) constrained parameter
T square(T x) { return x * x; }

template <typename T> requires Numeric<T>              // (2) requires clause
T cube(T x) { return x * x * x; }

template <typename T>
T quad(T x) requires Numeric<T> { return x*x*x*x; }    // (3) trailing requires

auto negate_it(Numeric auto x) { return -x; }          // (4) abbreviated (terse)

void demo_basics() {
    std::cout << "\n=== 1. Constraining templates ===\n";
    std::cout << "  square(5)=" << square(5) << " cube(2.0)=" << cube(2.0)
              << " quad(2)=" << quad(2) << " negate(3)=" << negate_it(3) << "\n";

    // square(std::string{"x"});
    //   Without concepts: 40 lines of "invalid operands to binary expression"
    //   With concepts:    "constraints not satisfied ... 'Numeric<std::string>'
    //                      evaluated to false"
    //   That difference is the entire reason concepts exist.

    // Concepts are usable as compile-time booleans anywhere:
    static_assert(Numeric<int>);
    static_assert(Numeric<double>);
    static_assert(!Numeric<std::string>);
    static_assert(Container<std::vector<int>>);
    static_assert(Container<std::string>);
    static_assert(!Container<int>);
    std::cout << "  Container<vector<int>> = " << std::boolalpha << Container<std::vector<int>>
              << ", Container<int> = " << Container<int> << "\n";
}

// ---------------------------------------------------------------------------
// 2. THE STANDARD CONCEPTS (know these — you'll use them constantly)
// ---------------------------------------------------------------------------
void demo_standard_concepts() {
    std::cout << "\n=== 2. Standard library concepts ===\n";

    auto show = [](const char* name, bool v) { std::printf("  %-42s %s\n", name, v?"yes":"no"); };

    // CORE LANGUAGE (<concepts>)
    show("same_as<int,int>",                    std::same_as<int,int>);
    show("derived_from<vector<int>, ...>",      std::derived_from<std::true_type, std::true_type>);
    show("convertible_to<int,double>",          std::convertible_to<int,double>);
    show("integral<int>",                       std::integral<int>);
    show("signed_integral<unsigned>",           std::signed_integral<unsigned>);
    show("floating_point<double>",              std::floating_point<double>);

    // OBJECT / LIFETIME
    show("destructible<std::string>",           std::destructible<std::string>);
    show("default_initializable<std::string>",  std::default_initializable<std::string>);
    show("copyable<std::unique_ptr<int>>",      std::copyable<std::unique_ptr<int>>);
    show("movable<std::unique_ptr<int>>",       std::movable<std::unique_ptr<int>>);
    show("regular<int>  (semiregular+equality)",std::regular<int>);

    // COMPARISON
    show("equality_comparable<std::string>",    std::equality_comparable<std::string>);
    show("totally_ordered<double>",             std::totally_ordered<double>);

    // CALLABLE
    show("invocable<decltype([]{}) >",          std::invocable<decltype([]{})>);

    // ITERATORS / RANGES (<iterator>, <ranges>)
    show("ranges::range<std::vector<int>>",             std::ranges::range<std::vector<int>>);
    show("ranges::random_access_range<vector<int>>",    std::ranges::random_access_range<std::vector<int>>);
    show("ranges::random_access_range<list<int>>",      std::ranges::random_access_range<std::list<int>>);
    show("ranges::contiguous_range<std::vector<int>>",  std::ranges::contiguous_range<std::vector<int>>);
    show("ranges::contiguous_range<std::list<int>>",    std::ranges::contiguous_range<std::list<int>>);
}

// ---------------------------------------------------------------------------
// 3. OVERLOAD RESOLUTION BY CONSTRAINT ("subsumption")
// ---------------------------------------------------------------------------
// The compiler picks the MOST CONSTRAINED overload that matches. This replaces
// the old SFINAE / tag-dispatch machinery with something readable.
template <std::ranges::range R>
void process(const R&) { std::cout << "    generic range\n"; }

template <std::ranges::random_access_range R>
void process(const R&) { std::cout << "    random-access range (can binary search / sort)\n"; }

template <std::ranges::contiguous_range R>
void process(const R&) { std::cout << "    CONTIGUOUS range (can memcpy / SIMD / pass to a C API)\n"; }

void demo_subsumption() {
    std::cout << "\n=== 3. Picking the best overload by constraint ===\n";
    std::cout << "  std::list<int>   -> "; process(std::list<int>{1,2,3});
    std::cout << "  std::vector<int> -> "; process(std::vector<int>{1,2,3});
    std::cout <<
      "\n  This is EXACTLY how the standard library specialises algorithms:\n"
      "  std::distance is O(1) for random-access and O(n) for forward iterators;\n"
      "  std::copy uses memmove for contiguous trivially-copyable ranges and a\n"
      "  loop otherwise. Same source, different code, chosen at compile time.\n"
      "\n"
      "  Before C++20 this was done with SFINAE / std::enable_if, which worked\n"
      "  but was unreadable. If you meet enable_if in old code, this is what it\n"
      "  was trying to say.\n";
}

// ---------------------------------------------------------------------------
// 4. A PRACTICAL CONCEPT-DRIVEN API
// ---------------------------------------------------------------------------
template <typename T>
concept Serializable = requires(const T& t, std::string& out) {
    { t.serialize() } -> std::convertible_to<std::string>;
    { T::type_name() } -> std::convertible_to<const char*>;
};

struct User {
    int id; std::string name;
    std::string serialize() const { return "{\"id\":" + std::to_string(id) + ",\"name\":\"" + name + "\"}"; }
    static const char* type_name() { return "User"; }
};
struct NotSerializable { int x; };

// The constraint documents the requirement AND enforces it AND produces a good
// error. Three jobs, one line. This is your `interface` — without inheritance,
// without a vtable, without the type having to know your concept exists.
template <Serializable T>
void write_record(const T& t) {
    std::cout << "    " << T::type_name() << " => " << t.serialize() << "\n";
}

void demo_practical() {
    std::cout << "\n=== 4. Concepts as interfaces (no inheritance needed) ===\n";
    write_record(User{1, "alice"});
    std::cout << "  Serializable<User>            = " << std::boolalpha << Serializable<User> << "\n";
    std::cout << "  Serializable<NotSerializable> = " << Serializable<NotSerializable> << "\n";
    // write_record(NotSerializable{1});   // clean, one-line error

    std::cout <<
      "\n  ⭐ NOTE WHAT JUST HAPPENED: `User` satisfies `Serializable` WITHOUT\n"
      "     inheriting from anything and without ever mentioning the concept.\n"
      "     You can retrofit a concept onto a third-party type you don't own.\n"
      "     Java interfaces cannot do this (that's what adapters are for).\n"
      "     And there is no vtable, no allocation, and full inlining.\n";
}

// ---------------------------------------------------------------------------
// 5. requires-EXPRESSION AS A LOCAL CHECK
// ---------------------------------------------------------------------------
template <typename T>
void smart_print(const T& x) {
    // An ad-hoc requires-expression inside `if constexpr` — no named concept
    // needed. Extremely useful for writing one generic function that adapts.
    if constexpr (requires { std::cout << x; }) {
        std::cout << "    streamable: " << x << "\n";
    } else if constexpr (requires { x.size(); x.begin(); }) {
        std::cout << "    container of " << x.size() << ": ";
        for (const auto& e : x) std::cout << e << " ";
        std::cout << "\n";
    } else {
        std::cout << "    (no idea how to print this)\n";
    }
}

void demo_requires_expression() {
    std::cout << "\n=== 5. Ad-hoc requires-expressions ===\n";
    smart_print(42);
    smart_print(std::string("hello"));
    smart_print(std::vector<int>{1,2,3});
    smart_print(std::list<double>{1.5, 2.5});
}

int main() {
    demo_basics();
    demo_standard_concepts();
    demo_subsumption();
    demo_practical();
    demo_requires_expression();

    std::cout <<
      "\n>>> PRACTICAL ADVICE\n"
      "    - Constrain every template you write. It costs one line and turns\n"
      "      unreadable errors into readable ones.\n"
      "    - Prefer the standard concepts (std::integral, std::ranges::range,\n"
      "      std::invocable) over hand-rolled ones.\n"
      "    - A concept describes CAPABILITY, not identity. Name it for what a\n"
      "      type can DO (Drawable, Hashable, Serializable).\n"
      "    - `requires requires` is legal and means 'a requires-clause containing\n"
      "      a requires-expression'. It looks like a typo. It isn't.\n";
    return 0;
}
