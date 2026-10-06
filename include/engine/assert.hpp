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
#    define ENGINE_ASSERT(cond, ...) ((void)0)
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
