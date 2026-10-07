#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/engine.hpp>
#    include <engine/error.hpp>
#    include <engine/vulkan/buffer_pool.hpp>
#    include <engine/vulkan/device.hpp>
#    include <engine/vulkan/pipeline.hpp>
#    include <engine/vulkan/vk.hpp>

#    include <string_view>

namespace engine::vulkan {

/// One requested instance layer, instance extension or device extension.
/// A missing required entry fails initialization; a missing optional entry logs a warning.
struct Request {
    const char* name;
    b8_t        required;
};

struct ContextCreateInfo {
    RunMode          mode = RunMode::Headless;
    std::string_view applicationName = "engine";
    b8_t             enableValidation   = false;
    b8_t             verboseValidation  = false;
    /// 32 hex characters that force a specific physical device. Empty selects automatically.
    std::string_view forcedDeviceUuid;
    /// Main window surface. Required in Graphics mode so presentation support can be verified
    /// before the device is chosen; must be `VK_NULL_HANDLE` in Headless mode.
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    /// Growth step of the section buffer pool.
    VkDeviceSize sectionBlockBytes = 256ull * 1024 * 1024;
    /// Staging and readback rings are sized for at least two sections in flight.
    VkDeviceSize stagingBytes  = 16ull * 1024 * 1024;
    VkDeviceSize readbackBytes = 16ull * 1024 * 1024;
};

/// Instance, debug messenger, physical device, logical device and queues.
///
/// Move-only RAII: destruction tears everything down in reverse creation order.
class Context {
public:
    Context() = default;
    ~Context();

    Context(Context&& other) noexcept;
    Context& operator=(Context&& other) noexcept;
    ENGINE_NO_COPY(Context);

    [[nodiscard]] static Result<Context> Create(const ContextCreateInfo& info);

    [[nodiscard]] VkInstance       Instance() const noexcept { return m_instance; }
    [[nodiscard]] VkPhysicalDevice PhysicalDevice() const noexcept { return m_physical.handle; }
    [[nodiscard]] VkDevice         Device() const noexcept { return m_device; }
    [[nodiscard]] const PhysicalDeviceInfo& Info() const noexcept { return m_physical; }
    [[nodiscard]] RunMode                   Mode() const noexcept { return m_mode; }

    /// Queue terrain compute work is submitted to. Always valid.
    [[nodiscard]] Queue& ComputeQueue() noexcept { return m_compute; }

    /// Graphics queue. Only valid in Graphics mode.
    [[nodiscard]] Queue& GraphicsQueue() noexcept { return m_graphics; }

    /// True when `VK_EXT_calibrated_timestamps` was enabled, so Tracy GPU zones are calibrated.
    [[nodiscard]] b8_t HasCalibratedTimestamps() const noexcept { return m_calibratedTimestamps; }

    /// The only path to `vkAllocateMemory`.
    [[nodiscard]] Allocator& Memory() noexcept { return m_memory; }

    /// Section buffer pool, sub-allocated with VMA virtual blocks.
    [[nodiscard]] SectionPool& Sections() noexcept { return m_sections; }

    /// Persistently mapped upload ring.
    [[nodiscard]] RingBuffer& Staging() noexcept { return m_staging; }

    /// Persistently mapped readback ring.
    [[nodiscard]] RingBuffer& Readback() noexcept { return m_readback; }

    /// Pipeline cache persisted next to the executable.
    [[nodiscard]] PipelineCache& Pipelines() noexcept { return m_pipelineCache; }

    /// Blocks until every queue is idle. Called before tearing resources down.
    [[nodiscard]] Status WaitIdle();

    /// Refreshes the memory budgets and the Tracy memory plots. Cheap enough to call per section.
    void UpdatePlots();

    /// Once per frame or tick: `UpdatePlots` plus a flush of any GPU zones that no command buffer
    /// picked up. Callers that record `Queue::CollectGpuZones` into their own command buffers only
    /// need this at the end of the job, to flush the last batch.
    void EndTick();

private:
    [[nodiscard]] Status CreateInstance(const ContextCreateInfo& info);
    [[nodiscard]] Status CreateMessenger(const ContextCreateInfo& info);
    [[nodiscard]] Status CreateDevice(const ContextCreateInfo& info);
    [[nodiscard]] Status CreateMemory(const ContextCreateInfo& info);

    RunMode                  m_mode     = RunMode::Headless;
    VkInstance               m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    PhysicalDeviceInfo       m_physical;
    VkDevice                 m_device = VK_NULL_HANDLE;
    Queue                    m_compute;
    Queue                    m_graphics;
    Allocator                m_memory;
    SectionPool              m_sections;
    RingBuffer               m_staging;
    RingBuffer               m_readback;
    PipelineCache            m_pipelineCache;
    b8_t                     m_calibratedTimestamps = false;
};

} // namespace engine::vulkan

#endif // IS_ENGINE
