// ===========================================================================
//  STRINGS — std::string, string_view, SSO, and why Java's String is different
//  run: make run F=01-fundamentals/08-strings.cpp
// ===========================================================================
//
//  Java's String: immutable, heap-allocated, interned literals, UTF-16 internally
//  (or Latin-1 compact since JDK 9), with a cached hashCode. Concatenation in a
//  loop is a famous performance trap solved by StringBuilder.
//
//  C++'s std::string: MUTABLE, owns a heap buffer (usually), is a sequence of
//  BYTES with no encoding attached, and has Small String Optimisation so short
//  strings need no allocation at all.
//
//  This is one of the places where C++ is genuinely simpler AND faster.
//
// ===========================================================================

#include <algorithm>
#include <charconv>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

void demo_basics() {
    std::cout << "\n=== 1. std::string basics ===\n";

    std::string s = "hello";
    s += ", world";                 // MUTABLE — no new object created
    s.append("!");
    std::cout << "  " << s << "  size=" << s.size() << " capacity=" << s.capacity() << "\n";

    std::cout << "  s[0]=" << s[0] << " s.front()=" << s.front()
              << " s.back()=" << s.back() << "\n";
    std::cout << "  substr(7,5)='" << s.substr(7, 5) << "'  (pos, LENGTH — not endIndex!)\n";
    //    ^ Java's substring(begin, end) takes an END index. C++ takes a COUNT.
    //      This trips up every Java dev exactly once.

    std::cout << "  find('world') = " << s.find("world")
              << "   (returns std::string::npos == "
              << std::string::npos << " if not found)\n";
    if (s.find("zzz") == std::string::npos) std::cout << "  'zzz' not found\n";

    std::cout << "  contains (C++23): " << std::boolalpha << s.contains("world") << "\n";
    std::cout << "  starts_with (C++20): " << s.starts_with("hello") << "\n";

    // Comparison uses actual value semantics — no `.equals()` vs `==` confusion,
    // and no string interning surprises. `==` compares CONTENT. Always.
    std::string a = "abc", b = "abc";
    std::cout << "  (a == b) = " << (a == b) << "  <- content comparison, always\n";
    // In Java, `new String("abc") == new String("abc")` is false. That entire
    // class of bug simply does not exist in C++.

    // Three-way comparison (C++20 spaceship) gives you <, <=, >, >= for free.
    std::cout << "  \"apple\" < \"banana\" = " << (std::string("apple") < "banana") << "\n";
}

void demo_sso() {
    std::cout << "\n=== 2. Small String Optimisation (SSO) ===\n";

    // sizeof(std::string) is 24 on libc++/libstdc++ (64-bit). Those 24 bytes are
    // EITHER {pointer, size, capacity} for a heap string, OR the characters
    // themselves inline for a short one. libc++ fits 22 chars + NUL inline.
    std::cout << "  sizeof(std::string) = " << sizeof(std::string) << " bytes\n";

    auto report = [](const std::string& str) {
        // If .data() points INSIDE the string object itself, it's SSO (no heap).
        const void* obj  = static_cast<const void*>(&str);
        const void* data = static_cast<const void*>(str.data());
        auto off = reinterpret_cast<const char*>(data) - reinterpret_cast<const char*>(obj);
        bool sso = (off >= 0 && off < static_cast<long>(sizeof(std::string)));
        std::cout << "  len=" << str.size() << "\tcap=" << str.capacity()
                  << "\t" << (sso ? "SSO — NO heap allocation" : "HEAP allocated") << "\n";
    };

    report(std::string(5,  'x'));
    report(std::string(15, 'x'));
    report(std::string(22, 'x'));
    report(std::string(23, 'x'));   // one past the SSO limit on libc++
    report(std::string(64, 'x'));

    std::cout << "\n  WHY THIS MATTERS: most strings in real programs are short —\n"
                 "  field names, HTTP headers, keys, enum names. SSO makes them\n"
                 "  cost ZERO allocations. Java allocates a heap object for every\n"
                 "  single one, plus a char[] behind it (two objects per string,\n"
                 "  pre-compact-strings). At a million strings per second, that is\n"
                 "  the difference between a GC problem and no GC.\n";
}

// ---------------------------------------------------------------------------
// 3. std::string_view — the single most useful C++17 addition for performance
// ---------------------------------------------------------------------------
// A string_view is {const char* ptr; size_t len}. 16 bytes. It OWNS NOTHING.
// It is the read-only "I just want to look at some characters" type.

// BAD: forces a copy (and possibly a heap allocation) at every call site that
// passes a literal or a char*.
bool is_get_bad(const std::string& method) { return method == "GET"; }

// GOOD: accepts std::string, const char*, literals, substrings of other strings,
// and slices of a network buffer — all with ZERO copies.
bool is_get(std::string_view method) { return method == "GET"; }

// Zero-copy tokenizer: splitting a request line normally allocates one string
// per token. With string_view it allocates NOTHING.
std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t pos = s.find(delim, start);
        if (pos == std::string_view::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

void demo_string_view() {
    std::cout << "\n=== 3. std::string_view — zero-copy views ===\n";

    std::cout << "  sizeof(string_view) = " << sizeof(std::string_view)
              << " bytes (ptr + len, owns nothing)\n";
    std::cout << "  is_get(\"GET\") = " << std::boolalpha << is_get("GET")
              << "   <- no allocation, no copy\n";

    std::string request = "GET /api/v1/users?id=42 HTTP/1.1";
    auto parts = split(request, ' ');
    std::cout << "  parsed " << parts.size() << " tokens with ZERO allocations:\n";
    for (auto p : parts) std::cout << "    '" << p << "'\n";

    // substr on a string_view is O(1) — it just adjusts ptr and len.
    // substr on a std::string is O(n) — it allocates and copies.
    std::string_view sv = request;
    std::cout << "  sv.substr(4,14) = '" << sv.substr(4, 14) << "' (O(1), no alloc)\n";

    // ⚠️⚠️ THE STRING_VIEW TRAP — this is THE way people get burned:
    //     std::string_view bad = std::string("temporary");   // DANGLES immediately
    //     std::string_view worse = get_string_by_value();    // DANGLES
    // The temporary dies at the end of the full expression; the view survives
    // pointing at freed memory. Rules:
    //   - string_view is for PARAMETERS and LOCALS with obvious lifetimes.
    //   - NEVER store a string_view in a class member unless you have documented
    //     and enforced that the owner outlives it.
    //   - NEVER return a string_view to a local string.
    //   - A string_view is NOT null-terminated. Do not pass .data() to a C API
    //     that expects a C string. Use std::string(sv).c_str().

    std::cout << "\n  ⚠️ string_view does NOT own and is NOT null-terminated.\n";
}

void demo_conversions_and_formatting() {
    std::cout << "\n=== 4. Parsing and formatting ===\n";

    // Old way (throws on failure, allocates, locale-aware = slow):
    std::cout << "  std::stoi(\"42\")   = " << std::stoi("42") << "\n";
    std::cout << "  std::to_string(3.5) = " << std::to_string(3.5) << "\n";

    // ⭐ MODERN & FAST: std::from_chars / std::to_chars (C++17).
    // No allocation, no locale, no exceptions, no UB. This is what high-perf
    // JSON parsers use. Often 5-10x faster than stoi/stringstream.
    std::string_view text = "12345rest";
    int value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec == std::errc{})
        std::cout << "  from_chars -> " << value
                  << ", stopped at '" << ptr << "'\n";

    char buf[32];
    auto [end, ec2] = std::to_chars(buf, buf + sizeof(buf), 98765);
    if (ec2 == std::errc{})
        std::cout << "  to_chars   -> '" << std::string_view(buf, end - buf) << "'\n";

    // std::format (C++20) — Python-style, type-safe, fast. Replaces both
    // printf (unsafe) and iostreams (verbose + slow).
    std::string msg = std::format("{} took {:.2f}ms, {:>8} bytes, hex={:#x}",
                                  "request", 12.3456, 4096, 255);
    std::cout << "  std::format: " << msg << "\n";

    // ⚠️ std::stringstream is CONVENIENT and SLOW (locale lookups, virtual calls,
    // allocations). Fine for setup code and logs, never for a hot path.
    std::ostringstream oss;
    oss << "stream " << 42;
    std::cout << "  stringstream: " << oss.str() << " (convenient, but slow)\n";
}

void demo_encoding() {
    std::cout << "\n=== 5. Encoding: C++ strings are BYTES, not characters ===\n";

    // std::string holds bytes. A UTF-8 "é" is TWO bytes. .size() returns BYTES.
    std::string utf8 = "héllo";     // source file is UTF-8
    std::cout << "  \"héllo\".size() = " << utf8.size()
              << " bytes, but 5 user-visible characters\n";
    std::cout << "  bytes: ";
    for (unsigned char c : utf8) std::printf("%02X ", c);
    std::cout << "\n";

    std::cout <<
      "\n  There is NO good Unicode support in the standard library. This is a\n"
      "  real, acknowledged weakness. In practice:\n"
      "    - Store UTF-8 in std::string. Do not use std::wstring (it's 2 bytes on\n"
      "      Windows and 4 on Unix — unportable). char8_t exists but is unloved.\n"
      "    - Treat strings as opaque bytes wherever you can (that covers 95% of\n"
      "      server work: HTTP, JSON, keys, paths).\n"
      "    - When you truly need grapheme clusters, normalisation, or collation,\n"
      "      use ICU or utf8cpp. Java gives you this in the JDK; C++ does not.\n"
      "      This is a genuine 'batteries not included' moment — get used to it.\n";
}

void demo_performance_notes() {
    std::cout << "\n=== 6. Performance rules of thumb ===\n";
    std::cout <<
      "  1. Take `std::string_view` for read-only string parameters.\n"
      "  2. Take `std::string` BY VALUE + std::move when you will store it (sink).\n"
      "  3. Take `const std::string&` when you need the null terminator or c_str().\n"
      "  4. `reserve()` before building a string in a loop (same idea as vector).\n"
      "  5. Use `+=` in a loop — std::string IS a StringBuilder. `a + b + c`\n"
      "     creates temporaries; `a += b; a += c;` does not.\n"
      "  6. Use from_chars/to_chars/std::format, not stringstream, on hot paths.\n"
      "  7. Comparing short strings is fast (SSO keeps them in cache). Comparing\n"
      "     long ones is memcmp, which is SIMD-accelerated. Both fine.\n";

    std::string built;
    built.reserve(64);          // one allocation instead of several
    for (int i = 0; i < 8; ++i) built += std::to_string(i) + ",";
    std::cout << "  built: " << built << " (cap=" << built.capacity() << ")\n";
}

int main() {
    demo_basics();
    demo_sso();
    demo_string_view();
    demo_conversions_and_formatting();
    demo_encoding();
    demo_performance_notes();
    return 0;
}

// ===========================================================================
//  EXAM NOTES
// ===========================================================================
//  * std::string is MUTABLE and owns its buffer. `==` compares content, always.
//  * substr(pos, COUNT) — not (begin, end) like Java.
//  * find() returns std::string::npos on failure, not -1.
//  * SSO: ~22 chars fit inline in libc++'s 24-byte string. Zero allocation.
//  * string_view = {const char*, size_t}, 16 bytes, non-owning, NOT
//    null-terminated. Use for read-only parameters. Never store one whose
//    owner might die first. Never return one to a local.
//  * Parse/format with from_chars / to_chars / std::format. stringstream is slow.
//  * std::string is a byte sequence with no encoding. Store UTF-8; use ICU for
//    real Unicode. Avoid std::wstring.
//  * Sink parameter idiom: `void set(std::string s) { name_ = std::move(s); }`
// ===========================================================================
