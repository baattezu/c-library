// ===========================================================================
//  FUNCTIONS — parameter passing, overloading, lambdas, and the ABI
//  run: make run F=01-fundamentals/06-functions.cpp
// ===========================================================================
//
//  In Java there is exactly ONE way to pass an argument: by value, where the
//  value of an object variable is a reference. Always. No choice, no cost model
//  to reason about.
//
//  In C++ you choose, and the choice determines whether you copy 8 bytes or
//  8 megabytes. Getting this right is most of "writing fast C++".
//
// ===========================================================================

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. THE PARAMETER-PASSING DECISION TABLE  (memorise this)
// ---------------------------------------------------------------------------
//
//   Want to...                          | Write                | Cost
//   ------------------------------------|----------------------|------------------
//   read a small type (int,double,ptr)  | T                    | free (in a register)
//   read a big type, no copy needed      | const T&             | free (pass address)
//   modify caller's object               | T&                   | free
//   TAKE OWNERSHIP (sink)                | T   (then std::move) | one move
//   TAKE OWNERSHIP, avoid one move       | T&&                  | zero
//   optional / maybe-null                | const T* or optional | free
//   generic, forward exactly as received | T&&  in a template   | zero ("forwarding ref")
//
//   Rule of thumb: "small" means <= 2 pointers (16 bytes) AND trivially copyable.
//   int, double, std::string_view, std::span: pass by value.
//   std::string, std::vector, any class with a destructor: const& or sink.
// ---------------------------------------------------------------------------

void by_value(std::string s)        { s += "!"; }         // full COPY (heap alloc!)
void by_const_ref(const std::string& s) { std::cout << "  read: " << s << "\n"; }
void by_ref(std::string& s)         { s += " (modified)"; }
void sink(std::string s)            { std::cout << "  owned now: " << s << "\n"; }

void demo_parameter_passing() {
    std::cout << "\n=== 1. Parameter passing ===\n";

    std::string s = "a fairly long string that will not fit in SSO storage";

    by_value(s);       std::cout << "  after by_value:     " << s.substr(0, 12) << "...\n";
    by_const_ref(s.substr(0, 12));
    by_ref(s);         std::cout << "  after by_ref:       ..." << s.substr(s.size()-12) << "\n";

    // Sink parameter: take by value, then MOVE into place. Caller decides:
    //   sink(s)             -> one copy  (caller keeps s)
    //   sink(std::move(s))  -> one move  (caller gives s away; s is now empty)
    sink(std::move(s));
    std::cout << "  after move, s.size()=" << s.size()
              << "  (moved-from = valid but unspecified; for libc++ string, empty)\n";
}

// ---------------------------------------------------------------------------
// 2. RETURN VALUES — do NOT return by reference to a local
// ---------------------------------------------------------------------------
std::vector<int> make_vector() {
    std::vector<int> v{1,2,3,4,5};
    return v;   // NOT a copy. See NRVO note below.
}

// int& broken() { int x = 5; return x; }   // DANGLING. Compiler warns; still UB.

void demo_returns() {
    std::cout << "\n=== 2. Returning by value is FREE (copy elision) ===\n";

    auto v = make_vector();
    std::cout << "  got " << v.size() << " elements, no copy was made\n";

    // WHY IT'S FREE:
    //  - RVO (Return Value Optimisation) / C++17 guaranteed copy elision: when
    //    you `return SomeType{...}`, the object is CONSTRUCTED DIRECTLY in the
    //    caller's storage. There is no temporary and no copy — this is mandated
    //    by the standard, not an optimisation.
    //  - NRVO (Named RVO): returning a named local like `v` above is elided by
    //    every real compiler, though it's technically optional.
    //  - Even without elision, the fallback is a MOVE, not a copy.
    //
    // => "Return big objects by value" is correct, idiomatic, modern C++.
    //    Do NOT write `void fill(std::vector<int>& out)` for performance reasons;
    //    that was 2005 advice and it is obsolete.
    //
    // ⚠️ Do NOT write `return std::move(v);` — it DEFEATS NRVO and makes it slower.
}

// ---------------------------------------------------------------------------
// 3. OVERLOADING AND DEFAULT ARGUMENTS
// ---------------------------------------------------------------------------
void log(const std::string& msg)              { std::cout << "  [info] " << msg << "\n"; }
void log(const std::string& msg, int level)   { std::cout << "  [" << level << "] " << msg << "\n"; }
void log(int code)                            { std::cout << "  [code] " << code << "\n"; }

// Default arguments — Java has no equivalent (it forces overloads / builders).
// ⚠️ Defaults are bound at the CALL SITE, in the header, at compile time. Changing
// a default in a library requires recompiling all callers. And a default in a
// virtual function uses the STATIC type — a classic bug. Avoid defaults on virtuals.
void connect(const std::string& host, int port = 8080, int timeout_ms = 5000) {
    std::cout << "  connect " << host << ":" << port << " t/o=" << timeout_ms << "\n";
}

void demo_overloading() {
    std::cout << "\n=== 3. Overloading and defaults ===\n";
    log("hello");
    log("warning", 2);
    log(404);
    connect("db.internal");
    connect("db.internal", 5432);

    // OVERLOAD RESOLUTION is done at COMPILE TIME using the STATIC types of the
    // arguments. (Java does the same thing — this is why Java's overload
    // resolution also ignores runtime types. Virtual dispatch is the *other*
    // mechanism; see 03-oop/.)
    // The compiler ranks candidates: exact match > promotion > standard
    // conversion > user-defined conversion > ellipsis. Ambiguity is an error.
}

// ---------------------------------------------------------------------------
// 4. LAMBDAS — closures with EXPLICIT capture, and no hidden allocation
// ---------------------------------------------------------------------------
void demo_lambdas() {
    std::cout << "\n=== 4. Lambdas ===\n";

    int factor = 3;
    std::string prefix = "result: ";

    // [capture](params) -> return_type { body }
    // A lambda is sugar for an anonymous struct with an operator(). It is a
    // VALUE, it has a unique unnameable type, and calling it INLINES. Zero cost.
    auto times = [factor](int x) { return x * factor; };          // capture by copy
    std::cout << "  " << times(7) << "\n";

    // Capture by reference: no copy, but the lambda must not outlive the referent.
    // *** This is the #1 lambda bug in C++ and it does not exist in Java. ***
    // Java forces captured locals to be effectively-final and copies them.
    auto append = [&prefix](const std::string& s) { prefix += s; };
    append("done");
    std::cout << "  " << prefix << "\n";

    // [=] captures everything used, by copy.  [&] captures everything, by reference.
    // Prefer explicit captures — they document lifetime, which is the whole game.
    // ⚠️ In a member function, [=] captures `this` BY POINTER (not the object!).
    //    C++17 added [*this] to capture a copy of the object. Know this: it is a
    //    top cause of use-after-free in async code.

    // `mutable` lets the lambda modify its by-copy captures (they're const by default).
    auto counter = [n = 0]() mutable { return ++n; };   // C++14 init-capture
    std::cout << "  counter: " << counter() << counter() << counter() << "\n";

    // Init-capture is also how you MOVE into a lambda (essential for async work):
    auto owned = std::make_unique<int>(42);
    auto holder = [p = std::move(owned)]() { return *p; };
    std::cout << "  moved into lambda: " << holder() << "\n";

    // ⭐ COST MODEL:
    //   auto f = [..]{..}      -> concrete type, INLINED, zero overhead. Use this.
    //   std::function<int(int)> -> TYPE ERASURE: virtual call + possible heap
    //                              allocation if captures exceed the small buffer.
    //                              This is roughly what a Java lambda costs, always.
    //   Use std::function only when you must store heterogeneous callables
    //   (e.g. a task queue). In hot paths, use templates or function pointers.
    std::function<int(int)> erased = times;   // now it's a virtual call
    std::cout << "  via std::function: " << erased(7)
              << "  (sizeof std::function = " << sizeof(erased) << " bytes)\n";

    // Generic lambda (C++14) = a template operator().
    auto print_any = [](const auto& x) { std::cout << "  generic: " << x << "\n"; };
    print_any(1); print_any("two"); print_any(3.0);
}

// ---------------------------------------------------------------------------
// 5. inline, and what actually makes calls fast
// ---------------------------------------------------------------------------
// `inline` does NOT mean "inline this function". It means "this definition may
// appear in multiple translation units; linker, pick one" (an ODR exemption).
// The compiler decides inlining on its own cost model, and it can only do so if
// it can SEE the body — i.e. it's in a header, or you enable LTO.
inline int square(int x) { return x * x; }

void demo_inline() {
    std::cout << "\n=== 5. inline / call cost ===\n";
    std::cout << "  square(9) = " << square(9) << "  (compiled to a constant)\n";
    std::cout << "  A non-inlined call costs ~1-2ns: push args to registers, call,\n"
                 "  save/restore, ret. An inlined call costs 0 AND unlocks constant\n"
                 "  folding, vectorisation, and dead-code elimination across the\n"
                 "  boundary. This is why C++ header-only libraries are fast, and\n"
                 "  why the JVM's JIT works so hard to inline.\n";
}

int main() {
    demo_parameter_passing();
    demo_returns();
    demo_overloading();
    demo_lambdas();
    demo_inline();
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * Pass small trivially-copyable types BY VALUE; everything else by
//    `const T&`; use `T&` to mutate; take a sink by value + std::move.
//  * Return by value. C++17 guarantees copy elision for prvalues; NRVO handles
//    named locals. NEVER `return std::move(x)` — it disables NRVO.
//  * Never return a reference or pointer to a local.
//  * Overload resolution is compile-time and uses static types.
//  * A lambda is an anonymous struct with operator(). `auto` lambda = zero cost
//    and inlinable. `std::function` = type erasure = indirect call + maybe heap.
//  * Capture by reference only when the lambda cannot outlive the captured
//    object. `[=]` in a member function captures `this`, not a copy.
//  * `inline` is a linkage rule, not an inlining command.
// ===========================================================================
