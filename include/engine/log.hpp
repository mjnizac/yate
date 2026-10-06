#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>

#include <format>
#include <string_view>

namespace engine::log {

enum class Level : u32_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5,
    Off   = 6,
};

/// Sink encoding. `Json` emits one JSON object per line so tools can parse errors reliably.
enum class Format : u32_t {
    Text = 0,
    Json = 1,
};

/// Which of the two loggers an entry belongs to. Selected automatically by the `IS_ENGINE`
/// compile definition through `ENGINE_LOGGER_ID`.
enum class LoggerId : u32_t {
    Engine = 0,
    App    = 1,
};

/// Queues one entry on the asynchronous logger and mirrors it into Tracy.
/// Safe to call before `log::Init`; entries are then dropped.
ENGINE_API void Write(LoggerId logger, Level level, std::string_view message) noexcept;

/// Blocks until the worker thread has drained its queue.
ENGINE_API void Flush() noexcept;

/// Compile-time verbosity floor. Entries below it are removed by the macros below.
#ifndef ENGINE_LOG_LEVEL
#    define ENGINE_LOG_LEVEL 2
#endif

inline constexpr Level kCompileLevel = static_cast<Level>(ENGINE_LOG_LEVEL);

} // namespace engine::log

#ifdef IS_ENGINE
#    define ENGINE_LOGGER_ID ::engine::log::LoggerId::Engine
#else
#    define ENGINE_LOGGER_ID ::engine::log::LoggerId::App
#endif

/// Formats and queues an entry when `level` survives the compile-time floor.
/// The arguments are still type-checked when the entry is stripped, so dead log calls cannot rot.
#define ENGINE_LOG_AT(level, ...)                                                                   \
    do {                                                                                            \
        if constexpr (static_cast<::engine::u32_t>(level) >= ENGINE_LOG_LEVEL) {                     \
            ::engine::log::Write(ENGINE_LOGGER_ID, (level), ::std::format(__VA_ARGS__));            \
        }                                                                                           \
    } while (false)

#define LOG_TRACE(...) ENGINE_LOG_AT(::engine::log::Level::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) ENGINE_LOG_AT(::engine::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...)  ENGINE_LOG_AT(::engine::log::Level::Info, __VA_ARGS__)
#define LOG_WARN(...)  ENGINE_LOG_AT(::engine::log::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ENGINE_LOG_AT(::engine::log::Level::Error, __VA_ARGS__)
#define LOG_FATAL(...) ENGINE_LOG_AT(::engine::log::Level::Fatal, __VA_ARGS__)

#ifdef IS_ENGINE

namespace engine::log {

struct Config {
    Level  level     = kCompileLevel;
    Format format    = Format::Text;
    /// Entries are queued without blocking; the queue drops oldest entries when full.
    usize_t queueSize = 8192;
};

/// Creates the `engine` and `app` loggers on a dedicated worker thread.
[[nodiscard]] Status Init(const Config& config);

/// Flushes and destroys both loggers and the worker thread.
void Shutdown() noexcept;

} // namespace engine::log

#endif // IS_ENGINE
