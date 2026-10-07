#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/vulkan/vk.hpp>

namespace engine::vulkan {

/// First depth format the device accepts as a depth-stencil attachment, preferring 32-bit float.
///
/// Asked rather than assumed: `D32_SFLOAT` is universal on desktop but not guaranteed, and a driver
/// that only offered `D24_UNORM_S8_UINT` would otherwise fail at image creation with nothing useful to
/// say about why.
[[nodiscard]] Result<VkFormat> PickDepthFormat(VkPhysicalDevice device);

/// The viewer's main render pass: one colour attachment and one depth attachment.
///
/// The classic render-pass path, not dynamic rendering (spec section 5). Every graphics pipeline is
/// created against this pass, so a second pass that reuses them has to stay render-pass compatible.
///
/// Load and store operations follow what the attachments are for. Colour clears on load and stores,
/// because it is presented. Depth clears on load and does not store, because nothing reads it after the
/// frame and saying so lets a driver keep it in tile memory.
class RenderPass {
public:
    RenderPass() = default;
    ~RenderPass();

    RenderPass(RenderPass&& other) noexcept;
    RenderPass& operator=(RenderPass&& other) noexcept;
    ENGINE_NO_COPY(RenderPass);

    [[nodiscard]] static Result<RenderPass> Create(VkDevice device, VkFormat colorFormat,
                                                 VkFormat depthFormat);

    [[nodiscard]] VkRenderPass Handle() const noexcept { return m_handle; }
    [[nodiscard]] VkFormat     ColorFormat() const noexcept { return m_colorFormat; }
    [[nodiscard]] VkFormat     DepthFormat() const noexcept { return m_depthFormat; }
    [[nodiscard]] b8_t         IsValid() const noexcept { return m_handle != VK_NULL_HANDLE; }

private:
    void Release() noexcept;

    VkDevice     m_device      = VK_NULL_HANDLE;
    VkRenderPass m_handle      = VK_NULL_HANDLE;
    VkFormat     m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat     m_depthFormat = VK_FORMAT_UNDEFINED;
};

} // namespace engine::vulkan

#endif // IS_ENGINE
