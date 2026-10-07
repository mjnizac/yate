#pragma once

#include <engine/common.hpp>
#include <engine/engine.hpp>
#include <engine/layer.hpp>

#include <new>
#include <type_traits>
#include <utility>

namespace engine {

/// Engine-owned application storage. Declared here as an incomplete type and defined inside
/// the `IS_ENGINE` block below, so the public interface exposes no engine internals.
struct ApplicationState;

class ENGINE_API Application {
public:
    using LayerDestructor = void (*)(Layer*) noexcept;

    /// Created only by `engine::init`, which also owns the state. Public so the engine can
    /// build it without a friend declaration crossing the export boundary.
    explicit Application(ApplicationState& state) noexcept;
    ~Application();

    ENGINE_NO_COPY(Application);
    ENGINE_NO_MOVE(Application);

    /// Constructs a layer inside engine-owned storage, attaches it and returns a typed handle.
    /// `T` must derive from `Layer` and take `Application&` as its first constructor argument.
    template <typename T, typename... Args>
    T& PushLayer(Args&&... args) {
        static_assert(std::is_base_of_v<Layer, T>, "layer must derive from engine::Layer");
        void* storage = AllocateLayerStorage(sizeof(T), alignof(T));
        T*    layer   = ::new (storage) T(*this, std::forward<Args>(args)...);
        AttachLayer(layer, [](Layer* base) noexcept { static_cast<T*>(base)->~T(); });
        return *layer;
    }

    /// Updates every layer in push order until `Stop()` is called.
    void Run();

    /// Requests the loop to finish after the current iteration.
    void Stop() noexcept;

    [[nodiscard]] b8_t IsRunning() const noexcept;

    [[nodiscard]] RunMode Mode() const noexcept;

    /// Number of completed iterations of the update loop.
    [[nodiscard]] u64_t TickCount() const noexcept;

    /// Accumulated exit code. Layers OR their failures into it.
    [[nodiscard]] ExitCode Exit() const noexcept;
    void                   AddExit(ExitCode code) noexcept;

    /// Logs `error`, ORs `code` into the exit status and stops the loop.
    void Fail(const Error& error, ExitCode code) noexcept;

    /// Destroys every layer in reverse push order. Called by `engine::shutdown`.
    void DetachLayers() noexcept;

private:
    /// Engine-internal accessor, defined in src/application.cpp.
    friend ApplicationState& StateOf(Application&) noexcept;

    [[nodiscard]] void* AllocateLayerStorage(usize_t size, usize_t align);
    void                AttachLayer(Layer* layer, LayerDestructor destructor);

    ApplicationState& m_state;
};

} // namespace engine

#ifdef IS_ENGINE

#    include <engine/terrain/kernels.hpp>

#    include <atomic>
#    include <memory_resource>
#    include <vector>

namespace engine::vulkan {
class Context;
class Swapchain;
class Window;
} // namespace engine::vulkan

namespace engine {

struct ApplicationState {
    struct LayerEntry {
        Layer*                      layer;
        Application::LayerDestructor destroy;
    };

    explicit ApplicationState(RunMode runMode, std::pmr::memory_resource& resource)
        : mode(runMode), layers(&resource) {}

    RunMode          mode;
    vulkan::Context* vulkan = nullptr;
    /// Main window and its swapchain, created by `engine::init` in Graphics mode and null in Headless.
    ///
    /// Owned by the engine rather than by the layer that uses them, because the window's surface has to
    /// exist before the device is chosen and the swapchain has to outlive any layer that is torn down and
    /// pushed again. The first `WindowLayer` adopts them.
    vulkan::Window*    window    = nullptr;
    vulkan::Swapchain* swapchain = nullptr;
    /// Pipeline map, which must outlive any single job: a pipeline is created on first use and kept
    /// for the life of the device (spec section 9, stage 5).
    terrain::KernelLibrary kernels;
    std::atomic<b8_t> running{false};
    /// Set by `Stop`, cleared by nothing.
    ///
    /// Separate from `running` because `Stop` can be called before `Run`: a layer's `OnAttach` runs
    /// during `PushLayer`, and a fatal error there used to be thrown away when `Run` set `running` to
    /// true a moment later. The viewer failing to load its script looked like a successful run of sixty
    /// empty frames.
    std::atomic<b8_t> stopRequested{false};
    u64_t                        tick = 0;
    ExitCode                     exit = ExitCode::Success;
    std::pmr::vector<LayerEntry> layers;
};

/// Vulkan context of the running application. Never null after a successful `init`.
[[nodiscard]] vulkan::Context& VulkanContext(Application& application) noexcept;

/// Engine-side view of the application state.
[[nodiscard]] ApplicationState& StateOf(Application& application) noexcept;

/// Pipeline map of the running application.
[[nodiscard]] terrain::KernelLibrary& KernelsOf(Application& application) noexcept;

} // namespace engine

#endif // IS_ENGINE
