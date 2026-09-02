// ===========================================================================
//  SMART POINTERS — unique_ptr, shared_ptr, weak_ptr
//  run: make run F=02-memory/04-smart-pointers.cpp
// ===========================================================================
//
//  These are not "C++'s garbage collector." They are RAII applied to heap
//  memory, with different ownership policies:
//
//    unique_ptr<T>  — ONE owner. Move-only. sizeof == 8. ZERO runtime overhead.
//    shared_ptr<T>  — N owners, atomic refcount. sizeof == 16. Real cost.
//    weak_ptr<T>    — observes a shared_ptr without owning. Breaks cycles.
//
//  Default to unique_ptr. shared_ptr is a design decision, not a convenience.
//
// ===========================================================================

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct Node {
    std::string name;
    explicit Node(std::string n) : name(std::move(n)) {
        std::cout << "    + " << name << "\n";
    }
    ~Node() { std::cout << "    - " << name << "\n"; }
    void greet() const { std::cout << "    hello from " << name << "\n"; }
};

// ---------------------------------------------------------------------------
void demo_unique_ptr() {
    std::cout << "\n=== 1. unique_ptr — the default ===\n";

    // ALWAYS use make_unique, never `unique_ptr<T>(new T)`:
    //   - exception-safe (no leak if another argument's evaluation throws)
    //   - says the type once instead of twice
    //   - value-initialises
    auto a = std::make_unique<Node>("A");
    a->greet();
    (*a).greet();

    std::cout << "  sizeof(unique_ptr<Node>) = " << sizeof(a)
              << " bytes — the SAME as a raw pointer. Zero overhead.\n";

    // Move-only: you cannot copy it. This is the type system enforcing
    // "exactly one owner" at COMPILE TIME.
    // auto b = a;                  // ERROR: call to deleted copy constructor
    auto b = std::move(a);          // ✅ ownership transferred
    std::cout << "  after move: a is " << (a ? "valid" : "null")
              << ", b is " << (b ? "valid" : "null") << "\n";

    // The API:
    std::cout << "  b.get()     -> raw pointer (NON-owning; never delete it)\n";
    b->greet();
    b.reset();                       // destroys now
    std::cout << "  after reset(): b is " << (b ? "valid" : "null") << "\n";

    auto c = std::make_unique<Node>("C");
    Node* raw = c.release();         // give up ownership; YOU must delete now
    std::cout << "  released " << raw->name << " — we own it manually again\n";
    delete raw;

    // Arrays: make_unique<T[]>(n). But honestly, use std::vector.
    auto arr = std::make_unique<int[]>(5);
    arr[0] = 42;
    std::cout << "  unique_ptr<int[]> works, but prefer std::vector<int>\n";

    // Custom deleter — this is how you wrap ANY C resource in unique_ptr.
    // Note the deleter is part of the TYPE, so a stateless one costs 0 bytes.
    auto fcloser = [](std::FILE* f) { if (f) { std::fclose(f); std::cout << "    (FILE closed by deleter)\n"; } };
    std::unique_ptr<std::FILE, decltype(fcloser)> file(std::fopen("/tmp/up_demo.txt", "w"), fcloser);
    if (file) std::fputs("written via unique_ptr\n", file.get());
    std::cout << "  sizeof with stateless lambda deleter = "
              << sizeof(file) << " (still 8! empty-class optimisation)\n";
}

// ---------------------------------------------------------------------------
void demo_unique_ptr_in_apis() {
    std::cout << "\n=== 2. unique_ptr expresses ownership in signatures ===\n";
    std::cout <<
      "    void takes_ownership(std::unique_ptr<Node> n);   // I will destroy it\n"
      "    void borrows(const Node& n);                     // I just read it\n"
      "    void borrows_nullable(const Node* n);            // ...and it may be null\n"
      "    std::unique_ptr<Node> factory();                 // caller owns the result\n"
      "\n"
      "  The signature IS the documentation, and the compiler enforces it.\n"
      "  In Java every one of these is `Node n` and you write a Javadoc comment\n"
      "  that nobody reads.\n";

    struct Api {
        static void takes(std::unique_ptr<Node> n) {
            std::cout << "    took ownership of " << n->name << "\n";
        }   // destroyed here
        static void borrows(const Node& n) {
            std::cout << "    borrowed " << n.name << " (still owned by caller)\n";
        }
        static std::unique_ptr<Node> factory() { return std::make_unique<Node>("F"); }
    };

    auto n = Api::factory();
    Api::borrows(*n);
    Api::takes(std::move(n));
    std::cout << "  caller's pointer is now " << (n ? "valid" : "null") << "\n";
}

// ---------------------------------------------------------------------------
void demo_shared_ptr() {
    std::cout << "\n=== 3. shared_ptr — shared ownership, with a real cost ===\n";

    auto a = std::make_shared<Node>("Shared");
    std::cout << "  use_count = " << a.use_count() << "\n";
    {
        auto b = a;                  // copy: ATOMIC increment
        auto c = a;
        std::cout << "  use_count = " << a.use_count() << " (3 owners)\n";
    }   // b, c destroyed: two atomic decrements
    std::cout << "  use_count = " << a.use_count() << " back to 1\n";
    std::cout << "  sizeof(shared_ptr) = " << sizeof(a)
              << " (object ptr + control block ptr)\n";

    std::cout <<
      "\n  THE CONTROL BLOCK holds: strong count, weak count, deleter, allocator.\n"
      "  make_shared allocates the control block AND the object in ONE block —\n"
      "  faster, better locality. (Downside: the memory isn't freed until the\n"
      "  last WEAK reference dies too, since the object is embedded.)\n"
      "\n"
      "  THE COSTS you are paying:\n"
      "   - 16 bytes per pointer instead of 8\n"
      "   - an ATOMIC inc/dec on every copy/destroy. Atomics are ~5-20ns and\n"
      "     they bounce the cache line between cores. Copying shared_ptrs in a\n"
      "     hot multi-threaded loop is a classic, invisible scalability killer.\n"
      "   - one extra indirection to reach the count\n"
      "   - unclear ownership in your design\n"
      "\n"
      "  ⭐ If you must pass a shared_ptr around, pass `const shared_ptr<T>&`\n"
      "     (no refcount touch) or just `T&`/`T*` when the callee doesn't store it.\n"
      "     Passing shared_ptr BY VALUE into every function is the #1 way people\n"
      "     accidentally make shared_ptr expensive.\n";

    // THREAD SAFETY, precisely:
    //   - The REFCOUNT is atomic. Copying/destroying shared_ptrs from multiple
    //     threads is safe.
    //   - The POINTEE is not protected at all. Two threads writing to *p is a
    //     data race, exactly as it would be with a raw pointer.
    //   - Two threads writing to the SAME shared_ptr object (reassigning it) is
    //     also a data race. Use std::atomic<std::shared_ptr<T>> (C++20).
}

// ---------------------------------------------------------------------------
void demo_cycles_and_weak_ptr() {
    std::cout << "\n=== 4. Cycles leak — and weak_ptr fixes them ===\n";

    // Reference counting CANNOT collect cycles. A tracing GC (Java) can.
    // This is the one place where Java's GC is genuinely more capable.
    struct Bad {
        std::string name;
        std::shared_ptr<Bad> peer;
        explicit Bad(std::string n) : name(std::move(n)) { std::cout << "    + " << name << "\n"; }
        ~Bad() { std::cout << "    - " << name << "\n"; }
    };
    {
        auto x = std::make_shared<Bad>("cycle-X");
        auto y = std::make_shared<Bad>("cycle-Y");
        x->peer = y;
        y->peer = x;      // ← now each holds the other. Count never reaches 0.
        std::cout << "  x.use_count=" << x.use_count() << " y.use_count=" << y.use_count() << "\n";
    }
    std::cout << "  ^^^ scope ended and NO destructors ran. That is a LEAK.\n";

    // THE FIX: the non-owning direction uses weak_ptr.
    struct Parent;
    struct Child {
        std::string name;
        std::weak_ptr<Parent> parent;     // ← weak: does NOT keep parent alive
        explicit Child(std::string n) : name(std::move(n)) { std::cout << "    + " << name << "\n"; }
        ~Child() { std::cout << "    - " << name << "\n"; }
    };
    struct Parent {
        std::string name;
        std::vector<std::shared_ptr<Child>> children;   // owns its children
        explicit Parent(std::string n) : name(std::move(n)) { std::cout << "    + " << name << "\n"; }
        ~Parent() { std::cout << "    - " << name << "\n"; }
    };

    std::cout << "\n  With weak_ptr for the back-edge:\n";
    {
        auto p = std::make_shared<Parent>("Parent");
        auto c = std::make_shared<Child>("Child");
        p->children.push_back(c);
        c->parent = p;                    // weak — no cycle

        // To USE a weak_ptr you must lock() it, which either gives you a valid
        // shared_ptr or an empty one. This is race-free by construction: you
        // cannot observe the object dying between the check and the use.
        if (auto locked = c->parent.lock())
            std::cout << "    child's parent is alive: " << locked->name << "\n";
    }
    std::cout << "  ^^^ both destroyed correctly.\n";

    // weak_ptr's other big use: CACHES. Hold weak_ptrs in the cache so entries
    // die when nobody's using them. And OBSERVERS: a callback that shouldn't
    // keep its subject alive.
    std::weak_ptr<Node> observer;
    {
        auto tmp = std::make_shared<Node>("Observed");
        observer = tmp;
        std::cout << "  in scope: expired? " << std::boolalpha << observer.expired() << "\n";
    }
    std::cout << "  out of scope: expired? " << observer.expired()
              << " lock() gives " << (observer.lock() ? "a pointer" : "nullptr") << "\n";
}

// ---------------------------------------------------------------------------
void demo_enable_shared_from_this() {
    std::cout << "\n=== 5. enable_shared_from_this (essential for async code) ===\n";

    // PROBLEM: inside a member function, `this` is a raw pointer. If you need a
    // shared_ptr to yourself (to keep yourself alive during an async operation),
    // you CANNOT write shared_ptr<T>(this) — that creates a SECOND control
    // block, and you get a double free.
    struct Session : std::enable_shared_from_this<Session> {
        int id;
        explicit Session(int i) : id(i) { std::cout << "    + session " << id << "\n"; }
        ~Session() { std::cout << "    - session " << id << "\n"; }

        void start_async_work(std::vector<std::shared_ptr<Session>>& queue) {
            // This is THE idiom for async servers: capture a shared_ptr to self
            // so the object cannot die while an operation is in flight.
            queue.push_back(shared_from_this());
            std::cout << "    session " << id << " queued work, use_count="
                      << shared_from_this().use_count() << "\n";
        }
    };

    std::vector<std::shared_ptr<Session>> pending;
    {
        auto s = std::make_shared<Session>(1);
        s->start_async_work(pending);
    }   // local `s` dies, but the session survives — `pending` holds a reference
    std::cout << "  session survived the scope because the queue owns it\n";
    pending.clear();
    std::cout <<
      "\n  In Boost.Asio / any C++ network server you will see this everywhere:\n"
      "      socket.async_read(buf, [self = shared_from_this()](auto ec, auto n) {...});\n"
      "  It is how you avoid use-after-free in callback-driven code.\n"
      "  ⚠️ shared_from_this() throws std::bad_weak_ptr if the object is NOT\n"
      "     already owned by a shared_ptr (e.g. it's on the stack, or you're in\n"
      "     the constructor).\n";
}

// ---------------------------------------------------------------------------
void decision_guide() {
    std::cout << "\n=== 6. Which one? ===\n";
    std::cout <<
      "  Value on the stack (T x;)          -> DEFAULT. Use this whenever possible.\n"
      "  std::vector<T>                     -> a bunch of them, contiguous\n"
      "  std::unique_ptr<T>                 -> heap, one owner, polymorphic, or huge\n"
      "  std::shared_ptr<T>                 -> genuinely shared, dynamic lifetime\n"
      "  std::weak_ptr<T>                   -> observe without owning; break cycles\n"
      "  T& / const T&                      -> borrow, non-null, callee doesn't store\n"
      "  T* (raw)                           -> borrow, nullable, callee doesn't store\n"
      "  std::span<T> / std::string_view    -> borrow a RANGE\n"
      "\n"
      "  ANTI-PATTERNS\n"
      "   ✗ shared_ptr everywhere 'to be safe'      -> atomics + unclear design\n"
      "   ✗ shared_ptr<T> passed by value in hot paths\n"
      "   ✗ unique_ptr<T> for something that could just be a member value\n"
      "   ✗ vector<unique_ptr<T>> when vector<T> would do (destroys locality)\n"
      "   ✗ shared_ptr<T>(this)                     -> use enable_shared_from_this\n"
      "   ✗ storing .get() results long-term        -> that's a dangling pointer\n"
      "     waiting to happen\n";
}

int main() {
    demo_unique_ptr();
    demo_unique_ptr_in_apis();
    demo_shared_ptr();
    demo_cycles_and_weak_ptr();
    demo_enable_shared_from_this();
    decision_guide();
    std::remove("/tmp/up_demo.txt");
    return 0;
}
