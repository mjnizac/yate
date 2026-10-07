#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/vulkan/allocator.hpp>
#    include <engine/vulkan/render_pass.hpp>
#    include <engine/vulkan/vk.hpp>

#    include <array>
#    include <memory_resource>
#    include <vector>

namespace engine::vulkan {

class Context;

/// Frames the CPU may run ahead of the GPU.
///
/// Two, not three: a third frame buys latency hiding that a terrain viewer does not need and costs a
/// third copy of every per-frame resource. The number is here rather than inline because the
/// synchronization objects, the command buffers and the fences all have to agree on it.
inline constexpr u32_t kFramesInFlight = 2;

/// What one frame needs to be recorded and presented without waiting on the one before it.
///
/// `renderFinished` is deliberately *not* here. It is signalled by the submit and waited on by the
/// present, so it stays in use until the presentation engine is done with the image, which the frame
/// fence says nothing about: the fence reports that the submit finished. With more swapchain images than
/// frame slots, a slot comes round again while the present that used its semaphore is still pending, and
/// signalling it a second time is the error validation reports as "may still be in use by
/// VkSwapchainKHR". So that semaphore belongs to the image, not to the frame: an image can only be
/// reused once it has been acquired again, and acquiring it means its previous present completed.
struct FrameSync {
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkFence         inFlight       = VK_NULL_HANDLE;
    VkCommandBuffer commands       = VK_NULL_HANDLE;
};

/// Swapchain, depth buffer, framebuffers and the per-frame synchronization around them.
///
/// One depth buffer rather than one per image: nothing reads it after the frame, and the render pass
/// has an external dependency on the late fragment tests that orders one frame's clear after the
/// previous frame's tests. One framebuffer per swapchain image, as the spec requires.
///
/// A resize or a lost surface is not an error. `Acquire` reports that the swapchain is out of date and
/// the caller calls `Recreate`, which is the one path that touches every handle in here.
class Swapchain {
public:
    Swapchain() = default;
    ~Swapchain();

    Swapchain(Swapchain&& other) noexcept;
    Swapchain& operator=(Swapchain&& other) noexcept;
    ENGINE_NO_COPY(Swapchain);

    [[nodiscard]] static Result<Swapchain> Create(Context& context, VkSurfaceKHR surface,
                                                u32_t width, u32_t height);

    /// Rebuilds everything that depends on the surface size. The caller waits for the device to be idle
    /// first; a zero extent means the window is minimized and the caller should skip the frame instead.
    [[nodiscard]] Status Recreate(u32_t width, u32_t height);

    /// What `Acquire` found.
    enum class AcquireResult : u32_t {
        /// An image is ready and `ImageIndex` names it.
        Ready = 0,
        /// The swapchain no longer matches the surface. Recreate and try again next frame.
        OutOfDate,
    };

    /// Waits for this frame's slot to be free, then acquires an image.
    [[nodiscard]] Result<AcquireResult> Acquire();

    /// Submits the recorded commands and presents. Reports `OutOfDate` the same way `Acquire` does.
    [[nodiscard]] Result<AcquireResult> Present();

    [[nodiscard]] const FrameSync& Frame() const noexcept { return m_frames[m_frameIndex]; }
    [[nodiscard]] u32_t            ImageIndex() const noexcept { return m_imageIndex; }
    [[nodiscard]] VkFramebuffer    Framebuffer() const noexcept;
    [[nodiscard]] VkExtent2D       Extent() const noexcept { return m_extent; }
    [[nodiscard]] const RenderPass& Pass() const noexcept { return m_renderPass; }
    [[nodiscard]] usize_t          ImageCount() const noexcept { return m_images.size(); }
    [[nodiscard]] b8_t IsValid() const noexcept { return m_handle != VK_NULL_HANDLE; }

    /// Frames presented since creation, for the frame-pacing plots and for the smoke test.
    [[nodiscard]] u64_t FramesPresented() const noexcept { return m_framesPresented; }

    /// Resets the frame command buffers, dropping every reference they hold.
    ///
    /// `vkDeviceWaitIdle` is not enough before destroying something a frame referenced. A recorded
    /// command buffer keeps referencing the pipelines, buffers and descriptor sets it mentions until it
    /// is reset, idle GPU or not, and destroying one of those is an error validation reports as "can't
    /// be called on X that is currently in use by VkCommandBuffer Y".
    ///
    /// It matters because layers detach in reverse push order: whatever was pushed above the window
    /// layer tears its resources down first, while the window layer's last recorded frame still names
    /// them. Such a layer calls this after waiting for the device and before destroying anything.
    [[nodiscard]] Status ResetFrames();

private:
    void   Release() noexcept;
    void   ReleaseSizeDependent() noexcept;
    [[nodiscard]] Status CreateSizeDependent(u32_t width, u32_t height);
    [[nodiscard]] Status CreateFrames();

    Context*     m_context = nullptr;
    VkDevice     m_device  = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    VkSwapchainKHR m_handle = VK_NULL_HANDLE;
    VkExtent2D     m_extent{};
    VkFormat       m_format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;

    std::pmr::vector<VkImage>       m_images;
    /// One per swapchain image. See the note on `FrameSync`.
    std::pmr::vector<VkSemaphore>   m_renderFinished;
    std::pmr::vector<VkImageView>   m_views;
    std::pmr::vector<VkFramebuffer> m_framebuffers;

    Image      m_depth;
    RenderPass m_renderPass;

    std::array<FrameSync, kFramesInFlight> m_frames{};
    VkCommandPool                          m_commandPool = VK_NULL_HANDLE;
    u32_t                                  m_frameIndex  = 0;
    u32_t                                  m_imageIndex  = 0;
    u64_t                                  m_framesPresented = 0;
};

} // namespace engine::vulkan

#endif // IS_ENGINE
