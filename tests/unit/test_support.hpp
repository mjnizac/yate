#pragma once

// Minimal check harness. Every failure prints `file:line: message` and the actual values, so a
// ctest log is enough to locate the problem without attaching a debugger.

#include <engine/common.hpp>
#include <engine/error.hpp>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>

namespace test {

inline engine::u32_t g_failures = 0;
inline engine::u32_t g_checks   = 0;
inline const char*   g_section  = "";

inline void Section(const char* name) {
    g_section = name;
    std::printf("-- %s\n", name);
}

inline void Fail(const char* file, int line, const char* expression) {
    ++g_failures;
    std::printf("FAIL %s:%d [%s] %s\n", file, line, g_section, expression);
}

/// Comparison behind a call, so a check on two compile-time constants does not trip MSVC's
/// "conditional expression is constant" warning.
template <typename A, typename B>
[[nodiscard]] inline bool AreEqual(const A& a, const B& b) {
    return a == b;
}

template <typename T>
[[nodiscard]] inline bool IsTrue(const T& value) {
    return static_cast<bool>(value);
}

inline int Summary(const char* suite) {
    std::printf("%s: %u check(s), %u failure(s)\n", suite, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace test

#define CHECK(expression)                                                                           \
    do {                                                                                            \
        ++::test::g_checks;                                                                         \
        if (!::test::IsTrue(expression)) {                                                                        \
            ::test::Fail(__FILE__, __LINE__, #expression);                                          \
        }                                                                                           \
    } while (false)

/// Compares two values and prints both when they differ.
#define CHECK_EQ(actual, expected)                                                                  \
    do {                                                                                            \
        ++::test::g_checks;                                                                         \
        const auto actualValue_   = (actual);                                                       \
        const auto expectedValue_ = (expected);                                                     \
        if (!::test::AreEqual(actualValue_, expectedValue_)) {                                                    \
            ::test::Fail(__FILE__, __LINE__, #actual " == " #expected);                             \
            std::printf("     actual   : %s\n", ::std::format("{}", actualValue_).c_str());          \
            std::printf("     expected : %s\n", ::std::format("{}", expectedValue_).c_str());        \
        }                                                                                           \
    } while (false)

/// Unwraps a `Result`/`Status`, printing the formatted error and aborting the suite on failure.
#define REQUIRE_OK(expression)                                                                      \
    do {                                                                                            \
        ++::test::g_checks;                                                                         \
        auto&& result_ = (expression);                                                               \
        if (!::test::IsTrue(result_.has_value())) {                                                                             \
            ::test::Fail(__FILE__, __LINE__, #expression);                                          \
            std::printf("     error: %s\n", result_.error().Format().data());                       \
            return ::test::Summary("aborted");                                                      \
        }                                                                                           \
    } while (false)
