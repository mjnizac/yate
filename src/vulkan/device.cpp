#include <engine/vulkan/device.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>

#include <cstring>
#include <utility>

namespace engine::vulkan {

namespace {

constexpr usize_t kMaxQueueFamilies   = 16;
constexpr usize_t kMaxPhysicalDevices = 8;

/// Preference order of the device types, highest first.
constexpr u64_t TypeScore(VkPhysicalDeviceType type) noexcept {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
        default: return 0;
    }
}

const char* TypeName(VkPhysicalDeviceType type) noexcept {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }
}

[[nodiscard]] b8_t HasExtension(VkPhysicalDevice device, const char* name) {
    u32_t count = 0;
    VK_CHECK_RETURN(false, vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr));
    if (count == 0) {
        return false;
    }
    std::array<VkExtensionProperties, 512> properties{};
    count = count < properties.size() ? count : static_cast<u32_t>(properties.size());
    VK_CHECK_RETURN(false,
                    vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                                         properties.data()));
    for (u32_t i = 0; i < count; ++i) {
        if (std::strcmp(properties[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

/// Sum of the sizes of every device-local memory heap. Breaks ties between devices.
[[nodiscard]] VkDeviceSize DeviceLocalMemory(const VkPhysicalDeviceMemoryProperties& memory) {
    VkDeviceSize total = 0;
    for (u32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            total += memory.memoryHeaps[i].size;
        }
    }
    return total;
}

/// Finds the graphics, dedicated-compute and dedicated-transfer families.
/// In Graphics mode the graphics family must also be able to present to `surface`.
QueueFamilyIndices FindFamilies(VkPhysicalDevice device, RunMode mode, VkSurfaceKHR surface) {
    u32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::array<VkQueueFamilyProperties, kMaxQueueFamilies> families{};
    count = count < kMaxQueueFamilies ? count : static_cast<u32_t>(kMaxQueueFamilies);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    QueueFamilyIndices indices;
    u32_t              anyCompute = kInvalidQueueFamily;

    for (u32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags       = families[i].queueFlags;
        const b8_t         hasGraphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const b8_t         hasCompute  = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
        const b8_t         hasTransfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0;

        if (mode == RunMode::Graphics && hasGraphics && indices.graphics == kInvalidQueueFamily) {
            VkBool32 present = VK_FALSE;
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present));
            if (present == VK_TRUE) {
                indices.graphics = i;
            }
        }

        if (hasCompute) {
            if (anyCompute == kInvalidQueueFamily) {
                anyCompute = i;
            }
            // A compute family without graphics support allows async compute.
            if (!hasGraphics && indices.compute == kInvalidQueueFamily) {
                indices.compute = i;
            }
        }

        if (hasTransfer && !hasCompute && !hasGraphics
            && indices.transfer == kInvalidQueueFamily) {
            indices.transfer = i;
        }
    }

    if (indices.compute == kInvalidQueueFamily) {
        indices.compute = mode == RunMode::Graphics && indices.graphics != kInvalidQueueFamily
                              ? indices.graphics
                              : anyCompute;
    }
    return indices;
}

[[nodiscard]] i32_t HexNibble(char c) noexcept {
    if (c >= 0x30 && c <= 0x39) {
        return c - 0x30; // 0-9
    }
    if (c >= 0x61 && c <= 0x66) {
        return c - 0x61 + 10; // a-f
    }
    if (c >= 0x41 && c <= 0x46) {
        return c - 0x41 + 10; // A-F
    }
    return -1;
}

/// Parses 32 hex characters into a UUID. Returns false on any other input.
b8_t ParseUuid(std::string_view text, std::array<u8_t, VK_UUID_SIZE>& out) {
    if (text.size() != 2 * VK_UUID_SIZE) {
        return false;
    }
    for (usize_t i = 0; i < VK_UUID_SIZE; ++i) {
        const i32_t high = HexNibble(text[2 * i]);
        const i32_t low  = HexNibble(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<u8_t>((high << 4) | low);
    }
    return true;
}

} // namespace

std::array<char, 2 * VK_UUID_SIZE + 1> PhysicalDeviceInfo::UuidHex() const noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::array<char, 2 * VK_UUID_SIZE + 1> text{};
    for (usize_t i = 0; i < VK_UUID_SIZE; ++i) {
        text[2 * i]     = kDigits[(uuid[i] >> 4) & 0xF];
        text[2 * i + 1] = kDigits[uuid[i] & 0xF];
    }
    text[2 * VK_UUID_SIZE] = 0;
    return text;
}

Result<PhysicalDeviceInfo> SelectPhysicalDevice(VkInstance instance, RunMode mode,
                                                VkSurfaceKHR     surface,
                                                std::string_view forcedUuid) {
    std::array<u8_t, VK_UUID_SIZE> wantedUuid{};
    const b8_t                     hasForcedUuid = !forcedUuid.empty();
    if (hasForcedUuid && !ParseUuid(forcedUuid, wantedUuid)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "device UUID {} is not 32 hex characters", forcedUuid);
    }

    u32_t count = 0;
    VK_TRY(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (count == 0) {
        ENGINE_FAIL(ErrorCode::NotFound, ErrorStage::Vulkan,
                    "no Vulkan physical device is available");
    }
    std::array<VkPhysicalDevice, kMaxPhysicalDevices> devices{};
    count = count < kMaxPhysicalDevices ? count : static_cast<u32_t>(kMaxPhysicalDevices);
    VK_TRY(vkEnumeratePhysicalDevices(instance, &count, devices.data()));

    PhysicalDeviceInfo best;

    for (u32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceIDProperties idProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idProperties};
        vkGetPhysicalDeviceProperties2(devices[i], &properties2);
        const VkPhysicalDeviceProperties& properties = properties2.properties;

        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(devices[i], &memory);

        PhysicalDeviceInfo candidate;
        candidate.handle            = devices[i];
        candidate.apiVersion        = properties.apiVersion;
        candidate.driverVersion     = properties.driverVersion;
        candidate.type              = properties.deviceType;
        candidate.deviceLocalMemory = DeviceLocalMemory(memory);
        candidate.timestampPeriod   = properties.limits.timestampPeriod;
        candidate.families          = FindFamilies(devices[i], mode, surface);
        std::memcpy(candidate.name.data(), properties.deviceName, candidate.name.size() - 1);
        std::memcpy(candidate.uuid.data(), idProperties.deviceUUID, VK_UUID_SIZE);

        const char* rejection = nullptr;
        if (properties.apiVersion < kTargetApiVersion) {
            rejection = "does not support Vulkan 1.4";
        } else if (candidate.families.compute == kInvalidQueueFamily) {
            rejection = "has no compute-capable queue family";
        } else if (mode == RunMode::Graphics) {
            if (candidate.families.graphics == kInvalidQueueFamily) {
                rejection = "cannot present to the main surface";
            } else if (!HasExtension(devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                rejection = "does not expose VK_KHR_swapchain";
            } else {
                u32_t formats = 0;
                u32_t modes   = 0;
                VK_CHECK(
                    vkGetPhysicalDeviceSurfaceFormatsKHR(devices[i], surface, &formats, nullptr));
                VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(devices[i], surface, &modes,
                                                                   nullptr));
                if (formats == 0 || modes == 0) {
                    rejection = "exposes no surface format or present mode";
                }
            }
        }

        if (rejection != nullptr) {
            LOG_WARN("physical device {} rejected: {}", candidate.Name(), rejection);
            continue;
        }

        // Discrete first, then a dedicated compute family, then device-local memory in MiB.
        candidate.score = (TypeScore(properties.deviceType) << 40)
                          | (candidate.families.HasDedicatedCompute() ? (1ull << 39) : 0ull)
                          | static_cast<u64_t>(candidate.deviceLocalMemory >> 20);

        LOG_INFO("physical device {} ({}, {} MiB device-local, api {}.{}.{}) score {}",
                 candidate.Name(), TypeName(properties.deviceType),
                 candidate.deviceLocalMemory / (1024 * 1024),
                 VK_API_VERSION_MAJOR(properties.apiVersion),
                 VK_API_VERSION_MINOR(properties.apiVersion),
                 VK_API_VERSION_PATCH(properties.apiVersion), candidate.score);

        if (hasForcedUuid) {
            if (std::memcmp(candidate.uuid.data(), wantedUuid.data(), VK_UUID_SIZE) == 0) {
                best = candidate;
                break;
            }
            continue;
        }
        if (candidate.score > best.score) {
            best = candidate;
        }
    }

    if (best.handle == VK_NULL_HANDLE) {
        if (hasForcedUuid) {
            ENGINE_FAIL(ErrorCode::NotFound, ErrorStage::Vulkan,
                        "no suitable physical device matches UUID {}", forcedUuid);
        }
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Vulkan,
                    "no physical device meets the requirements for {} mode", ToString(mode));
    }

    LOG_INFO("selected {} (uuid {}), compute family {}{}", best.Name(), best.UuidHex().data(),
             best.families.compute, best.families.HasDedicatedCompute() ? " (dedicated)" : "");
    return best;
}

// --- Queue ----------------------------------------------------------------------------------

Queue::~Queue() { Destroy(); }

Queue::Queue(Queue&& other) noexcept { *this = std::move(other); }

Queue& Queue::operator=(Queue&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Destroy();
    m_device        = other.m_device;
    m_queue         = other.m_queue;
    m_pool          = other.m_pool;
    m_timeline      = other.m_timeline;
    m_timelineValue = other.m_timelineValue;
    m_family        = other.m_family;
    m_name          = other.m_name;
    m_pending       = other.m_pending;
    m_pendingCount  = other.m_pendingCount;
#ifdef TRACY_ENABLE
    m_tracyContext        = other.m_tracyContext;
    m_tracyCommands       = other.m_tracyCommands;
    other.m_tracyContext  = nullptr;
    other.m_tracyCommands = VK_NULL_HANDLE;
#endif
    other.m_device       = VK_NULL_HANDLE;
    other.m_queue        = VK_NULL_HANDLE;
    other.m_pool         = VK_NULL_HANDLE;
    other.m_timeline     = VK_NULL_HANDLE;
    other.m_pendingCount = 0;
    return *this;
}

Status Queue::Create(VkInstance instance, VkDevice device, VkPhysicalDevice physicalDevice,
                     u32_t family, u32_t index, const char* name, b8_t calibrated) {
    m_device = device;
    m_family = family;
    m_name   = name;
    vkGetDeviceQueue(device, family, index, &m_queue);

    const VkCommandPoolCreateInfo poolInfo{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family};
    VK_TRY(vkCreateCommandPool(device, &poolInfo, nullptr, &m_pool));

    const VkSemaphoreTypeCreateInfo timelineInfo{
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0};
    const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                              .pNext = &timelineInfo};
    VK_TRY(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_timeline));

#ifdef TRACY_ENABLE
    const VkCommandBufferAllocateInfo allocateInfo{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = m_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VK_TRY(vkAllocateCommandBuffers(device, &allocateInfo, &m_tracyCommands));
    if (calibrated) {
        m_tracyContext = TracyVkContextCalibrated(
            physicalDevice, device, m_queue, m_tracyCommands,
            // An instance-level entry point: asking vkGetDeviceProcAddr for it is a validation
            // error, even though the driver would hand one back.
            reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
                vkGetInstanceProcAddr(instance,
                                      "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT")),
            reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
                vkGetDeviceProcAddr(device, "vkGetCalibratedTimestampsEXT")));
    } else {
        m_tracyContext = TracyVkContext(physicalDevice, device, m_queue, m_tracyCommands);
    }
#else
    (void)instance;
    (void)physicalDevice;
    (void)calibrated;
#endif

    LOG_DEBUG("queue {} ready on family {}", name, family);
    return {};
}

void Queue::Destroy() noexcept {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
#ifdef TRACY_ENABLE
    if (m_tracyContext != nullptr) {
        TracyVkDestroy(m_tracyContext);
        m_tracyContext = nullptr;
    }
    m_tracyCommands = VK_NULL_HANDLE;
#endif
    if (m_timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, m_timeline, nullptr);
        m_timeline = VK_NULL_HANDLE;
    }
    if (m_pool != VK_NULL_HANDLE) {
        // Destroying the pool frees every command buffer allocated from it.
        vkDestroyCommandPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    m_queue        = VK_NULL_HANDLE;
    m_device       = VK_NULL_HANDLE;
    m_pendingCount = 0;
}

Result<VkCommandBuffer> Queue::BeginOneShot() {
    if (m_pendingCount == kMaxPending) {
        // Full means nobody has waited on this queue in a while, not necessarily that the GPU is behind.
        // The viewer is the case that proved it: its frames go through the graphics queue, so nothing
        // ever calls `WaitTimeline` on the compute one, and the per-frame GPU zone collection filled the
        // pool after sixteen frames and then failed for the rest of the run. Polling the counter here
        // costs one call on a path that was about to fail outright, and makes the pool self-healing for
        // every caller instead of only for the ones that happen to wait.
        u64_t completed = 0;
        if (vkGetSemaphoreCounterValue(m_device, m_timeline, &completed) == VK_SUCCESS) {
            RecyclePending(completed);
        }
    }
    if (m_pendingCount == kMaxPending) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Vulkan,
                    "queue {} already has {} command buffers in flight and none have completed",
                    m_name, kMaxPending);
    }

    VkCommandBuffer                   commands = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo allocateInfo{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = m_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VK_TRY(vkAllocateCommandBuffers(m_device, &allocateInfo, &commands));

    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    const VkResult begun = vkBeginCommandBuffer(commands, &beginInfo);
    if (begun != VK_SUCCESS) [[unlikely]] {
        vkFreeCommandBuffers(m_device, m_pool, 1, &commands);
        return std::unexpected(MakeVulkanError(begun, "vkBeginCommandBuffer"));
    }
    return commands;
}

Result<u64_t> Queue::EndAndSubmit(VkCommandBuffer commands) {
    VK_TRY(vkEndCommandBuffer(commands));

    const u64_t value = ++m_timelineValue;

    const VkCommandBufferSubmitInfo commandInfo{
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = commands};
    const VkSemaphoreSubmitInfo signalInfo{.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                           .semaphore = m_timeline,
                                           .value     = value,
                                           .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSubmitInfo2 submitInfo{.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                   .commandBufferInfoCount   = 1,
                                   .pCommandBufferInfos      = &commandInfo,
                                   .signalSemaphoreInfoCount = 1,
                                   .pSignalSemaphoreInfos    = &signalInfo};
    VK_TRY(vkQueueSubmit2(m_queue, 1, &submitInfo, VK_NULL_HANDLE));

    m_pending[m_pendingCount++] = Pending{.commands = commands, .value = value};
    return value;
}

void Queue::RecyclePending(u64_t completedValue) noexcept {
    usize_t kept = 0;
    for (usize_t i = 0; i < m_pendingCount; ++i) {
        if (m_pending[i].value <= completedValue) {
            vkFreeCommandBuffers(m_device, m_pool, 1, &m_pending[i].commands);
        } else {
            m_pending[kept++] = m_pending[i];
        }
    }
    m_pendingCount = kept;
}

Status Queue::WaitTimeline(u64_t value, u64_t timeoutNanoseconds) {
    const VkSemaphoreWaitInfo waitInfo{.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                       .semaphoreCount = 1,
                                       .pSemaphores    = &m_timeline,
                                       .pValues        = &value};
    VK_TRY(vkWaitSemaphores(m_device, &waitInfo, timeoutNanoseconds));

    u64_t completed = 0;
    VK_TRY(vkGetSemaphoreCounterValue(m_device, m_timeline, &completed));
    RecyclePending(completed);
    return {};
}

void Queue::CollectGpuZones(VkCommandBuffer commands) {
#ifdef TRACY_ENABLE
    if (m_tracyContext != nullptr) {
        TracyVkCollect(m_tracyContext, commands);
    }
#else
    (void)commands;
#endif
}

void Queue::CollectGpuZones() {
#ifdef TRACY_ENABLE
    if (m_tracyContext == nullptr) {
        return;
    }
    // TracyVkCollect records a query-pool reset and a result copy, so the command buffer has to be
    // in the recording state and the caller has to submit it. Handing it a buffer that was never
    // begun is a validation error, and worse, it means the timestamps are never read back: the GPU
    // zones simply never reach the profiler.
    Result<VkCommandBuffer> commands = BeginOneShot();
    if (!commands) {
        LOG_WARN("could not collect GPU zones on queue {}: {}", m_name,
                 commands.error().Format().data());
        return;
    }
    TracyVkCollect(m_tracyContext, *commands);
    Result<u64_t> submitted = EndAndSubmit(*commands);
    if (!submitted) {
        LOG_WARN("could not submit the GPU zone collection on queue {}: {}", m_name,
                 submitted.error().Format().data());
    }
    // Not waited on here: it is recycled by the next WaitTimeline, which every section already does.
#endif
}

} // namespace engine::vulkan
