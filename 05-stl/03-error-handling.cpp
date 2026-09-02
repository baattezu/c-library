// ===========================================================================
//  ERROR HANDLING — exceptions, optional, expected, error codes
//  run: make run F=05-stl/03-error-handling.cpp
// ===========================================================================
//
//  Java gives you one answer: checked and unchecked exceptions, and everyone
//  uses them for everything.
//
//  C++ gives you four, and the industry is genuinely split. You need to know
//  all four, when each is right, and — importantly — that large, serious
//  codebases (games, embedded, some HFT, parts of Google) compile with
//  -fno-exceptions and never use them at all.
//
// ===========================================================================

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <expected>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

// ===========================================================================
// 1. EXCEPTIONS
// ===========================================================================
class ConfigError : public std::runtime_error {
    std::string key_;
public:
    ConfigError(std::string key, const std::string& msg)
        : std::runtime_error("config error [" + key + "]: " + msg),
          key_(std::move(key)) {}
    const std::string& key() const noexcept { return key_; }
};

int parse_port_or_throw(const std::string& s) {
    if (s.empty()) throw ConfigError("port", "empty");
    int value = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || p != s.data() + s.size())
        throw ConfigError("port", "not a number: '" + s + "'");
    if (value < 1 || value > 65535)
        throw ConfigError("port", "out of range: " + std::to_string(value));
    return value;
}

void demo_exceptions() {
    std::cout << "\n=== 1. Exceptions ===\n";

    for (const std::string& s : {std::string("8080"), std::string("abc"), std::string("99999")}) {
        try {
            std::cout << "  '" << s << "' -> " << parse_port_or_throw(s) << "\n";
        }
        // ⭐ ALWAYS catch by CONST REFERENCE. Catching by value SLICES the
        // exception down to the base class, losing the derived type's data.
        catch (const ConfigError& e) {
            std::cout << "  '" << s << "' -> ConfigError on key '" << e.key()
                      << "': " << e.what() << "\n";
        }
        catch (const std::exception& e) {   // more general -> LATER in the list
            std::cout << "  generic: " << e.what() << "\n";
        }
    }

    std::cout <<
      "\n  DIFFERENCES FROM JAVA\n"
      "   - NO `finally`. RAII replaces it — destructors run during unwinding.\n"
      "   - NO checked exceptions. Any function can throw unless `noexcept`.\n"
      "   - Catch order matters: most-derived FIRST (no compiler check, unlike\n"
      "     Java's unreachable-catch error).\n"
      "   - You can throw ANY type (`throw 42;` is legal). Don't. Throw types\n"
      "     derived from std::exception.\n"
      "   - `catch (...)` catches everything but gives you no object. Use\n"
      "     std::current_exception()/std::rethrow_exception() to move it around.\n"
      "\n  THE COST MODEL (\"zero-cost\" exceptions)\n"
      "   - Not throwing: literally ZERO runtime cost. No try/catch instructions.\n"
      "     The unwind information lives in a side table in the binary.\n"
      "   - Throwing: EXPENSIVE. Allocate the exception, consult unwind tables,\n"
      "     run destructors frame by frame. Microseconds, and historically it\n"
      "     took a global lock in the runtime — so a server throwing on every\n"
      "     request under load can serialise itself.\n"
      "   - Binary size: +10-20% for the unwind tables.\n"
      "\n  ⇒ EXCEPTIONS ARE FOR EXCEPTIONAL THINGS. 'User typed a bad port' is\n"
      "    not exceptional — it's an expected outcome. Use a return type.\n";
}

// ===========================================================================
// 2. std::optional — "a value, or nothing"
// ===========================================================================
std::optional<int> parse_port(std::string_view s) {
    int value = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    if (value < 1 || value > 65535) return std::nullopt;
    return value;
}

void demo_optional() {
    std::cout << "\n=== 2. std::optional ===\n";

    for (std::string_view s : {"8080", "abc", "99999"}) {
        if (auto port = parse_port(s)) {
            std::cout << "  '" << s << "' -> " << *port << "\n";
        } else {
            std::cout << "  '" << s << "' -> nothing (but WHY? optional can't say)\n";
        }
    }

    auto p = parse_port("443");
    std::cout << "  value_or(80)  = " << parse_port("bad").value_or(80) << "  (default)\n";
    std::cout << "  has_value()   = " << std::boolalpha << p.has_value() << "\n";
    // p.value() throws std::bad_optional_access if empty. *p is UNCHECKED (UB).

    // C++23 monadic operations — chain without nesting ifs. This is Java's
    // Optional.map/flatMap/orElse, but with no allocation.
    auto doubled = parse_port("100")
                     .transform([](int x){ return x * 2; })          // map
                     .and_then([](int x) -> std::optional<int> {     // flatMap
                         return x < 1000 ? std::optional{x} : std::nullopt;
                     })
                     .or_else([]{ return std::optional{0}; });       // orElse
    std::cout << "  monadic chain -> " << *doubled << "\n";

    std::cout <<
      "\n  LAYOUT: optional<T> is sizeof(T) + a bool + padding, stored INLINE.\n"
      "  sizeof(optional<int>) = " << sizeof(std::optional<int>) << " bytes. "
      "No heap allocation, ever.\n"
      "  Java's Optional<T> is a heap object holding a reference to another heap\n"
      "  object — two indirections and an allocation for 'maybe an int'.\n"
      "\n  USE optional WHEN: absence is normal and there's only ONE way to fail\n"
      "  (a map lookup miss, an optional config field, a find() that found nothing).\n"
      "  DON'T USE IT when the caller needs to know WHY it failed.\n";
}

// ===========================================================================
// 3. std::expected (C++23) — "a value, or an error that explains itself"
// ===========================================================================
enum class ParseError { Empty, NotANumber, OutOfRange };

constexpr std::string_view to_string(ParseError e) {
    switch (e) {
        case ParseError::Empty:       return "empty input";
        case ParseError::NotANumber:  return "not a number";
        case ParseError::OutOfRange:  return "out of range (1-65535)";
    }
    return "?";
}

std::expected<int, ParseError> parse_port_ex(std::string_view s) {
    if (s.empty()) return std::unexpected(ParseError::Empty);
    int value = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::unexpected(ParseError::NotANumber);
    if (value < 1 || value > 65535)
        return std::unexpected(ParseError::OutOfRange);
    return value;                        // implicit conversion to expected
}

void demo_expected() {
    std::cout << "\n=== 3. std::expected (C++23) — the modern answer ===\n";

    for (std::string_view s : {"8080", "", "abc", "99999"}) {
        auto r = parse_port_ex(s);
        if (r) std::cout << "  '" << s << "' -> " << *r << "\n";
        else   std::cout << "  '" << s << "' -> ERROR: " << to_string(r.error()) << "\n";
    }

    // Monadic chaining, with the error propagating automatically. This is
    // Rust's `?` operator, spelled with method calls.
    auto result = parse_port_ex("8080")
                    .and_then([](int p) -> std::expected<std::string, ParseError> {
                        return "listening on port " + std::to_string(p);
                    })
                    .transform([](std::string s){ return "[" + s + "]"; });
    std::cout << "  chained: " << (result ? *result : std::string("failed")) << "\n";

    std::cout <<
      "\n  ⭐ std::expected IS THE DEFAULT CHOICE FOR MODERN C++ ERROR HANDLING\n"
      "     when failure is an expected outcome:\n"
      "      - the error type is in the SIGNATURE (self-documenting, like Java's\n"
      "        `throws`, but enforced by the type system on the CALLER side)\n"
      "      - no allocation, no unwinding, no runtime cost\n"
      "      - works with -fno-exceptions\n"
      "      - [[nodiscard]] by design: you cannot silently ignore it\n"
      "      - sizeof = max(T, E) + a tag, inline\n"
      "\n  DOWNSIDE: it does not propagate automatically. Every call site must\n"
      "  handle or forward the error, which is verbose (Rust's `?` and Java's\n"
      "  exceptions both hide this). C++ has no `?` operator. Yet.\n";
    std::cout << "  sizeof(expected<int,ParseError>) = "
              << sizeof(std::expected<int, ParseError>) << " bytes\n";
}

// ===========================================================================
// 4. ERROR CODES AND std::error_code
// ===========================================================================
void demo_error_codes() {
    std::cout << "\n=== 4. Error codes (talking to the OS) ===\n";

    // Every syscall reports failure via a return value + errno. This is the
    // world you enter in 08-systems/.
    std::FILE* f = std::fopen("/definitely/not/a/real/path", "r");
    if (!f) {
        // std::error_code wraps an int + a category, so you can compare it
        // against portable enumerators instead of raw platform numbers.
        std::error_code ec(errno, std::system_category());
        std::cout << "  fopen failed: value=" << ec.value()
                  << " message='" << ec.message() << "'\n";
        std::cout << "  is it 'no such file'? " << std::boolalpha
                  << (ec == std::errc::no_such_file_or_directory) << "\n";
    }

    std::cout <<
      "\n  The dual-API pattern used throughout <filesystem> and Boost.Asio:\n"
      "      void   copy(path from, path to);                      // throws\n"
      "      void   copy(path from, path to, std::error_code& ec); // sets ec\n"
      "  Callers who want exceptions get them; callers who can't afford them\n"
      "  (or who are in a loop where failure is routine) use the error_code form.\n"
      "  Offering both is good library design.\n";
}

// ===========================================================================
// 5. CHOOSING
// ===========================================================================
void the_decision_guide() {
    std::cout << "\n=== 5. Which mechanism? ===\n";
    std::cout <<
      "  PROGRAMMER ERROR (a bug — a precondition was violated)\n"
      "    -> assert() in debug, or std::abort(). Do NOT make it recoverable.\n"
      "       An out-of-range index, a null that can't be null, a broken\n"
      "       invariant: these mean your code is wrong. Crash loudly and fix it.\n"
      "       (This is the distinction Java blurs with unchecked exceptions.)\n"
      "\n"
      "  EXPECTED FAILURE (input is bad, the file isn't there, the parse failed)\n"
      "    -> std::expected<T,E>. The most common case in a server.\n"
      "    -> std::optional<T> if there's only one failure mode and no detail.\n"
      "\n"
      "  UNRECOVERABLE / RARE / DEEP (out of memory, corrupted state, a failure\n"
      "  10 frames below the only code that can handle it)\n"
      "    -> exceptions. This is what they're genuinely good at: they skip the\n"
      "       intermediate frames without every function forwarding an error.\n"
      "\n"
      "  CONSTRUCTOR FAILURE\n"
      "    -> exceptions are the ONLY option (a constructor has no return value).\n"
      "       The alternative is a private constructor + a static factory\n"
      "       returning std::expected. That's what -fno-exceptions codebases do.\n"
      "\n"
      "  OS / C API BOUNDARY\n"
      "    -> error codes, wrapped in std::error_code or std::expected at the\n"
      "       boundary so the rest of your code doesn't see errno.\n"
      "\n"
      "  RULES REGARDLESS OF MECHANISM\n"
      "   1. Destructors must not throw (implicitly noexcept -> terminate).\n"
      "   2. `noexcept` on moves, swaps, destructors. Nowhere you might be wrong.\n"
      "   3. Catch by const reference. Throw by value.\n"
      "   4. Provide at least the BASIC guarantee everywhere: after a failure,\n"
      "      everything is still valid and destructible.\n"
      "   5. Never use exceptions for control flow.\n"
      "   6. RAII is what makes ALL of these safe. Without it, every error path\n"
      "      is a leak. With it, you get correct cleanup for free.\n";
}

int main() {
    demo_exceptions();
    demo_optional();
    demo_expected();
    demo_error_codes();
    the_decision_guide();
    return 0;
}
