# 05 — Value semantics, copies, and move semantics

> Move semantics is the feature that made modern C++ fast *and* safe. It's also
> the feature Java developers most consistently misunderstand, because Java's
> assignment is always a cheap pointer copy so there was never anything to learn.

---

## Copies are real and they cost real money

```cpp
std::vector<std::string> v = load_million_strings();

auto a = v;              // COPY: 1 allocation for the vector buffer
                         //     + up to 1M allocations for the strings
                         //     + 1M memcpys.  Milliseconds.
auto b = std::move(v);   // MOVE: copy 24 bytes (3 pointers). Nanoseconds.
                         //       v is now empty.
```

The compiler will *never* warn you about the first line. Finding accidental
copies is a core C++ skill, and the tools are: read the code, look at the
profiler, and put a `std::cout` in your copy constructor when suspicious.

---

## What a move actually is

**A move is a copy that is allowed to gut the source.**

There is no magic. A move constructor is just a constructor you write that
*steals* the source's resources instead of duplicating them, and leaves the
source in a valid-but-empty state so its destructor is harmless.

```cpp
class Buffer {
    char*       data_ = nullptr;
    std::size_t size_ = 0;
public:
    // COPY: allocate new storage, duplicate the bytes. O(n).
    Buffer(const Buffer& o) : data_(new char[o.size_]), size_(o.size_) {
        std::memcpy(data_, o.data_, size_);
    }

    // MOVE: take the pointer, null out the source. O(1). Zero allocations.
    Buffer(Buffer&& o) noexcept : data_(o.data_), size_(o.size_) {
        o.data_ = nullptr;      // ← THIS LINE IS THE ENTIRE POINT.
        o.size_ = 0;            //    Without it, both destructors free the same
    }                           //    pointer → double free.

    ~Buffer() { delete[] data_; }   // safe on nullptr
};
```

That's it. `std::move` doesn't do any of this. `std::move` is:

```cpp
template <class T>
constexpr std::remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<std::remove_reference_t<T>&&>(t);
}
```

**`std::move` is a cast. It generates zero instructions. It moves nothing.**
It just changes the expression's *value category* so overload resolution picks
the `T&&` overload. A better name would have been `std::rvalue_cast`.

---

## Value categories, one more time (this is the exam question)

Every expression has a type **and** a value category:

```
              expression
             /          \
        glvalue          rvalue
      (has identity)   (movable)
       /       \        /      \
   lvalue      xvalue          prvalue
                  \_____________/
                   (both are rvalues)
```

- **lvalue** — "has a name / you can take its address": `x`, `obj.f`, `a[i]`,
  `*p`, a function call returning `T&`.
- **prvalue** — "a temporary being born": `42`, `a+b`, `T{}`, a function call
  returning `T` by value.
- **xvalue** — "an lvalue you've marked as expendable": `std::move(x)`, a call
  returning `T&&`, `arr[i]` where arr is an rvalue array.

**Binding rules — the whole thing:**

| Parameter | binds lvalue | binds const lvalue | binds rvalue |
|---|---|---|---|
| `T&` | ✅ | ❌ | ❌ |
| `const T&` | ✅ | ✅ | ✅ |
| `T&&` | ❌ | ❌ | ✅ |
| `T&&` **in a template** | ✅ | ✅ | ✅ | ← different thing! see below |

### The rule everyone forgets
> **A named rvalue reference is an lvalue.**

```cpp
void f(Buffer&& b) {
    Buffer x = b;              // ← COPY! `b` has a name, so it's an lvalue.
    Buffer y = std::move(b);   // ← move. You must re-cast.
}
```

Because if it *were* an rvalue, then using `b` twice would silently use a
gutted object. C++ makes you say "yes, gut it" explicitly, once, at the last use.

---

## Forwarding references (a.k.a. "universal references")

`T&&` means "rvalue reference" **except** when `T` is a template parameter being
deduced — then it's a *forwarding reference* that binds to anything:

```cpp
template <class T>
void wrapper(T&& arg) {              // forwarding reference (T is deduced)
    inner(std::forward<T>(arg));     // preserve the original value category
}

void f(Widget&& w);                  // plain rvalue reference (no deduction)
auto&& x = expr;                     // forwarding reference
```

**Reference collapsing** makes it work: `T& &` → `T&`, `T& &&` → `T&`,
`T&& &` → `T&`, `T&& &&` → `T&&`. ("An lvalue reference wins.")
- Called with an lvalue → `T` deduces to `Widget&` → parameter is `Widget& &&` = `Widget&`
- Called with an rvalue → `T` deduces to `Widget` → parameter is `Widget&&`

**`std::forward<T>(x)`** is a conditional cast: it casts to rvalue *only if* `T`
was deduced as a non-reference. It is how `emplace_back`, `make_unique`, and
every wrapper in the standard library preserve your arguments exactly.

> Mnemonic: **`std::move` on a real rvalue reference. `std::forward` on a
> forwarding reference.** Never mix them up — `std::move` on a forwarding
> reference will steal from an lvalue the caller still owns.

---

## The moved-from state

After `auto b = std::move(a);`, what is `a`?

> The standard says: **valid but unspecified**.

Meaning:
- ✅ You may destroy it. (Always.)
- ✅ You may assign a new value to it. (Always.)
- ✅ You may call any function with no precondition (`clear()`, `size()`, `empty()`).
- ❌ You must not assume *what* value it has.
- ❌ You must not call anything with a precondition (`front()` on an empty vector).

In practice: libc++'s moved-from `std::string`/`std::vector` are empty. But
`std::optional` moved-from **still has a value** (an empty-ish one) — it does not
become `nullopt`. Don't rely on it either way.

**Best practice: don't touch a moved-from object except to reassign or let it die.**

---

## Copy elision — why returning by value is free

```cpp
std::vector<int> make() {
    std::vector<int> v(1'000'000);
    return v;                          // no copy, no move. Zero.
}
auto x = make();                       // v was constructed IN x's storage.
```

- **Guaranteed copy elision (C++17)**: for prvalues (`return T{...}`), the
  standard *mandates* that no copy or move happens. The object is constructed
  directly in the destination. This is a language rule, not an optimisation —
  it works even for non-copyable, non-movable types.
- **NRVO** (named return value optimisation): returning a named local like `v`.
  Technically optional but implemented by every real compiler. Falls back to a
  *move*, never a copy.

**So: return big objects by value. It is idiomatic, correct, and free.**

### The one thing that breaks it
```cpp
return std::move(v);     // ❌ NEVER. Turns a free elision into a real move.
```
`std::move` makes the return expression an xvalue, which disqualifies NRVO.
Same for `return (v);`. Just `return v;`.

(Exception: `return std::move(member_)` inside a function where the return type
differs from the local's type, e.g. returning a base from a derived local. Rare.)

---

## Parameter passing, decided

```cpp
void read(const std::string& s);   // I only look at it
void read(std::string_view s);     //   ...and I don't need a null terminator
                                   //      or the std::string type: BETTER

void modify(std::string& s);       // I change the caller's object

void store(std::string s) {        // SINK: I will keep it
    member_ = std::move(s);
}
// caller: store(x)            -> one copy (into the parameter) + one move
//         store(std::move(x)) -> one move + one move
// This "pass by value and move" idiom is the standard modern answer.

template <class S>
void store2(S&& s) {               // PERFECT FORWARDING: zero extra operations
    member_ = std::forward<S>(s);  // but it's a template (header-only, worse errors)
}
```

**Decision procedure:**
1. Only reading, and it's a string? → `std::string_view`.
2. Only reading, and it's a range? → `std::span<const T>`.
3. Only reading, other type? Small & trivially copyable (≤16 bytes)? → by value.
   Otherwise → `const T&`.
4. Mutating the caller's object? → `T&`.
5. Storing a copy? → by value + `std::move` (sink).
6. In a hot generic path where the extra move matters? → forwarding reference.

---

## `emplace` vs `push`

```cpp
std::vector<std::string> v;
v.push_back(std::string("hi"));   // construct temp, move into vector, destroy temp
v.push_back("hi");                // same: implicit conversion creates a temp
v.emplace_back("hi");             // construct the string DIRECTLY in the vector's
                                  // storage from the char*. No temporary at all.
```

`emplace_back(args...)` perfect-forwards `args` to `T`'s constructor, in place.

Caveats:
- `emplace_back` uses **direct** initialisation, so it can call `explicit`
  constructors. `v.emplace_back(10)` on a `vector<vector<int>>` creates a vector
  of 10 elements; `push_back(10)` wouldn't compile. Sometimes that's a bug.
- For trivially-copyable small types (`int`), they're identical. Don't churn code.
- `emplace_back` returns a reference to the new element (C++17). `push_back` doesn't.

---

## `noexcept` and why it's a performance feature

```cpp
Buffer(Buffer&& o) noexcept;              // ← the noexcept is load-bearing
Buffer& operator=(Buffer&&) noexcept;
void swap(Buffer&, Buffer&) noexcept;
```

When `std::vector` reallocates, it must move `n` elements to the new buffer. If
a move throws halfway through, it can't restore the old state (the already-moved
elements are gutted) — so it couldn't offer the strong exception guarantee.

Its solution: `std::move_if_noexcept`. **If your move constructor is not
`noexcept`, vector copies instead of moving.** Silently. Every growth. O(n)
allocations instead of O(1) pointer swaps.

> Mark every move constructor, move assignment, `swap`, and destructor
> `noexcept`. `static_assert(std::is_nothrow_move_constructible_v<T>)` in your
> tests if it matters.

`noexcept` also lets the compiler omit unwind tables and enables optimisations
around the call. But **`noexcept` is a promise, not a wish**: if a `noexcept`
function throws, `std::terminate()` is called immediately. No unwinding, no
catch. Don't put it on things that can genuinely fail.

---

## Small object optimisation (SOO/SSO) — moves that aren't free

`std::string` with SSO stores short strings **inline**. Moving one therefore
cannot just steal a pointer — it must **copy the bytes**. So:
- moving a short `std::string` ≈ copying it (~24 bytes, still cheap)
- moving a long `std::string` = stealing a pointer (free)

Same for `std::function` (small captures inline), `std::any`, and most
small-vector types. Moves are cheap, but "move is always O(1)" is not a law —
it's a property of the specific type.

Types where **move == copy** (no benefit at all): all trivially-copyable types
(`int`, `Point`, `std::array<int,N>`), and any type without a move constructor.

---

## Debug checklist: "why is my code slow?"

1. Put a `std::cout << "COPY\n";` in your copy constructor. Run. Count.
2. Search for `for (auto x : ` — every one of those is a copy per element.
3. Search for functions taking `std::string` or `std::vector` by value that
   don't move from them.
4. Check every move constructor is `noexcept`.
5. Check you're not calling `std::move` on a `const` object — **`std::move` on a
   `const T` silently produces a `const T&&`, which binds to `const T&`, which
   is a COPY.** This is a completely silent pessimisation.
   ```cpp
   const std::string s = "...";
   std::string t = std::move(s);   // ❌ COPIES. const kills the move.
   ```
6. Check for `return std::move(x)`.
7. Check `reserve()` before loops that `push_back`.

Next: `notes/06-templates-and-compile-time.md`, or jump into `02-memory/`.
