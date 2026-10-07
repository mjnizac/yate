#include <engine/window_layer.hpp>

#include <engine/application.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/swapchain.hpp>
#include <engine/vulkan/window.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <array>
#include <chrono>
#include <new>

namespace engine {
namespace {
using Clock = std::chrono::steady_clock;
}

/// Everything the layer needs beyond what the application already owns.
struct WindowLayer::State {
    vulkan::Window*    window    = nullptr;
    vulkan::Swapchain* swapchain = nullptr;
    Recorder           recorder  = nullptr;
    void*              recorderUser = nullptr;
    /// Set on the first update, so the first frame reports a delta of zero rather than the time since
    /// the process started.
    b8_t  timing = false;
    f64_t lastFrameSeconds = 0.0;
    f64_t deltaSeconds     = 0.0;
    /// Stops the application after this many presents. Zero runs until the window is closed.
    u64_t stopAfter = 0;
    /// The window is shown only after the first present, so the user never sees an unpainted rectangle
    /// (spec section 6).
    b8_t shown = false;
};

WindowLayer::WindowLayer(Application& application) noexcept : Layer(application) {}

WindowLayer::~WindowLayer() {
    if (m_state != nullptr) {
        m_state->~State();
        memory::General().Free(m_state);
        m_state = nullptr;
    }
}

void WindowLayer::OnAttach() {
    ApplicationState& state = StateOf(App());
    if (state.window == nullptr || state.swapchain == nullptr) {
        LOG_ERROR("WindowLayer needs a main window; the application is not in Graphics mode");
        App().Stop();
        return;
    }

    void* storage = memory::General().Allocate(sizeof(State), alignof(State));
    if (storage == nullptr) {
        LOG_ERROR("could not allocate the window layer state");
        App().Stop();
        return;
    }
    m_state = ::new (storage) State{};
    // Adopting the main window rather than creating one: its surface already decided which physical
    // device was acceptable, so a second window would have to be checked against that choice instead of
    // making it.
    m_state->window    = state.window;
    m_state->swapchain = state.swapchain;

    LOG_INFO("window layer attached to the main window");
}

void WindowLayer::OnDetach() {
    if (m_state == nullptr) {
        return;
    }
    // The swapchain is the application's, not this layer's, but the GPU has to be done with the frames
    // this layer submitted before anything is torn down.
    if (Status idle = VulkanContext(App()).WaitIdle(); !idle) {
        LOG_WARN("could not wait for the device before detaching the window layer: {}",
                 idle.error().Format().data());
    }
}

u64_t WindowLayer::FramesPresented() const noexcept {
    return m_state != nullptr && m_state->swapchain != nullptr
               ? m_state->swapchain->FramesPresented()
               : 0;
}

void WindowLayer::StopAfter(u64_t frames) noexcept {
    if (m_state != nullptr) {
        m_state->stopAfter = frames;
    }
}

f64_t WindowLayer::DeltaSeconds() const noexcept {
    return m_state != nullptr ? m_state->deltaSeconds : 0.0;
}

void WindowLayer::SetRecorder(Recorder recorder, void* user) noexcept {
    if (m_state != nullptr) {
        m_state->recorder     = recorder;
        m_state->recorderUser = user;
    }
}

void WindowLayer::OnUpdate() {
#ifdef TRACY_ENABLE
    ZoneScopedN("WindowLayer::OnUpdate");
#endif
    if (m_state == nullptr) {
        return;
    }

    vulkan::Window&    window    = *m_state->window;
    vulkan::Swapchain& swapchain = *m_state->swapchain;

    // Measured here rather than around the whole loop, so a frame that was skipped for a minimized
    // window does not hand the next one a huge delta and fling the camera across the world.
    const f64_t now = std::chrono::duration<f64_t>(Clock::now().time_since_epoch()).count();
    m_state->deltaSeconds =
        m_state->timing ? now - m_state->lastFrameSeconds : 0.0;
    m_state->lastFrameSeconds = now;
    m_state->timing           = true;

    vulkan::Window::PollEvents();
    if (window.ShouldClose()) {
        App().Stop();
        return;
    }

    // A minimized window has a zero-sized framebuffer, which no swapchain can have. Blocking on events
    // rather than spinning through frames that cannot be presented.
    const std::array<u32_t, 2> size = window.FramebufferSize();
    if (size[0] == 0 || size[1] == 0) {
        if (!window.WaitWhileMinimized()) {
            App().Stop();
        }
        return;
    }

    const auto recreate = [&]() -> b8_t {
        if (Status idle = VulkanContext(App()).WaitIdle(); !idle) {
            LOG_ERROR("could not wait for the device before recreating the swapchain: {}",
                      idle.error().Format().data());
            App().Stop();
            return false;
        }
        if (Status recreated = swapchain.Recreate(size[0], size[1]); !recreated) {
            LOG_ERROR("could not recreate the swapchain: {}", recreated.error().Format().data());
            App().Stop();
            return false;
        }
        return true;
    };

    Result<vulkan::Swapchain::AcquireResult> acquired = swapchain.Acquire();
    if (!acquired) {
        LOG_ERROR("could not acquire a swapchain image: {}", acquired.error().Format().data());
        App().Stop();
        return;
    }
    if (*acquired == vulkan::Swapchain::AcquireResult::OutOfDate) {
        // Nothing was acquired, so there is nothing to present: rebuild and let the next frame try.
        (void)recreate();
        return;
    }

    const vulkan::FrameSync& frame = swapchain.Frame();

    const VkCommandBufferBeginInfo beginInfo{.sType =
                                                 VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .pNext            = nullptr,
                                             .flags            = 0,
                                             .pInheritanceInfo = nullptr};
    if (vkBeginCommandBuffer(frame.commands, &beginInfo) != VK_SUCCESS) {
        LOG_ERROR("could not begin the frame command buffer");
        App().Stop();
        return;
    }

    // Deep enough to be obviously not black, so an empty viewer still shows that presentation works.
    const std::array<VkClearValue, 2> clears{
        VkClearValue{.color = {{0.05f, 0.06f, 0.09f, 1.0f}}},
        VkClearValue{.depthStencil = {.depth = 1.0f, .stencil = 0}}};
    const VkRenderPassBeginInfo passInfo{
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext           = nullptr,
        .renderPass      = swapchain.Pass().Handle(),
        .framebuffer     = swapchain.Framebuffer(),
        .renderArea      = {.offset = {0, 0}, .extent = swapchain.Extent()},
        .clearValueCount = static_cast<u32_t>(clears.size()),
        .pClearValues    = clears.data()};
    vkCmdBeginRenderPass(frame.commands, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

    if (m_state->recorder != nullptr) {
        const FrameContext context{.commands     = frame.commands,
                                   .extent       = swapchain.Extent(),
                                   .frameIndex   = swapchain.ImageIndex(),
                                   .deltaSeconds = m_state->deltaSeconds};
        m_state->recorder(context, m_state->recorderUser);
    }
    vkCmdEndRenderPass(frame.commands);

    if (vkEndCommandBuffer(frame.commands) != VK_SUCCESS) {
        LOG_ERROR("could not end the frame command buffer");
        App().Stop();
        return;
    }

    Result<vulkan::Swapchain::AcquireResult> presented = swapchain.Present();
    if (!presented) {
        LOG_ERROR("could not present: {}", presented.error().Format().data());
        App().Stop();
        return;
    }

    if (!m_state->shown) {
        window.Show();
        m_state->shown = true;
    }
#ifdef TRACY_ENABLE
    FrameMark;
#endif

    if (*presented == vulkan::Swapchain::AcquireResult::OutOfDate) {
        // The image was presented, so the semaphore was consumed; rebuilding now is safe and the next
        // frame gets a swapchain that matches the surface.
        (void)recreate();
    }

    if (m_state->stopAfter != 0 && swapchain.FramesPresented() >= m_state->stopAfter) {
        LOG_INFO("stopping after {} presented frame(s)", swapchain.FramesPresented());
        App().Stop();
    }
}

} // namespace engine
