// ===========================================================================
//  ALGORITHMS, ITERATORS, AND RANGES
//  run: make run F=05-stl/02-algorithms-and-ranges.cpp
// ===========================================================================
//
//  "The STL algorithms are the best-kept secret in C++" — and unlike Java
//  Streams, they cost nothing: no boxing, no virtual dispatch, no allocation,
//  and your lambda is INLINED into the loop body.
//
//  Sean Parent's rule: "No raw loops." Not dogma — a heuristic. A named
//  algorithm states INTENT, is already correct, and is usually faster than the
//  loop you'd write (std::find uses memchr; std::copy uses memmove; std::sort
//  is an introsort with insertion-sort tails).
//
// ===========================================================================

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

static void print(std::string_view label, const auto& r) {
    std::cout << "  " << label;
    for (const auto& x : r) std::cout << x << " ";
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
void demo_core_algorithms() {
    std::cout << "\n=== 1. The algorithms you'll use every week ===\n";

    std::vector<int> v{5, 3, 8, 1, 9, 2, 7, 4, 6};

    // --- SEARCHING ---
    auto it = std::find(v.begin(), v.end(), 8);
    std::cout << "  find(8) at index " << std::distance(v.begin(), it) << "\n";
    std::cout << "  any_of(>8)  = " << std::boolalpha
              << std::any_of(v.begin(), v.end(), [](int x){ return x > 8; }) << "\n";
    std::cout << "  all_of(>0)  = " << std::all_of(v.begin(), v.end(), [](int x){ return x > 0; }) << "\n";
    std::cout << "  count_if(even) = " << std::count_if(v.begin(), v.end(), [](int x){ return x%2==0; }) << "\n";

    // --- MIN / MAX ---
    auto [mn, mx] = std::minmax_element(v.begin(), v.end());   // ONE pass for both
    std::cout << "  min=" << *mn << " max=" << *mx << "\n";

    // --- SORTING ---
    std::sort(v.begin(), v.end());                        // introsort, O(n log n)
    print("sorted:            ", v);
    std::sort(v.begin(), v.end(), std::greater<>{});      // custom comparator
    print("descending:        ", v);

    // partial_sort: you only need the top k. O(n log k), much cheaper.
    std::vector<int> w{5,3,8,1,9,2,7,4,6};
    std::partial_sort(w.begin(), w.begin()+3, w.end(), std::greater<>{});
    std::cout << "  top 3 (partial_sort): " << w[0] << " " << w[1] << " " << w[2] << "\n";

    // nth_element: O(n) selection. THE way to find a median or a percentile.
    std::vector<int> p{5,3,8,1,9,2,7,4,6};
    auto mid = p.begin() + p.size()/2;
    std::nth_element(p.begin(), mid, p.end());
    std::cout << "  median via nth_element: " << *mid << "  (O(n), not O(n log n))\n";

    // stable_sort preserves the relative order of equal elements (Java's
    // Collections.sort is stable; std::sort is NOT).
    // ⚠️ std::sort's comparator must be a STRICT WEAK ORDERING. Using <= instead
    //    of < is UB and can crash or loop forever. Real bug, seen in production.

    // --- BINARY SEARCH (requires a sorted range) ---
    std::sort(v.begin(), v.end());
    std::cout << "  binary_search(7) = " << std::binary_search(v.begin(), v.end(), 7) << "\n";
    auto lb = std::lower_bound(v.begin(), v.end(), 5);   // first >= 5
    auto ub = std::upper_bound(v.begin(), v.end(), 5);   // first > 5
    std::cout << "  lower_bound(5) idx=" << (lb-v.begin())
              << " upper_bound(5) idx=" << (ub-v.begin()) << "\n";

    // --- TRANSFORM / ACCUMULATE ---
    std::vector<int> squares(v.size());
    std::transform(v.begin(), v.end(), squares.begin(), [](int x){ return x*x; });
    print("squares:           ", squares);

    // accumulate = Java's reduce. NOTE: the init value determines the TYPE.
    // std::accumulate(v.begin(), v.end(), 0) on doubles truncates to int!
    std::cout << "  sum       = " << std::accumulate(v.begin(), v.end(), 0) << "\n";
    std::cout << "  product   = " << std::accumulate(v.begin(), v.end(), 1LL, std::multiplies<>{}) << "\n";
    std::cout << "  inner_product = "
              << std::inner_product(v.begin(), v.end(), squares.begin(), 0LL) << "\n";

    // --- MODIFYING ---
    std::vector<int> u{1,2,2,3,3,3,4};
    // unique() only removes CONSECUTIVE duplicates, and doesn't shrink — it
    // returns the new logical end. Sort first, then erase the tail.
    u.erase(std::unique(u.begin(), u.end()), u.end());
    print("after unique:      ", u);

    std::vector<int> r{1,2,3,4,5,6};
    std::erase_if(r, [](int x){ return x % 2 == 0; });     // C++20, one-liner
    print("erase_if(even):    ", r);

    std::rotate(r.begin(), r.begin()+1, r.end());
    print("rotate by 1:       ", r);

    std::vector<int> shuffled{1,2,3,4,5,6,7,8};
    std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937{42});
    print("shuffled:          ", shuffled);
}

// ---------------------------------------------------------------------------
void demo_ranges() {
    std::cout << "\n=== 2. Ranges (C++20) — Java Streams, but free ===\n";

    std::vector<int> v(20);
    std::iota(v.begin(), v.end(), 1);

    // The ranges versions take a RANGE, not two iterators. Less typing, fewer
    // "I passed begin() of one container and end() of another" bugs.
    std::ranges::sort(v);
    std::cout << "  std::ranges::sort(v)  — no .begin()/.end() needed\n";

    // Projections: apply a function before comparing. Kills most custom
    // comparators. This is std::ranges' best feature and Java has no equivalent.
    struct Person { std::string name; int age; };
    std::vector<Person> people{{"carol",30},{"alice",25},{"bob",35}};
    std::ranges::sort(people, {}, &Person::age);            // sort BY age
    std::cout << "  sorted by age (projection &Person::age): ";
    for (const auto& p : people) std::cout << p.name << "(" << p.age << ") ";
    std::cout << "\n";
    std::ranges::sort(people, {}, &Person::name);           // sort BY name
    auto oldest = std::ranges::max_element(people, {}, &Person::age);
    std::cout << "  oldest = " << oldest->name << "\n";

    // VIEWS: lazy, composable, non-owning pipelines.
    // Java: v.stream().filter(...).map(...).limit(5).toList()
    auto pipeline = v
                  | std::views::filter([](int x){ return x % 2 == 0; })
                  | std::views::transform([](int x){ return x * x; })
                  | std::views::take(5);
    print("even squares (first 5): ", pipeline);

    // Nothing was computed until the range-for iterated. Nothing was allocated.
    // The whole pipeline is a stack-allocated struct holding iterators+lambdas,
    // and at -O2 it fuses into a single loop.

    print("reversed:          ", v | std::views::reverse | std::views::take(5));
    print("drop 15:           ", v | std::views::drop(15));
    print("iota(1,6):         ", std::views::iota(1, 6));

    // Views over a map
    std::map<std::string,int> scores{{"alice",90},{"bob",85}};
    print("map keys:          ", scores | std::views::keys);
    print("map values:        ", scores | std::views::values);

    // ranges::to (C++23) — materialise a lazy view into a real container.
    // This is Java's .toList(). It IS available in Apple clang 16.
    auto materialised = v | std::views::take(4) | std::ranges::to<std::vector<int>>();
    print("ranges::to<vector>:", materialised);

    // ⚠️ PLATFORM NOTE: Apple clang 16's libc++ does NOT yet ship these C++23
    // views: zip, enumerate, chunk, slide, join_with, chunk_by, stride.
    // (Check with __cpp_lib_ranges_zip / __cpp_lib_ranges_enumerate.)
    // On Linux with a recent libstdc++ or `brew install llvm`, they work.
    // The portable stand-ins:
    std::cout << "  enumerate (hand-rolled): ";
    for (std::size_t i = 0; const auto& x : std::vector{10,20,30})
        std::cout << i++ << "->" << x << " ";
    std::cout << "\n";

    std::cout << "  zip (hand-rolled): ";
    std::vector<std::string> names{"a","b","c"};
    std::vector<int> nums{1,2,3};
    for (std::size_t i = 0; i < names.size() && i < nums.size(); ++i)
        std::cout << names[i] << nums[i] << " ";
    std::cout << "\n";

    std::cout <<
      "\n  ⚠️ THE DANGLING TRAP:\n"
      "       auto bad = get_vector() | std::views::filter(...);   // ☠️\n"
      "     The temporary vector dies at the semicolon; the view points at freed\n"
      "     memory. C++20 added `borrowed_range` and `std::ranges::dangling` to\n"
      "     catch some of these, but not all. Bind the container to a named\n"
      "     variable first.\n"
      "\n"
      "  ⚠️ COMPILE TIMES AND ERRORS: ranges are template-heavy. A typo produces\n"
      "     an impressive wall of text. Build up pipelines one stage at a time.\n";
}

// ---------------------------------------------------------------------------
void demo_iterator_adaptors() {
    std::cout << "\n=== 3. Iterator adaptors ===\n";

    std::vector<int> src{1,2,3,4,5};
    std::vector<int> dst;

    // back_inserter turns "assign through the iterator" into "push_back".
    // Without it, std::copy would write past the end of an empty vector (UB).
    std::copy(src.begin(), src.end(), std::back_inserter(dst));
    print("copied via back_inserter: ", dst);

    // ⚠️ back_inserter causes a push_back per element -> possible reallocations.
    // If you know the size: dst.reserve(src.size()) first, or just
    // `dst.assign(src.begin(), src.end())` which sizes correctly in one go.

    // ostream_iterator: stream elements out with a separator.
    std::cout << "  via ostream_iterator: ";
    std::copy(src.begin(), src.end(), std::ostream_iterator<int>(std::cout, ","));
    std::cout << "\n";

    // Reverse iterators
    print("reverse:                  ", std::ranges::subrange(src.rbegin(), src.rend()));

    // move_iterator: the elements are MOVED out, not copied. Essential when
    // merging vectors of expensive types.
    std::vector<std::string> a{"one","two","three"};
    std::vector<std::string> b;
    b.reserve(a.size());
    std::copy(std::make_move_iterator(a.begin()), std::make_move_iterator(a.end()),
              std::back_inserter(b));
    std::cout << "  after move-copy: b[0]='" << b[0] << "' a[0]='" << a[0]
              << "' (a's strings were moved out)\n";
}

// ---------------------------------------------------------------------------
void the_catalogue() {
    std::cout << "\n=== 4. The catalogue (bookmark this) ===\n";
    std::cout <<
      "  NON-MODIFYING\n"
      "    all_of any_of none_of  for_each  count count_if\n"
      "    find find_if find_if_not  find_first_of  adjacent_find  search\n"
      "    mismatch  equal  lexicographical_compare\n"
      "\n  MODIFYING\n"
      "    copy copy_if copy_n copy_backward  move move_backward\n"
      "    fill fill_n  generate generate_n  transform\n"
      "    remove remove_if  replace replace_if  swap_ranges  reverse  rotate\n"
      "    shuffle  sample  unique\n"
      "\n  PARTITIONING\n"
      "    is_partitioned  partition  stable_partition  partition_point\n"
      "\n  SORTING & SEARCHING (sorted ranges)\n"
      "    sort stable_sort partial_sort nth_element  is_sorted\n"
      "    lower_bound upper_bound equal_range binary_search\n"
      "    merge inplace_merge  includes set_union set_intersection\n"
      "    set_difference set_symmetric_difference\n"
      "\n  HEAP\n"
      "    make_heap push_heap pop_heap sort_heap is_heap\n"
      "\n  NUMERIC (<numeric>)\n"
      "    iota  accumulate  reduce  inner_product  transform_reduce\n"
      "    partial_sum  inclusive_scan exclusive_scan  adjacent_difference\n"
      "    gcd lcm midpoint\n"
      "\n  ⭐ PARALLEL ALGORITHMS (C++17): pass an execution policy as arg 1.\n"
      "       std::sort(std::execution::par, v.begin(), v.end());\n"
      "       std::transform_reduce(std::execution::par_unseq, ...);\n"
      "     Policies: seq, par, par_unseq, unseq.\n"
      "     ⚠️ NOT implemented in Apple's libc++ (needs Intel TBB). On Linux with\n"
      "        libstdc++ + TBB it works and is a genuinely free multicore speedup.\n"
      "        This is C++'s answer to Java's parallelStream().\n"
      "\n  ⭐ std::reduce vs std::accumulate: reduce may REORDER operations (so it\n"
      "     can vectorise and parallelise) and requires associativity.\n"
      "     accumulate is strictly left-to-right. For floats they can differ.\n";
}

int main() {
    demo_core_algorithms();
    demo_ranges();
    demo_iterator_adaptors();
    the_catalogue();
    return 0;
}
