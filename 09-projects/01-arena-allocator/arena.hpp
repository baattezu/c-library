// ===========================================================================
//  PROJECT 1: A PRODUCTION-SHAPED ARENA ALLOCATOR
// ===========================================================================
//
//  Building an allocator is the single best exercise for understanding memory.
//  This one is deliberately realistic: chained blocks, alignment, destructor
//  registration, scoped markers, statistics, and a std::pmr adapter so the
//  standard containers can allocate from it.
//
//  WHY AN ARENA
//    allocate = bump a pointer (~2 instructions)
//    free     = nothing; you release the WHOLE arena at once
//
//  Perfect whenever memory has a natural PHASE that ends:
//    - per HTTP request       (nginx's ngx_pool_t, Envoy)
//    - per game frame
//    - per compilation unit   (clang's BumpPtrAllocator holds the whole AST)
//    - per query              (DuckDB, ClickHouse)
//
//  It is structurally impossible to leak from an arena, and impossible to
//  fragment it. Those two properties are worth as much as the speed.
//
// ===========================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace arena {

// ---------------------------------------------------------------------------
//  Round `n` up to the next multiple of `align` (which must be a power of 2).
//
//  How it works: for a power of two, align-1 is a mask of the low bits.
//  Adding it pushes us to or past the next boundary; ANDing with ~(align-1)
//  clears the low bits, snapping back down to the boundary. Two instructions.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr std::size_t align_up(std::size_t n, std::size_t align) noexcept {
    return (n + align - 1) & ~(align - 1);
}
static_assert(align_up(1, 8)  == 8);
static_assert(align_up(8, 8)  == 8);
static_assert(align_up(9, 16) == 16);

// ===========================================================================
//  Arena
// ===========================================================================
class Arena {
public:
    struct Stats {
        std::size_t blocks            = 0;   // how many OS allocations we made
        std::size_t bytes_reserved    = 0;   // total capacity across blocks
        std::size_t bytes_used        = 0;   // handed out (includes alignment gaps)
        std::size_t allocations       = 0;
        std::size_t destructors       = 0;
        std::size_t high_water        = 0;   // peak bytes_used ever
    };

private:
    // A block is one chunk of memory obtained from the OS/global allocator.
    // When it runs out, we allocate a bigger one and chain it. The blocks form
    // a singly-linked list ONLY for cleanup — allocation never walks it.
    struct Block {
        std::unique_ptr<std::byte[]> data;
        std::size_t                  capacity = 0;
        std::size_t                  used     = 0;
    };

    // A registered non-trivial destructor. We store a type-erased function
    // pointer plus the object address. This is exactly how the standard
    // library's monotonic_buffer_resource *doesn't* work — it never runs
    // destructors — and why our version is safer to use with real objects.
    struct Finalizer {
        void (*fn)(void*);
        void* obj;
    };

    std::vector<Block>     blocks_;
    std::vector<Finalizer> finalizers_;
    std::size_t            next_block_size_;
    Stats                  stats_{};

    static constexpr std::size_t MAX_BLOCK = 64u * 1024 * 1024;   // don't grow past 64 MB

    void add_block(std::size_t min_size) {
        std::size_t size = next_block_size_;
        while (size < min_size) size *= 2;
        next_block_size_ = std::min(size * 2, MAX_BLOCK);   // geometric growth

        Block b;
        b.data     = std::make_unique<std::byte[]>(size);
        b.capacity = size;
        b.used     = 0;
        stats_.bytes_reserved += size;
        ++stats_.blocks;
        blocks_.push_back(std::move(b));
    }

public:
    explicit Arena(std::size_t initial_block = 64 * 1024)
        : next_block_size_(initial_block) {
        blocks_.reserve(8);
        add_block(initial_block);
    }

    ~Arena() { reset(); }

    // An arena owns raw memory and registered destructors: move-only.
    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) noexcept        = default;
    Arena& operator=(Arena&&) noexcept = default;

    // -----------------------------------------------------------------------
    //  THE HOT PATH. In the common case this is: align, compare, add, return.
    // -----------------------------------------------------------------------
    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = alignof(std::max_align_t)) {
        Block& b = blocks_.back();
        std::size_t offset = align_up(b.used, alignment);

        if (offset + bytes > b.capacity) [[unlikely]] {
            // Slow path: this block is full. Note we do NOT try to reuse the
            // leftover space in old blocks — that's what makes allocation O(1)
            // with no search. The wasted tail is the price of the simplicity.
            add_block(bytes + alignment);
            Block& nb = blocks_.back();
            offset = align_up(nb.used, alignment);
            void* p = nb.data.get() + offset;
            nb.used = offset + bytes;
            stats_.bytes_used += bytes;
            stats_.high_water = std::max(stats_.high_water, stats_.bytes_used);
            ++stats_.allocations;
            return p;
        }

        void* p = b.data.get() + offset;
        b.used  = offset + bytes;
        stats_.bytes_used += bytes;
        stats_.high_water = std::max(stats_.high_water, stats_.bytes_used);
        ++stats_.allocations;
        return p;
    }

    // -----------------------------------------------------------------------
    //  Construct an object in the arena.
    //
    //  For a TRIVIALLY DESTRUCTIBLE type this is pure win: no destructor needs
    //  to run, so reset() really is O(1).
    //
    //  For a non-trivial type we REGISTER a destructor. That costs 16 bytes and
    //  makes reset() O(number of such objects) — still far cheaper than N
    //  individual frees, but no longer free. Prefer trivially-destructible
    //  types in arenas where you can.
    // -----------------------------------------------------------------------
    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        T* obj = ::new (mem) T(std::forward<Args>(args)...);

        if constexpr (!std::is_trivially_destructible_v<T>) {
            finalizers_.push_back(Finalizer{
                [](void* p) { static_cast<T*>(p)->~T(); },   // stateless lambda -> fn ptr
                obj
            });
            ++stats_.destructors;
        }
        return obj;
    }

    // Uninitialised array of a trivial type — the common case for buffers.
    template <class T>
    [[nodiscard]] T* create_array(std::size_t n) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "arena array only supports trivially destructible types");
        return static_cast<T*>(allocate(sizeof(T) * n, alignof(T)));
    }

    // -----------------------------------------------------------------------
    //  Release everything. Destructors run in REVERSE registration order, which
    //  matches normal C++ destruction semantics.
    //  We KEEP the first block so the next phase reuses the same memory — this
    //  is what makes a per-request arena allocation-free after warm-up.
    // -----------------------------------------------------------------------
    void reset() noexcept {
        for (auto it = finalizers_.rbegin(); it != finalizers_.rend(); ++it)
            it->fn(it->obj);
        finalizers_.clear();

        if (!blocks_.empty()) {
            blocks_.resize(1);                 // drop all but the first block
            blocks_[0].used = 0;
            stats_.bytes_reserved = blocks_[0].capacity;
            stats_.blocks = 1;
        }
        stats_.bytes_used   = 0;
        stats_.allocations  = 0;
        stats_.destructors  = 0;
    }

    // -----------------------------------------------------------------------
    //  SCOPED MARKER: a nested sub-arena. Rewind to exactly where we were.
    //  Useful for recursive algorithms with scratch space.
    //  ⚠️ Only valid if no new BLOCK was added since the mark; we handle that
    //     by recording the block index too.
    // -----------------------------------------------------------------------
    class Scope {
        Arena&      a_;
        std::size_t block_index_;
        std::size_t used_;
        std::size_t finalizers_;
    public:
        explicit Scope(Arena& a)
            : a_(a), block_index_(a.blocks_.size() - 1),
              used_(a.blocks_.back().used), finalizers_(a.finalizers_.size()) {}

        ~Scope() {
            for (std::size_t i = a_.finalizers_.size(); i > finalizers_; --i)
                a_.finalizers_[i - 1].fn(a_.finalizers_[i - 1].obj);
            a_.finalizers_.resize(finalizers_);

            if (a_.blocks_.size() > block_index_ + 1)
                a_.blocks_.resize(block_index_ + 1);
            a_.blocks_[block_index_].used = used_;
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    [[nodiscard]] Scope scope() { return Scope(*this); }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
};

// ===========================================================================
//  std::pmr ADAPTER
//
//  C++17's <memory_resource> lets STANDARD CONTAINERS allocate from a custom
//  source WITHOUT changing their type. std::pmr::vector<int> is a normal
//  vector that takes a memory_resource* at construction.
//
//  This is the bridge between "my custom allocator" and "the whole standard
//  library", and it is genuinely underused.
//
//  (Note: the standard already ships std::pmr::monotonic_buffer_resource, which
//  is an arena. Ours adds destructor tracking and stats. Writing it yourself
//  once is how you learn what pmr is doing.)
// ===========================================================================
class ArenaResource : public std::pmr::memory_resource {
    Arena& arena_;
public:
    explicit ArenaResource(Arena& a) : arena_(a) {}

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return arena_.allocate(bytes, alignment);
    }
    void do_deallocate(void*, std::size_t, std::size_t) override {
        // Intentionally empty! That IS the arena contract: individual frees are
        // no-ops; you release everything at once with reset().
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

} // namespace arena
