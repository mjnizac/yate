#include <engine/vulkan/swapchain.hpp>

#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/vulkan/context.hpp>

#include <algorithm>
#include <utility>

namespace engine::vulkan {
namespace {

/// How long a frame may take before the wait is treated as a hang rather than as slowness.
///
/// Generous on purpose: a first frame that has to compile pipelines, or a driver doing a shader cache
/// miss, can take a visible fraction of a second. Anything past this is not slow, it is stuck.
constexpr u64_t kFrameTimeoutNanoseconds = 5ull * 1000 * 1000 * 1000;

/// Preferred surface format, in order. The viewer renders in linear space and writes an sRGB
/// attachment, so the hardware does the encode on write; picking a UNORM format instead would mean
/// doing it in the shader and getting it wrong somewhere.
[[nodiscard]] VkSurfaceFormatKHR PickSurfaceFormat(const VkSurfaceFormatKHR* formats, u32_t count) {
    for (u32_t i = 0; i < count; ++i) {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_SRGB
             || formats[i].format == VK_FORMAT_R8G8B8A8_SRGB)
            && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return formats[i];
        }
    }
    // The spec guarantees at least one format, so the first is a valid fallback rather than a guess.
    return count != 0 ? formats[0] : VkSurfaceFormatKHR{};
}

/// `MAILBOX` when offered, `FIFO` otherwise.
///
/// Mailbox replaces a queued image instead of blocking, so the viewer stays responsive while the camera
/// moves without tearing. `FIFO` is the only mode the specification requires to exist, so it is the
/// fallback and never a failure.
[[nodiscard]] VkPresentModeKHR PickPresentMode(const VkPresentModeKHR* modes, u32_t count) {
    for (u32_t i = 0; i < count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

/// Images to ask for: one more than the minimum, so the presentation engine always has one to show
/// while the application draws into another, capped by whatever maximum the surface declares.
[[nodiscard]] u32_t PickImageCount(const VkSurfaceCapabilitiesKHR& capabilities) {
    u32_t count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0 && count > capabilities.maxImageCount) {
        count = capabilities.maxImageCount;
    }
    return count;
}

} // namespace

Result<Swapchain> Swapchain::Create(Context& context, VkSurfaceKHR surface, u32_t width,
                                    u32_t height) {
    if (surface == VK_NULL_HANDLE) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Vulkan, "a swapchain needs a surface");
    }

    Swapchain swapchain;
    swapchain.m_context = &context;
    swapchain.m_device  = context.Device();
    swapchain.m_surface = surface;
    swapchain.m_images = std::pmr::vector<VkImage>(&memory::General().Resource());
    swapchain.m_renderFinished = std::pmr::vector<VkSemaphore>(&memory::General().Resource());
    swapchain.m_views  = std::pmr::vector<VkImageView>(&memory::General().Resource());
    swapchain.m_framebuffers = std::pmr::vector<VkFramebuffer>(&memory::General().Resource());

    if (Status frames = swapchain.CreateFrames(); !frames) {
        return std::unexpected(frames.error());
    }
    if (Status sized = swapchain.CreateSizeDependent(width, height); !sized) {
        return std::unexpected(sized.error());
    }
    return swapchain;
}

Status Swapchain::CreateFrames() {
    // One pool for the frame command buffers, with RESET_COMMAND_BUFFER because each frame's buffer is
    // re-recorded every time its slot comes round.
    const VkCommandPoolCreateInfo poolInfo{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_context->GraphicsQueue().Family()};
    VK_TRY(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool));

    std::array<VkCommandBuffer, kFramesInFlight> buffers{};
    const VkCommandBufferAllocateInfo           allocateInfo{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext              = nullptr,
        .commandPool        = m_commandPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = kFramesInFlight};
    VK_TRY(vkAllocateCommandBuffers(m_device, &allocateInfo, buffers.data()));

    const VkSemaphoreCreateInfo semaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    // Signalled, so the first frame does not wait for a submission that never happened.
    const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                      .pNext = nullptr,
                                      .flags = VK_FENCE_CREATE_SIGNALED_BIT};

    for (u32_t i = 0; i < kFramesInFlight; ++i) {
        m_frames[i].commands = buffers[i];
        VK_TRY(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_frames[i].imageAvailable));
        VK_TRY(vkCreateFence(m_device, &fenceInfo, nullptr, &m_frames[i].inFlight));
    }
    return {};
}

Status Swapchain::CreateSizeDependent(u32_t width, u32_t height) {
    const VkPhysicalDevice physicalDevice = m_context->PhysicalDevice();

    VkSurfaceCapabilitiesKHR capabilities{};
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_surface, &capabilities));

    // `currentExtent` of 0xFFFFFFFF means the surface defers to the swapchain, which is when the
    // window's framebuffer size is the answer; otherwise the surface dictates and the request is
    // ignored, so it is not worth arguing with.
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == ~0u) {
        extent.width  = std::clamp(width, capabilities.minImageExtent.width,
                                   capabilities.maxImageExtent.width);
        extent.height = std::clamp(height, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "the surface is {}x{}; the caller must skip the frame while it is minimized",
                    extent.width, extent.height);
    }

    u32_t formatCount = 0;
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &formatCount, nullptr));
    std::pmr::vector<VkSurfaceFormatKHR> formats(formatCount, &memory::General().Resource());
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_surface, &formatCount,
                                                formats.data()));

    u32_t modeCount = 0;
    VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &modeCount,
                                                     nullptr));
    std::pmr::vector<VkPresentModeKHR> modes(modeCount, &memory::General().Resource());
    VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_surface, &modeCount,
                                                     modes.data()));

    const VkSurfaceFormatKHR surfaceFormat = PickSurfaceFormat(formats.data(), formatCount);
    m_presentMode                          = PickPresentMode(modes.data(), modeCount);
    m_format                               = surfaceFormat.format;
    m_extent                               = extent;

    const VkSwapchainKHR previous = m_handle;
    const VkSwapchainCreateInfoKHR info{
        .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext                 = nullptr,
        .flags                 = 0,
        .surface               = m_surface,
        .minImageCount         = PickImageCount(capabilities),
        .imageFormat           = surfaceFormat.format,
        .imageColorSpace       = surfaceFormat.colorSpace,
        .imageExtent           = extent,
        .imageArrayLayers      = 1,
        .imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
        .preTransform          = capabilities.currentTransform,
        .compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode           = m_presentMode,
        .clipped               = VK_TRUE,
        // Handing the old swapchain over lets the driver reuse its images instead of allocating a new
        // set and freeing the old one.
        .oldSwapchain = previous};
    VK_TRY(vkCreateSwapchainKHR(m_device, &info, nullptr, &m_handle));
    if (previous != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, previous, nullptr);
    }

    u32_t imageCount = 0;
    VK_TRY(vkGetSwapchainImagesKHR(m_device, m_handle, &imageCount, nullptr));
    m_images.resize(imageCount);
    VK_TRY(vkGetSwapchainImagesKHR(m_device, m_handle, &imageCount, m_images.data()));

    const VkSemaphoreCreateInfo renderFinishedInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    m_renderFinished.resize(imageCount, VK_NULL_HANDLE);
    for (u32_t i = 0; i < imageCount; ++i) {
        VK_TRY(vkCreateSemaphore(m_device, &renderFinishedInfo, nullptr, &m_renderFinished[i]));
    }

    m_views.resize(imageCount, VK_NULL_HANDLE);
    for (u32_t i = 0; i < imageCount; ++i) {
        const VkImageViewCreateInfo viewInfo{
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .image            = m_images[i],
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = m_format,
            .components       = {},
            .subresourceRange = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel   = 0,
                                 .levelCount     = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount     = 1}};
        VK_TRY(vkCreateImageView(m_device, &viewInfo, nullptr, &m_views[i]));
    }

    Result<VkFormat> depthFormat = PickDepthFormat(physicalDevice);
    if (!depthFormat) {
        return std::unexpected(depthFormat.error());
    }
    Result<Image> depth = m_context->Memory().CreateImage(
        ImageDesc{.width    = extent.width,
                  .height   = extent.height,
                  .format   = *depthFormat,
                  .usage    = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                  .aspect   = VK_IMAGE_ASPECT_DEPTH_BIT,
                  .category = VramCategory::Viewer});
    if (!depth) {
        return std::unexpected(depth.error());
    }
    m_depth = std::move(*depth);

    // Created once and kept across resizes: a render pass depends on formats, not on extents, and the
    // graphics pipelines are built against this one handle.
    if (!m_renderPass.IsValid()) {
        Result<RenderPass> pass = RenderPass::Create(m_device, m_format, *depthFormat);
        if (!pass) {
            return std::unexpected(pass.error());
        }
        m_renderPass = std::move(*pass);
    }

    m_framebuffers.resize(imageCount, VK_NULL_HANDLE);
    for (u32_t i = 0; i < imageCount; ++i) {
        const std::array<VkImageView, 2> attachments{m_views[i], m_depth.view};
        const VkFramebufferCreateInfo    framebufferInfo{
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .flags           = 0,
            .renderPass      = m_renderPass.Handle(),
            .attachmentCount = static_cast<u32_t>(attachments.size()),
            .pAttachments    = attachments.data(),
            .width           = extent.width,
            .height          = extent.height,
            .layers          = 1};
        VK_TRY(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]));
    }

    LOG_INFO("swapchain ready, {}x{}, {} image(s), {} present mode", extent.width, extent.height,
             imageCount, m_presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    return {};
}

Status Swapchain::Recreate(u32_t width, u32_t height) {
    ReleaseSizeDependent();
    return CreateSizeDependent(width, height);
}

Result<Swapchain::AcquireResult> Swapchain::Acquire() {
    const FrameSync& frame = m_frames[m_frameIndex];

    // Waiting on this frame's fence is what bounds the pipeline: the CPU may be at most
    // `kFramesInFlight` frames ahead, and the command buffer about to be re-recorded is known to be
    // free.
    VK_TRY(vkWaitForFences(m_device, 1, &frame.inFlight, VK_TRUE, kFrameTimeoutNanoseconds));

    const VkResult acquired = vkAcquireNextImageKHR(m_device, m_handle, kFrameTimeoutNanoseconds,
                                                    frame.imageAvailable, VK_NULL_HANDLE,
                                                    &m_imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        return AcquireResult::OutOfDate;
    }
    // SUBOPTIMAL means the image still presents, just not ideally. Using it and recreating after the
    // present keeps the semaphore that was just signalled from being abandoned, which is what a
    // recreate here would do.
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        return std::unexpected(MakeVulkanError(acquired, "vkAcquireNextImageKHR"));
    }

    // Reset only now that the frame is certain to be submitted, so an early return above cannot leave an
    // unsignalled fence nothing will ever signal.
    VK_TRY(vkResetFences(m_device, 1, &frame.inFlight));
    return AcquireResult::Ready;
}

Result<Swapchain::AcquireResult> Swapchain::Present() {
    const FrameSync& frame = m_frames[m_frameIndex];
    // Keyed by image, not by frame: see the note on `FrameSync`.
    const VkSemaphore renderFinished = m_renderFinished[m_imageIndex];

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo         submitInfo{.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                          .pNext                = nullptr,
                                          .waitSemaphoreCount   = 1,
                                          .pWaitSemaphores      = &frame.imageAvailable,
                                          .pWaitDstStageMask    = &waitStage,
                                          .commandBufferCount   = 1,
                                          .pCommandBuffers      = &frame.commands,
                                          .signalSemaphoreCount = 1,
                                          .pSignalSemaphores    = &renderFinished};
    VK_TRY(vkQueueSubmit(m_context->GraphicsQueue().Handle(), 1, &submitInfo, frame.inFlight));

    const VkPresentInfoKHR presentInfo{.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                       .pNext              = nullptr,
                                       .waitSemaphoreCount = 1,
                                       .pWaitSemaphores    = &renderFinished,
                                       .swapchainCount     = 1,
                                       .pSwapchains        = &m_handle,
                                       .pImageIndices      = &m_imageIndex,
                                       .pResults           = nullptr};
    const VkResult presented = vkQueuePresentKHR(m_context->GraphicsQueue().Handle(), &presentInfo);

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
    ++m_framesPresented;

    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        return AcquireResult::OutOfDate;
    }
    if (presented != VK_SUCCESS) {
        return std::unexpected(MakeVulkanError(presented, "vkQueuePresentKHR"));
    }
    return AcquireResult::Ready;
}

VkFramebuffer Swapchain::Framebuffer() const noexcept {
    return m_imageIndex < m_framebuffers.size() ? m_framebuffers[m_imageIndex] : VK_NULL_HANDLE;
}

void Swapchain::ReleaseSizeDependent() noexcept {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    for (VkFramebuffer framebuffer : m_framebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
    }
    m_framebuffers.clear();
    for (VkImageView view : m_views) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, view, nullptr);
        }
    }
    m_views.clear();
    for (VkSemaphore semaphore : m_renderFinished) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
    }
    m_renderFinished.clear();
    m_images.clear();
    if (m_depth.IsValid() && m_context != nullptr) {
        m_context->Memory().DestroyImage(m_depth);
    }
}

Swapchain::~Swapchain() { Release(); }

void Swapchain::Release() noexcept {
    ReleaseSizeDependent();
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    for (FrameSync& frame : m_frames) {
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, frame.imageAvailable, nullptr);
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, frame.inFlight, nullptr);
        }
        frame = FrameSync{};
    }
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    m_renderPass = RenderPass{};
    if (m_handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_handle, nullptr);
        m_handle = VK_NULL_HANDLE;
    }
    m_device  = VK_NULL_HANDLE;
    m_context = nullptr;
}

Swapchain::Swapchain(Swapchain&& other) noexcept { *this = std::move(other); }

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    m_context         = other.m_context;
    m_device          = other.m_device;
    m_surface         = other.m_surface;
    m_handle          = other.m_handle;
    m_extent          = other.m_extent;
    m_format          = other.m_format;
    m_presentMode     = other.m_presentMode;
    m_images          = std::move(other.m_images);
    m_renderFinished  = std::move(other.m_renderFinished);
    m_views           = std::move(other.m_views);
    m_framebuffers    = std::move(other.m_framebuffers);
    m_depth           = other.m_depth;
    m_renderPass      = std::move(other.m_renderPass);
    m_frames          = other.m_frames;
    m_commandPool     = other.m_commandPool;
    m_frameIndex      = other.m_frameIndex;
    m_imageIndex      = other.m_imageIndex;
    m_framesPresented = other.m_framesPresented;

    other.m_context     = nullptr;
    other.m_device      = VK_NULL_HANDLE;
    other.m_handle      = VK_NULL_HANDLE;
    other.m_commandPool = VK_NULL_HANDLE;
    other.m_depth       = Image{};
    other.m_frames      = {};
    return *this;
}

} // namespace engine::vulkan
