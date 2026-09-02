# 02 — Compilation, linking, and why C++ builds are the way they are

> Java has one compilation model and you never think about it. C++'s model is
> from 1972 and it leaks into everything: headers, the ODR, forward declarations,
> `inline`, template instantiation, 40-minute builds, and cryptic linker errors.
> Understanding it takes 30 minutes and saves you months.

---

## The pipeline

```
foo.cpp ──[preprocessor]──> foo.i ──[compiler]──> foo.s ──[assembler]──> foo.o ─┐
bar.cpp ──[preprocessor]──> bar.i ──[compiler]──> bar.s ──[assembler]──> bar.o ─┤
                                                                                ├─[linker]─> a.out
libfoo.a  (static library = archive of .o files) ───────────────────────────────┤
libbar.dylib / .so  (shared library, resolved at load time) ────────────────────┘
```

Run each step yourself — do this once, it demystifies everything:

```sh
clang++ -E foo.cpp -o foo.i     # preprocess only  (look at the size!)
clang++ -S foo.cpp -o foo.s     # compile to assembly
clang++ -c foo.cpp -o foo.o     # compile to object file
clang++ foo.o bar.o -o app      # link
nm -C app | head                # list symbols (-C = demangle)
otool -L app                    # which dynamic libs does it need? (Linux: ldd)
```

---

## Step 1: The preprocessor — textual, dumb, and powerful

`#include <vector>` means **literally paste the contents of that file here**.
It is not an import. It has no notion of namespaces or types.

```sh
$ echo '#include <vector>
int main(){}' > t.cpp
$ clang++ -std=c++23 -E t.cpp | wc -l
   ~40,000 lines
```

Forty thousand lines of code, compiled from scratch, **in every single `.cpp`
that includes `<vector>`**. Include `<iostream>` and it's ~50,000. This is why
C++ builds are slow. It's not the optimiser — it's that you compile the same
headers hundreds of times.

**Header guards** stop the same header being pasted twice into one TU:

```cpp
#pragma once            // non-standard but universally supported. Use this.
// or the portable classic:
#ifndef MYLIB_FOO_H
#define MYLIB_FOO_H
...
#endif
```

**Mitigations for slow builds** (in order of effort):
1. Include less. Forward-declare instead (see below).
2. `ccache` — caches compilation results. Massive win on rebuilds.
3. Precompiled headers (PCH).
4. Unity builds (concatenate .cpp files). Ugly, effective.
5. **C++20 modules** (`import std;`) — the real fix; tooling support is still
   maturing in 2026. Apple clang supports `-fmodules` for its own headers.

### Forward declaration

```cpp
// widget.h — DON'T include gadget.h just to hold a pointer
class Gadget;                 // forward declaration: "Gadget is a class type"

class Widget {
    Gadget* g_;               // OK: a pointer to an incomplete type is fine
    // Gadget  g_;            // ERROR: needs the full definition (needs its size)
public:
    void use();               // OK: declaration only
};
```

Rule: you need the **complete type** to know its size, access members, or
construct/destroy it. You need only a **declaration** to have a pointer or
reference to it. Forward-declaring aggressively is the #1 build-time fix.

The industrial version of this is the **PIMPL idiom** (pointer to implementation):
put all private members behind an opaque pointer so the header exposes nothing,
and changing internals doesn't recompile the world. Cost: one heap allocation
and one indirection. Worth it for library boundaries, not for hot data.

---

## Step 2: The compiler — one translation unit, total isolation

A **translation unit (TU)** = one `.cpp` after preprocessing. The compiler sees
*exactly one* TU at a time and knows *nothing* about any other. It emits a `.o`
containing machine code plus a **symbol table**:
- **defined** symbols ("I have `foo()`, here it is")
- **undefined** symbols ("I call `bar()`, someone please provide it")

This is why:
- A declaration in a header is enough to *compile*, but you need the definition
  somewhere to *link*.
- The compiler can't inline across TUs — unless you enable **LTO** (`-flto`),
  which defers real code generation to link time so it can see everything. LTO
  typically buys 5-20% and costs build time. Enable it for release builds.

### Name mangling

C++ has overloading and namespaces, but object files have flat string symbols.
So the compiler **mangles** names to encode the signature:

```sh
$ echo 'namespace app { int add(int,int){return 0;} }' | clang++ -x c++ -c - -o t.o
$ nm t.o
0000000000000000 T __ZN3app3addEii
$ nm -C t.o                     # -C demangles
0000000000000000 T app::add(int, int)
```

`extern "C"` turns mangling off — that's how you talk to C libraries and how you
export a stable ABI from a shared library.

---

## Step 3: The linker, and the One Definition Rule

The linker matches undefined symbols to defined ones. Two classic errors:

**`undefined symbol: foo()`** — you declared it and called it, but no `.o` or
library defines it. Causes: forgot to compile that `.cpp`; forgot `-lfoo`;
signature mismatch (mangled name differs); defined a template's method in a
`.cpp` instead of the header.

**`duplicate symbol: foo()`** — two TUs define the same thing.

### The One Definition Rule (ODR)

1. Every entity may be **declared** many times.
2. Every entity must be **defined exactly once** in the whole program...
3. ...**except** for `inline` functions, `inline` variables, templates, and class
   definitions, which may appear in every TU — but every copy must be *token-for-
   token identical*.

This is why:

```cpp
// header.h
int  counter = 0;                   // ❌ ODR violation if included twice
inline int counter2 = 0;            // ✅ C++17 inline variable — one shared copy
extern int counter3;                // ✅ declaration; define once in a .cpp
constexpr int MAX = 100;            // ✅ implicitly inline-ish (internal linkage)

void f() { }                        // ❌ duplicate symbol
inline void g() { }                 // ✅ inline = "linker, dedupe this"
template <class T> void h(T) { }    // ✅ templates are implicitly OK
class Widget { void m() {} };       // ✅ methods defined in-class are implicitly inline
```

**`inline` never meant "inline this function."** It means "this definition may
appear in multiple TUs; linker, pick one." The optimiser decides actual inlining
on its own.

**ODR violations are UB and often silent.** If two TUs define the same class with
different members (e.g. one compiled with `-DDEBUG` adding a field), the linker
picks one arbitrarily and you get memory corruption. This is why build flags
must be consistent across a project.

---

## Where do I put things? (the practical rules)

| Thing | Header | .cpp |
|---|---|---|
| class definition | ✅ | |
| method **declaration** | ✅ | |
| method **definition** | only if `inline`/template/in-class | ✅ preferred |
| free function declaration | ✅ | |
| free function definition | only if `inline` | ✅ |
| `template <...>` anything | ✅ **must be in the header** | ❌ |
| `constexpr`/`consteval` fn | ✅ (needs to be visible) | |
| global constant | `inline constexpr` ✅ | |
| global mutable variable | `extern` decl ✅ | definition ✅ (but: don't) |
| `static` free function | | ✅ (internal linkage, TU-private) |
| anonymous namespace | | ✅ (modern "static": TU-private) |

**Why templates must live in headers:** the compiler can only generate code for
`vector<Foo>` when it can see *both* the template body *and* `Foo`. If the body
is in another TU, it never gets instantiated → linker error. (Escape hatch:
explicit instantiation in a `.cpp` — `template class MyVec<int>;` — used when you
want to hide the body and only support a fixed set of types.)

---

## Linkage and storage — the four-word vocabulary

- **External linkage**: visible to other TUs. Default for non-`const`,
  non-`static` namespace-scope things.
- **Internal linkage**: TU-private. Achieved with `static` at namespace scope, or
  (preferred) an **anonymous namespace**:
  ```cpp
  namespace { int helper_counter = 0; void helper() {} }   // private to this TU
  ```
- **No linkage**: locals.
- **Module linkage** (C++20): the modern replacement for all of this.

---

## Static vs shared libraries

```sh
# Static library: an archive of .o files. Copied INTO your binary at link time.
ar rcs libmath.a add.o mul.o
clang++ main.o libmath.a -o app          # or -L. -lmath

# Shared library: loaded at runtime by the dynamic linker.
clang++ -dynamiclib -fPIC add.cpp mul.cpp -o libmath.dylib   # Linux: -shared, .so
clang++ main.cpp -L. -lmath -o app
otool -L app                             # Linux: ldd app
DYLD_LIBRARY_PATH=. ./app                # Linux: LD_LIBRARY_PATH
```

| | Static (`.a`) | Shared (`.dylib`/`.so`) |
|---|---|---|
| Binary size | large | small |
| Startup | fast | dynamic symbol resolution |
| Calls | direct, inlinable with LTO | through the PLT (an indirect jump) |
| Updates | relink everything | swap the file |
| ABI stability | irrelevant | **critical and hard** |

**The C++ ABI problem** (worth knowing, it bites everyone eventually): C++ has no
standard ABI. Changing a private member, adding a virtual function, or changing
a template default *changes your class's layout* and silently breaks every
already-compiled caller. This is why:
- Library boundaries are often `extern "C"` with opaque handles.
- You cannot mix libraries built with libstdc++ and libc++.
- Debug and release STL builds are incompatible (different `_ITERATOR_DEBUG_LEVEL`,
  different `std::string` layouts).
- `std::string`/`std::vector` should not cross a shared library boundary unless
  both sides are built identically.

---

## Optimisation levels — what they actually mean

| Flag | Meaning |
|---|---|
| `-O0` | none. Every variable in memory, every call real. Fast builds, correct debugging. **All benchmarks at -O0 are lies.** |
| `-O1` | basic: dead code, simple inlining |
| `-O2` | **the production default.** Full inlining, vectorisation, loop transforms |
| `-O3` | more aggressive inlining/unrolling. Sometimes slower (I-cache pressure). Measure. |
| `-Os` | optimise for size. Often *faster* than -O3 on large codebases (I-cache!) |
| `-Ofast` | -O3 + `-ffast-math`. **Breaks IEEE FP semantics.** Never in finance/science. |
| `-g` | debug info. Costs nothing at runtime. **Always ship with `-g`** (then strip separately). |
| `-march=native` | use this CPU's full instruction set. Not portable to other machines. |
| `-flto` | link-time optimisation: cross-TU inlining. 5-20% typical. |
| `-fno-exceptions -fno-rtti` | smaller/faster; breaks parts of the stdlib. Common in games/embedded. |
| `-fsanitize=address` | ASan: use-after-free, overflow, leaks. ~2x slower. |
| `-fsanitize=undefined` | UBSan: signed overflow, bad shifts, misalignment. |
| `-fsanitize=thread` | TSan: data races. ~10x slower. **Run your tests under it.** |

**Debug and release are different programs.** Bugs that appear only in release
are usually (a) UB the optimiser exploited, (b) uninitialised memory that
happened to be zero at -O0, or (c) a data race. Sanitizers find all three.

---

## CMake, minimally

You have no `cmake` installed yet (`brew install cmake`). When you need it:

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # generates compile_commands.json for clangd

add_library(core STATIC src/engine.cpp src/alloc.cpp)
target_include_directories(core PUBLIC include)      # PUBLIC = propagates to users
target_compile_options(core PRIVATE -Wall -Wextra)   # PRIVATE = doesn't propagate

add_executable(myapp src/main.cpp)
target_link_libraries(myapp PRIVATE core)
```

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build -j
```

The whole mental model of **modern** CMake: *everything is a target, and you
attach properties (includes, defines, flags, dependencies) to targets with
`PUBLIC`/`PRIVATE`/`INTERFACE`.* Never use the old `include_directories()` /
`link_libraries()` global commands. Anything you read from before ~2016 is wrong.

Dependency management: `vcpkg` or `Conan`, or CMake's `FetchContent` for small
things. There is no Maven Central. This is genuinely worse than Java. Accept it
and move on.

---

## Reading a build error

**Compiler error** → a problem inside one TU: syntax, types, missing declaration.
**Linker error** → a problem *between* TUs: missing definition, duplicate
definition, ABI mismatch.

Template errors: read from the **bottom up**, find the first line mentioning
*your* file, and ignore the 400 lines of instantiation backtrace.
`-fconcepts-diagnostics-depth=1` and using `concepts` instead of raw SFINAE turns
novels into sentences.

Next: `notes/03-memory-layout.md`.
