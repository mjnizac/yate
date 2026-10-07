#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/memory/tracy_memory.hpp>
#    include <engine/vulkan/vk.hpp>

#    include <vk_mem_alloc.h>

#    include <array>

namespace engine::vulkan {

class Context;

/// Purpose of a GPU allocation. Each category is a Tracy pool of its own (spec section 7.3),
/// so the memory view answers what VRAM is used for, not only how much.
enum class VramCategory : u32_t {
    SectionBuffersR2 = 0,
    SectionBuffersR3,
    Intermediate,
    Staging,
    Readback,
    Viewer,
    Uniforms,
    Count,
};

/// Tracy pool name of a category, for example `VRAM/Section buffers R2`.
[[nodiscard]] const char* ToString(VramCategory category) noexcept;

/// A buffer with its VMA allocation and, when host-visible, its persistent mapping.
struct Buffer {
    VkBuffer        handle     = VK_NULL_HANDLE;
    VmaAllocation   allocation = nullptr;
    VkDeviceSize    size       = 0;
    VkDeviceAddress address    = 0;
    /// Non-null only for buffers created with a host-visible mapping.
    void*        mapped   = nullptr;
    VramCategory category = VramCategory::Intermediate;
    /// Set for section-pool blocks: see `BufferDesc::poolBlock`.
    b8_t poolBlock = false;

    [[nodiscard]] b8_t IsValid() const noexcept { return handle != VK_NULL_HANDLE; }
};

/// An image with its VMA allocation and its default view.
///
/// The view comes with the image because every image the engine creates is used through exactly one
/// view: a depth buffer, or a render target. Anything that needs a second view of the same image can
/// make one itself, but nothing does yet, and bundling them means one object to move and one to destroy.
struct Image {
    VkImage       handle     = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat      format     = VK_FORMAT_UNDEFINED;
    VkExtent2D    extent{};
    VramCategory  category = VramCategory::Viewer;

    [[nodiscard]] b8_t IsValid() const noexcept { return handle != VK_NULL_HANDLE; }
};

struct ImageDesc {
    u32_t              width  = 0;
    u32_t              height = 0;
    VkFormat           format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags  usage  = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VramCategory       category = VramCategory::Viewer;
};

struct BufferDesc {
    VkDeviceSize       size     = 0;
    VkBufferUsageFlags usage    = 0;
    VramCategory       category = VramCategory::Intermediate;
    /// Host-visible and persistently mapped. Used by the staging and readback rings.
    b8_t hostVisible = false;
    /// Host memory optimised for CPU reads. Set for readback, cleared for staging uploads.
    b8_t randomAccess = false;
    /// This buffer is a sub-allocated pool block, so it counts only as *reserved* in its
    /// category. What gets reported as *used* are the slots the pool hands out, through
    /// `ReportSubAllocation` (spec section 7.4 rule 2). An ordinary buffer counts as both.
    b8_t poolBlock = false;
};

/// The only path to `vkAllocateMemory` in the engine.
///
/// Every `VkDeviceMemory` block is reported as a reserved event through
/// `VmaDeviceMemoryCallbacks`, and every sub-allocation as a used event in its category pool,
/// keyed by its `VmaAllocation` handle (spec section 7.4 rules 2 and 5).
class Allocator {
public:
    Allocator() = default;
    ~Allocator();

    Allocator(Allocator&& other) noexcept;
    Allocator& operator=(Allocator&& other) noexcept;
    ENGINE_NO_COPY(Allocator);

    [[nodiscard]] static Result<Allocator> Create(Context& context);

    [[nodiscard]] VmaAllocator Handle() const noexcept { return m_allocator; }
    [[nodiscard]] VkDevice     Device() const noexcept { return m_device; }
    [[nodiscard]] b8_t         IsValid() const noexcept { return m_allocator != nullptr; }

    [[nodiscard]] Result<Buffer> CreateBuffer(const BufferDesc& desc);
    void                         DestroyBuffer(Buffer& buffer) noexcept;

    [[nodiscard]] Result<Image> CreateImage(const ImageDesc& desc);
    void                        DestroyImage(Image& image) noexcept;


    /// Flushes a host-visible range before the GPU reads it. No-op on coherent memory.
    [[nodiscard]] Status FlushBuffer(const Buffer& buffer, VkDeviceSize offset, VkDeviceSize size);

    /// Invalidates a host-visible range before the CPU reads it. No-op on coherent memory.
    [[nodiscard]] Status InvalidateBuffer(const Buffer& buffer, VkDeviceSize offset,
                                          VkDeviceSize size);

    /// Reports one slot a pool handed to a caller. `key` must be unique while the slot lives;
    /// a virtual allocation handle is the natural choice.
    void ReportSubAllocation(VramCategory category, const void* key, u64_t size) noexcept;
    void ReleaseSubAllocation(VramCategory category, const void* key, u64_t size) noexcept;

    /// Bytes currently handed to callers under `category`.
    [[nodiscard]] u64_t CategoryBytes(VramCategory category) const noexcept;

    /// Bytes reserved from the driver under `category`, including pool-block slack.
    [[nodiscard]] u64_t CategoryReserved(VramCategory category) const noexcept;

    /// Highest value `CategoryBytes` ever reached. Reported in the export metadata sidecar.
    [[nodiscard]] u64_t CategoryPeak(VramCategory category) const noexcept;

    /// Refreshes the `unused` plot of `category`.
    void UpdateCategoryPlots(VramCategory category) const noexcept;

    /// Total bytes of `VkDeviceMemory` the driver has handed VMA across the process.
    [[nodiscard]] static u64_t ReservedDeviceMemory() noexcept;

    /// What a heap has left: bytes in use and the driver's budget for it.
    struct HeapBudget {
        u64_t usage  = 0;
        u64_t budget = 0;

        [[nodiscard]] u64_t Available() const noexcept {
            return budget > usage ? budget - usage : 0;
        }
    };

    /// The device-local heap with the largest budget, which is where section buffers come from.
    ///
    /// Used before the first dispatch to decide whether a section's planned VRAM fits at all, so a
    /// job that cannot run says so with the numbers rather than failing inside VMA.
    [[nodiscard]] HeapBudget DeviceLocalBudget() const;

    /// Queries `vmaGetHeapBudgets`, plots usage and budget per heap and warns above 90%.
    /// Returns true while every heap stays under the pressure threshold.
    [[nodiscard]] b8_t UpdateBudgets();

    /// True when the last `UpdateBudgets` found a heap above 90% of its budget, which makes the
    /// evaluator reduce the number of sections in flight (spec section 7.4 rule 6).
    [[nodiscard]] b8_t UnderPressure() const noexcept { return m_underPressure; }

private:
    static constexpr usize_t kCategoryCount = static_cast<usize_t>(VramCategory::Count);

    VmaAllocator m_allocator = nullptr;
    VkDevice     m_device    = VK_NULL_HANDLE;

    /// One used pool per category. The pools themselves are process-wide, because Tracy needs
    /// their names to stay at the same address for the whole session.
    std::array<memory::TracyPool*, kCategoryCount> m_categoryPools{};
    std::array<u64_t, kCategoryCount>              m_categoryBytes{};
    std::array<u64_t, kCategoryCount>              m_categoryReserved{};
    std::array<u64_t, kCategoryCount>              m_categoryPeak{};
    void TrackPeak(usize_t index) noexcept;

    b8_t                                           m_underPressure = false;
};

} // namespace engine::vulkan

#endif // IS_ENGINE
