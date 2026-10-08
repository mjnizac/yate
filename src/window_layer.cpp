#include <engine/window_layer.hpp>

#include <engine/application.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/swapchain.hpp>
#include <engine/vulkan/window.hpp>

#include <spng.h>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <array>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <new>
#include <vector>

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
    /// Where the next frame is written, empty when no screenshot was asked for.
    std::array<char, 512> capturePath{};
    /// Where the last frame is written. Armed into `capturePath` when that frame comes round.
    std::array<char, 512> screenshotPath{};
    /// Host-visible destination for the copy, created on the first capture and kept for the next one.
    vulkan::Buffer capture;
};

namespace {

/// Writes a presented frame as an 8-bit RGB PNG.
///
/// The swapchain is `B8G8R8A8` on every driver worth targeting and `R8G8B8A8` on the rest, so the only
/// per-format work is whether the first and third bytes are swapped. The alpha is dropped: it is 1
/// everywhere, and carrying it would make the file larger for nothing.
[[nodiscard]] b8_t WriteFramePng(const char* path, const u8_t* pixels, u32_t width, u32_t height,
                                 u32_t rowPitch, b8_t swapRedAndBlue) {
    std::FILE* file = nullptr;
    if (::fopen_s(&file, path, "wb") != 0 || file == nullptr) {
        LOG_ERROR("could not open {} for the screenshot", path);
        return false;
    }
    spng_ctx* context = spng_ctx_new(SPNG_CTX_ENCODER);
    if (context == nullptr) {
        (void)std::fclose(file);
        return false;
    }
    spng_ihdr header{};
    header.width      = width;
    header.height     = height;
    header.bit_depth  = 8;
    header.color_type = static_cast<u8_t>(SPNG_COLOR_TYPE_TRUECOLOR);

    const auto fail = [&](int code, const char* call) {
        LOG_ERROR("{} failed while writing {}: {}", call, path, spng_strerror(code));
        spng_ctx_free(context);
        (void)std::fclose(file);
        return false;
    };
    if (const int code = spng_set_png_file(context, file); code != 0) {
        return fail(code, "spng_set_png_file");
    }
    if (const int code = spng_set_ihdr(context, &header); code != 0) {
        return fail(code, "spng_set_ihdr");
    }
    if (const int code = spng_encode_image(context, nullptr, 0, SPNG_FMT_PNG,
                                           SPNG_ENCODE_PROGRESSIVE | SPNG_ENCODE_FINALIZE);
        code != 0) {
        return fail(code, "spng_encode_image");
    }

    std::pmr::vector<u8_t> row(static_cast<usize_t>(width) * 3, 0, &memory::General().Resource());
    for (u32_t y = 0; y < height; ++y) {
        const u8_t* source = pixels + static_cast<u64_t>(y) * rowPitch;
        for (u32_t x = 0; x < width; ++x) {
            const u8_t* texel = source + static_cast<u64_t>(x) * 4;
            row[x * 3 + 0]    = swapRedAndBlue ? texel[2] : texel[0];
            row[x * 3 + 1]    = texel[1];
            row[x * 3 + 2]    = swapRedAndBlue ? texel[0] : texel[2];
        }
        const int code = spng_encode_row(context, row.data(), row.size());
        if (code != 0 && code != SPNG_EOI) {
            return fail(code, "spng_encode_row");
        }
    }
    spng_ctx_free(context);
    (void)std::fclose(file);
    LOG_INFO("screenshot written to {} ({}x{})", path, width, height);
    return true;
}

} // namespace

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
    if (m_state->capture.IsValid()) {
        VulkanContext(App()).Memory().DestroyBuffer(m_state->capture);
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

void WindowLayer::ScreenshotOnLastFrame(const char* path) noexcept {
    if (m_state != nullptr && path != nullptr) {
        detail::CopyBounded(m_state->screenshotPath, std::string_view{path});
    }
}

b8_t WindowLayer::CaptureNextFrame(const char* path) noexcept {
    if (m_state == nullptr || path == nullptr || m_state->swapchain == nullptr) {
        return false;
    }
    if (!m_state->swapchain->CanCapture()) {
        LOG_WARN("the surface does not allow its images to be a transfer source, so no screenshot "
                 "can be taken");
        return false;
    }
    const VkExtent2D   extent = m_state->swapchain->Extent();
    const VkDeviceSize bytes  = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    if (m_state->capture.size < bytes) {
        vulkan::Allocator& memory = VulkanContext(App()).Memory();
        memory.DestroyBuffer(m_state->capture);
        Result<vulkan::Buffer> buffer =
            memory.CreateBuffer(vulkan::BufferDesc{.size         = bytes,
                                                   .usage        = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   .category     = vulkan::VramCategory::Readback,
                                                   .hostVisible  = true,
                                                   .randomAccess = true});
        if (!buffer) {
            LOG_ERROR("could not allocate the screenshot buffer: {}",
                      buffer.error().Format().data());
            return false;
        }
        m_state->capture = *buffer;
    }
    detail::CopyBounded(m_state->capturePath, std::string_view{path});
    return true;
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

    if (m_state->screenshotPath[0] != '\0' && m_state->stopAfter != 0
        && swapchain.FramesPresented() + 1 >= m_state->stopAfter) {
        (void)CaptureNextFrame(m_state->screenshotPath.data());
        m_state->screenshotPath[0] = '\0';
    }

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
    //
    // Depth clears to *zero*, not one: the projection is reversed, so near is 1 and far is 0, and the
    // pipeline compares with `GREATER_OR_EQUAL`. Clearing to one leaves only the near plane able to pass
    // the test, which draws every tile and discards every fragment — a viewer that presents the clear
    // colour with the panels on top of it, which is exactly what it did.
    const std::array<VkClearValue, 2> clears{
        VkClearValue{.color = {{0.05f, 0.06f, 0.09f, 1.0f}}},
        VkClearValue{.depthStencil = {.depth = 0.0f, .stencil = 0}}};
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

    // A screenshot is recorded into the frame's own command buffer, after the pass: the image is in
    // `PRESENT_SRC` by then, so it moves to `TRANSFER_SRC`, is copied out and moves back. Doing it here
    // rather than on a second submission means the copy sees exactly the frame that gets presented.
    const b8_t capturing = m_state->capturePath[0] != '\0' && m_state->capture.IsValid();
    if (capturing) {
        const VkExtent2D     extent = swapchain.Extent();
        const VkImageMemoryBarrier toTransfer{
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = 0,
            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = swapchain.CurrentImage(),
            .subresourceRange    = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .baseMipLevel   = 0,
                                    .levelCount     = 1,
                                    .baseArrayLayer = 0,
                                    .layerCount     = 1}};
        vkCmdPipelineBarrier(frame.commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toTransfer);

        const VkBufferImageCopy region{
            .bufferOffset      = 0,
            .bufferRowLength   = extent.width,
            .bufferImageHeight = extent.height,
            .imageSubresource  = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .mipLevel       = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount     = 1},
            .imageOffset       = {0, 0, 0},
            .imageExtent       = {extent.width, extent.height, 1}};
        vkCmdCopyImageToBuffer(frame.commands, swapchain.CurrentImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_state->capture.handle, 1,
                               &region);

        VkImageMemoryBarrier toPresent = toTransfer;
        toPresent.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
        toPresent.dstAccessMask        = 0;
        toPresent.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toPresent.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(frame.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toPresent);
    }

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

    if (capturing) {
        // A debug path, so a full device wait rather than another fence: the frame has to be on the
        // host before the pixels can be read, and nothing else is waiting on this.
        if (Status idle = VulkanContext(App()).WaitIdle(); !idle) {
            LOG_ERROR("could not wait for the screenshot frame: {}", idle.error().Format().data());
        } else {
            const VkExtent2D extent = swapchain.Extent();
            (void)WriteFramePng(m_state->capturePath.data(),
                                static_cast<const u8_t*>(m_state->capture.mapped), extent.width,
                                extent.height, extent.width * 4,
                                swapchain.Format() == VK_FORMAT_B8G8R8A8_SRGB
                                    || swapchain.Format() == VK_FORMAT_B8G8R8A8_UNORM);
        }
        m_state->capturePath[0] = '\0';
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
