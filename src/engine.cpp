#include <engine/engine.hpp>

#include <engine/application.hpp>
#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/platform.hpp>
#include <engine/terrain/kernels.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/swapchain.hpp>
#include <engine/vulkan/window.hpp>

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
    /// Graphics mode only. The window is created before the context, because its surface is what makes
    /// a physical device acceptable, and destroyed after it, because the surface belongs to the instance.
    vulkan::Window*    window    = nullptr;
    vulkan::Swapchain* swapchain = nullptr;

    alignas(ApplicationState) std::byte stateStorage[sizeof(ApplicationState)]{};
    alignas(Application) std::byte applicationStorage[sizeof(Application)]{};
    alignas(vulkan::Context) std::byte vulkanStorage[sizeof(vulkan::Context)]{};
    alignas(vulkan::Window) std::byte windowStorage[sizeof(vulkan::Window)]{};
    alignas(vulkan::Swapchain) std::byte swapchainStorage[sizeof(vulkan::Swapchain)]{};

    b8_t platformReady     = false;
    b8_t logReady          = false;
    b8_t memoryReady       = false;
    b8_t windowSystemReady = false;
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

    // 5. Graphics mode only: GLFW and the main window. The window is created here, hidden, because its
    //    surface has to exist before the physical device is chosen: presentation support is part of what
    //    makes a device acceptable in Graphics mode (spec section 6). In Headless mode GLFW is never
    //    initialized at all.
    //
    //    The surface cannot be created yet, though, because it needs the instance the context creates.
    //    So this is in two parts: GLFW now, and the window itself once the instance exists. The context
    //    therefore creates its instance first, hands it over, and only then picks a device.
    if (info.mode == RunMode::Graphics) {
        LOG_DEBUG("init step 'window system' starting");
        if (Status status = vulkan::Window::InitializeWindowSystem(); !status) {
            return std::unexpected(FailInit("window system", status.error()));
        }
        g_runtime.windowSystemReady = true;
    }

    // 6. Vulkan context: instance, messenger and device, selected for the run mode.
    LOG_DEBUG("init step 'vulkan' starting");
    Result<vulkan::Context> context = vulkan::Context::Create(
        vulkan::ContextCreateInfo{.mode              = info.mode,
                                  .applicationName   = info.name,
                                  .enableValidation  = kValidation,
                                  .verboseValidation = kValidationVerbose,
                                  .forcedDeviceUuid  = info.deviceUuid,
                                  .surface           = VK_NULL_HANDLE,
                                  // The context calls this once its instance exists and before it looks
                                  // at any device, so the surface it needs is there when it needs it.
                                  .createSurface =
                                      info.mode != RunMode::Graphics
                                          ? nullptr
                                          : +[](VkInstance instance, void* user)
                                                -> Result<VkSurfaceKHR> {
                                        const AppInfo& appInfo = *static_cast<const AppInfo*>(user);
                                        Result<vulkan::Window> window =
                                            vulkan::Window::Create(instance, appInfo.window);
                                        if (!window) {
                                            return std::unexpected(window.error());
                                        }
                                        g_runtime.window = ::new (g_runtime.windowStorage)
                                            vulkan::Window(std::move(*window));
                                        return g_runtime.window->Surface();
                                    },
                                  .createSurfaceUser = const_cast<AppInfo*>(&info)});
    if (!context) {
        return std::unexpected(FailInit("vulkan", context.error()));
    }
    g_runtime.vulkan =
        ::new (g_runtime.vulkanStorage) vulkan::Context(std::move(*context));

    // 7. Graphics mode only: swapchain, render pass, depth buffer and framebuffers.
    if (info.mode == RunMode::Graphics) {
        LOG_DEBUG("init step 'swapchain' starting");
        const std::array<u32_t, 2> size = g_runtime.window->FramebufferSize();
        Result<vulkan::Swapchain>  swapchain =
            vulkan::Swapchain::Create(*g_runtime.vulkan, g_runtime.window->Surface(),
                                      size[0] != 0 ? size[0] : info.window.width,
                                      size[1] != 0 ? size[1] : info.window.height);
        if (!swapchain) {
            return std::unexpected(FailInit("swapchain", swapchain.error()));
        }
        g_runtime.swapchain =
            ::new (g_runtime.swapchainStorage) vulkan::Swapchain(std::move(*swapchain));
    }

    // 8. Lua runtime. Nothing to start: a script runs in a state created and destroyed per run, so the
    //    runtime has no process-wide setup beyond the `CPU/Lua` allocator the memory system already
    //    made (spec section 8).

    // 9. Application.
    LOG_DEBUG("init step 'application' starting");
    g_runtime.state = ::new (g_runtime.stateStorage)
        ApplicationState(info.mode, memory::General().Resource());
    g_runtime.state->vulkan    = g_runtime.vulkan;
    g_runtime.state->window    = g_runtime.window;
    g_runtime.state->swapchain = g_runtime.swapchain;

    // The pipeline map belongs to the device, not to a job: creating it per export would rebuild
    // every pipeline on every run.
    Result<terrain::KernelLibrary> kernels = terrain::KernelLibrary::Create(*g_runtime.vulkan);
    if (!kernels) {
        return std::unexpected(FailInit("kernels", kernels.error()));
    }
    g_runtime.state->kernels = std::move(*kernels);
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
    // The swapchain holds device objects, so it goes before the context; the window holds a surface that
    // belongs to the instance, so it goes before the context too, and GLFW is terminated only after
    // every window is gone (spec section 6).
    if (g_runtime.vulkan != nullptr) {
        if (Status status = g_runtime.vulkan->WaitIdle(); !status) {
            LOG_ERROR("waiting for the device to go idle failed: {}",
                      status.error().Format().data());
        }
    }
    if (g_runtime.swapchain != nullptr) {
        g_runtime.swapchain->~Swapchain();
        g_runtime.swapchain = nullptr;
    }
    if (g_runtime.window != nullptr) {
        g_runtime.window->~Window();
        g_runtime.window = nullptr;
    }
    if (g_runtime.vulkan != nullptr) {
        g_runtime.vulkan->~Context();
        g_runtime.vulkan = nullptr;
    }
    if (g_runtime.windowSystemReady) {
        vulkan::Window::ShutdownWindowSystem();
        g_runtime.windowSystemReady = false;
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
