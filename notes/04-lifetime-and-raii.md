# 04 — Lifetime, RAII, and ownership

> If you only truly learn one thing from C++, learn this. RAII is the idea that
> Rust's borrow checker was invented to *enforce*, that Python's `with` and
> Java's `try-with-resources` are weak imitations of, and that makes
> deterministic, GC-free, leak-free systems programming possible.

---

## The core idea

> **A resource's lifetime is tied to the lifetime of an object on the stack.
> Acquire in the constructor. Release in the destructor.**

Because C++ *guarantees* that a stack object's destructor runs when its scope
exits — on the normal path, on an early `return`, on a `break`, and during
exception unwinding — you cannot forget to release. There is no path out of the
scope that skips the destructor.

```cpp
{
    std::lock_guard<std::mutex> lock(mtx);   // acquire
    if (bad) return;                          // released
    if (worse) throw std::runtime_error("");  // released
    do_work();                                // released at the brace
}
```

Compare Java:
```java
lock.lock();
try { ... } finally { lock.unlock(); }   // you must write this. Every time.
```
And Java's version only works for things you remember to wrap.

**Anything with an "undo" belongs in a destructor:** memory, file descriptors,
sockets, mutexes, database transactions, GPU buffers, timers, thread joins,
temp files, `epoll` registrations, refcount decrements, tracing spans.

---

## The lifecycle of an object

```
1. Storage allocated (stack bump / operator new / static area)
2. Constructor runs
     a. base class constructors, in declaration order
     b. member constructors, IN MEMBER DECLARATION ORDER (not init-list order!)
     c. the constructor body
   ← the object's lifetime BEGINS here
3. ... object is used ...
   ← lifetime ENDS here
4. Destructor runs
     a. the destructor body
     b. members destroyed in REVERSE declaration order
     c. base destructors in reverse order
5. Storage released
```

Two rules people get wrong:
- **Member initialisation order follows declaration order in the class, not the
  order you wrote in the member-init list.** Clang warns (`-Wreorder`). Get the
  declaration order right.
- **Prefer the member-init list over assignment in the body.** The body runs
  *after* members are already default-constructed, so assigning there means
  construct-then-assign (two operations) instead of construct-once.

```cpp
class Conn {
    std::string host_;      // declared first  -> constructed first
    int         port_;
public:
    Conn(std::string h, int p)
        : host_(std::move(h)), port_(p) {}   // ✅ direct initialisation
    // Conn(std::string h) { host_ = h; }    // ❌ default-construct then assign
};
```

---

## The Rule of Zero / Three / Five

There are six **special member functions**:
```cpp
class T {
    T();                          // default constructor
    ~T();                         // destructor
    T(const T&);                  // copy constructor
    T& operator=(const T&);       // copy assignment
    T(T&&) noexcept;              // move constructor
    T& operator=(T&&) noexcept;   // move assignment
};
```

### ⭐ Rule of Zero (what you should almost always do)
> **Write none of them.** Compose your class out of members that already manage
> themselves (`std::string`, `std::vector`, `std::unique_ptr`) and the compiler
> generates all six correctly.

```cpp
class Session {
    std::string          id_;
    std::vector<char>    buffer_;
    std::unique_ptr<TLS> tls_;
    // no destructor, no copy, no move written. All six are correct.
    // (It's automatically move-only, because unique_ptr is move-only. Perfect.)
};
```

### Rule of Three / Five (when you're the one managing the resource)
> If you write **any** of destructor / copy ctor / copy assign, you almost
> certainly need **all three** — and in modern C++, the two move operations too.

Because: if your destructor frees something, the default *copy* will copy the
pointer, and then **both objects free it → double free**.

### The generation rules (memorise the two that bite)
- **Declaring a destructor suppresses the implicit MOVE operations.** Your class
  silently becomes copy-only, and every "move" becomes a deep copy. This is a
  classic silent performance bug.
- Declaring a move operation deletes the implicit copy operations.
- Declaring any constructor suppresses the implicit default constructor.

Use `= default` and `= delete` to be explicit:
```cpp
class Buffer {
public:
    ~Buffer();
    Buffer(const Buffer&)            = delete;   // non-copyable
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) noexcept;                   // movable
    Buffer& operator=(Buffer&&) noexcept;
};
```

---

## Ownership: the vocabulary that ends 90% of C++ bugs

Every pointer/reference in your program has exactly one of these roles.
**Say it out loud for every pointer you write.**

| Type | Meaning | Cost |
|---|---|---|
| `T` (value) | I *am* it. Sole owner, lives inline. | free |
| `std::unique_ptr<T>` | I am the **sole owner**; it's on the heap; move-only. | zero over a raw ptr |
| `std::shared_ptr<T>` | **Shared ownership**, refcounted. Dies when the last one dies. | 16 bytes + atomic refcount |
| `std::weak_ptr<T>` | I observe a shared_ptr **without keeping it alive**. Must `.lock()`. | breaks cycles |
| `T&` / `const T&` | I **borrow** it. It outlives me. Cannot be null. | free |
| `T*` (raw) | I **borrow** it, and it may be null. **I never delete it.** | free |
| `std::span<T>` / `string_view` | I borrow a **range**. Owner outlives me. | free |

**The rule that makes C++ safe:**
> **Raw pointers and references NEVER own. Owning is done by value types and
> smart pointers, only.**

If you follow that one rule, `delete` disappears from your codebase and so do
leaks, double frees, and most use-after-frees.

### unique_ptr vs shared_ptr — choose unique
`shared_ptr` is *not* the default. It costs:
- 16 bytes (object ptr + control block ptr)
- an **atomic** increment/decrement on every copy — a synchronised operation that
  bounces cache lines between cores
- a second allocation, unless you use `make_shared` (which fuses them)
- and worst: **unclear ownership**. "Who owns this?" → "…everyone?" is a design
  smell, not an answer.

Reach for `shared_ptr` when lifetime genuinely is dynamic and shared (a cache
entry being used by in-flight requests; a node in a graph with no clear owner).
Otherwise `unique_ptr` — and it converts to `shared_ptr` for free if you're wrong.

`weak_ptr` exists because **`shared_ptr` cycles leak**. Refcounting cannot collect
cycles; a tracing GC can. Parent→child = `shared_ptr`, child→parent = `weak_ptr`.

---

## Lifetime bugs, catalogued

### 1. Dangling reference to a local
```cpp
const std::string& bad() { std::string s = "x"; return s; }   // ☠️
```

### 2. Dangling into a reallocated container
```cpp
auto& first = v[0];
v.push_back(x);        // may reallocate
first;                 // ☠️ use-after-free
```
Which operations invalidate what:
| Container | Invalidated by |
|---|---|
| `vector` | any growth invalidates **everything**; `erase` invalidates from that point on |
| `deque` | insert/erase in the middle: all iterators; at the ends: iterators but **not references** |
| `list`/`forward_list` | only the erased element |
| `map`/`set` | only the erased element (node-based, stable) |
| `unordered_*` | rehash invalidates **iterators** but **not references/pointers** |

### 3. Dangling `string_view` / `span`
```cpp
std::string_view sv = std::string("temp");   // ☠️ temp dies at the semicolon
std::string_view f() { std::string s; return s; }   // ☠️
```

### 4. Lambda capturing by reference, outliving the scope
```cpp
std::function<void()> make() {
    int x = 42;
    return [&x]{ std::cout << x; };   // ☠️ x is gone when the lambda runs
}
```
**The async version of this is the #1 real-world C++ bug.** Any lambda posted to
a thread pool, event loop, or timer must own everything it uses:
```cpp
pool.post([data = std::move(data), self = shared_from_this()]{ ... });
```

### 5. `[=]` in a member function captures `this`, not the object
```cpp
class Server {
    std::string name_;
    void start() {
        pool.post([=]{ log(name_); });        // ☠️ captures `this`; if the Server
                                              //    dies, name_ dangles
        pool.post([self = shared_from_this()]{ log(self->name_); });  // ✅
        pool.post([*this]{ log(name_); });     // ✅ C++17: copies the object
    }
};
```

### 6. Returning a reference to a member of a temporary
```cpp
auto& x = getConfig().timeouts;   // ☠️ if getConfig() returns by value
```

### 7. Self-move / self-assignment
```cpp
v = std::move(v);            // must not corrupt. Guard, or use copy-and-swap.
```

---

## Exception safety guarantees (the vocabulary)

| Guarantee | Meaning |
|---|---|
| **No-throw** (`noexcept`) | never throws. Required of destructors, swap, and move ops. |
| **Strong** | if it throws, the state is unchanged (commit-or-rollback). `vector::push_back` gives this. |
| **Basic** | if it throws, everything is still valid and destructible, but state may have changed. **This is the minimum you must always provide.** |
| **None** | UB on throw. Unacceptable. |

**Copy-and-swap** gives you the strong guarantee almost for free:
```cpp
T& operator=(T other) {       // by value: the copy/move already happened
    swap(*this, other);       // swap is noexcept
    return *this;             // `other` (the old state) dies here
}
```

**`noexcept` on moves is not optional.** `std::vector` uses
`std::move_if_noexcept` when reallocating: if your move constructor might throw,
it cannot maintain the strong guarantee, so it **copies instead**. Forgetting
`noexcept` silently turns O(1) moves into O(n) copies during every vector growth.

---

## Writing your own RAII wrapper

The pattern, in full:

```cpp
class FileDescriptor {
    int fd_ = -1;
public:
    explicit FileDescriptor(const char* path, int flags)
        : fd_(::open(path, flags)) {
        if (fd_ < 0) throw std::system_error(errno, std::system_category(), path);
    }
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }

    FileDescriptor(const FileDescriptor&)            = delete;   // an fd has one owner
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    FileDescriptor& operator=(FileDescriptor&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) ::close(fd_); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }

    int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { int t = fd_; fd_ = -1; return t; }
};
```

Note every piece: **null state** (`-1`) so a moved-from object is destructible,
**deleted copies**, **noexcept moves that null the source**, **self-assignment
guard**, and `release()` for handing ownership to a C API.

Runnable version in `02-memory/03-raii.cpp` and `08-systems/01-syscalls-and-fds.cpp`.

### Generic scope guard (the poor man's `finally`)
```cpp
template <class F> struct ScopeExit {
    F f; bool active = true;
    ~ScopeExit() { if (active) f(); }
    void dismiss() { active = false; }
};
template <class F> ScopeExit(F) -> ScopeExit<F>;   // CTAD

// usage:
auto guard = ScopeExit{[&]{ std::cout << "always runs\n"; }};
```

---

## Summary card

- Destructors run **deterministically** at scope exit, including on exceptions.
- Rule of Zero: compose from self-managing members and write nothing.
- If you write a destructor, you need copy+move (Rule of Five) or `= delete`.
- **Declaring a destructor kills your implicit move operations.**
- `unique_ptr` by default; `shared_ptr` only for genuinely shared lifetime;
  `weak_ptr` to break cycles.
- **Raw pointers/references never own.**
- Mark moves, swap, and destructors `noexcept`.
- Every lambda that outlives its scope must own its captures.
- Ask, always: *who owns this, and when does it die?*

Next: `notes/05-value-semantics-and-move.md`.
