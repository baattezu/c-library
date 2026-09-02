// ===========================================================================
//  VARIABLES, STORAGE, AND THE THING JAVA HID FROM YOU
//  run: make run F=01-fundamentals/01-variables.cpp
// ===========================================================================
//
//  THE ONE BIG IDEA OF THIS FILE
//  -----------------------------
//  In Java, `Foo f = new Foo();` means: f is a *reference* (a hidden pointer) to
//  an object that lives on the heap, managed by the GC.  There is no other option.
//  Every object is heap-allocated. Every variable of class type is a handle.
//
//  In C++, `Foo f;` means: **f IS the object**. The bytes of Foo live right here,
//  in this stack frame, inline. No heap. No pointer. No GC. When this scope ends,
//  those bytes are gone and Foo's destructor runs — deterministically, immediately.
//
//  This is called VALUE SEMANTICS and it is the single biggest mental shift.
//  Java: variables name references.   C++: variables name storage.
//
// ===========================================================================

#include <iostream>
#include <string>
#include <typeinfo>

// A namespace-scope variable. Lives for the entire program (static storage
// duration), zero-initialised before main() runs. This is the closest thing to
// a Java `static` field. Prefer to avoid these — global mutable state is the
// enemy of concurrency.
int g_counter = 0;

void demo_declaration_and_initialization() {
    std::cout << "\n=== 1. Declaration vs initialization ===\n";

    // C-style / "copy" initialization. Familiar from Java.
    int a = 5;

    // Direct initialization (calls a constructor with these arguments).
    int b(6);

    // BRACE INITIALIZATION, a.k.a. "uniform initialization" (C++11).
    // *** Prefer this by default. *** Reason: it forbids narrowing conversions.
    int c{7};

    // This is a compile ERROR, and that is a *feature*:
    //     int bad{3.7};   // error: type 'double' cannot be narrowed to 'int'
    // while this compiles (with only a warning) and silently loses data:
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wliteral-conversion"   // we WANT to show this
    int narrowed = 3.7;   // == 3. Java would refuse; C's = initialization won't.
#pragma clang diagnostic pop

    std::cout << "a=" << a << " b=" << b << " c=" << c
              << " narrowed(3.7)=" << narrowed << "\n";

    // ---------------------------------------------------------------------
    // ⚠️ THE TRAP THAT DOES NOT EXIST IN JAVA:
    // A local variable of built-in type is NOT zero-initialised. It contains
    // whatever garbage was in that stack slot before. Reading it is UNDEFINED
    // BEHAVIOUR — not "returns 0", not "throws" — undefined. The optimiser is
    // allowed to assume it never happens and delete code around it.
    // ---------------------------------------------------------------------
    int uninitialised;      // garbage!
    int zeroed{};           // value-initialised to 0. ALWAYS write the {}.
    (void)uninitialised;    // (void) means "yes I know, I'm deliberately ignoring"
    std::cout << "zeroed{} = " << zeroed << "  (uninitialised would be garbage)\n";

    // Java has no equivalent hazard: fields default to 0/null, and locals must be
    // definitely-assigned before use (the compiler enforces it). C++ enforces
    // nothing here. This is the tax you pay for zero-overhead: no hidden memset.
}

void demo_auto() {
    std::cout << "\n=== 2. auto — like Java's var, but stronger ===\n";

    auto x = 42;             // int
    auto y = 42.0;           // double
    auto z = 42u;            // unsigned int
    auto s = std::string{"hello"};   // std::string
    auto p = "hello";        // ⚠️ const char*, NOT std::string! (string literal)

    std::cout << "sizeof x=" << sizeof(x) << " y=" << sizeof(y)
              << " z=" << sizeof(z) << "\n";
    std::cout << "s=" << s << " (a real string object) p=" << p
              << " (a pointer into read-only memory)\n";

    // `auto` is deduced at COMPILE TIME. There is no runtime type here at all.
    // Unlike Java's `var`, C++'s auto is essential (not just convenience) because
    // many types are unnameable — lambdas, iterators, template expression types.

    // Rule of thumb: use auto when the type is obvious from the right-hand side
    // or is unspeakably long. Write the type when it documents intent.
}

void demo_storage_duration() {
    std::cout << "\n=== 3. Storage duration: where do the bytes live? ===\n";

    // AUTOMATIC storage duration ("on the stack"). Freed when scope exits.
    // Allocation cost: literally zero — the compiler just moves the stack pointer
    // once for the whole frame. This is why C++ can be fast: most objects cost
    // nothing to create.
    int on_stack = 1;

    // STATIC storage duration. One instance for the whole program.
    // NOTE: a function-local static is initialised on FIRST CALL, and C++11
    // guarantees that initialisation is THREAD-SAFE ("magic statics"). This is
    // the standard way to write a lazy singleton in C++ — and unlike the Java
    // double-checked-locking mess, it is correct by construction.
    static int call_count = 0;
    ++call_count;

    // DYNAMIC storage duration ("on the heap"). YOU own this.
    // Raw new/delete shown here for education only — in real code you use
    // smart pointers (02-memory/04-smart-pointers.cpp). Never write raw `new`
    // in application code.
    int* on_heap = new int{99};

    std::cout << "stack value=" << on_stack
              << "  heap value=" << *on_heap
              << "  static call_count=" << call_count << "\n";

    // Approximate addresses tell you the memory map. Stack addresses are high
    // and grow *downward*; heap addresses are lower and grow upward; globals sit
    // in the data segment. (Exact numbers vary; ASLR randomises them per run.)
    std::cout << "  &stack  ~ " << static_cast<const void*>(&on_stack)  << "\n";
    std::cout << "  heap    ~ " << static_cast<const void*>(on_heap)    << "\n";
    std::cout << "  &global ~ " << static_cast<const void*>(&g_counter) << "\n";

    delete on_heap;   // If you forget this, you leak. There is no GC coming.
}

void demo_sizes_and_layout() {
    std::cout << "\n=== 4. sizeof — you can SEE the memory ===\n";

    struct Point { double x, y; };            // 16 bytes, no header, no vtable
    struct Padded { char c; int i; char d; }; // 12 bytes! padding is real

    std::cout << "sizeof(char)        = " << sizeof(char)        << "  (always 1 by definition)\n";
    std::cout << "sizeof(int)         = " << sizeof(int)         << "\n";
    std::cout << "sizeof(long)        = " << sizeof(long)        << "  (8 on macOS/Linux, 4 on Windows!)\n";
    std::cout << "sizeof(void*)       = " << sizeof(void*)       << "  (pointer size == word size)\n";
    std::cout << "sizeof(Point)       = " << sizeof(Point)       << "  (2 doubles, exactly)\n";
    std::cout << "sizeof(Padded)      = " << sizeof(Padded)      << "  (char,int,char -> 1+3pad+4+1+3pad)\n";
    std::cout << "sizeof(std::string) = " << sizeof(std::string) << "  (libc++: 24 bytes, SSO inside!)\n";

    // *** THIS IS THE POINT. ***
    // In Java, `new Point(x,y)` costs 16 bytes of doubles PLUS a 12–16 byte object
    // header PLUS 8 bytes for the reference PLUS heap fragmentation PLUS GC
    // bookkeeping — and an array of 1000 Points is 1000 separate heap objects
    // scattered across memory, each a potential cache miss.
    // In C++, `Point pts[1000]` is exactly 16000 contiguous bytes. One allocation.
    // Perfectly prefetchable. That difference IS the performance difference.
}

int main() {
    // Keeping your original code as demo 0 — it still works.
    std::cout << "=== 0. Your original program ===\n";
    std::string name = "Temirkhan";
    int age = 21;
    double height = 176.5;
    bool is_student = true;

    std::cout << "Name: " << name << '\n';
    std::cout << "Age: " << age << '\n';
    std::cout << "Height: " << height << '\n';
    // std::boolalpha makes bool print as true/false instead of 1/0.
    std::cout << "Student: " << std::boolalpha << is_student << '\n';

    demo_declaration_and_initialization();
    demo_auto();
    demo_storage_duration();
    demo_storage_duration();   // called twice so you can see the static counter
    demo_sizes_and_layout();

    std::cout << "\n>>> TRY IT: delete the `delete on_heap;` line and run\n"
                 "    `make asan F=01-fundamentals/01-variables.cpp` — the leak detector\n"
                 "    will name the exact line. That tool is your new GC.\n";
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  1. `T x;`  in C++ constructs an object of type T *here*. It is not a reference.
//  2. Locals of built-in type are UNINITIALISED garbage. Always use `{}`.
//  3. Three storage durations: automatic (stack), static (whole program),
//     dynamic (heap, you own it). Later you'll meet a 4th: thread_local.
//  4. Prefer `{}` initialization — it bans narrowing conversions.
//  5. `auto` is compile-time deduction with zero runtime cost.
//  6. Function-local `static` init is thread-safe since C++11 — the idiomatic
//     lazy singleton.
//  7. sizeof tells you the truth about layout. Java can't tell you this.
// ===========================================================================
