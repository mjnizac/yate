#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>
#include <engine/log.hpp>

#include <string_view>

namespace engine {

class Application;

/// Chosen once at init. Graphics mode selects the GPU for rendering and presentation;
/// Headless mode never touches the window system and selects the GPU for compute.
enum class RunMode : u32_t {
    Graphics = 0,
    Headless = 1,
};

ENGINE_API const char* ToString(RunMode mode) noexcept;

/// Bit-flag process exit codes, so a combined failure stays readable.
enum class ExitCode : i32_t {
    Success                  = 0,
    FailedToInitializeEngine = 1 << 0,
    FailedToLoadScript       = 1 << 1,
    FailedToCompileGraph     = 1 << 2,
    FailedToEvaluate         = 1 << 3,
    FailedToExport           = 1 << 4,
    FailedToShutdown         = 1 << 5,
};

constexpr ExitCode operator|(ExitCode a, ExitCode b) noexcept {
    return static_cast<ExitCode>(static_cast<i32_t>(a) | static_cast<i32_t>(b));
}

constexpr ExitCode& operator|=(ExitCode& a, ExitCode b) noexcept {
    a = a | b;
    return a;
}

constexpr b8_t HasFlag(ExitCode value, ExitCode flag) noexcept {
    return (static_cast<i32_t>(value) & static_cast<i32_t>(flag)) != 0;
}

/// Main window description. Ignored in Headless mode.
struct WindowDesc {
    std::string_view title     = "Terrain";
    u32_t            width     = 1600;
    u32_t            height    = 900;
    b8_t             resizable = true;
};

struct AppInfo {
    RunMode          mode   = RunMode::Headless;
    std::string_view name   = "engine";
    log::Level       logLevel  = log::kCompileLevel;
    log::Format      logFormat = log::Format::Text;
    /// Forces a physical device by its 32-hex-character UUID. Empty selects automatically.
    std::string_view deviceUuid;
    WindowDesc       window;
};

/// Runs the startup sequence described in spec section 5 and returns the application,
/// which the engine owns. Shutdown runs the same steps in reverse.
[[nodiscard]] ENGINE_API Result<Application*> init(const AppInfo& info);

/// Destroys the application and every engine subsystem. Safe to call with `nullptr`.
[[nodiscard]] ENGINE_API Status shutdown(Application* application);

/// Semantic version of the engine, as `major.minor.patch`.
ENGINE_API const char* Version() noexcept;

/// Short commit hash the engine was built from, or `unknown`.
ENGINE_API const char* CommitHash() noexcept;

} // namespace engine
