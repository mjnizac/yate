#include <engine/log.hpp>

#include <engine/platform.hpp>

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <array>
#include <atomic>
#include <memory>

namespace engine::log {

namespace {

constexpr const char* kLoggerNames[] = {"engine", "app"};

/// Text layout: `[time] [logger] [level] message`. JSON layout: one object per line, so the
/// export tooling and LLMs can parse errors without guessing (spec section 5).
constexpr const char* kTextPattern = "[%T.%e] [%n] [%^%l%$] %v";
constexpr const char* kJsonPattern =
    R"({"time":"%Y-%m-%dT%H:%M:%S.%e","logger":"%n","level":"%l","thread":%t,"message":"%v"})";

std::array<std::shared_ptr<spdlog::logger>, 2> g_loggers{};
std::atomic<b8_t>                              g_ready{false};

constexpr spdlog::level::level_enum ToSpdlog(Level level) noexcept {
    switch (level) {
        case Level::Trace: return spdlog::level::trace;
        case Level::Debug: return spdlog::level::debug;
        case Level::Info: return spdlog::level::info;
        case Level::Warn: return spdlog::level::warn;
        case Level::Error: return spdlog::level::err;
        case Level::Fatal: return spdlog::level::critical;
        case Level::Off: break;
    }
    return spdlog::level::off;
}

#ifdef TRACY_ENABLE
/// Timeline colour per level, so errors stand out on the Tracy message list.
constexpr u32_t TracyColour(Level level) noexcept {
    switch (level) {
        case Level::Trace: return 0x808080;
        case Level::Debug: return 0x60A0C0;
        case Level::Info: return 0xFFFFFF;
        case Level::Warn: return 0xFFC000;
        case Level::Error: return 0xFF4040;
        case Level::Fatal: return 0xFF00FF;
        case Level::Off: break;
    }
    return 0xFFFFFF;
}
#endif

} // namespace

void Write(LoggerId logger, Level level, std::string_view message) noexcept {
#ifdef TRACY_ENABLE
    TracyMessageC(message.data(), message.size(), TracyColour(level));
#endif
    if (!g_ready.load(std::memory_order_acquire)) {
        return;
    }
    const auto& sink = g_loggers[static_cast<usize_t>(logger)];
    if (sink) {
        sink->log(ToSpdlog(level), message);
    }
}

void Flush() noexcept {
    if (!g_ready.load(std::memory_order_acquire)) {
        return;
    }
    for (const auto& logger : g_loggers) {
        if (logger) {
            logger->flush();
        }
    }
}

Status Init(const Config& config) {
    if (g_ready.load(std::memory_order_acquire)) {
        return {};
    }

    // One shared worker thread: producers never block on I/O. The queue drops the oldest
    // entries when full rather than stalling the caller.
    spdlog::init_thread_pool(config.queueSize, 1, [] { platform::SetThreadName("engine-log"); });

    const auto  sink    = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    const char* pattern = config.format == Format::Json ? kJsonPattern : kTextPattern;

    for (usize_t i = 0; i < ArrayCount(kLoggerNames); ++i) {
        auto logger = std::make_shared<spdlog::async_logger>(
            kLoggerNames[i], sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
        logger->set_pattern(pattern);
        logger->set_level(ToSpdlog(config.level));
        logger->flush_on(spdlog::level::err);
        g_loggers[i] = std::move(logger);
    }

    g_ready.store(true, std::memory_order_release);
    return {};
}

void Shutdown() noexcept {
    if (!g_ready.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    for (auto& logger : g_loggers) {
        if (logger) {
            logger->flush();
            logger.reset();
        }
    }
    spdlog::shutdown();
}

} // namespace engine::log
