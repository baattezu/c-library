// ===========================================================================
//  PROJECT 2: WRITE std::vector FROM SCRATCH
// ===========================================================================
//
//  This is THE exercise. If you can write a correct vector, you understand:
//    - raw storage vs constructed objects (capacity vs size)
//    - placement new and explicit destructor calls
//    - the Rule of Five, done for real
//    - strong exception safety
//    - move_if_noexcept and why noexcept matters
//    - iterator invalidation, from the inside
//    - why growth is geometric
//    - perfect forwarding (emplace_back)
//
//  Everything here is what libc++ actually does, minus the allocator machinery
//  and about 2000 lines of standard-conformance detail.
//
// ===========================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mystd {

template <class T>
class vector {
    // ⭐ THE CENTRAL IDEA: we own RAW BYTES, and separately track how many of
    // them hold CONSTRUCTED OBJECTS.
    //
    //   [ T0 ][ T1 ][ T2 ][ raw ][ raw ][ raw ]
    //     ^                 ^                  ^
    //     data_             data_+size_        data_+capacity_
    //
    // Elements [0, size_) are alive. Elements [size_, capacity_) are
    // UNINITIALISED MEMORY — touching them as T is undefined behaviour.
    //
    // This is why we cannot use `new T[capacity]`: that would DEFAULT-CONSTRUCT
    // every slot, which is wasteful and impossible for types without a default
    // constructor. We allocate raw bytes and placement-new into them.
    T*          data_     = nullptr;
    std::size_t size_     = 0;
    std::size_t capacity_ = 0;

    // Allocate raw, correctly-aligned storage. No constructors run.
    static T* allocate_raw(std::size_t n) {
        if (n == 0) return nullptr;
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
    }
    static void deallocate_raw(T* p, std::size_t /*n*/) noexcept {
        if (p) ::operator delete(p, std::align_val_t{alignof(T)});
    }

    void destroy_all() noexcept {
        // ⭐ OPTIMISATION THE STANDARD LIBRARY MAKES: if T is trivially
        // destructible, there is nothing to do. This turns clear() from O(n)
        // into O(1) for int, float, POD structs, etc.
        if constexpr (!std::is_trivially_destructible_v<T>)
            for (std::size_t i = size_; i > 0; --i) data_[i - 1].~T();
        size_ = 0;
    }

    // -----------------------------------------------------------------------
    //  GROWTH — the heart of the class.
    //
    //  We must move `size_` elements from the old buffer to the new one. If a
    //  move throws halfway, the already-moved elements are gutted and we cannot
    //  restore the original — so we could not offer the strong guarantee.
    //
    //  std::move_if_noexcept solves it: it returns an rvalue reference (move)
    //  only when T's move constructor is noexcept OR T isn't copyable.
    //  Otherwise it returns a const lvalue reference, so we COPY instead —
    //  which leaves the source intact and lets us roll back.
    //
    //  ⇒ THIS is the mechanism behind "always mark your move ctor noexcept."
    // -----------------------------------------------------------------------
    void reallocate(std::size_t new_cap) {
        T* new_data = allocate_raw(new_cap);
        std::size_t constructed = 0;
        try {
            for (; constructed < size_; ++constructed)
                ::new (new_data + constructed) T(std::move_if_noexcept(data_[constructed]));
        } catch (...) {
            // Roll back: destroy what we built in the new buffer, free it,
            // and leave *this exactly as it was. STRONG GUARANTEE.
            for (std::size_t i = constructed; i > 0; --i) new_data[i - 1].~T();
            deallocate_raw(new_data, new_cap);
            throw;
        }
        // Success: destroy the originals and swap in the new buffer.
        std::size_t old_size = size_;
        destroy_all();
        deallocate_raw(data_, capacity_);
        data_     = new_data;
        size_     = old_size;
        capacity_ = new_cap;
    }

    [[nodiscard]] std::size_t next_capacity() const {
        // GEOMETRIC growth is what makes push_back amortised O(1). With a
        // growth factor k, n pushes cost O(n) total copies instead of O(n^2).
        // libc++/libstdc++ use 2x; MSVC uses 1.5x (better memory reuse: with
        // 2x, the new block is always larger than the sum of all freed ones,
        // so the allocator can never reuse them).
        return capacity_ == 0 ? 1 : capacity_ * 2;
    }

public:
    // ---- types (so standard algorithms work with us) ----
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = T&;
    using const_reference = const T&;
    using iterator        = T*;              // a raw pointer IS a valid
    using const_iterator  = const T*;        // contiguous iterator

    // ---- construction -----------------------------------------------------
    vector() noexcept = default;

    explicit vector(std::size_t n, const T& value = T()) {
        data_ = allocate_raw(n);
        capacity_ = n;
        try {
            for (; size_ < n; ++size_) ::new (data_ + size_) T(value);
        } catch (...) { destroy_all(); deallocate_raw(data_, capacity_); throw; }
    }

    vector(std::initializer_list<T> init) {
        data_ = allocate_raw(init.size());
        capacity_ = init.size();
        try {
            for (const T& v : init) { ::new (data_ + size_) T(v); ++size_; }
        } catch (...) { destroy_all(); deallocate_raw(data_, capacity_); throw; }
    }

    // ---- RULE OF FIVE -----------------------------------------------------
    ~vector() {
        destroy_all();
        deallocate_raw(data_, capacity_);
    }

    // COPY: deep. Allocate exactly what we need (not the source's capacity —
    // copying slack would be wasteful).
    vector(const vector& other) {
        data_ = allocate_raw(other.size_);
        capacity_ = other.size_;
        try {
            for (; size_ < other.size_; ++size_)
                ::new (data_ + size_) T(other.data_[size_]);
        } catch (...) { destroy_all(); deallocate_raw(data_, capacity_); throw; }
    }

    // MOVE: steal the pointers, null the source. noexcept — MANDATORY, so that
    // a vector<vector<T>> can move instead of copy when IT reallocates.
    vector(vector&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}

    // COPY ASSIGNMENT via copy-and-swap: strong exception guarantee and
    // self-assignment safety, for free.
    vector& operator=(const vector& other) {
        if (this != &other) { vector tmp(other); swap(tmp); }
        return *this;
    }

    vector& operator=(vector&& other) noexcept {
        if (this != &other) {
            destroy_all();
            deallocate_raw(data_, capacity_);
            data_     = std::exchange(other.data_, nullptr);
            size_     = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }

    void swap(vector& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
    }
    friend void swap(vector& a, vector& b) noexcept { a.swap(b); }

    // ---- capacity ---------------------------------------------------------
    [[nodiscard]] std::size_t size()     const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool        empty()    const noexcept { return size_ == 0; }

    void reserve(std::size_t n) {
        if (n > capacity_) reallocate(n);      // never SHRINKS — that's the contract
    }

    void shrink_to_fit() {
        if (size_ < capacity_) reallocate(size_);
    }

    // ---- element access ---------------------------------------------------
    // operator[] is UNCHECKED. This is deliberate: bounds checking in the hot
    // path is a cost the standard refuses to impose. .at() is the checked one.
    T&       operator[](std::size_t i)       noexcept { return data_[i]; }
    const T& operator[](std::size_t i) const noexcept { return data_[i]; }

    T& at(std::size_t i) {
        if (i >= size_) throw std::out_of_range("mystd::vector::at");
        return data_[i];
    }
    const T& at(std::size_t i) const {
        if (i >= size_) throw std::out_of_range("mystd::vector::at");
        return data_[i];
    }

    T&       front()       noexcept { return data_[0]; }
    const T& front() const noexcept { return data_[0]; }
    T&       back()        noexcept { return data_[size_ - 1]; }
    const T& back()  const noexcept { return data_[size_ - 1]; }
    T*       data()        noexcept { return data_; }
    const T* data()  const noexcept { return data_; }

    // ---- iterators --------------------------------------------------------
    iterator       begin()        noexcept { return data_; }
    iterator       end()          noexcept { return data_ + size_; }
    const_iterator begin()  const noexcept { return data_; }
    const_iterator end()    const noexcept { return data_ + size_; }
    const_iterator cbegin() const noexcept { return data_; }
    const_iterator cend()   const noexcept { return data_ + size_; }

    // ---- modifiers --------------------------------------------------------
    void push_back(const T& v) {
        if (size_ == capacity_) reallocate(next_capacity());
        ::new (data_ + size_) T(v);      // construct in place; ++size_ only AFTER
        ++size_;                          // so a throwing ctor leaves us consistent
    }

    void push_back(T&& v) {
        if (size_ == capacity_) reallocate(next_capacity());
        ::new (data_ + size_) T(std::move(v));
        ++size_;
    }

    // emplace_back: construct T DIRECTLY from the arguments, in the vector's
    // own storage. No temporary is created and no move happens.
    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == capacity_) reallocate(next_capacity());
        ::new (data_ + size_) T(std::forward<Args>(args)...);
        return data_[size_++];
    }

    void pop_back() noexcept {
        if (size_ == 0) return;
        --size_;
        if constexpr (!std::is_trivially_destructible_v<T>) data_[size_].~T();
    }

    void clear() noexcept { destroy_all(); }   // KEEPS capacity, deliberately

    void resize(std::size_t n, const T& value = T()) {
        if (n < size_) {
            if constexpr (!std::is_trivially_destructible_v<T>)
                for (std::size_t i = size_; i > n; --i) data_[i - 1].~T();
            size_ = n;
        } else if (n > size_) {
            if (n > capacity_) reallocate(std::max(n, next_capacity()));
            for (; size_ < n; ++size_) ::new (data_ + size_) T(value);
        }
    }

    // ---- comparison -------------------------------------------------------
    friend bool operator==(const vector& a, const vector& b) {
        return a.size_ == b.size_ && std::equal(a.begin(), a.end(), b.begin());
    }
};

} // namespace mystd
