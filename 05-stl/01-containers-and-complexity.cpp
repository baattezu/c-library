// ===========================================================================
//  CONTAINERS — layout, complexity, and which one to actually use
//  run: make opt F=05-stl/01-containers-and-complexity.cpp
// ===========================================================================
//
//  ⭐ THE COMPLEXITY TABLE — memorise this. It's the most-asked interview
//     content and the most-needed daily knowledge.
//
//  container          | access | insert/erase        | memory layout       | Java analogue
//  -------------------|--------|---------------------|---------------------|---------------
//  array<T,N>         | O(1)   | —                   | contiguous, stack   | T[] (fixed)
//  vector<T>          | O(1)   | O(1) amort. at back | contiguous, 1 alloc | ArrayList
//                     |        | O(n) elsewhere      |                     |
//  deque<T>           | O(1)   | O(1) at BOTH ends   | array of chunks     | ArrayDeque
//  list<T>            | O(n)   | O(1) given iterator | scattered nodes     | LinkedList
//  forward_list<T>    | O(n)   | O(1) given iterator | scattered, 1 ptr    | —
//  set/map<K,V>       | O(logn)| O(log n)            | red-black tree      | TreeSet/TreeMap
//  unordered_set/map  | O(1)~  | O(1)~               | buckets of nodes    | HashSet/HashMap
//  priority_queue     | O(1)top| O(log n) push/pop   | heap over a vector  | PriorityQueue
//  flat_map (C++23)   | O(logn)| O(n)                | two vectors!        | —
//
//  ⚠️ AND THE RULE THAT OVERRIDES THE TABLE:
//     BIG-O COUNTS OPERATIONS. THE MACHINE BILLS YOU FOR CACHE MISSES.
//     For n up to a few thousand, a linear scan over a contiguous vector
//     usually beats an O(log n) tree and an O(1) hash map. Measure.
//
// ===========================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <iostream>
#include <list>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
void demo_layout() {
    std::cout << "\n=== 1. What each container actually IS in memory ===\n";

    std::printf("  sizeof(vector<int>)        = %2zu  (3 ptrs: begin, end, cap_end)\n", sizeof(std::vector<int>));
    std::printf("  sizeof(deque<int>)         = %2zu  (map of chunk ptrs + bookkeeping)\n", sizeof(std::deque<int>));
    std::printf("  sizeof(list<int>)          = %2zu  (head/tail ptrs + size)\n", sizeof(std::list<int>));
    std::printf("  sizeof(map<int,int>)       = %2zu  (tree root + size)\n", sizeof(std::map<int,int>));
    std::printf("  sizeof(unordered_map<int,int>) = %2zu (bucket array ptr + counts)\n", sizeof(std::unordered_map<int,int>));
    std::printf("  sizeof(string)             = %2zu  (SSO buffer or ptr/size/cap)\n", sizeof(std::string));
    std::printf("  sizeof(array<int,10>)      = %2zu  (JUST the ints — no overhead)\n", sizeof(std::array<int,10>));

    std::cout <<
      "\n  PER-ELEMENT OVERHEAD (this is what matters at scale):\n"
      "    vector<int>              4 bytes/elem  + up to 100% slack capacity\n"
      "    list<int>               ~32 bytes/elem (prev + next + int + padding)\n"
      "                                          + one malloc EACH\n"
      "    map<int,int>            ~48 bytes/elem (left/right/parent/colour+data)\n"
      "                                          + one malloc EACH\n"
      "    unordered_map<int,int>  ~32 bytes/elem + bucket array\n"
      "                                          + one malloc EACH\n"
      "\n"
      "  A std::list<int> uses 8x the memory of a std::vector<int> AND scatters\n"
      "  it across the heap. That is why list is almost never the right answer.\n";
}

// ---------------------------------------------------------------------------
void demo_vector_internals() {
    std::cout << "\n=== 2. vector: the one you'll use 90% of the time ===\n";

    std::vector<int> v;
    std::cout << "  growth: ";
    std::size_t cap = 0;
    for (int i = 0; i < 40; ++i) {
        v.push_back(i);
        if (v.capacity() != cap) { std::cout << v.capacity() << " "; cap = v.capacity(); }
    }
    std::cout << "  (libc++ doubles)\n";

    // The API you must know cold:
    std::cout << "  size=" << v.size() << " capacity=" << v.capacity() << "\n";
    v.clear();
    std::cout << "  after clear(): size=" << v.size() << " capacity=" << v.capacity()
              << "  <- capacity KEPT on purpose, so you can refill for free\n";
    v.shrink_to_fit();
    std::cout << "  after shrink_to_fit(): capacity=" << v.capacity() << "\n";

    // The idiom for a reusable buffer in a server loop:
    //   static thread_local std::vector<char> buf;
    //   buf.clear();            // O(1), keeps the allocation
    //   buf.insert(...);        // no malloc after the first request
    // This is how you get to zero allocations per request.

    std::cout <<
      "\n  ⚠️ ERASING FROM THE MIDDLE IS O(n) — it memmoves the tail. That's fine\n"
      "     (memmove is fast) up to thousands of elements, but if you erase in a\n"
      "     loop you have O(n²). Use std::erase_if / the erase-remove idiom.\n"
      "\n"
      "  ⚠️ swap-and-pop: if ORDER DOESN'T MATTER, erasing is O(1):\n"
      "       std::swap(v[i], v.back()); v.pop_back();\n"
      "     This one trick turns many O(n²) loops into O(n).\n";
}

// ---------------------------------------------------------------------------
void demo_map_vs_unordered() {
    std::cout << "\n=== 3. map vs unordered_map vs sorted vector ===\n";

    constexpr int N = 200'000;
    std::vector<int> keys(N);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    auto time_it = [](const char* label, auto&& fn) {
        auto t0 = std::chrono::steady_clock::now();
        auto r  = fn();
        auto t1 = std::chrono::steady_clock::now();
        std::printf("  %-34s %8.2f ms  (checksum %lld)\n", label,
                    std::chrono::duration<double,std::milli>(t1-t0).count(), (long long)r);
    };

    std::map<int,int>           tree;
    std::unordered_map<int,int> hash;
    std::vector<std::pair<int,int>> flat;

    time_it("build std::map (RB tree)",      [&]{ for (int k : keys) tree[k]=k; return tree.size(); });
    time_it("build std::unordered_map",      [&]{ hash.reserve(N); for (int k : keys) hash[k]=k; return hash.size(); });
    time_it("build sorted vector<pair>",     [&]{
        flat.reserve(N);
        for (int k : keys) flat.emplace_back(k,k);
        std::sort(flat.begin(), flat.end());
        return flat.size();
    });

    time_it("lookup x200k std::map",         [&]{ long long s=0; for (int k:keys) s+=tree.find(k)->second; return s; });
    time_it("lookup x200k unordered_map",    [&]{ long long s=0; for (int k:keys) s+=hash.find(k)->second; return s; });
    time_it("lookup x200k binary-search vec",[&]{
        long long s=0;
        for (int k : keys) {
            auto it = std::lower_bound(flat.begin(), flat.end(), std::pair{k,0},
                                       [](auto& a, auto& b){ return a.first < b.first; });
            s += it->second;
        }
        return s;
    });

    std::cout <<
      "\n  READING THIS (typical numbers on this machine, n=200k):\n"
      "   - unordered_map wins lookups decisively — roughly 10-15x faster than\n"
      "     std::map. One hash + one or two pointer hops beats ~18 dependent\n"
      "     pointer chases. When you need pure key->value lookup, hashing wins.\n"
      "   - std::map is the SLOWEST at both build and lookup. A red-black tree\n"
      "     does O(log n) POINTER CHASES, each a likely cache miss, plus one\n"
      "     malloc per node on insert. You pay that for ORDERING — only buy it\n"
      "     if you actually need sorted iteration or range queries.\n"
      "   - The sorted vector does O(log n) comparisons too, but over CONTIGUOUS\n"
      "     memory, so the final probes land in already-loaded cache lines. It\n"
      "     comes in ~1.5-2x faster than std::map here, while using ~4x less\n"
      "     memory and giving you sorted order. That is the flat_map trade.\n"
      "   - Note `hash.reserve(N)` in the build: without it, rehashing dominates\n"
      "     and the build time roughly doubles. Try deleting it and re-running.\n"
      "\n"
      "  ⭐ PRACTICAL RULES\n"
      "   - Need ordering / range queries?  std::map, or a sorted vector if\n"
      "     writes are rare (C++23 gives you std::flat_map for exactly this).\n"
      "   - Need fast lookup, no ordering?  A hash map — but NOT std::unordered_map\n"
      "     in a hot path. It's mandated to be node-based. Use\n"
      "     absl::flat_hash_map, boost::unordered_flat_map, or ankerl::unordered_dense.\n"
      "     Typically 2-5x faster. This is a known standard-library wart.\n"
      "   - n < ~100 and lookups are rare?  Just a std::vector and std::find.\n"
      "     Seriously. It will beat both.\n"
      "   - ALWAYS `reserve()` a hash map if you know the size — rehashing is\n"
      "     the dominant cost of building one.\n";
}

// ---------------------------------------------------------------------------
void demo_list_reality() {
    std::cout << "\n=== 4. Why std::list is (almost) never right ===\n";

    constexpr int N = 500'000;
    std::vector<int> v(N); std::iota(v.begin(), v.end(), 0);
    std::list<int>   l(v.begin(), v.end());

    auto t0 = std::chrono::steady_clock::now();
    long long sv = 0; for (int rep=0; rep<10; ++rep) for (int x : v) sv += x;
    auto t1 = std::chrono::steady_clock::now();
    long long sl = 0; for (int rep=0; rep<10; ++rep) for (int x : l) sl += x;
    auto t2 = std::chrono::steady_clock::now();

    auto ms = [](auto a, auto b){ return std::chrono::duration<double,std::milli>(b-a).count(); };
    std::printf("  traverse vector: %7.2f ms (checksum %lld)\n", ms(t0,t1), sv);
    std::printf("  traverse list:   %7.2f ms (checksum %lld) -> %.1fx slower, same O(n)\n",
                ms(t1,t2), sl, ms(t1,t2)/ms(t0,t1));

    std::cout <<
      "\n  Both are O(n). The list is slower because each `next` pointer is a\n"
      "  DEPENDENT LOAD: the CPU can't fetch node k+1 until node k arrives, so\n"
      "  there's no prefetching and no memory-level parallelism.\n"
      "\n"
      "  std::list is the right answer ONLY when ALL of these hold:\n"
      "   - you have MANY insertions/removals in the middle, AND\n"
      "   - you already hold the iterator (no search needed), AND\n"
      "   - you need REFERENCE STABILITY (pointers to elements must survive), AND\n"
      "   - you need splice() (O(1) transfer between lists)\n"
      "  In 15 years of C++, that combination comes up maybe twice. Even then,\n"
      "  an intrusive list (boost::intrusive) is usually better.\n"
      "\n"
      "  This is the same lesson as Java's ArrayList vs LinkedList — but in C++\n"
      "  the gap is bigger, because vector<int> stores raw ints while Java's\n"
      "  ArrayList<Integer> stores pointers to boxed objects.\n";
}

// ---------------------------------------------------------------------------
void demo_the_rest() {
    std::cout << "\n=== 5. The rest of the toolbox ===\n";

    // deque: O(1) push/pop at BOTH ends. Stored as a map of fixed chunks, so
    // references to elements stay valid when you push at either end.
    std::deque<int> d{2,3};
    d.push_front(1); d.push_back(4);
    std::cout << "  deque: "; for (int x : d) std::cout << x << " ";
    std::cout << "  (O(1) both ends; NOT contiguous; slightly slower iteration)\n";

    // Container ADAPTORS — these wrap another container, they're not containers.
    std::stack<int> st;              // default: deque
    st.push(1); st.push(2);
    std::queue<int> q;               // default: deque
    q.push(1); q.push(2);
    std::priority_queue<int> pq;     // default: vector + heap algorithms
    for (int x : {3,1,4,1,5,9,2,6}) pq.push(x);
    std::cout << "  priority_queue (max-heap) top 3: ";
    for (int i = 0; i < 3; ++i) { std::cout << pq.top() << " "; pq.pop(); }
    std::cout << "\n";

    // Min-heap: pass std::greater as the comparator.
    std::priority_queue<int, std::vector<int>, std::greater<>> minq;
    for (int x : {3,1,4,1,5}) minq.push(x);
    std::cout << "  min-heap top: " << minq.top() << "\n";

    // set: sorted, unique. Great for range queries.
    std::set<int> s{5,3,8,1,9};
    std::cout << "  set (sorted): "; for (int x : s) std::cout << x << " ";
    auto lo = s.lower_bound(3), hi = s.upper_bound(8);
    std::cout << " | range [3,8]: "; for (auto it=lo; it!=hi; ++it) std::cout << *it << " ";
    std::cout << "\n";

    // multimap / multiset allow duplicate keys.
    std::multimap<std::string,int> mm{{"a",1},{"a",2},{"b",3}};
    std::cout << "  multimap count(\"a\") = " << mm.count("a") << "\n";

    std::cout <<
      "\n  C++23 additions worth knowing:\n"
      "    std::flat_map / std::flat_set  — a sorted vector with a map interface.\n"
      "      Cache-friendly, tiny memory, O(n) insert. PERFECT for read-mostly\n"
      "      lookup tables, which is most lookup tables.\n"
      "    std::mdspan  — a multidimensional VIEW over contiguous memory.\n"
      "\n"
      "  DECISION FLOWCHART:\n"
      "    Need it contiguous / will I iterate a lot?         -> vector\n"
      "    Fixed size known at compile time?                  -> array\n"
      "    Push/pop at both ends?                             -> deque\n"
      "    Key -> value, need sorted order or range queries?  -> map (or flat_map)\n"
      "    Key -> value, just lookups, small n?               -> sorted vector\n"
      "    Key -> value, just lookups, large n, hot path?     -> a good open-\n"
      "                                                          addressing hash map\n"
      "    Always need the min/max?                           -> priority_queue\n"
      "    Anything else?                                     -> vector. Really.\n";
}

int main() {
    demo_layout();
    demo_vector_internals();
    demo_map_vs_unordered();
    demo_list_reality();
    demo_the_rest();
    std::cout << "\n>>> Run with `make opt` for meaningful timings.\n";
    return 0;
}
