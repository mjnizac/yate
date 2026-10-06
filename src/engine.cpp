#include <engine/engine.hpp>

#include <engine/application.hpp>
#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/platform.hpp>
#include <engine/vulkan/context.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <cstdio>
#include <new>
#include <utility>

#ifndef ENGINE_COMMIT_HASH
#    define ENGINE_COMMIT_HASH "unknown"
#endif
#ifndef ENGINE_VERSION_STRING
#    define ENGINE_VERSION_STRING "0.0.0"
#endif

namespace engine {

const char* ToString(RunMode mode) noexcept {
    return mode == RunMode::Graphics ? "Graphics" : "Headless";
}

const char* ToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::None: return "none";
        case ErrorCode::InvalidArgument: return "invalid-argument";
        case ErrorCode::NotFound: return "not-found";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::OutOfMemory: return "out-of-memory";
        case ErrorCode::VulkanError: return "vulkan";
        case ErrorCode::IoError: return "io";
        case ErrorCode::ScriptError: return "script";
        case ErrorCode::InternalError: return "internal";
    }
    return "unknown";
}

const char* ToString(ErrorStage stage) noexcept {
    switch (stage) {
        case ErrorStage::None: return "engine";
        case ErrorStage::Init: return "init";
        case ErrorStage::Platform: return "platform";
        case ErrorStage::Memory: return "memory";
        case ErrorStage::Vulkan: return "vulkan";
        case ErrorStage::Script: return "script";
        case ErrorStage::Validation: return "validation";
        case ErrorStage::Compile: return "compile";
        case ErrorStage::Evaluate: return "evaluate";
        case ErrorStage::Export: return "export";
        case ErrorStage::Shutdown: return "shutdown";
    }
    return "unknown";
}

std::array<char, Error::kMaxFormat> Error::Format() const noexcept {
    std::array<char, kMaxFormat> text{};
    // `file:line: [stage] message` is the one error format the whole engine emits, so scripts,
    // the viewer panel and the JSON log all point at the same place (spec section 11).
    if (file[0] != 0) {
        std::snprintf(text.data(), text.size(), "%s:%u: [%s] %s", file.data(), line,
                      ToString(stage), message.data());
    } else {
        std::snprintf(text.data(), text.size(), "[%s] %s", ToString(stage), message.data());
    }
    return text;
}

const char* Version() noexcept { return ENGINE_VERSION_STRING; }

const char* CommitHash() noexcept { return ENGINE_COMMIT_HASH; }

namespace {

/// Subsystems the engine owns, created in startup order and destroyed in exact reverse order.
///
/// Held in static storage with placement construction, because engine code never calls `new`.
struct Runtime {
    ApplicationState* state       = nullptr;
    Application*      application = nullptr;
    vulkan::Context*  vulkan      = nullptr;

    alignas(ApplicationState) std::byte stateStorage[sizeof(ApplicationState)]{};
    alignas(Application) std::byte applicationStorage[sizeof(Application)]{};
    alignas(vulkan::Context) std::byte vulkanStorage[sizeof(vulkan::Context)]{};

    b8_t platformReady = false;
    b8_t logReady      = false;
    b8_t memoryReady   = false;
};

Runtime g_runtime;

/// Logs the host banner once the loggers exist. `platform::Init` runs before logging, so it
/// cannot do this itself.
void LogHostBanner(const AppInfo& info) {
    LOG_INFO("terrain engine {} ({}) starting in {} mode", Version(), CommitHash(),
             ToString(info.mode));
    LOG_INFO("host: {} | {} | {} logical cores | {} MiB RAM", platform::OsName(),
             platform::CpuName(), platform::LogicalCoreCount(),
             platform::PhysicalMemoryBytes() / (1024 * 1024));
    LOG_DEBUG("executable directory: {}", platform::ExecutableDirectory());
}

Error FailInit(std::string_view step, const Error& error) {
    LOG_ERROR("init step {} failed: {}", step, error.Format().data());
    return error;
}

#ifdef ENGINE_VULKAN_VALIDATION
constexpr b8_t kValidation = true;
#else
constexpr b8_t kValidation = false;
#endif
#ifdef ENGINE_VULKAN_VALIDATION_VERBOSE
constexpr b8_t kValidationVerbose = true;
#else
constexpr b8_t kValidationVerbose = false;
#endif

} // namespace

Result<Application*> init(const AppInfo& info) {
#ifdef TRACY_ENABLE
    ZoneScopedN("engine::init");
#endif

    if (g_runtime.application != nullptr) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Init,
                    "the engine is already initialized");
    }

    // 1. Platform.
    if (Status status = platform::Init(); !status) {
        return std::unexpected(FailInit("platform", status.error()));
    }
    g_runtime.platformReady = true;

    // 2. Tracy. The client starts with the process; naming the session makes traces identifiable.
#ifdef TRACY_ENABLE
    TracyAppInfo(info.name.data(), info.name.size());
#endif

    // 3. Logging.
    if (Status status = log::Init(log::Config{.level = info.logLevel, .format = info.logFormat});
        !status) {
        return std::unexpected(status.error());
    }
    g_runtime.logReady = true;
    LogHostBanner(info);

    // 4. Memory system.
    LOG_DEBUG("init step 'memory' starting");
    if (Status status = memory::Init(); !status) {
        return std::unexpected(FailInit("memory", status.error()));
    }
    g_runtime.memoryReady = true;

    // 5. Graphics mode only: GLFW, the main window and its surface, which must exist before the
    //    device is selected. Not implemented yet (milestone 9).
    if (info.mode == RunMode::Graphics) {
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Init,
                    "Graphics mode is not implemented yet; see docs/status.md (milestone 9)");
    }

    // 6. Vulkan context: instance, messenger and device, selected for the run mode.
    LOG_DEBUG("init step 'vulkan' starting");
    Result<vulkan::Context> context =
        vulkan::Context::Create(vulkan::ContextCreateInfo{.mode              = info.mode,
                                                         .applicationName   = info.name,
                                                         .enableValidation  = kValidation,
                                                         .verboseValidation = kValidationVerbose,
                                                         .forcedDeviceUuid  = info.deviceUuid,
                                                         .surface           = VK_NULL_HANDLE});
    if (!context) {
        return std::unexpected(FailInit("vulkan", context.error()));
    }
    g_runtime.vulkan =
        ::new (g_runtime.vulkanStorage) vulkan::Context(std::move(*context));

    // 7. Graphics mode only: swapchain, render pass, depth buffer and framebuffers (milestone 9).
    // 8. Lua runtime (milestone 6).

    // 9. Application.
    LOG_DEBUG("init step 'application' starting");
    g_runtime.state = ::new (g_runtime.stateStorage)
        ApplicationState(info.mode, memory::General().Resource());
    g_runtime.state->vulkan = g_runtime.vulkan;
    g_runtime.application =
        ::new (g_runtime.applicationStorage) Application(*g_runtime.state);

    LOG_INFO("engine ready");
    return g_runtime.application;
}

Status shutdown(Application* application) {
#ifdef TRACY_ENABLE
    ZoneScopedN("engine::shutdown");
#endif
    // A failed `init` leaves some subsystems up, so shutdown(nullptr) must still tear them down.
    if (application == nullptr && g_runtime.application == nullptr && !g_runtime.platformReady
        && !g_runtime.logReady && !g_runtime.memoryReady && g_runtime.vulkan == nullptr) {
        return {};
    }
    if (application != nullptr && application != g_runtime.application) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Shutdown,
                    "shutdown received an application the engine does not own");
    }

    LOG_INFO("engine shutting down");

    // Reverse of the startup order.
    if (g_runtime.application != nullptr) {
        g_runtime.application->DetachLayers();
        g_runtime.application->~Application();
        g_runtime.application = nullptr;
    }
    if (g_runtime.state != nullptr) {
        g_runtime.state->~ApplicationState();
        g_runtime.state = nullptr;
    }
    if (g_runtime.vulkan != nullptr) {
        if (Status status = g_runtime.vulkan->WaitIdle(); !status) {
            LOG_ERROR("waiting for the device to go idle failed: {}",
                      status.error().Format().data());
        }
        g_runtime.vulkan->~Context();
        g_runtime.vulkan = nullptr;
    }
    if (g_runtime.memoryReady) {
        memory::Shutdown();
        g_runtime.memoryReady = false;
    }
    if (g_runtime.logReady) {
        LOG_INFO("engine shutdown complete");
        log::Shutdown();
        g_runtime.logReady = false;
    }
    if (g_runtime.platformReady) {
        platform::Shutdown();
        g_runtime.platformReady = false;
    }
    return {};
}

} // namespace engine
