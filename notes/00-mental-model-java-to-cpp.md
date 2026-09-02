# 00 — The mental model shift: Java → C++

> Read this before anything else. Everything that confuses Java developers about
> C++ traces back to four differences. Internalise these four and 80% of the
> confusion evaporates.

---

## The four differences

### 1. Variables ARE objects, not handles to objects

```java
// Java
Point p = new Point(1,2);   // p is a REFERENCE. The Point is on the heap.
Point q = p;                // q and p point to the SAME Point.
q.x = 99;                   // p.x is now 99 too.
```

```cpp
// C++
Point p{1,2};   // p IS the Point. 8 bytes, right here in this stack frame.
Point q = p;    // q is a COPY. A separate, complete Point.
q.x = 99;       // p.x is still 1.
```

This is called **value semantics** and it is the root of everything:
- Assignment copies (unless you move).
- Passing to a function copies (unless you pass a reference).
- Storing in a container copies the object *into* the container's memory.
- `std::vector<Point>` is `n * sizeof(Point)` contiguous bytes.
  `ArrayList<Point>` is `n` pointers to `n` scattered heap objects.

**Consequence:** in Java you never think about copies because they never happen.
In C++ an accidental copy of a `std::vector<std::string>` in a hot loop is a
performance disaster. The `const&` habit exists for exactly this reason.

### 2. Lifetime is deterministic and scope-based, not GC'd

```java
// Java: when does this file handle close? WHENEVER. You must use try-with-resources.
FileInputStream f = new FileInputStream("x");   // finalize() is a lie
```

```cpp
{
    std::ifstream f("x");   // opened
    // ...
}   // <-- CLOSED HERE. Exactly here. Guaranteed. Even if an exception is thrown.
```

The destructor runs **at the closing brace**, deterministically, in reverse order
of construction. This mechanism is called **RAII** (Resource Acquisition Is
Initialization) and it is C++'s single best idea. It handles memory, files,
sockets, mutexes, database transactions, timers — anything with an "undo".

Java's `try-with-resources` is RAII with worse ergonomics, applied only to things
that implement `AutoCloseable`, and only when you remember to write the block.
C++ applies it to *everything*, automatically, including on the exception path.

> **Mental substitution:** every C++ destructor is a `finally { close(); }` that
> you cannot forget to write.

### 3. You choose where memory lives

| | Java | C++ |
|---|---|---|
| Object storage | always heap | stack, heap, static, thread-local, or inside another object |
| Cost of creation | allocation + eventual GC | stack: literally free. heap: ~50-100ns |
| Layout control | none | total (`alignas`, packing, `sizeof` is knowable) |
| Contiguity | only for primitive arrays | for everything |

The stack is not "a small special place." **Most objects in well-written C++ live
on the stack**, and creating them costs *zero instructions* — the compiler bumps
the stack pointer once for the entire function frame.

### 4. Compilation is real and separate

Java: `javac` → bytecode → JVM loads classes by name at runtime, JIT compiles hot
paths, reflection can find anything.

C++: preprocessor → compiler (one `.cpp` at a time, in total isolation) → linker
(stitch it together) → a native binary with **no runtime, no metadata, no
reflection, and no class names**. Templates are expanded and gone. Type names
survive only if you asked for RTTI.

This is why C++ has `#include`, header files, the One Definition Rule, forward
declarations, and hour-long build times. See `notes/02-compilation-and-linking.md`.

---

## The translation table

| Java | C++ | Note |
|---|---|---|
| `Object o` | `T obj` / `T& ref` / `T* ptr` / `std::unique_ptr<T>` | you pick ownership |
| `final` | `const` | C++ `const` is deep and viral |
| `static` field | namespace-scope or `static` member | init order across TUs is a hazard |
| `ArrayList<T>` | `std::vector<T>` | contiguous, elements stored inline |
| `HashMap<K,V>` | `std::unordered_map<K,V>` | but see note below — it's slow |
| `TreeMap<K,V>` | `std::map<K,V>` | red-black tree, sorted, O(log n) |
| `HashSet<T>` | `std::unordered_set<T>` | |
| `StringBuilder` | `std::string` (it's mutable) | just use `+=` |
| `String` | `std::string` (owning) / `std::string_view` (borrowing) | |
| `Optional<T>` | `std::optional<T>` | inline, no allocation |
| `interface` | abstract class w/ pure virtuals, **or** a `concept`, **or** a template | |
| `implements` | `: public Interface` | |
| `@Override` | `override` (a real keyword — use it always) | |
| `instanceof` + cast | `dynamic_cast<T*>` | needs a virtual function; costs RTTI lookup |
| generics `<T>` | templates `template<typename T>` | **completely different mechanism** |
| `Runnable` / lambda | lambda `[](){}` | C++ lambdas inline; Java's allocate |
| `synchronized` | `std::lock_guard<std::mutex>` | RAII lock |
| `volatile` | `std::atomic<T>` | C++ `volatile` is NOT for threading! |
| `Thread` | `std::thread` (must `join()`) | |
| `ExecutorService` | you write it (see `09-projects/04-thread-pool`) | |
| `Future<T>` | `std::future<T>` | but it's weak; real code uses libraries |
| `try/catch/finally` | `try/catch` + **RAII instead of finally** | there is no `finally` |
| `throws` clause | nothing (or `noexcept` for "never throws") | |
| `System.out.println` | `std::cout <<` or `std::println` (C++23) | |
| `equals()` | `operator==` | `==` on objects compares *values* |
| `hashCode()` | `std::hash<T>` specialisation | |
| `toString()` | `operator<<` for ostream, or `std::formatter` | |
| `Comparable` | `operator<=>` (C++20 spaceship) | one operator gives you all six |
| `Iterable` | `begin()`/`end()` | duck-typed, no interface needed |
| `null` | `nullptr` | but references can't be null at all |
| `package` | `namespace` | no relation to the filesystem |
| `import` | `#include` (textual!) or `import` (C++20 modules) | |
| Maven/Gradle | CMake + vcpkg/Conan | genuinely worse. Sorry. |
| javadoc | Doxygen | |
| JUnit | Catch2 / GoogleTest / doctest | |
| Spring DI | constructor parameters. That's it. | you'll find you don't miss it |

---

## Java generics vs C++ templates — this one really matters

**Java generics are erasure.** `List<String>` and `List<Integer>` are the *same
class* at runtime. The compiler inserts casts. There is one copy of the code. You
cannot have `List<int>` (must box). You cannot do `new T()`. Everything is an
`Object` behind a checked cast.

**C++ templates are code generation.** `std::vector<int>` and
`std::vector<std::string>` are *different types* with *different machine code*,
generated at compile time.

Consequences:
- **Zero abstraction cost.** `std::vector<int>` stores raw ints. `sort` on it
  inlines the comparison. There is no boxing, no cast, no `Object`.
- **Compile-time duck typing.** A template doesn't need an interface — it just
  needs the operations it uses to compile. This is why `std::sort` works on
  anything with random-access iterators and `<`.
- **Templates are Turing-complete at compile time.** You can compute, generate
  types, and specialise behaviour with zero runtime cost.
- **The costs:** binary bloat (each instantiation is real code), long compile
  times, and error messages that are famously terrible (mitigated by C++20
  `concepts`, which are the closest thing to `<T extends Comparable<T>>`).

```cpp
// Java's <T extends Comparable<T>> becomes:
template <std::totally_ordered T>
T max_of(const T& a, const T& b) { return a < b ? b : a; }
```

---

## `unordered_map` is not `HashMap` (a performance warning)

`std::unordered_map` is **required by the standard** to use separate chaining with
stable references, which forces it to be a bucket array of *pointers* to
individually heap-allocated nodes. Every lookup is: hash → index → pointer chase
→ possible second pointer chase. Java's `HashMap` has the same shape, but the JVM
at least allocates nodes contiguously in the young gen for a while.

For real performance work, everyone uses an open-addressing hash map instead:
`absl::flat_hash_map`, `boost::unordered_flat_map`, `ankerl::unordered_dense`,
or `tsl::robin_map`. Typically **2-5x faster**. This is a well-known standard
library wart. Know it, and don't reach for `unordered_map` in a hot path.

---

## Exceptions: same syntax, different culture

```cpp
try { risky(); }
catch (const std::runtime_error& e) { std::cerr << e.what(); }   // catch by CONST REF
catch (...) { /* catch-all */ }
```

Differences that matter:
- **No `finally`.** RAII replaces it. Destructors run during stack unwinding.
- **No checked exceptions.** Any function can throw unless marked `noexcept`.
- **Cost:** zero when nothing throws (the "zero-cost" / table-driven model). But
  *throwing* is expensive — microseconds, involving unwind tables and often a
  global lock in the runtime. Never use exceptions for control flow.
- **Culture:** large parts of the industry (games, embedded, some HFT, Google's
  own style guide historically) disable exceptions entirely with
  `-fno-exceptions`. The alternatives are `std::optional<T>`,
  `std::expected<T,E>` (C++23), and error codes.
- **Destructors must not throw.** If a destructor throws during unwinding,
  `std::terminate` is called. Destructors are implicitly `noexcept`.
- **`catch` by `const&`.** Catching by value slices the exception object.

---

## Things you will miss from Java (and what to do instead)

| You'll miss | Reality | Do this instead |
|---|---|---|
| Garbage collection | RAII + smart pointers cover ~99% | `unique_ptr`, `vector`, value types |
| A standard build tool | CMake is the de facto standard and it's awkward | learn CMake; use vcpkg or Conan for deps |
| A package repo (Maven Central) | vcpkg/Conan exist but are less complete | vendor small deps; use header-only libs |
| Reflection | none. Zero. | code generation, macros, or C++26 static reflection (coming) |
| Networking/HTTP in the stdlib | genuinely absent | Boost.Asio, or raw sockets (`08-systems/`) |
| A JSON library in the stdlib | absent | `nlohmann/json` (nice), `simdjson` (fast) |
| `String.split`, `String.join` | `<ranges>` has `split`/`join` (C++20/23), clunky | write a `string_view` splitter once |
| Threads that are easy | `std::thread` is bare metal | build a thread pool once, reuse it |
| Stack traces on crash | C++23 `<stacktrace>` (not yet in Apple clang) | lldb, or `backtrace()` |
| Fast compile times | you'll wait | precompiled headers, unity builds, ccache, modules |

## Things Java devs consistently get wrong for the first month

1. **Passing big objects by value.** `void f(std::vector<int> v)` copies the whole
   vector. Write `const std::vector<int>&`.
2. **`for (auto x : container)`** — copies every element. Write `const auto&`.
3. **Using `new` at all.** Use `make_unique`, or better, a value on the stack.
4. **Returning a reference/pointer to a local.** Dangling. The compiler warns
   sometimes; ASan catches it always.
5. **Storing a `string_view` or iterator whose owner might die or reallocate.**
6. **Thinking `auto` preserves references.** `auto x = v[0]` copies.
   `auto& x = v[0]` doesn't.
7. **Writing getters/setters and deep hierarchies by reflex.** Idiomatic C++ uses
   free functions, plain structs, and composition. Deep virtual hierarchies are
   an anti-pattern here — every level is a cache miss.
8. **Using `unordered_map` in a hot loop** (see above).
9. **Forgetting to `join()` a `std::thread`** — destroying a joinable thread calls
   `std::terminate`. Your program aborts.
10. **Expecting bounds checks.** `v[i]` never checks. `v.at(i)` throws.

---

## The one-sentence summary

> **Java gives you one correct way to manage objects and hides the machine.
> C++ gives you every way, checks nothing, and shows you the machine —
> and that is why the fast infrastructure of the world is written in it.**

Next: `notes/01-machine-model.md`.
