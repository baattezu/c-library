# 10 — Undefined behaviour: the complete catalogue

> The runnable version is `02-memory/09-dangling-and-ub.cpp`. This is the
> reference list — read it once end to end, then use it as a checklist.

---

## What UB actually means

The standard imposes **no requirements**. Not "it crashes", not "it returns
garbage" — *no requirements at all*.

The consequence that matters is not the crash. It's this:

> **The optimiser is entitled to assume UB never happens, and to transform your
> code on that assumption.**

```cpp
int f(int* p) {
    int x = *p;              // dereference implies p != nullptr...
    if (p == nullptr) return -1;   // ...so the compiler DELETES this check
    return x;
}
```
Your null check is gone. This is not a compiler bug; it's the contract. Real
CVEs have been created exactly this way.

**This is the single biggest difference from Java**, where every operation is
defined (NPE, `ArrayIndexOutOfBoundsException`, arithmetic wraps) and the JVM is
a memory-safe sandbox.

---

## The catalogue

### Memory
- dereferencing a **null**, **dangling**, or **uninitialised** pointer
- **out-of-bounds** access — even merely *computing* `arr + n + 1` is UB, let
  alone reading it. No bounds checking exists.
- **use after free**, **double free**
- mismatched forms: `new`/`delete[]`, `new[]`/`delete`, `malloc`/`delete`,
  `new`/`free`
- reading an **uninitialised** variable
- **misaligned** access
- **strict aliasing** violations — reading bytes through the wrong type.
  Use `std::memcpy` or `std::bit_cast`, never a `reinterpret_cast` dereference.
- modifying a **string literal** or any `const` object (via `const_cast`)
- exceeding **object lifetime**: using storage before the constructor or after
  the destructor

### Arithmetic
- **signed** integer overflow (unsigned wrapping is *defined*)
- division or modulo by zero
- shifting by `>=` the bit width, or left-shifting a negative value
- converting a float to an integer type that can't represent it
- `INT_MIN / -1`

### Objects and lifetime
- deleting through a base pointer with **no virtual destructor**
- calling a **pure virtual** from a constructor or destructor
- falling off the end of a value-returning function
- infinite loop with no side effects (yes, really — it lets the compiler assume
  forward progress)
- self-move that leaves the object broken
- returning a reference or pointer to a local

### Concurrency
- a **data race**: two threads, same location, ≥1 write, no synchronisation
- destroying a joinable `std::thread` → `std::terminate` (defined, but fatal)
- a `noexcept` function that throws → `std::terminate`
- a destructor that throws during unwinding → `std::terminate`

### Program structure
- violating the **One Definition Rule** across translation units — often silent,
  and a classic cause of mysterious corruption when build flags differ between
  TUs

---

## The subtle ones people miss

**Iterator/reference invalidation.** The C++ analogue of
`ConcurrentModificationException`, except it doesn't throw:

| Container | Invalidated by |
|---|---|
| `vector` | any growth invalidates **everything**; `erase` invalidates from that point on |
| `deque` | middle insert/erase: all iterators; at the ends: iterators but **not references** |
| `list`/`forward_list` | only the erased element |
| `map`/`set` | only the erased element (node-based, stable addresses) |
| `unordered_*` | rehash invalidates **iterators** but **not** references/pointers |

**Dangling `string_view` / `span`.** They borrow. `std::string_view sv =
make_string();` dangles at the semicolon.

**Lambda capturing by reference and outliving the scope.** The async version of
this is the #1 real-world modern-C++ bug. `[=]` in a member function captures
`this`, not a copy of the object.

**`std::sort` with a non-strict-weak-ordering comparator.** Using `<=` instead
of `<` is UB and really does crash or loop forever in libstdc++.

**`std::move` on a `const` object.** Not UB, but a silent pessimisation: it
produces `const T&&`, which binds to `const T&`, so you get a **copy**.

**Virtual call in a constructor.** Not UB (unless pure), but it calls the *base*
version — the opposite of Java. A genuine portability trap.

---

## Your defences, in order of value

### 1. Types
Use `std::string`, `std::vector`, `std::span`, `std::string_view`,
`std::unique_ptr`, `std::optional`, `std::expected`. Most UB is *unreachable* if
you never touch raw memory. This is the highest-leverage defence by far.

### 2. Sanitizers — run these on every test
```sh
# Address + UB. The Makefile's `make asan` target.
clang++ -fsanitize=address,undefined -fno-sanitize-recover=all -O1 -g f.cpp

# Data races. Separate build (incompatible with ASan). ~10x slower. Worth it.
clang++ -fsanitize=thread -O1 -g f.cpp

# Uninitialised reads. Linux/clang only — NOT available on macOS.
clang++ -fsanitize=memory -O1 -g f.cpp
```

**Platform note for this machine:** LeakSanitizer is **not supported on
macOS/arm64** — setting `ASAN_OPTIONS=detect_leaks=1` makes the program abort at
startup. Use Apple's tool instead:
```sh
make leaks F=path/to/file.cpp
# which runs: MallocStackLogging=1 leaks --atExit -- ./binary
```
On Linux, ASan detects leaks by default.

### 3. Warnings as errors
```sh
-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Werror
```

### 4. Hardened standard library
```sh
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE   # libc++
-D_GLIBCXX_ASSERTIONS                                       # libstdc++
```
Makes `operator[]`, iterator ops, and more bounds-checked **even in release**,
for a small cost. Google and others ship with this on. Strongly recommended.

### 5. Static analysis
`clang-tidy` with `bugprone-*`, `cppcoreguidelines-*`, `performance-*`.
And the compiler's own `--analyze`.

### 6. Fuzzing
Anything that parses untrusted input: `-fsanitize=fuzzer,address`. Non-negotiable
for a network protocol parser.

### 7. Hardening flags for production
```sh
-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -Wl,-z,relro,-z,now
-fcf-protection            # x86 control-flow integrity
-mbranch-protection=standard  # arm64 pointer authentication + BTI
```

---

## The honest position on C++ safety

C++ is memory-unsafe by default. That is a real, serious property, and it's why
Rust exists and why CISA and the NSA publish memory-safety guidance naming C++.

But the gap between "C++ code" and "safe C++ code" is mostly **tooling and
discipline**, and the tooling is genuinely good:

- Use the standard library types. Never raw `new`/`delete`, never raw owning
  pointers, never C arrays.
- Answer, for every pointer: *who owns this and when does it die?*
- Run ASan + UBSan on every test, TSan on concurrency tests, in CI.
- Turn on hardened mode and warnings-as-errors.
- Fuzz your parsers.

Do those five and you eliminate the large majority of the risk. The C++ Core
Guidelines and the ongoing "profiles" work in the committee are attempts to make
this the default rather than a choice.

What you *cannot* get is Rust's compile-time proof. You get very good dynamic
detection instead. Know the difference, and choose deliberately.

Next: `notes/11-exam-cram-sheet.md`.
