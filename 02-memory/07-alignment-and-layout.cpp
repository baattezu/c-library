// ===========================================================================
//  ALIGNMENT, PADDING, AND SEEING YOUR OBJECTS AS BYTES
//  run: make run F=02-memory/07-alignment-and-layout.cpp
// ===========================================================================
//
//  Java cannot tell you how big an object is or where its fields are. C++ can
//  tell you exactly, at compile time, and lets you control it. This is the
//  foundation of: cache optimisation, zero-copy IO, shared memory, binary
//  protocols, and SIMD.
//
// ===========================================================================

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

// A generic hex-dumper so we can literally look at any object's bytes.
template <class T>
void dump(const T& obj, const char* label) {
    const auto* p = reinterpret_cast<const unsigned char*>(&obj);
    std::printf("  %-22s size=%-3zu align=%-2zu  ", label, sizeof(T), alignof(T));
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        if (i && i % 8 == 0) std::printf("| ");
        std::printf("%02X ", p[i]);
    }
    std::printf("\n");
}

// ---------------------------------------------------------------------------
void demo_alignment_basics() {
    std::cout << "\n=== 1. Every type has a size AND an alignment ===\n";
    std::printf("  char       size=%zu align=%zu\n", sizeof(char),   alignof(char));
    std::printf("  short      size=%zu align=%zu\n", sizeof(short),  alignof(short));
    std::printf("  int        size=%zu align=%zu\n", sizeof(int),    alignof(int));
    std::printf("  long       size=%zu align=%zu\n", sizeof(long),   alignof(long));
    std::printf("  double     size=%zu align=%zu\n", sizeof(double), alignof(double));
    std::printf("  void*      size=%zu align=%zu\n", sizeof(void*),  alignof(void*));
    std::printf("  max_align_t align=%zu  <- what malloc guarantees\n",
                alignof(std::max_align_t));

    std::cout <<
      "\n  THE RULE: an object of alignment A must live at an address divisible\n"
      "  by A. sizeof(T) is always a multiple of alignof(T), so that arrays\n"
      "  work: element i sits at base + i*sizeof(T), and all of them stay aligned.\n"
      "\n  WHY the CPU cares: an aligned load never straddles a cache line, so\n"
      "  it's one memory transaction. An unaligned one may need two, and on\n"
      "  some ARM configurations it's a hardware fault (SIGBUS).\n";
}

// ---------------------------------------------------------------------------
struct Bad  { char a; double b; char c; int d; char e; };   // sloppy order
struct Good { double b; int d; char a, c, e; };             // biggest first

void demo_padding() {
    std::cout << "\n=== 2. Padding: the same fields, two sizes ===\n";

    Bad  bad{};  bad.a='A'; bad.b=1.0; bad.c='C'; bad.d=0x11223344; bad.e='E';
    Good good{}; good.a='A'; good.b=1.0; good.c='C'; good.d=0x11223344; good.e='E';

    dump(bad,  "Bad  {c,d,c,i,c}");
    dump(good, "Good {d,i,c,c,c}");

    // offsetof shows exactly where each field starts.
    std::printf("\n  Bad offsets:  a=%zu b=%zu c=%zu d=%zu e=%zu  -> sizeof=%zu\n",
        offsetof(Bad,a), offsetof(Bad,b), offsetof(Bad,c), offsetof(Bad,d),
        offsetof(Bad,e), sizeof(Bad));
    std::printf("  Good offsets: b=%zu d=%zu a=%zu c=%zu e=%zu  -> sizeof=%zu\n",
        offsetof(Good,b), offsetof(Good,d), offsetof(Good,a), offsetof(Good,c),
        offsetof(Good,e), sizeof(Good));

    std::printf("\n  %zu bytes -> %zu bytes: a %.0f%% reduction, for FREE.\n",
        sizeof(Bad), sizeof(Good),
        100.0 * (1.0 - double(sizeof(Good)) / double(sizeof(Bad))));
    std::printf("  In an array of 10 million: %.1f MB vs %.1f MB.\n",
        sizeof(Bad) * 10e6 / 1e6, sizeof(Good) * 10e6 / 1e6);
    std::cout <<
      "  Since traversal cost is proportional to BYTES TOUCHED, not element\n"
      "  count, that reduction is a direct, proportional speedup.\n"
      "\n  ⭐ RULE: declare members in DECREASING size order.\n"
      "     Tool: clang++ -Xclang -fdump-record-layouts -c file.cpp\n";
}

// ---------------------------------------------------------------------------
void demo_bitfields_and_packing() {
    std::cout << "\n=== 3. Bit fields and packed structs ===\n";

    // Bit fields: pack several small values into one integer. Great for flags
    // and protocol headers, but note the layout is IMPLEMENTATION-DEFINED
    // (bit order varies by compiler/endianness), so don't use them for wire
    // formats you must interoperate on. Use explicit shifts/masks for those.
    struct Flags {
        std::uint32_t version  : 4;   // 4 bits
        std::uint32_t type     : 4;
        std::uint32_t length   : 16;
        std::uint32_t reserved : 8;
    };
    static_assert(sizeof(Flags) == 4);
    Flags f{};
    f.version = 2; f.type = 7; f.length = 1024;
    std::printf("  Flags packed into %zu bytes: v=%u t=%u len=%u\n",
                sizeof(Flags), f.version, f.type, f.length);

    // Packed struct: no padding at all. For reading binary formats.
#pragma pack(push, 1)
    struct WireHeader { std::uint8_t type; std::uint32_t length; std::uint16_t flags; };
#pragma pack(pop)
    struct NaturalHeader { std::uint8_t type; std::uint32_t length; std::uint16_t flags; };

    std::printf("  packed WireHeader   = %zu bytes (1+4+2, no padding)\n", sizeof(WireHeader));
    std::printf("  natural NaturalHeader = %zu bytes (padding inserted)\n", sizeof(NaturalHeader));

    std::cout <<
      "\n  ⚠️ Packed structs create UNALIGNED members. Taking a pointer/reference\n"
      "     to one is UB, and reading them can be slow or fault on ARM. For\n"
      "     network protocols, prefer explicit memcpy of each field plus\n"
      "     byte-order conversion. It's more code and it's always correct.\n";
}

// ---------------------------------------------------------------------------
void demo_over_alignment() {
    std::cout << "\n=== 4. alignas — asking for MORE alignment ===\n";

    // On Apple Silicon the cache line is 128 bytes (not 64!). Verify:
    //   sysctl hw.cachelinesize
    constexpr std::size_t CACHE_LINE = 128;

    struct alignas(CACHE_LINE) Padded { std::atomic<std::uint64_t> counter{0}; };
    struct Unpadded { std::atomic<std::uint64_t> counter{0}; };

    std::printf("  Unpadded: size=%zu align=%zu\n", sizeof(Unpadded), alignof(Unpadded));
    std::printf("  Padded:   size=%zu align=%zu\n", sizeof(Padded),   alignof(Padded));

    Padded arr[4];
    for (int i = 0; i < 4; ++i)
        std::printf("    arr[%d] @ %p  (offset %ld)\n", i, (void*)&arr[i],
                    (long)(reinterpret_cast<char*>(&arr[i]) - reinterpret_cast<char*>(&arr[0])));

    std::cout <<
      "\n  WHY: two atomics on the SAME cache line, written by two different\n"
      "  cores, cause FALSE SHARING — the line ping-pongs between the cores'\n"
      "  caches at ~100ns per bounce, even though the threads never touch the\n"
      "  same variable. Padding each to its own line fixes it, at the cost of\n"
      "  memory. See 07-performance/04-false-sharing.cpp for a live measurement.\n"
      "\n  (Standard names for this: std::hardware_destructive_interference_size\n"
      "   and ..._constructive_... . Apple clang doesn't provide them; hardcode 128.)\n";

    // SIMD types also need over-alignment (16 for NEON, 32 for AVX2, 64 for AVX-512).
    alignas(16) float vec4[4] = {1,2,3,4};
    std::printf("  alignas(16) float[4] @ %p (16-aligned? %s)\n", (void*)vec4,
                (reinterpret_cast<std::uintptr_t>(vec4) % 16 == 0) ? "yes" : "no");
}

// ---------------------------------------------------------------------------
void demo_type_traits() {
    std::cout << "\n=== 5. What the compiler knows about your type ===\n";

    struct POD      { int a; double b; };
    struct WithDtor { int a; ~WithDtor() {} };
    struct WithVirt { int a; virtual ~WithVirt() = default; };

    auto report = [](const char* n, bool triv_copy, bool triv_dtor, bool std_layout, std::size_t sz) {
        std::printf("  %-12s size=%-3zu trivially_copyable=%-5s trivially_destructible=%-5s standard_layout=%s\n",
                    n, sz, triv_copy?"yes":"no", triv_dtor?"yes":"no", std_layout?"yes":"no");
    };
    report("POD",      std::is_trivially_copyable_v<POD>,      std::is_trivially_destructible_v<POD>,      std::is_standard_layout_v<POD>,      sizeof(POD));
    report("WithDtor", std::is_trivially_copyable_v<WithDtor>, std::is_trivially_destructible_v<WithDtor>, std::is_standard_layout_v<WithDtor>, sizeof(WithDtor));
    report("WithVirt", std::is_trivially_copyable_v<WithVirt>, std::is_trivially_destructible_v<WithVirt>, std::is_standard_layout_v<WithVirt>, sizeof(WithVirt));
    report("std::string", std::is_trivially_copyable_v<std::string>, std::is_trivially_destructible_v<std::string>, std::is_standard_layout_v<std::string>, sizeof(std::string));

    std::cout <<
      "\n  WHY THESE MATTER:\n"
      "   trivially_copyable  -> you may memcpy it, write(fd, &obj, sizeof obj),\n"
      "                          mmap it, put it in shared memory, bit_cast it.\n"
      "                          std::vector uses memmove instead of a loop.\n"
      "   trivially_destructible -> vector::clear() is O(1), not O(n).\n"
      "   standard_layout     -> offsetof works; layout matches a C struct, so\n"
      "                          it is safe across an ABI/FFI boundary.\n"
      "   WithVirt is 16 bytes: 8 for the hidden vptr + 4 for `a` + 4 padding.\n"
      "                          A virtual function DOUBLED the size of this object.\n"
      "\n  Java objects are NEVER trivially copyable. That is precisely why you\n"
      "  can't do zero-copy IO in Java without ByteBuffer gymnastics.\n";
}

// ---------------------------------------------------------------------------
void demo_practical() {
    std::cout << "\n=== 6. Practical: shrinking a hot struct ===\n";

    // A typical "designed by accident" event struct.
    struct EventV1 {
        std::uint64_t timestamp_ns;   // 8
        std::string   source;         // 24 (!) — usually not needed in the hot loop
        double        value;          // 8
        bool          valid;          // 1 + 7 padding
        std::uint64_t sequence;       // 8
        char          type;           // 1 + 7 padding
    };

    // Redesigned: interned ids instead of strings, small types, ordered.
    struct EventV2 {
        std::uint64_t timestamp_ns;   // 8
        std::uint64_t sequence;       // 8
        double        value;          // 8
        std::uint32_t source_id;      // 4   <- index into a string table
        std::uint8_t  type;           // 1
        bool          valid;          // 1
                                      // 2 padding
    };

    std::printf("  EventV1: %zu bytes  -> %.1f MB per million events\n",
                sizeof(EventV1), sizeof(EventV1) * 1e6 / 1e6);
    std::printf("  EventV2: %zu bytes  -> %.1f MB per million events\n",
                sizeof(EventV2), sizeof(EventV2) * 1e6 / 1e6);
    std::printf("  %.1fx less memory traffic, and V2 is trivially copyable: %s\n",
                double(sizeof(EventV1)) / sizeof(EventV2),
                std::is_trivially_copyable_v<EventV2> ? "yes (memcpy/write-able!)" : "no");

    std::cout <<
      "\n  This is real performance work. No algorithm changed. No cleverness.\n"
      "  Just: know your layout, use the smallest types, intern your strings,\n"
      "  and order fields by size.\n";
}

int main() {
    demo_alignment_basics();
    demo_padding();
    demo_bitfields_and_packing();
    demo_over_alignment();
    demo_type_traits();
    demo_practical();
    std::cout << "\n>>> Try: sysctl hw.cachelinesize hw.pagesize\n";
    return 0;
}
