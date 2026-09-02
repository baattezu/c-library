// ===========================================================================
//  TEMPLATES — code generation, not type erasure
//  run: make run F=04-templates/01-function-and-class-templates.cpp
// ===========================================================================
//
//  THE ONE THING TO UNDERSTAND:
//
//    Java generics ERASE. List<String> and List<Integer> are the SAME class at
//    runtime. One copy of the code. Casts inserted. Everything is Object.
//
//    C++ templates GENERATE. vector<int> and vector<string> are DIFFERENT types
//    with DIFFERENT machine code, produced at compile time.
//
//  Consequences:
//    ✅ zero abstraction cost: vector<int> stores raw ints, sort() inlines the
//       comparison, no boxing, no casts, no Object
//    ✅ works with primitives (Java can't do List<int>)
//    ✅ compile-time duck typing: no interface needed
//    ✅ Turing-complete compile-time computation
//    ❌ binary bloat (each instantiation is real code)
//    ❌ slow compiles
//    ❌ historically terrible error messages (concepts fix this)
//    ❌ must live in HEADERS
//
// ===========================================================================

#include <array>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// 1. FUNCTION TEMPLATES
// ---------------------------------------------------------------------------
template <typename T>            // `typename` and `class` are interchangeable here
T max_of(const T& a, const T& b) {
    return a < b ? b : a;
}

// Two type parameters + a deduced return type. `auto` return means "figure it
// out from the return statement" — essential when the result type is a
// combination of the inputs.
template <typename A, typename B>
auto add(const A& a, const B& b) -> decltype(a + b) {
    return a + b;
}

// NON-TYPE template parameter: a compile-time VALUE, not a type.
// This is how std::array<T, N> knows its size, and it has no Java equivalent
// at all.
template <typename T, std::size_t N>
constexpr std::size_t array_size(const T (&)[N]) { return N; }

// Template with a default argument.
template <typename T = int>
struct Box { T value{}; };

void demo_function_templates() {
    std::cout << "\n=== 1. Function templates ===\n";

    // The compiler DEDUCES T from the arguments and generates a separate
    // function for each distinct T you use.
    std::cout << "  max_of(3, 7)        = " << max_of(3, 7) << "        [T=int]\n";
    std::cout << "  max_of(3.5, 2.5)    = " << max_of(3.5, 2.5) << "      [T=double]\n";
    std::cout << "  max_of(\"abc\"s, \"abd\"s) = " << max_of(std::string("abc"), std::string("abd"))
              << "  [T=std::string]\n";

    // Explicit instantiation when deduction is ambiguous or you want a specific T:
    std::cout << "  max_of<double>(3, 7.5) = " << max_of<double>(3, 7.5) << "\n";

    std::cout << "  add(1, 2.5)         = " << add(1, 2.5) << "  [A=int, B=double -> double]\n";

    int arr[7]{};
    std::cout << "  array_size(arr)     = " << array_size(arr)
              << "  (a compile-time constant — no runtime work at all)\n";
    static_assert(array_size(arr) == 7);

    Box<> b1;            // T defaults to int
    Box<std::string> b2{"hello"};
    std::cout << "  Box<> default T=int: " << b1.value << ", Box<string>: " << b2.value << "\n";
}

// ---------------------------------------------------------------------------
// 2. CLASS TEMPLATES — a minimal Stack
// ---------------------------------------------------------------------------
template <typename T, typename Container = std::vector<T>>
class Stack {
    Container c_;
public:
    void push(const T& v) { c_.push_back(v); }
    void push(T&& v)      { c_.push_back(std::move(v)); }

    template <typename... Args>          // a member template
    T& emplace(Args&&... args) { return c_.emplace_back(std::forward<Args>(args)...); }

    void pop() { c_.pop_back(); }
    [[nodiscard]] T&       top()        { return c_.back(); }
    [[nodiscard]] const T& top()  const { return c_.back(); }
    [[nodiscard]] bool     empty() const { return c_.empty(); }
    [[nodiscard]] std::size_t size() const { return c_.size(); }
};

void demo_class_templates() {
    std::cout << "\n=== 2. Class templates ===\n";
    Stack<int> s;
    s.push(1); s.push(2); s.push(3);
    std::cout << "  Stack<int>: size=" << s.size() << " top=" << s.top() << "\n";

    Stack<std::string> ss;
    ss.emplace("built in place");
    std::cout << "  Stack<string>: top='" << ss.top() << "'\n";

    // CTAD (Class Template Argument Deduction, C++17): the compiler deduces the
    // template arguments from the constructor. `std::vector v{1,2,3}` works now.
    std::vector v{1, 2, 3};                  // deduced as std::vector<int>
    std::pair  p{1, std::string("x")};       // deduced as pair<int, string>
    std::cout << "  CTAD: vector v{1,2,3} -> vector<int> of size " << v.size()
              << ", pair -> (" << p.first << ", " << p.second << ")\n";
}

// ---------------------------------------------------------------------------
// 3. SPECIALISATION — different code for specific types
// ---------------------------------------------------------------------------
template <typename T>
struct TypeInfo {
    static constexpr const char* name() { return "unknown"; }
    static constexpr bool is_fast_to_copy = std::is_trivially_copyable_v<T>;
};

// FULL specialisation: an entirely different definition for one exact type.
template <> struct TypeInfo<int> {
    static constexpr const char* name() { return "int"; }
    static constexpr bool is_fast_to_copy = true;
};
template <> struct TypeInfo<std::string> {
    static constexpr const char* name() { return "std::string"; }
    static constexpr bool is_fast_to_copy = false;
};

// PARTIAL specialisation: applies to a FAMILY of types.
// (Only class templates can be partially specialised; function templates can't
//  — you overload them instead.)
template <typename T> struct TypeInfo<std::vector<T>> {
    static constexpr const char* name() { return "std::vector<...>"; }
    static constexpr bool is_fast_to_copy = false;
};
template <typename T> struct TypeInfo<T*> {
    static constexpr const char* name() { return "pointer"; }
    static constexpr bool is_fast_to_copy = true;
};

void demo_specialisation() {
    std::cout << "\n=== 3. Specialisation ===\n";
    auto show = [](const char* label, const char* n, bool fast) {
        std::printf("  %-22s -> %-18s fast_to_copy=%s\n", label, n, fast?"yes":"no");
    };
    show("TypeInfo<int>",           TypeInfo<int>::name(),                 TypeInfo<int>::is_fast_to_copy);
    show("TypeInfo<double>",        TypeInfo<double>::name(),              TypeInfo<double>::is_fast_to_copy);
    show("TypeInfo<std::string>",   TypeInfo<std::string>::name(),         TypeInfo<std::string>::is_fast_to_copy);
    show("TypeInfo<vector<int>>",   TypeInfo<std::vector<int>>::name(),    TypeInfo<std::vector<int>>::is_fast_to_copy);
    show("TypeInfo<char*>",         TypeInfo<char*>::name(),               TypeInfo<char*>::is_fast_to_copy);

    std::cout <<
      "\n  This is how the standard library optimises: std::vector checks\n"
      "  is_trivially_copyable and uses memmove instead of a per-element loop;\n"
      "  it checks is_trivially_destructible and makes clear() O(1).\n"
      "  Specialisation is how you write ONE interface with N implementations,\n"
      "  chosen at compile time, at zero cost.\n";
}

// ---------------------------------------------------------------------------
// 4. `if constexpr` — compile-time branching (C++17)
// ---------------------------------------------------------------------------
// The untaken branch is NOT COMPILED. This means each branch may contain code
// that would be invalid for the other type. Before C++17 this needed SFINAE or
// tag dispatch and was genuinely painful.
template <typename T>
std::string describe(const T& x) {
    if constexpr (std::is_integral_v<T>) {
        return "integer: " + std::to_string(x);
    } else if constexpr (std::is_floating_point_v<T>) {
        return "float: " + std::to_string(x);
    } else if constexpr (std::is_pointer_v<T>) {
        return x ? "non-null pointer" : "null pointer";
    } else if constexpr (requires { x.size(); }) {      // C++20 requires-expression
        return "container with " + std::to_string(x.size()) + " elements";
    } else {
        return "something else";
    }
}

void demo_if_constexpr() {
    std::cout << "\n=== 4. if constexpr ===\n";
    std::cout << "  describe(42)          -> " << describe(42) << "\n";
    std::cout << "  describe(3.14)        -> " << describe(3.14) << "\n";
    std::cout << "  describe(nullptr_ptr) -> " << describe((int*)nullptr) << "\n";
    std::cout << "  describe(vector)      -> " << describe(std::vector<int>{1,2,3}) << "\n";
    std::cout << "  describe(string)      -> " << describe(std::string("hi")) << "\n";
    std::cout <<
      "\n  Note `to_string(x)` appears in the integral branch — that call would\n"
      "  not compile for a std::vector. With a plain `if` this file wouldn't\n"
      "  build. `if constexpr` DISCARDS the untaken branch entirely.\n";
}

// ---------------------------------------------------------------------------
// 5. THE MECHANICS YOU MUST KNOW
// ---------------------------------------------------------------------------
void demo_mechanics() {
    std::cout << "\n=== 5. Template mechanics ===\n";
    std::cout <<
      "  (a) TWO-PHASE LOOKUP. At definition time, the compiler checks anything\n"
      "      not dependent on T. At instantiation time, it checks the rest. This\n"
      "      is why a template with an error in an unused branch can still compile.\n"
      "\n"
      "  (b) `typename` FOR DEPENDENT TYPES. Inside a template, the compiler\n"
      "      doesn't know if `T::value_type` is a type or a static member, so\n"
      "      you must say:\n"
      "          typename T::value_type v;      // it's a type\n"
      "          T::value_type * p;             // without typename: parsed as\n"
      "                                         //   MULTIPLICATION!\n"
      "\n"
      "  (c) `template` FOR DEPENDENT TEMPLATES:\n"
      "          obj.template get<int>();       // needed when obj's type depends on T\n"
      "\n"
      "  (d) TEMPLATES LIVE IN HEADERS. The compiler needs the body to generate\n"
      "      code for each T. Body in a .cpp -> 'undefined symbol' at link time.\n"
      "      Escape hatch: EXPLICIT INSTANTIATION in one .cpp:\n"
      "          template class Stack<int>;     // generate it here, once\n"
      "      plus `extern template class Stack<int>;` in the header to stop other\n"
      "      TUs re-instantiating it. This is how you cut template build times.\n"
      "\n"
      "  (e) BINARY BLOAT is real. vector<int>, vector<long>, vector<size_t> are\n"
      "      three complete copies of the code even though they're all 'a vector\n"
      "      of an 8-byte thing'. Mitigation: put type-independent logic in a\n"
      "      non-template base (e.g. operate on void* + size), and keep the\n"
      "      template layer thin. This is what real libraries do.\n"
      "\n"
      "  (f) COMPILE TIMES. Templates are the main reason C++ builds are slow.\n"
      "      Measure with `clang++ -ftime-trace` and view the JSON in\n"
      "      chrome://tracing or https://ui.perfetto.dev.\n";
}

int main() {
    demo_function_templates();
    demo_class_templates();
    demo_specialisation();
    demo_if_constexpr();
    demo_mechanics();
    return 0;
}
