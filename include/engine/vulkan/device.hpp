#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/engine.hpp>
#    include <engine/error.hpp>
#    include <engine/vulkan/vk.hpp>

#    include <array>
#    include <string_view>

#    ifdef TRACY_ENABLE
#        include <tracy/TracyVulkan.hpp>
#    endif

namespace engine::vulkan {

inline constexpr u32_t kInvalidQueueFamily = ~0u;

/// Minimum API version the engine targets. A device that reports less is rejected.
inline constexpr u32_t kTargetApiVersion = VK_API_VERSION_1_4;

struct QueueFamilyIndices {
    /// Graphics-capable family that can present to the main surface. Graphics mode only.
    u32_t graphics = kInvalidQueueFamily;
    /// Family used for terrain compute: a compute-without-graphics family when one exists,
    /// otherwise the graphics family (spec section 5).
    u32_t compute = kInvalidQueueFamily;
    /// Transfer-only family for staging and readback when one exists.
    u32_t transfer = kInvalidQueueFamily;

    [[nodiscard]] b8_t HasDedicatedCompute() const noexcept {
        return compute != kInvalidQueueFamily && compute != graphics;
    }
};

struct PhysicalDeviceInfo {
    VkPhysicalDevice                                   handle = VK_NULL_HANDLE;
    std::array<char, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE> name{};
    std::array<u8_t, VK_UUID_SIZE>                     uuid{};
    u32_t                                              apiVersion    = 0;
    u32_t                                              driverVersion = 0;
    VkPhysicalDeviceType                               type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    VkDeviceSize                                       deviceLocalMemory = 0;
    QueueFamilyIndices                                 families;
    /// Higher is better. Zero means the device was rejected.
    u64_t score = 0;

    [[nodiscard]] std::string_view Name() const noexcept { return {name.data()}; }

    /// 32 lowercase hex characters, as accepted by `--device`.
    [[nodiscard]] std::array<char, 2 * VK_UUID_SIZE + 1> UuidHex() const noexcept;
};

/// Picks the best physical device for `mode`, logging every candidate with its score or the
/// reason it was rejected. `surface` is required in Graphics mode and ignored in Headless mode.
[[nodiscard]] Result<PhysicalDeviceInfo> SelectPhysicalDevice(VkInstance instance, RunMode mode,
                                                              VkSurfaceKHR     surface,
                                                              std::string_view forcedUuid);

/// A queue together with its command pool, its timeline semaphore and its Tracy GPU context.
///
/// Submissions signal a monotonically increasing timeline value, which the evaluator uses to
/// pipeline sections without fences (spec section 10).
class Queue {
public:
    Queue() = default;
    ~Queue();

    Queue(Queue&& other) noexcept;
    Queue& operator=(Queue&& other) noexcept;
    ENGINE_NO_COPY(Queue);

    /// `calibrated` enables Tracy's calibrated GPU timestamps, which needs
    /// `VK_EXT_calibrated_timestamps`.
    [[nodiscard]] Status Create(VkDevice device, VkPhysicalDevice physicalDevice, u32_t family,
                                u32_t index, const char* name, b8_t calibrated);
    void                 Destroy() noexcept;

    [[nodiscard]] VkQueue       Handle() const noexcept { return m_queue; }
    [[nodiscard]] u32_t         Family() const noexcept { return m_family; }
    [[nodiscard]] VkCommandPool Pool() const noexcept { return m_pool; }
    [[nodiscard]] const char*   Name() const noexcept { return m_name; }
    [[nodiscard]] b8_t          IsValid() const noexcept { return m_queue != VK_NULL_HANDLE; }

    /// Allocates and begins a primary one-shot command buffer from this queue's pool.
    [[nodiscard]] Result<VkCommandBuffer> BeginOneShot();

    /// Ends `commands`, submits it signalling the next timeline value and returns that value.
    /// The command buffer is freed by `WaitTimeline` or `Destroy`.
    [[nodiscard]] Result<u64_t> EndAndSubmit(VkCommandBuffer commands);

    /// Waits until the timeline reaches `value`, then recycles the finished command buffers.
    [[nodiscard]] Status WaitTimeline(u64_t value, u64_t timeoutNanoseconds);

    /// Timeline value of the last submission.
    [[nodiscard]] u64_t LastSubmittedValue() const noexcept { return m_timelineValue; }

    /// Collects the GPU zones recorded since the previous call. No-op without Tracy.
    void CollectGpuZones();

#    ifdef TRACY_ENABLE
    [[nodiscard]] TracyVkCtx TracyContext() const noexcept { return m_tracyContext; }
#    endif

private:
    /// One-shot buffers in flight, recycled once the timeline passes their submission value.
    static constexpr usize_t kMaxPending = 16;

    struct Pending {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        u64_t           value    = 0;
    };

    void RecyclePending(u64_t completedValue) noexcept;

    VkDevice        m_device        = VK_NULL_HANDLE;
    VkQueue         m_queue         = VK_NULL_HANDLE;
    VkCommandPool   m_pool          = VK_NULL_HANDLE;
    VkSemaphore     m_timeline      = VK_NULL_HANDLE;
    u64_t           m_timelineValue = 0;
    u32_t           m_family        = kInvalidQueueFamily;
    const char*     m_name          = "queue";
    std::array<Pending, kMaxPending> m_pending{};
    usize_t                          m_pendingCount = 0;
#    ifdef TRACY_ENABLE
    TracyVkCtx      m_tracyContext = nullptr;
    VkCommandBuffer m_tracyCommands = VK_NULL_HANDLE;
#    endif
};

} // namespace engine::vulkan

#endif // IS_ENGINE
