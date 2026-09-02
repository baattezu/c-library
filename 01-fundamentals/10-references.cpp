// ===========================================================================
//  REFERENCES — lvalue refs, rvalue refs, and the value-category system
//  run: make run F=01-fundamentals/10-references.cpp
// ===========================================================================
//
//  ⚠️ WARNING FOR JAVA DEVELOPERS: the word "reference" means something COMPLETELY
//  DIFFERENT here. A Java "reference" is a nullable, reseatable handle to a heap
//  object — that is a C++ *pointer*.
//
//  A C++ reference is an ALIAS: another name for an existing object.
//    - It can never be null.
//    - It can never be reseated to refer to something else.
//    - It has no independent identity; &ref gives you the address of the referent.
//    - It usually compiles to a pointer under the hood, but semantically it IS
//      the object.
//
//  Then C++11 added a second kind of reference — the RVALUE reference (T&&) —
//  and with it, move semantics. This is the most important C++11 feature and the
//  thing that makes modern C++ fast without a GC. This file builds the mental
//  model; 02-memory/05-copy-move-semantics.cpp does the deep dive.
//
// ===========================================================================

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

void demo_lvalue_refs() {
    std::cout << "\n=== 1. Lvalue references (T&) = aliases ===\n";

    int x = 10;
    int& r = x;        // r IS x. Not a copy, not a pointer you must deref.

    r = 20;
    std::cout << "  x=" << x << " r=" << r << "  (same object)\n";
    std::cout << "  &x=" << static_cast<void*>(&x)
              << " &r=" << static_cast<void*>(&r) << "  (same address)\n";

    // A reference MUST be initialised, and can never be rebound:
    // int& bad;        // ERROR: declaration of reference variable requires init
    int y = 99;
    r = y;             // this ASSIGNS y's value into x. It does NOT rebind r.
    std::cout << "  after `r = y;`  x=" << x << " y=" << y
              << "  <- r still aliases x\n";

    // const T& binds to anything, including TEMPORARIES, and extends their
    // lifetime to the lifetime of the reference. This is a special rule and it
    // is why `const std::string&` parameters accept string literals.
    const std::string& tmp = std::string("temporary");
    std::cout << "  const ref to temporary: " << tmp
              << "  (lifetime extended to this scope)\n";
}

// The three roles of references in an interface:
void observe(const std::string& s) { std::cout << "  observe: " << s << "\n"; }
void mutate (std::string& s)       { s += " [mutated]"; }
void consume(std::string&& s)      { std::string mine = std::move(s);
                                     std::cout << "  consume: took ownership of '"
                                               << mine << "'\n"; }

void demo_reference_parameters() {
    std::cout << "\n=== 2. References as parameters ===\n";
    std::string s = "payload";
    observe(s);
    mutate(s);   std::cout << "  after mutate: " << s << "\n";
    observe("a literal — binds to const& via a temporary");
    consume(std::move(s));
    std::cout << "  after consume, s = '" << s << "' (moved-from: valid, unspecified)\n";
}

// ---------------------------------------------------------------------------
// 3. VALUE CATEGORIES — the grammar underneath all of this
// ---------------------------------------------------------------------------
//
//  Every EXPRESSION in C++ has a type AND a value category. There are three
//  you need (the standard has five, arranged in a hierarchy):
//
//      lvalue  — has identity, cannot be moved from.
//                "It has a name / an address you could take."
//                Examples: x, obj.field, arr[i], *p, a function returning T&
//
//      prvalue — no identity, can be moved from. ("pure rvalue")
//                A temporary being born.
//                Examples: 42, x + y, std::string("hi"), a function returning T
//
//      xvalue  — has identity, CAN be moved from. ("eXpiring value")
//                An lvalue you have explicitly given permission to gut.
//                Examples: std::move(x), a function returning T&&
//
//  Combined:  glvalue = lvalue | xvalue   ("has identity")
//             rvalue  = prvalue | xvalue  ("can be moved from")
//
//  THE ONLY RULE YOU NEED:
//      T&        binds to lvalues only
//      const T&  binds to EVERYTHING (that's why it's the safe default)
//      T&&       binds to rvalues only  (prvalues and xvalues)
//
//  And std::move is NOT a function that moves anything. It is a CAST:
//      static_cast<T&&>(x)
//  It just changes the value category so overload resolution picks the
//  move-taking overload. It generates zero instructions.
// ---------------------------------------------------------------------------

void which(int&)        { std::cout << "  -> int&        (lvalue)\n"; }
void which(const int&)  { std::cout << "  -> const int&  (const lvalue)\n"; }
void which(int&&)       { std::cout << "  -> int&&       (rvalue)\n"; }

void demo_value_categories() {
    std::cout << "\n=== 3. Value categories in action ===\n";

    int x = 1;
    const int cx = 2;

    std::cout << "which(x):";            which(x);              // lvalue
    std::cout << "which(cx):";           which(cx);             // const lvalue
    std::cout << "which(3):";            which(3);              // prvalue
    std::cout << "which(x + 1):";        which(x + 1);          // prvalue
    std::cout << "which(std::move(x)):"; which(std::move(x));   // xvalue

    // ⚠️ THE MOST CONFUSING RULE IN C++, STATED PLAINLY:
    //     Inside `void f(T&& param)`, the expression `param` is an LVALUE.
    // It has a name, therefore it is an lvalue, even though its TYPE is
    // rvalue-reference. That's why you must write std::move(param) again to
    // pass the moveability along. "Named rvalue references are lvalues."
    struct Demo {
        static void take(std::string&& s) {
            // `s` here is an lvalue of type std::string&&.
            std::cout << "  inside take(): `s` is an LVALUE, so this would COPY:\n";
            std::string copy = s;                  // COPY (s is an lvalue!)
            std::string moved = std::move(s);      // MOVE (re-cast to rvalue)
            std::cout << "    copy='" << copy << "' moved='" << moved
                      << "' s='" << s << "'\n";
        }
    };
    Demo::take(std::string("hello world, a longish string to defeat SSO"));
}

// ---------------------------------------------------------------------------
// 4. WHY MOVE SEMANTICS EXIST — the demonstration
// ---------------------------------------------------------------------------
struct Buffer {
    std::size_t n = 0;
    char* data = nullptr;

    explicit Buffer(std::size_t size) : n(size), data(new char[size]) {
        std::cout << "    ALLOC " << n << " bytes\n";
    }
    ~Buffer() { delete[] data; if (data) std::cout << "    FREE  " << n << " bytes\n"; }

    // COPY constructor: must duplicate the buffer. O(n), one allocation.
    Buffer(const Buffer& o) : n(o.n), data(new char[o.n]) {
        std::copy(o.data, o.data + o.n, data);
        std::cout << "    COPY  " << n << " bytes (expensive!)\n";
    }

    // MOVE constructor: steal the pointer, null out the source. O(1), zero allocs.
    // `noexcept` matters: std::vector will only use your move constructor during
    // reallocation if it is noexcept (otherwise it can't offer the strong
    // exception guarantee, so it copies instead). ALWAYS mark moves noexcept.
    Buffer(Buffer&& o) noexcept : n(o.n), data(o.data) {
        o.data = nullptr; o.n = 0;
        std::cout << "    MOVE  (pointer stolen, O(1), no allocation)\n";
    }

    Buffer& operator=(const Buffer&) = delete;
    Buffer& operator=(Buffer&&) = delete;
};

void demo_why_move_matters() {
    std::cout << "\n=== 4. Copy vs move: the actual reason C++11 happened ===\n";

    Buffer a(1024);
    std::cout << "  Buffer b = a;            ->";
    Buffer b = a;                          // COPY: allocate + memcpy 1024 bytes
    std::cout << "  Buffer c = std::move(a); ->";
    Buffer c = std::move(a);               // MOVE: copy 16 bytes, done
    std::cout << "  (a is now empty: n=" << a.n << ", data=" << (void*)a.data << ")\n";
    (void)b; (void)c;
    std::cout << "  destructors:\n";
}

// ---------------------------------------------------------------------------
// 5. REFERENCE PITFALLS
// ---------------------------------------------------------------------------
void demo_pitfalls() {
    std::cout << "\n=== 5. Reference pitfalls ===\n";

    // (a) DANGLING REFERENCE — a reference outliving its referent.
    //     Same bug as a dangling pointer, but references *look* safe, so people
    //     forget. This is the #1 modern-C++ memory bug.
    //
    //     const std::string& bad = get_temporary_string().substr(0,3);  // dangles!
    //     Lifetime extension applies to the FULL temporary bound directly, not to
    //     a subobject or the result of a member call on a temporary.

    // (b) REFERENCE TO A VECTOR ELEMENT + push_back = dangling.
    std::vector<int> v{1,2,3};
    int& first = v[0];
    std::cout << "  first=" << first << " cap=" << v.capacity() << "\n";
    v.push_back(4); v.push_back(5);
    std::cout << "  after growth cap=" << v.capacity()
              << " -> `first` now dangles. (see 02-memory/09)\n";
    (void)first;

    // (c) `auto` DROPS references and const. This silently copies!
    std::vector<std::string> names{"alpha","beta"};
    auto  copy = names[0];        // std::string — a COPY
    auto& ref  = names[0];        // std::string& — an alias
    copy += "-modified";
    std::cout << "  names[0]=" << names[0] << " copy=" << copy << " ref=" << ref << "\n";
    std::cout << "  -> `auto x = container[i]` COPIES. Write `auto&` or `const auto&`.\n";

    // (d) You cannot have an array of references, a pointer to a reference, or
    //     a reference to a reference (reference collapsing handles the last one
    //     inside templates: T& &  -> T&,  T&& && -> T&&,  everything else -> T&).

    // (e) std::reference_wrapper<T> is how you store references in containers:
    //     std::vector<std::reference_wrapper<Foo>>. It's a rebindable, copyable
    //     reference — essentially a pointer with reference syntax.
}

int main() {
    demo_lvalue_refs();
    demo_reference_parameters();
    demo_value_categories();
    demo_why_move_matters();
    demo_pitfalls();

    std::cout << "\n>>> THE TAKEAWAY: `const T&` for reading, `T&` for mutating,\n"
                 "    `T&&` + std::move for transferring ownership. Master these\n"
                 "    three and you have mastered C++ function interfaces.\n";
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * A C++ reference is an ALIAS: never null, never reseated. A Java
//    "reference" is a C++ POINTER.
//  * Binding rules:  T& <- lvalues.  const T& <- everything.  T&& <- rvalues.
//  * Value categories: lvalue (has identity), prvalue (temporary being born),
//    xvalue (identity + moveable). glvalue = has identity; rvalue = moveable.
//  * std::move is a CAST to T&&. It moves nothing and costs nothing.
//  * A NAMED rvalue reference is an LVALUE. You must std::move it again to
//    forward its moveability.
//  * Always mark move constructors/assignments `noexcept`, or std::vector will
//    copy instead of move when it reallocates.
//  * const& extends the lifetime of a directly-bound temporary — but not of a
//    subobject or a member-call result on a temporary.
//  * `auto x = ...` drops const and & — it copies. Use `const auto&`.
// ===========================================================================
