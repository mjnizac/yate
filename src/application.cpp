#include <engine/application.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/memory/arena.hpp>
#include <engine/memory/general.hpp>
#include <engine/vulkan/context.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

namespace engine {

Application::Application(ApplicationState& state) noexcept : m_state(state) {}

Application::~Application() { DetachLayers(); }

ApplicationState& StateOf(Application& application) noexcept { return application.m_state; }

terrain::KernelLibrary& KernelsOf(Application& application) noexcept {
    return StateOf(application).kernels;
}

vulkan::Context& VulkanContext(Application& application) noexcept {
    ApplicationState& state = StateOf(application);
    ENGINE_ASSERT(state.vulkan != nullptr, "the application has no Vulkan context");
    return *state.vulkan;
}

void* Application::AllocateLayerStorage(usize_t size, usize_t align) {
    return memory::General().Allocate(size, align);
}

void Application::AttachLayer(Layer* layer, LayerDestructor destructor) {
    ENGINE_ASSERT_RETURN(, layer != nullptr, "AttachLayer received a null layer");
    m_state.layers.push_back(ApplicationState::LayerEntry{.layer = layer, .destroy = destructor});
    LOG_INFO("layer '{}' attached", layer->Name());
    layer->OnAttach();
}

void Application::DetachLayers() noexcept {
    while (!m_state.layers.empty()) {
        const ApplicationState::LayerEntry entry = m_state.layers.back();
        m_state.layers.pop_back();
        LOG_INFO("layer '{}' detached", entry.layer->Name());
        entry.layer->OnDetach();
        entry.destroy(entry.layer);
        memory::General().Free(entry.layer);
    }
}

void Application::Run() {
    if (m_state.stopRequested.load(std::memory_order_acquire)) {
        LOG_INFO("not entering the update loop: a layer asked to stop while attaching");
        return;
    }
    m_state.running.store(true, std::memory_order_release);
    LOG_INFO("entering the update loop with {} layer(s)", m_state.layers.size());

    while (m_state.running.load(std::memory_order_acquire)) {
#ifdef TRACY_ENABLE
        ZoneScopedN("Application::Tick");
#endif
        for (const ApplicationState::LayerEntry& entry : m_state.layers) {
#ifdef TRACY_ENABLE
            ZoneTransientN(layerZone, entry.layer->Name(), true);
#endif
            entry.layer->OnUpdate();
        }

        // Per-tick scratch is released as a whole, and every memory plot is refreshed at
        // least once per tick (spec section 7.4 rule 2).
        memory::FrameArena().Reset();
        memory::UpdatePlots();
        if (m_state.vulkan != nullptr) {
            m_state.vulkan->EndTick();
        }
        ++m_state.tick;

#ifdef TRACY_ENABLE
        if (m_state.mode == RunMode::Graphics) {
            FrameMark;
        }
#endif
    }

    LOG_INFO("update loop finished after {} tick(s), exit code {}", m_state.tick,
             static_cast<i32_t>(m_state.exit));
}

void Application::Stop() noexcept {
    // The flag is only set when there is no loop to stop. Stopping *during* a run is the ordinary end of
    // that run and must not poison a later one, which is what a flag set unconditionally does: the first
    // `StopAfter` would make every subsequent `Run` return immediately. Set outside a run, it means a
    // layer failed while attaching and the loop must not start at all.
    if (!m_state.running.load(std::memory_order_acquire)) {
        m_state.stopRequested.store(true, std::memory_order_release);
    }
    m_state.running.store(false, std::memory_order_release);
}

b8_t Application::IsRunning() const noexcept {
    return m_state.running.load(std::memory_order_acquire);
}

RunMode Application::Mode() const noexcept { return m_state.mode; }

u64_t Application::TickCount() const noexcept { return m_state.tick; }

ExitCode Application::Exit() const noexcept { return m_state.exit; }

void Application::AddExit(ExitCode code) noexcept { m_state.exit |= code; }

void Application::Fail(const Error& error, ExitCode code) noexcept {
    LOG_ERROR("{}", error.Format().data());
    m_state.exit |= code;
    Stop();
}

} // namespace engine
