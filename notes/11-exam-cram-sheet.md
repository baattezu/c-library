# 11 — The cram sheet

> Everything that matters, compressed. Read it before you start and again at the
> end. If you can explain every line here, you can write and review C++.

---

## The 10 sentences

1. **A variable IS the object**, not a handle to it. Assignment copies; you opt
   into references.
2. **Destructors run deterministically at scope exit** — that's RAII, and it
   replaces `finally`, GC, and most of your bug budget.
3. **Ownership is a design decision you make explicitly**: value, `unique_ptr`,
   `shared_ptr`, or borrow. Raw pointers and references **never own**.
4. **`std::move` is a cast**, not a move. It costs nothing and moves nothing;
   it just selects the `&&` overload.
5. **Templates generate code**; Java generics erase types. That's why C++
   abstractions can be zero-cost and why the errors are terrible.
6. **Undefined behaviour is not "wrong output"** — the optimiser assumes it
   can't happen and deletes your code accordingly.
7. **The machine is memory-bound, not compute-bound.** One DRAM access ≈ 1000
   instructions. Layout beats cleverness.
8. **`virtual` costs 8 bytes per object and blocks inlining.** Use it at coarse
   granularity, never in a hot inner loop.
9. **The fastest synchronisation is no sharing.** Shard by core, then combine.
10. **There is no networking, JSON, HTTP, or async in the standard library.**
    You adopt a library or you build a runtime.

---

## Memory & lifetime

```
Storage: automatic (stack, free) | static | thread_local | dynamic (heap, you own it)
Stack:   8 MB main thread (macOS), 512 KB per non-main thread. Allocation = free.
Heap:    ~25-100 ns per malloc. Can fragment permanently (no compacting GC).
```

**Rule of Zero** — write none of the six special members; compose from
self-managing types. **Rule of Five** — if you write one of
`~T`, copy-ctor, copy-assign, you need all five.

⚠️ **Declaring a destructor silently suppresses the implicit MOVE operations.**
Your class becomes copy-only, with no diagnostic. Add
`T(T&&)=default; T& operator=(T&&)=default;` or don't declare the destructor.

⚠️ **Mark every move ctor/assign `noexcept`.** `std::vector` uses
`move_if_noexcept` on reallocation: without `noexcept` it **copies** instead.

**Copy elision:** `return T{...}` is guaranteed elision (C++17); `return t;` for
a named local gets NRVO. **Never write `return std::move(t);`** — it defeats it.

**Value categories:** lvalue (has identity) · prvalue (temporary being born) ·
xvalue (identity + movable). `T&`←lvalues, `const T&`←everything, `T&&`←rvalues.
**A named rvalue reference is an lvalue** — `std::move` it again to forward it.

**`std::move` vs `std::forward`:** `move` on a real `T&&`; `forward<T>` on a
*deduced* `T&&` (a forwarding reference).

---

## Parameter passing

| Intent | Signature |
|---|---|
| read a small trivially-copyable type (≤16 B) | `T` |
| read a string | `std::string_view` |
| read a contiguous range | `std::span<const T>` |
| read anything else | `const T&` |
| mutate the caller's object | `T&` |
| take ownership (sink) | `T` then `std::move` |
| forward exactly, in a template | `T&&` + `std::forward<T>` |
| return a big object | **by value** |

---

## Containers

| | access | insert/erase | layout |
|---|---|---|---|
| `array<T,N>` | O(1) | — | contiguous, stack, zero overhead |
| `vector<T>` | O(1) | O(1) amort. back / O(n) middle | contiguous, 1 alloc, 2x growth |
| `deque<T>` | O(1) | O(1) both ends | chunked |
| `list<T>` | O(n) | O(1) with iterator | scattered nodes, ~32 B/elem |
| `map`/`set` | O(log n) | O(log n) | RB tree, ~48 B/elem, 1 malloc each |
| `unordered_*` | O(1)~ | O(1)~ | buckets of nodes, 1 malloc each |
| `flat_map` (C++23) | O(log n) | O(n) | **two vectors** — cache-friendly |

**Measured (n=200k, this machine):** `unordered_map` lookups ~10-15x faster than
`map`; a sorted vector ~1.5-2x faster than `map` at 1/4 the memory.
**`std::vector` is the answer ~90% of the time.**
**`std::list` is almost never the answer** — measured 687x slower to traverse
than a flat array at the same big-O.
⚠️ `std::unordered_map` is mandated node-based; use `absl::flat_hash_map` or
`boost::unordered_flat_map` in hot paths (2-5x).
⚠️ `vector<bool>` is a bit-packed proxy type. Avoid.

**`reserve()`** when you know the size. **`clear()` keeps capacity** (reuse it).
**`emplace_back`** constructs in place; **`push_back`** moves a temporary in.
**`at()` throws; `operator[]` is unchecked UB.**

---

## Numbers to know

```
L1 ~1ns · L2 ~4ns · L3 ~15ns · DRAM ~80-100ns (≈1000 instructions)
cache line: 64 B on x86, 128 B on Apple Silicon
branch mispredict ~15-20 cycles     mutex uncontended ~20ns / contended ~1µs+
atomic uncontended ~5-15ns          CONTENDED cache-line bounce ~100ns
malloc ~25-100ns                    syscall ~100ns-1µs
context switch ~1-3µs + cold cache  thread stack 512KB (macOS) / 8MB (Linux)
SSD read ~50-150µs                  same-DC RTT ~500µs   cross-region ~50ms
```

**Measured in this repo, all at identical big-O:**
random vs sequential access **45x** · linked list vs array **687x** ·
AoS vs SoA **3.5x** · halving a struct **1.9x** · false sharing **52x** ·
arena vs malloc **5.4x** · move vs copy a `vector<string>` **~1900x** ·
one buffered write vs 20k small writes **~5000x**

---

## OOP

- `virtual` adds an 8-byte vptr per object and blocks inlining. `sizeof` goes
  from 4 → 16 for a one-`int` class.
- **A polymorphic base needs a `virtual` destructor** (or a protected
  non-virtual one). Otherwise `delete base_ptr` is UB.
- **`override` always. `final` where you can** (enables devirtualisation).
- **Slicing:** `Base b = derived;` copies only the base part, silently. Store
  polymorphic objects as `unique_ptr<Base>`; pass as `const Base&`.
- Virtual call in a constructor calls the **base** version (Java calls derived).
- Alternatives to `virtual`, in hot-path preference order: don't be polymorphic
  → `std::variant` + `visit` → templates/CRTP → virtual at coarse granularity.
- `class` vs `struct`: only default access differs. `class` = has an invariant.
- `auto operator<=>(const T&) const = default;` gives you all six comparisons.

---

## Concurrency

```
C++ volatile ≠ Java volatile.   Java volatile == std::atomic<T> (seq_cst).
A data race in C++ is UNDEFINED BEHAVIOUR, not a stale read.
```

- `lock_guard` (one mutex) · `scoped_lock` (several, deadlock-free) ·
  `unique_lock` (flexible, required by `condition_variable`) · `shared_lock`
- `cv.wait(lock, predicate)` — **always** the predicate overload (spurious wakeups)
- **Orderings:** `relaxed` (atomicity only) · `acquire`/`release` (publish/consume)
  · `seq_cst` (default, global order) · never `consume`
- **The publish pattern:** plain writes, then `flag.store(true, release)`;
  reader does `flag.load(acquire)` then reads the plain data. One atomic carries
  the ordering for everything.
- Deadlock: impose a **lock order**; never call unknown code under a lock; keep
  critical sections tiny.
- ⚠️ x86 is strongly ordered, **ARM is not**. Broken code often works on x86 and
  fails on ARM. Your laptop and Graviton are both ARM.
- **Bound your queues** (backpressure) or you OOM.
- Run `-fsanitize=thread`.

---

## Compilation

- `#include` is **textual paste**. `<vector>` is ~40k lines, recompiled in every
  TU that includes it.
- One TU at a time, in total isolation. Compiler error = inside a TU.
  **Linker error = between TUs** (missing/duplicate definition, ABI mismatch).
- **ODR:** define exactly once, except `inline` functions/variables, templates,
  and class definitions — and every copy must be identical.
- **`inline` means "linker, dedupe this"**, not "inline this function".
- **Templates must live in headers** (the compiler needs the body to instantiate).
- Forward-declare aggressively; PIMPL at library boundaries.
- `-O0` for debugging, `-O2` for production, `-O2 -g` always (debug info is free
  at runtime), `-flto` for release.

---

## Error handling

| Situation | Use |
|---|---|
| programmer error / broken invariant | `assert` / abort. Don't make it recoverable. |
| expected failure with a reason | `std::expected<T,E>` |
| expected absence, one failure mode | `std::optional<T>` |
| deep, rare, unrecoverable | exceptions |
| constructor failure | exceptions (or a static factory returning `expected`) |
| OS boundary | error codes → wrap at the boundary |

Catch by `const&`. No `finally` — RAII replaces it. Destructors must not throw.
Not throwing costs zero; throwing costs microseconds.

---

## The 12 mistakes Java developers make in month one

1. Passing big objects by value (`std::vector<T> v` copies the whole thing)
2. `for (auto x : c)` — copies every element. Write `const auto&`.
3. Using `new` at all. Use `make_unique`, or a value.
4. Returning a reference or pointer to a local
5. Storing a `string_view`/iterator/reference whose owner may die or reallocate
6. Expecting `auto` to keep references — `auto x = v[0]` copies
7. Deep virtual hierarchies and getter/setter classes by reflex
8. `unordered_map` in a hot loop
9. Forgetting to `join()` a thread → `std::terminate`
10. Expecting bounds checks (`v[i]` never checks)
11. Writing a destructor and silently losing move semantics
12. Treating a data race as "just a stale value"

---

## The five questions to ask about any C++ code

1. **Who owns this memory, and when does it die?**
2. **What happens if this throws here?**
3. **Is this a copy?** (and did I mean it to be?)
4. **What does this touch in memory, and in what order?**
5. **Which thread runs this, and what does it share?**

---

## The commands

```sh
make run  F=file.cpp     # -O0 -g, correctness
make asan F=file.cpp     # Address+UB sanitizer — use constantly
make leaks F=file.cpp    # macOS leak check (LeakSanitizer is unavailable here)
make opt  F=file.cpp     # -O2 -march=native — REQUIRED for any benchmark
make asm  F=file.cpp     # see the actual machine code

clang++ -fsanitize=thread -O1 -g f.cpp        # data races
clang++ -O2 -Rpass-missed=loop-vectorize f.cpp # why didn't it vectorise?
clang++ -Xclang -fdump-record-layouts -c f.cpp # exact struct layout
clang++ -ftime-trace f.cpp                     # why is the build slow?
perf stat -e cycles,instructions,cache-misses ./prog   # Linux: IPC & misses
sample <pid> 10                                 # macOS: quick profile
nm -C binary | c++filt                          # demangled symbols
```

---

Now go build something. `09-projects/` is where the learning consolidates.
