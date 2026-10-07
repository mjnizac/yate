#include <engine/vulkan/render_pass.hpp>

#include <engine/log.hpp>

#include <array>
#include <utility>

namespace engine::vulkan {

Result<VkFormat> PickDepthFormat(VkPhysicalDevice device) {
    static constexpr std::array<VkFormat, 3> kCandidates{
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (const VkFormat candidate : kCandidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device, candidate, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            != 0) {
            return candidate;
        }
    }
    ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Vulkan,
                "the device supports none of D32_SFLOAT, D32_SFLOAT_S8_UINT or D24_UNORM_S8_UINT as a "
                "depth attachment");
}

Result<RenderPass> RenderPass::Create(VkDevice device, VkFormat colorFormat,
                                      VkFormat depthFormat) {
    if (device == VK_NULL_HANDLE) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Vulkan,
                    "a render pass needs a device");
    }

    const std::array<VkAttachmentDescription, 2> attachments{
        VkAttachmentDescription{.flags          = 0,
                                .format         = colorFormat,
                                .samples        = VK_SAMPLE_COUNT_1_BIT,
                                .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
                                .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
                                .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
        VkAttachmentDescription{.flags   = 0,
                                .format  = depthFormat,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                // Nothing reads depth after the frame, and saying so lets a tiled
                                // architecture keep it in tile memory and never write it out.
                                .storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
                                .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};

    const VkAttachmentReference colorReference{
        .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthReference{
        .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    const VkSubpassDescription subpass{.flags                   = 0,
                                       .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       .inputAttachmentCount    = 0,
                                       .pInputAttachments       = nullptr,
                                       .colorAttachmentCount    = 1,
                                       .pColorAttachments       = &colorReference,
                                       .pResolveAttachments     = nullptr,
                                       .pDepthStencilAttachment = &depthReference,
                                       .preserveAttachmentCount = 0,
                                       .pPreserveAttachments    = nullptr};

    // Two dependencies rather than one, because the two attachments are synchronized against different
    // things. The colour attachment waits for the presentation engine to be done with the swapchain
    // image that was just acquired. The depth attachment waits for the *previous frame* to finish
    // reading and writing it, because there is one depth buffer shared by every frame in flight; without
    // this, frame N's depth clear can race frame N-1's depth tests.
    const std::array<VkSubpassDependency, 2> dependencies{
        VkSubpassDependency{.srcSubpass    = VK_SUBPASS_EXTERNAL,
                            .dstSubpass    = 0,
                            .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            .srcAccessMask = 0,
                            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            .dependencyFlags = 0},
        VkSubpassDependency{.srcSubpass   = VK_SUBPASS_EXTERNAL,
                            .dstSubpass   = 0,
                            .srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                             | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            .dependencyFlags = 0}};

    const VkRenderPassCreateInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                      .pNext = nullptr,
                                      .flags = 0,
                                      .attachmentCount =
                                          static_cast<u32_t>(attachments.size()),
                                      .pAttachments    = attachments.data(),
                                      .subpassCount    = 1,
                                      .pSubpasses      = &subpass,
                                      .dependencyCount = static_cast<u32_t>(dependencies.size()),
                                      .pDependencies   = dependencies.data()};

    RenderPass pass;
    pass.m_device      = device;
    pass.m_colorFormat = colorFormat;
    pass.m_depthFormat = depthFormat;
    VK_TRY(vkCreateRenderPass(device, &info, nullptr, &pass.m_handle));

    LOG_DEBUG("render pass ready (colour {}, depth {})", static_cast<u32_t>(colorFormat),
              static_cast<u32_t>(depthFormat));
    return pass;
}

RenderPass::~RenderPass() { Release(); }

void RenderPass::Release() noexcept {
    if (m_handle != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_handle, nullptr);
        m_handle = VK_NULL_HANDLE;
    }
    m_device = VK_NULL_HANDLE;
}

RenderPass::RenderPass(RenderPass&& other) noexcept { *this = std::move(other); }

RenderPass& RenderPass::operator=(RenderPass&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    m_device           = other.m_device;
    m_handle           = other.m_handle;
    m_colorFormat      = other.m_colorFormat;
    m_depthFormat      = other.m_depthFormat;
    other.m_device     = VK_NULL_HANDLE;
    other.m_handle     = VK_NULL_HANDLE;
    return *this;
}

} // namespace engine::vulkan
