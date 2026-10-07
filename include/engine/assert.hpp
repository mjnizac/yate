#pragma once

#include <engine/common.hpp>
#include <engine/log.hpp>

#include <cstdlib>
#include <format>

// The single sanctioned platform #ifdef outside src/platform/ (spec section 3).
#if defined(_MSC_VER)
#    define ENGINE_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#    define ENGINE_DEBUG_BREAK() __builtin_trap()
#else
#    define ENGINE_DEBUG_BREAK() ::std::abort()
#endif

/// Logs an assertion failure at FATAL level and drains the logger, so the entry survives a crash.
#define ENGINE_ASSERT_REPORT(expr, ...)                                                             \
    do {                                                                                            \
        ::engine::log::Write(ENGINE_LOGGER_ID, ::engine::log::Level::Fatal,                          \
                             ::std::format("assertion failed ({}) at {}:{}: {}", (expr), __FILE__,   \
                                           static_cast<::engine::u32_t>(__LINE__),                   \
                                           ::std::format(__VA_ARGS__)));                             \
        ::engine::log::Flush();                                                                      \
    } while (false)

/// Hard invariant. Aborts in debug builds, removed entirely in release builds.
#ifdef ENGINE_DEBUG
#    define ENGINE_ASSERT(cond, ...)                                                                \
        do {                                                                                        \
            if (!(cond)) [[unlikely]] {                                                             \
                ENGINE_ASSERT_REPORT(#cond, __VA_ARGS__);                                           \
                ENGINE_DEBUG_BREAK();                                                               \
                ::std::abort();                                                                     \
            }                                                                                       \
        } while (false)
#else
// Release keeps the condition and the message in an unevaluated operand instead of dropping the text
// on the floor. `sizeof` never evaluates what it measures, so this costs nothing at run time, yet both
// still have to compile: a condition that names a member that was renamed, or a message whose format
// string stops matching its arguments, fails the Release build instead of rotting until someone builds
// Debug.
//
// It does not make a side effect inside a condition safe. C++ cannot decide whether an expression has
// one, so the rule stays a rule; what this does is guarantee the Release behaviour is "never
// evaluated" rather than "never compiled", which is the half of the problem a macro can own.
#    define ENGINE_ASSERT(cond, ...)                                                                \
        ((void)sizeof(static_cast<bool>(cond)), (void)sizeof(::std::format(__VA_ARGS__)))
#endif

/// Recoverable invariant. Logs and returns `ret` in every build configuration.
#define ENGINE_ASSERT_RETURN(ret, cond, ...)                                                        \
    do {                                                                                            \
        if (!(cond)) [[unlikely]] {                                                                 \
            ENGINE_ASSERT_REPORT(#cond, __VA_ARGS__);                                               \
            return ret;                                                                             \
        }                                                                                           \
    } while (false)

/// Recoverable invariant with a cleanup block, which must contain no top-level commas.
#define ENGINE_ASSERT_X(ret, cond, cleanup, ...)                                                    \
    do {                                                                                            \
        if (!(cond)) [[unlikely]] {                                                                 \
            ENGINE_ASSERT_REPORT(#cond, __VA_ARGS__);                                               \
            cleanup;                                                                                \
            return ret;                                                                             \
        }                                                                                           \
    } while (false)
