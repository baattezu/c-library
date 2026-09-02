// ===========================================================================
//  PLACEMENT NEW, MANUAL LIFETIME, AND YOUR FIRST ARENA ALLOCATOR
//  run: make opt F=02-memory/08-placement-new-and-arena.cpp
// ===========================================================================
//
//  This is where you stop being a C++ *user* and start being able to build a
//  runtime. Everything below — vector, string, optional, variant, any,
//  small-buffer optimisation, object pools, arenas, garbage collectors, JIT
//  compilers, database page caches — is built on ONE idea:
//
//      SEPARATE "getting memory" FROM "creating an object in it."
//
//  new T   = operator new(sizeof(T))  +  construct T there
//  delete p = p->~T()                 +  operator delete(p)
//
//  Once you can do those two halves independently, you control everything.
//
// ===========================================================================

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. PLACEMENT NEW: construct an object in memory you already own
// ---------------------------------------------------------------------------
struct Loud {
    int  id;
    char pad[24];
    explicit Loud(int i) : id(i), pad{} { std::printf("    Loud(%d) ctor @ %p\n", id, (void*)this); }
    ~Loud() { std::printf("    Loud(%d) dtor\n", id); }
};

void demo_placement_new() {
    std::printf("\n=== 1. Placement new ===\n");

    // Raw, uninitialised, correctly-aligned storage on the STACK.
    // `alignas(T)` is essential — constructing an object in under-aligned
    // storage is undefined behaviour.
    alignas(Loud) unsigned char storage[sizeof(Loud) * 2];

    // Construct two Louds in that buffer. NO ALLOCATION HAPPENS.
    Loud* a = new (storage) Loud(1);
    Loud* b = new (storage + sizeof(Loud)) Loud(2);

    std::printf("    a->id=%d b->id=%d  (both live in a stack array)\n", a->id, b->id);

    // You MUST destroy manually, in the right order, and you must NOT call
    // `delete` (the memory didn't come from operator new).
    b->~Loud();
    a->~Loud();

    std::printf("  -> This is exactly what std::vector does internally: it owns\n"
                "     a raw byte buffer and placement-news elements into it, so\n"
                "     capacity can exceed size without constructing anything.\n");
}

// ---------------------------------------------------------------------------
// 2. A TINY std::optional, to prove you understand it
// ---------------------------------------------------------------------------
template <class T>
class MyOptional {
    // Raw storage, correctly sized and aligned for T. No T is constructed
    // until we say so.
    alignas(T) unsigned char storage_[sizeof(T)];
    bool has_value_ = false;

    T*       ptr()       { return reinterpret_cast<T*>(storage_); }
    const T* ptr() const { return reinterpret_cast<const T*>(storage_); }

public:
    MyOptional() = default;

    template <class... Args>
    void emplace(Args&&... args) {
        reset();
        ::new (storage_) T(std::forward<Args>(args)...);   // placement new
        has_value_ = true;
    }
    void reset() {
        if (has_value_) { ptr()->~T(); has_value_ = false; }   // explicit destructor
    }
    ~MyOptional() { reset(); }

    // Rule of five omitted for brevity — a real one needs it.
    MyOptional(const MyOptional&)            = delete;
    MyOptional& operator=(const MyOptional&) = delete;

    explicit operator bool() const { return has_value_; }
    T&       operator*()       { return *ptr(); }
    const T& operator*() const { return *ptr(); }
};

void demo_my_optional() {
    std::printf("\n=== 2. Building std::optional from placement new ===\n");
    MyOptional<std::string> o;
    std::printf("    empty: has_value=%d, sizeof=%zu (no heap allocation yet)\n",
                bool(o), sizeof(o));
    o.emplace("constructed in place, no separate allocation");
    std::printf("    now:   '%s'\n", (*o).c_str());
    o.reset();
    std::printf("    reset: has_value=%d\n", bool(o));
    std::printf("  -> optional<T> is sizeof(T) + a bool + padding, all INLINE.\n"
                "     Java's Optional<T> is a separate heap object holding a\n"
                "     reference to another heap object.\n");
}

// ---------------------------------------------------------------------------
// 3. THE ARENA (bump / linear / region allocator)
// ---------------------------------------------------------------------------
//  The simplest and often fastest allocator in existence:
//    - allocate = advance a pointer. ~2 instructions.
//    - free     = nothing. You free the WHOLE arena at once.
//
//  Perfect for: per-request memory in a server, per-frame memory in a game,
//  parse trees, compiler ASTs — anything with a natural "phase" that ends.
//  This is the single highest-leverage allocation technique for high-load
//  servers, and it is how you get rid of malloc from your hot path entirely.
// ---------------------------------------------------------------------------
class Arena {
    std::unique_ptr<std::byte[]> buffer_;
    std::size_t capacity_ = 0;
    std::size_t offset_   = 0;
    std::size_t peak_     = 0;
    std::size_t allocations_ = 0;

    static std::size_t align_up(std::size_t n, std::size_t a) {
        // Round n up to the next multiple of a (a must be a power of two).
        return (n + a - 1) & ~(a - 1);
    }

public:
    explicit Arena(std::size_t capacity)
        : buffer_(std::make_unique<std::byte[]>(capacity)), capacity_(capacity) {}

    // The entire allocator. Three lines.
    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
        std::size_t aligned = align_up(offset_, alignment);
        if (aligned + bytes > capacity_) return nullptr;     // arena exhausted
        void* p = buffer_.get() + aligned;
        offset_ = aligned + bytes;
        peak_ = std::max(peak_, offset_);
        ++allocations_;
        return p;
    }

    // Construct an object in the arena. Note: the destructor is NOT registered.
    // This arena only supports TRIVIALLY DESTRUCTIBLE types — which is the
    // common, fast case. (A real arena can keep a list of destructors; see
    // 09-projects/01-arena-allocator for that version.)
    template <class T, class... Args>
    T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "this simple arena never runs destructors");
        void* mem = allocate(sizeof(T), alignof(T));
        if (!mem) return nullptr;
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    // "Free everything" — O(1), no matter how many objects.
    void reset() { offset_ = 0; allocations_ = 0; }

    std::size_t used() const { return offset_; }
    std::size_t peak() const { return peak_; }
    std::size_t count() const { return allocations_; }
};

struct Particle { float x, y, z, vx, vy, vz; };   // trivially destructible

void demo_arena() {
    std::printf("\n=== 3. Arena allocator ===\n");

    Arena arena(1 << 20);   // 1 MB, one allocation, up front

    for (int i = 0; i < 5; ++i) {
        auto* p = arena.create<Particle>(Particle{float(i), 0, 0, 1, 0, 0});
        std::printf("    particle %d @ %p (arena used: %zu bytes)\n",
                    i, (void*)p, arena.used());
    }
    arena.reset();
    std::printf("    after reset(): used=%zu — freed 5 objects in ONE instruction\n",
                arena.used());
}

// ---------------------------------------------------------------------------
// 4. BENCHMARK: arena vs malloc
// ---------------------------------------------------------------------------
void benchmark_arena() {
    std::printf("\n=== 4. Arena vs new/delete ===\n");

    constexpr int N     = 200'000;
    constexpr int ROUNDS = 20;

    // --- new/delete ---
    auto t0 = std::chrono::steady_clock::now();
    double sink1 = 0;
    for (int r = 0; r < ROUNDS; ++r) {
        std::vector<Particle*> ps;
        ps.reserve(N);
        for (int i = 0; i < N; ++i) ps.push_back(new Particle{float(i),0,0,1,0,0});
        for (auto* p : ps) sink1 += p->x;
        for (auto* p : ps) delete p;
    }
    auto t1 = std::chrono::steady_clock::now();

    // --- arena ---
    Arena arena(std::size_t(N) * sizeof(Particle) + 4096);
    double sink2 = 0;
    for (int r = 0; r < ROUNDS; ++r) {
        std::vector<Particle*> ps;
        ps.reserve(N);
        for (int i = 0; i < N; ++i) ps.push_back(arena.create<Particle>(Particle{float(i),0,0,1,0,0}));
        for (auto* p : ps) sink2 += p->x;
        arena.reset();                       // <-- "free" everything
    }
    auto t2 = std::chrono::steady_clock::now();

    auto ms = [](auto a, auto b) { return std::chrono::duration<double,std::milli>(b-a).count(); };
    std::printf("    new/delete: %8.2f ms   (%.1f ns per object)\n",
                ms(t0,t1), ms(t0,t1)*1e6/(N*ROUNDS));
    std::printf("    arena:      %8.2f ms   (%.1f ns per object)\n",
                ms(t1,t2), ms(t1,t2)*1e6/(N*ROUNDS));
    std::printf("    speedup:    %.1fx      (sinks %.0f %.0f)\n", ms(t0,t1)/ms(t1,t2), sink1, sink2);

    std::printf(
      "\n  WHY the arena wins, on three separate axes:\n"
      "    1. allocation is a pointer bump (~2 instructions) vs malloc's size-class\n"
      "       lookup, free-list walk, and possible lock.\n"
      "    2. deallocation is ONE instruction for ALL objects.\n"
      "    3. the objects are CONTIGUOUS, so traversing them is a linear scan the\n"
      "       prefetcher loves — while malloc'd objects are scattered.\n"
      "\n  THE HIGH-LOAD PATTERN:\n"
      "    one arena per request (or per connection, or per frame).\n"
      "    handle_request(): arena.reset(); parse into arena; work; respond.\n"
      "    Zero malloc calls on the hot path. Zero fragmentation. Zero leaks —\n"
      "    it is structurally impossible to leak, because you free everything.\n"
      "    This is what nginx does (ngx_pool_t), what Envoy does, what every\n"
      "    game engine does, and what a JVM's TLAB is a cousin of.\n");
}

// ---------------------------------------------------------------------------
// 5. THE OTHER ALLOCATOR SHAPES YOU SHOULD KNOW
// ---------------------------------------------------------------------------
void allocator_taxonomy() {
    std::printf("\n=== 5. The allocator taxonomy ===\n");
    std::printf(
      "  BUMP / ARENA / REGION\n"
      "    alloc = ptr++. free = nothing. Reset all at once.\n"
      "    Use: per-request, per-frame, parse trees. FASTEST possible.\n"
      "\n"
      "  POOL / FREELIST (fixed-size objects)\n"
      "    Preallocate N slots of one size; keep a linked list of free ones\n"
      "    THREADED THROUGH the free slots themselves (zero metadata overhead).\n"
      "    alloc = pop head. free = push head. Both O(1), no fragmentation.\n"
      "    Use: connection objects, packet buffers, nodes of one type.\n"
      "\n"
      "  SLAB\n"
      "    Pools of pools, per size class, per CPU. This is what the Linux kernel\n"
      "    and jemalloc/tcmalloc do. Adds thread-local caches to avoid locks.\n"
      "\n"
      "  BUDDY\n"
      "    Power-of-two splitting/merging. O(log n), bounded fragmentation.\n"
      "    Use: general-purpose allocators, kernel page allocators.\n"
      "\n"
      "  STACK / LIFO\n"
      "    Like an arena but supports popping back to a saved marker.\n"
      "    Use: recursive algorithms with scratch space.\n"
      "\n"
      "  RING / CIRCULAR\n"
      "    Fixed buffer, producer advances head, consumer advances tail.\n"
      "    Use: network IO buffers, log buffers, lock-free queues.\n"
      "\n"
      "  If you build ONE of these well, you understand memory management\n"
      "  better than most engineers who've used a GC for a decade.\n");
}

int main() {
    demo_placement_new();
    demo_my_optional();
    demo_arena();
    benchmark_arena();
    allocator_taxonomy();
    std::printf("\n>>> NEXT: 09-projects/01-arena-allocator/ builds the production\n"
                "    version with destructor tracking, chained blocks, and a\n"
                "    std::pmr integration so std::vector can allocate from it.\n");
    return 0;
}
