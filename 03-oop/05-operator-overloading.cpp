// ===========================================================================
//  OPERATOR OVERLOADING — making your types feel built-in
//  run: make run F=03-oop/05-operator-overloading.cpp
// ===========================================================================
//
//  Java deliberately has no operator overloading (only `+` on String, hardcoded).
//  C++ lets you overload almost everything, and this is how std::string, the
//  smart pointers, iterators, std::complex, and every matrix library work.
//
//  Used well: your types are indistinguishable from built-ins, and generic
//  algorithms work on them for free.
//  Used badly: `<<` means "shift" AND "output" AND, in someone's DSL, "append
//  to a database". Discipline required.
//
// ===========================================================================

#include <algorithm>
#include <cctype>
#include <cmath>
#include <compare>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// 1. A VALUE TYPE THAT BEHAVES LIKE A NUMBER
// ---------------------------------------------------------------------------
class Vec2 {
    double x_{}, y_{};
public:
    constexpr Vec2() = default;
    constexpr Vec2(double x, double y) : x_(x), y_(y) {}

    constexpr double x() const noexcept { return x_; }
    constexpr double y() const noexcept { return y_; }

    // ---- COMPOUND ASSIGNMENT: implement THESE as members ----
    // They modify *this, so they belong to the class. Return *this by reference
    // so `a += b += c` works, matching built-in semantics.
    constexpr Vec2& operator+=(const Vec2& o) { x_ += o.x_; y_ += o.y_; return *this; }
    constexpr Vec2& operator-=(const Vec2& o) { x_ -= o.x_; y_ -= o.y_; return *this; }
    constexpr Vec2& operator*=(double s)      { x_ *= s;    y_ *= s;    return *this; }

    // ---- UNARY ----
    constexpr Vec2 operator-() const { return {-x_, -y_}; }

    // ---- COMPARISON: one line gives you ==, !=, <, <=, >, >= ----
    // (Lexicographic on members, in declaration order. For a Vec2 that ordering
    //  is arbitrary but useful for putting them in a std::map.)
    constexpr auto operator<=>(const Vec2&) const = default;
    constexpr bool operator==(const Vec2&) const = default;

    // ---- SUBSCRIPT: provide BOTH const and non-const overloads ----
    constexpr double& operator[](int i)       { return i == 0 ? x_ : y_; }
    constexpr double  operator[](int i) const { return i == 0 ? x_ : y_; }

    constexpr double length() const { return std::sqrt(x_*x_ + y_*y_); }
};

// ---- BINARY ARITHMETIC: implement as FREE functions ----
// WHY free, not member? Because a member's LEFT operand must be your type, so
// `2.0 * v` would not compile if operator* were a member. Free functions allow
// implicit conversion on BOTH sides and keep the class interface smaller.
//
// The idiomatic implementation: take the left operand BY VALUE (that copy is
// the result), use the compound assignment, and return it.
constexpr Vec2 operator+(Vec2 a, const Vec2& b) { a += b; return a; }
constexpr Vec2 operator-(Vec2 a, const Vec2& b) { a -= b; return a; }
constexpr Vec2 operator*(Vec2 v, double s)      { v *= s; return v; }
constexpr Vec2 operator*(double s, Vec2 v)      { v *= s; return v; }   // symmetric!

// ---- STREAM OUTPUT: must be free (left operand is std::ostream) ----
std::ostream& operator<<(std::ostream& os, const Vec2& v) {
    return os << '(' << v.x() << ", " << v.y() << ')';
}

void demo_value_type() {
    std::cout << "\n=== 1. A number-like value type ===\n";
    constexpr Vec2 a{3, 4}, b{1, 2};

    std::cout << "  a=" << a << " b=" << b << "\n";
    std::cout << "  a+b=" << (a+b) << "  a-b=" << (a-b) << "\n";
    std::cout << "  a*2=" << (a*2) << "  2*a=" << (2*a) << "  <- symmetric, thanks to free functions\n";
    std::cout << "  -a=" << (-a) << "  a[0]=" << a[0] << "  a.length()=" << a.length() << "\n";
    std::cout << "  a==b? " << std::boolalpha << (a==b) << "  a<b? " << (a<b)
              << "  (all six from one operator<=>)\n";

    // Everything above is constexpr, so this happens at COMPILE TIME:
    static_assert((Vec2{1,2} + Vec2{3,4}) == Vec2{4,6});
    std::cout << "  static_assert passed — the arithmetic ran at compile time\n";
}

// ---------------------------------------------------------------------------
// 2. THE SPACESHIP OPERATOR IN DETAIL
// ---------------------------------------------------------------------------
void demo_spaceship() {
    std::cout << "\n=== 2. operator<=> (C++20) ===\n";

    // The three comparison categories, from strongest to weakest:
    //   std::strong_ordering  — a==b implies they are interchangeable (ints)
    //   std::weak_ordering    — a==b means "equivalent" but distinguishable
    //                           (case-insensitive strings)
    //   std::partial_ordering — some pairs are unordered (floats, because NaN)
    auto r1 = 1 <=> 2;
    std::cout << "  (1 <=> 2) < 0 : " << ((1 <=> 2) < 0) << "\n";
    std::cout << "  (2 <=> 2) == 0: " << ((2 <=> 2) == 0) << "\n";
    (void)r1;

    // A custom ordering: sort by priority, then by name.
    struct Task {
        int         priority;
        std::string name;
        // Defaulted <=> compares members in DECLARATION ORDER, lexicographically.
        auto operator<=>(const Task&) const = default;
        bool operator==(const Task&) const = default;
    };
    std::vector<Task> tasks{{2,"beta"}, {1,"zulu"}, {1,"alpha"}, {2,"alpha"}};
    std::sort(tasks.begin(), tasks.end());
    std::cout << "  sorted: ";
    for (const auto& t : tasks) std::cout << t.priority << ":" << t.name << " ";
    std::cout << "\n";

    // Hand-written <=> when you need custom logic:
    struct CaseInsensitive {
        std::string s;
        std::weak_ordering operator<=>(const CaseInsensitive& o) const {
            auto lower = [](const std::string& x) {
                std::string r = x;
                for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
                return r;
            };
            return lower(s) <=> lower(o.s);
        }
        bool operator==(const CaseInsensitive& o) const { return (*this <=> o) == 0; }
    };
    std::cout << "  \"Hello\" == \"HELLO\" (case-insensitive)? "
              << (CaseInsensitive{"Hello"} == CaseInsensitive{"HELLO"}) << "\n";

    std::cout <<
      "\n  ⚠️ `operator==` is NOT generated from a defaulted `<=>` for the\n"
      "     rewritten forms in all cases — declare `bool operator==(...) const\n"
      "     = default;` alongside it. (`!=` IS generated from `==`.)\n"
      "     Compare to Java: equals + hashCode + compareTo + Comparator, all\n"
      "     hand-written, all easy to make inconsistent.\n";
}

// ---------------------------------------------------------------------------
// 3. THE SPECIAL OPERATORS
// ---------------------------------------------------------------------------
// A minimal smart pointer, to show the two operators that make it feel native.
template <class T>
class Ptr {
    T* p_;
public:
    explicit Ptr(T* p) : p_(p) {}
    ~Ptr() { delete p_; }
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;

    T& operator*()  const { return *p_; }
    T* operator->() const { return p_; }    // note: returns a POINTER; the
                                            // compiler chains -> until it gets one
    explicit operator bool() const { return p_ != nullptr; }
    //       ^^^^^^^^ EXPLICIT conversion. Without `explicit`, your pointer would
    //       implicitly convert to bool then to int, and `p1 + p2` would compile.
    //       "Contextual conversion" still lets `if (p)` work. Always explicit.
};

struct Widget { int id = 7; void hello() const { std::cout << "  widget " << id << "\n"; } };

void demo_special_operators() {
    std::cout << "\n=== 3. The operators that make library types work ===\n";

    // (a) operator() — the CALL operator. A type with one is a "functor".
    //     Every lambda is a compiler-generated class with an operator().
    //     Unlike a function pointer, a functor can be INLINED, because its type
    //     is known at compile time.
    struct Multiplier {
        double factor;
        double operator()(double x) const { return x * factor; }
    };
    Multiplier times3{3.0};
    std::cout << "  functor: times3(7) = " << times3(7) << "\n";

    // (b) operator* and operator-> — make a type act like a pointer.
    //     This is exactly how unique_ptr, shared_ptr, optional, and iterators work.
    Ptr<Widget> p(new Widget);
    p->hello();                                        // operator->
    std::cout << "  (*p).id = " << (*p).id << "\n";    // operator*
    if (p) std::cout << "  explicit operator bool works in `if`\n";
    // int x = p;     // correctly REJECTED because the conversion is explicit
}

// ---------------------------------------------------------------------------
// 4. MAKING YOUR TYPE WORK WITH THE STANDARD LIBRARY
// ---------------------------------------------------------------------------
struct UserId {
    std::uint64_t value;
    auto operator<=>(const UserId&) const = default;
    bool operator==(const UserId&) const = default;
};

// std::hash specialisation = Java's hashCode(). Now UserId works as a key in
// unordered_map/unordered_set. Note you must specialise in namespace std, which
// is one of the few times that's legal.
template <>
struct std::hash<UserId> {
    std::size_t operator()(const UserId& u) const noexcept {
        // Don't just return the value — bad hashes cause clustering. Mix it.
        // This is splitmix64's finaliser, a good cheap 64-bit mixer.
        std::uint64_t x = u.value;
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27; x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x);
    }
};

void demo_stl_integration() {
    std::cout << "\n=== 4. Integrating with the standard library ===\n";
    std::unordered_map<UserId, std::string> users;
    users[UserId{1}] = "alice";
    users[UserId{2}] = "bob";
    std::cout << "  unordered_map<UserId, string> works: " << users[UserId{1}] << "\n";
    std::cout <<
      "  You needed: operator== (for bucket comparison) + std::hash (for the\n"
      "  bucket index). That is Java's equals()/hashCode() contract, spelled\n"
      "  differently. The same rule applies: EQUAL OBJECTS MUST HASH EQUAL.\n";
}

// ---------------------------------------------------------------------------
void the_rules() {
    std::cout << "\n=== 5. Rules ===\n";
    std::cout <<
      "  WHICH FORM?\n"
      "   member  : = [] () -> ->* and all COMPOUND assignments (+=, -=, ...)\n"
      "             (the standard requires = [] () -> to be members)\n"
      "   free    : binary arithmetic (+ - * /), comparison, <<, >>\n"
      "             -> allows conversions on the LEFT operand too\n"
      "   friend  : a free function needing private access; define it INSIDE the\n"
      "             class body (a 'hidden friend') — better for ADL and compiles faster\n"
      "\n"
      "  CANNOT BE OVERLOADED:  ::   .   .*   ?:   sizeof   alignof   typeid\n"
      "\n"
      "  RULES OF TASTE\n"
      "   1. Preserve the built-in MEANING. `+` must be addition-like and must\n"
      "      not modify its operands. `==` must be reflexive, symmetric,\n"
      "      transitive. Surprising operators are worse than named functions.\n"
      "   2. Implement `a + b` in terms of `a += b`, not the other way around.\n"
      "   3. `explicit operator bool()`, never an implicit conversion operator.\n"
      "      Implicit conversions cause ambiguities and accidental arithmetic.\n"
      "   4. Provide const and non-const `operator[]`.\n"
      "   5. If you write `<`, write `<=>`; you get all six for free.\n"
      "   6. If you write `==`, specialise `std::hash` too.\n"
      "   7. Don't overload `&&`, `||`, or `,` — you silently lose short-circuit\n"
      "      evaluation and sequencing.\n"
      "   8. Don't build DSLs out of operators unless the domain has real\n"
      "      mathematical notation (matrices, units, complex numbers).\n";
}

int main() {
    demo_value_type();
    demo_spaceship();
    demo_special_operators();
    demo_stl_integration();
    the_rules();
    return 0;
}
