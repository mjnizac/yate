#include <engine/vulkan/context.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

namespace engine::vulkan {

const char* ResultName(VkResult result) noexcept {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
            return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
        case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
        default: break;
    }
    // Keeping the spelling in static storage means callers can treat it as a string literal.
    static thread_local std::array<char, 24> unknown{};
    std::snprintf(unknown.data(), unknown.size(), "VkResult(%d)", static_cast<int>(result));
    return unknown.data();
}

namespace {
std::atomic<u64_t> g_validationErrors{0};
} // namespace

u64_t ValidationErrorCount() noexcept {
    return g_validationErrors.load(std::memory_order_relaxed);
}

Error MakeVulkanError(VkResult result, const char* call) noexcept {
    return MakeError(ErrorCode::VulkanError, ErrorStage::Vulkan, "{} returned {}", call,
                     ResultName(result));
}

namespace {

constexpr usize_t kMaxRequests = 24;

/// Merged list of `{name, required}` requests. Duplicates keep the strictest `required` flag.
class RequestSet {
public:
    void Add(const char* name, b8_t required) {
        for (usize_t i = 0; i < m_count; ++i) {
            if (std::strcmp(m_requests[i].name, name) == 0) {
                m_requests[i].required = m_requests[i].required || required;
                return;
            }
        }
        ENGINE_ASSERT_RETURN(, m_count < kMaxRequests, "more than {} requests", kMaxRequests);
        m_requests[m_count++] = Request{.name = name, .required = required};
    }

    [[nodiscard]] usize_t        Count() const noexcept { return m_count; }
    [[nodiscard]] const Request& operator[](usize_t index) const noexcept {
        return m_requests[index];
    }

private:
    std::array<Request, kMaxRequests> m_requests{};
    usize_t                           m_count = 0;
};

/// Keeps the names that were actually found, ready for `ppEnabled*Names`.
class EnabledNames {
public:
    void Add(const char* name) {
        ENGINE_ASSERT_RETURN(, m_count < kMaxRequests, "more than {} enabled names", kMaxRequests);
        m_names[m_count++] = name;
    }

    [[nodiscard]] b8_t Contains(const char* name) const noexcept {
        for (usize_t i = 0; i < m_count; ++i) {
            if (std::strcmp(m_names[i], name) == 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const char* const* Data() const noexcept { return m_names.data(); }
    [[nodiscard]] u32_t              Count() const noexcept { return static_cast<u32_t>(m_count); }

private:
    std::array<const char*, kMaxRequests> m_names{};
    usize_t                               m_count = 0;
};

/// Resolves `requests` against `available`, filling `out` and failing on a missing required entry.
template <typename Available, typename NameOf>
[[nodiscard]] Status Resolve(const RequestSet& requests, const Available& available, u32_t count,
                             NameOf nameOf, const char* kind, EnabledNames& out) {
    for (usize_t i = 0; i < requests.Count(); ++i) {
        const Request& request = requests[i];
        b8_t           found   = false;
        for (u32_t j = 0; j < count; ++j) {
            if (std::strcmp(nameOf(available[j]), request.name) == 0) {
                found = true;
                break;
            }
        }
        if (found) {
            out.Add(request.name);
        } else if (request.required) {
            ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Vulkan, "required {} {} is missing",
                        kind, request.name);
        } else {
            LOG_WARN("optional {} {} is not available", kind, request.name);
        }
    }
    return {};
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT        types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* userData) {
    // Never throws: this crosses a C boundary (spec section 3).
    (void)userData;
    const char* message = data != nullptr && data->pMessage != nullptr ? data->pMessage : "";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        // Only validation and performance messages are counted. The loader also reports at error
        // severity about things that are not our API usage at all, such as a third-party layer with
        // a broken manifest installed on the machine, and counting those would make the test gate
        // fail for reasons no change to this codebase could fix.
        constexpr VkDebugUtilsMessageTypeFlagsEXT kOurs =
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        if ((types & kOurs) != 0) {
            g_validationErrors.fetch_add(1, std::memory_order_relaxed);
        }
        LOG_ERROR("[vulkan] {}", message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        LOG_WARN("[vulkan] {}", message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0) {
        LOG_INFO("[vulkan] {}", message);
    } else {
        LOG_TRACE("[vulkan] {}", message);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MessengerCreateInfo(b8_t verbose) {
    VkDebugUtilsMessageSeverityFlagsEXT severity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    if (verbose) {
        severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
    }
    return VkDebugUtilsMessengerCreateInfoEXT{
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = severity,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = &DebugCallback};
}

} // namespace

Context::~Context() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }
    // Reverse creation order: pipelines and pooled memory first, then the queues and the device.
    if (m_pipelineCache.Handle() != VK_NULL_HANDLE) {
        m_pipelineCache.Save();
    }
    m_pipelineCache = PipelineCache{};
    m_readback      = RingBuffer{};
    m_staging       = RingBuffer{};
    m_sections      = SectionPool{};
    m_memory        = Allocator{};
    m_graphics.Destroy();
    m_compute.Destroy();
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_messenger != VK_NULL_HANDLE) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) {
            destroy(m_instance, m_messenger, nullptr);
        }
        m_messenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

Context::Context(Context&& other) noexcept { *this = std::move(other); }

Context& Context::operator=(Context&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    this->~Context();
    m_mode                 = other.m_mode;
    m_instance             = other.m_instance;
    m_messenger            = other.m_messenger;
    m_physical             = other.m_physical;
    m_device               = other.m_device;
    m_compute              = std::move(other.m_compute);
    m_graphics             = std::move(other.m_graphics);
    m_memory               = std::move(other.m_memory);
    m_sections             = std::move(other.m_sections);
    m_staging              = std::move(other.m_staging);
    m_readback             = std::move(other.m_readback);
    m_pipelineCache        = std::move(other.m_pipelineCache);
    m_calibratedTimestamps = other.m_calibratedTimestamps;
    // The pool and the rings keep a pointer to the allocator, whose address moved with us.
    if (m_memory.IsValid()) {
        m_sections.Rebind(m_memory);
        m_staging.Rebind(m_memory);
        m_readback.Rebind(m_memory);
    }
    other.m_instance       = VK_NULL_HANDLE;
    other.m_messenger      = VK_NULL_HANDLE;
    other.m_device         = VK_NULL_HANDLE;
    other.m_physical       = PhysicalDeviceInfo{};
    return *this;
}

Result<Context> Context::Create(const ContextCreateInfo& info) {
    if (info.mode == RunMode::Graphics && info.surface == VK_NULL_HANDLE) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "Graphics mode needs the main surface before the device is selected");
    }
    if (info.mode == RunMode::Headless && info.surface != VK_NULL_HANDLE) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "Headless mode must not be given a surface");
    }

    Context context;
    context.m_mode = info.mode;

    if (Status status = context.CreateInstance(info); !status) {
        return std::unexpected(status.error());
    }
    if (Status status = context.CreateMessenger(info); !status) {
        return std::unexpected(status.error());
    }

    Result<PhysicalDeviceInfo> selected =
        SelectPhysicalDevice(context.m_instance, info.mode, info.surface, info.forcedDeviceUuid);
    if (!selected) {
        return std::unexpected(selected.error());
    }
    context.m_physical = *selected;

    if (Status status = context.CreateDevice(info); !status) {
        return std::unexpected(status.error());
    }
    if (Status status = context.CreateMemory(info); !status) {
        return std::unexpected(status.error());
    }
    return context;
}

Status Context::CreateMemory(const ContextCreateInfo& info) {
    Result<Allocator> allocator = Allocator::Create(*this);
    if (!allocator) {
        return std::unexpected(allocator.error());
    }
    m_memory = std::move(*allocator);

    Result<SectionPool> sections =
        SectionPool::Create(m_memory, SectionPoolConfig{.blockSize = info.sectionBlockBytes});
    if (!sections) {
        return std::unexpected(sections.error());
    }
    m_sections = std::move(*sections);

    Result<RingBuffer> staging =
        RingBuffer::Create(m_memory, info.stagingBytes, VramCategory::Staging);
    if (!staging) {
        return std::unexpected(staging.error());
    }
    m_staging = std::move(*staging);

    Result<RingBuffer> readback =
        RingBuffer::Create(m_memory, info.readbackBytes, VramCategory::Readback);
    if (!readback) {
        return std::unexpected(readback.error());
    }
    m_readback = std::move(*readback);

    Result<PipelineCache> cache = PipelineCache::Create(m_device, "pipeline_cache.bin");
    if (!cache) {
        return std::unexpected(cache.error());
    }
    m_pipelineCache = std::move(*cache);

    (void)m_memory.UpdateBudgets();
    return {};
}

void Context::UpdatePlots() {
    (void)m_memory.UpdateBudgets();
    m_sections.UpdatePlots();
}

void Context::EndTick() {
    m_compute.CollectGpuZones();
    if (m_graphics.IsValid()) {
        m_graphics.CollectGpuZones();
    }
    UpdatePlots();
}

Status Context::CreateInstance(const ContextCreateInfo& info) {
    u32_t apiVersion = 0;
    VK_TRY(vkEnumerateInstanceVersion(&apiVersion));
    if (apiVersion < kTargetApiVersion) {
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Vulkan,
                    "the Vulkan loader reports {}.{}, the engine needs 1.4",
                    VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion));
    }

    RequestSet layers;
    RequestSet extensions;
    if (info.enableValidation) {
        layers.Add("VK_LAYER_KHRONOS_validation", false);
        extensions.Add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false);
    }
    // Surface extensions are requested only in Graphics mode (spec section 5).
    if (info.mode == RunMode::Graphics) {
        extensions.Add(VK_KHR_SURFACE_EXTENSION_NAME, true);
#if defined(_WIN32)
        extensions.Add("VK_KHR_win32_surface", true);
#else
        extensions.Add("VK_KHR_xlib_surface", false);
        extensions.Add("VK_KHR_wayland_surface", false);
#endif
    }

    u32_t layerCount = 0;
    VK_TRY(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));
    std::array<VkLayerProperties, 128> availableLayers{};
    layerCount = layerCount < availableLayers.size() ? layerCount
                                                     : static_cast<u32_t>(availableLayers.size());
    VK_TRY(vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data()));

    u32_t extensionCount = 0;
    VK_TRY(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
    std::array<VkExtensionProperties, 256> availableExtensions{};
    extensionCount = extensionCount < availableExtensions.size()
                         ? extensionCount
                         : static_cast<u32_t>(availableExtensions.size());
    VK_TRY(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                                  availableExtensions.data()));

    EnabledNames enabledLayers;
    EnabledNames enabledExtensions;
    if (Status status = Resolve(
            layers, availableLayers, layerCount,
            [](const VkLayerProperties& p) { return p.layerName; }, "instance layer",
            enabledLayers);
        !status) {
        return status;
    }
    if (Status status = Resolve(
            extensions, availableExtensions, extensionCount,
            [](const VkExtensionProperties& p) { return p.extensionName; }, "instance extension",
            enabledExtensions);
        !status) {
        return status;
    }

    std::array<char, 64> applicationName{};
    detail::CopyBounded(applicationName, info.applicationName);

    const VkApplicationInfo applicationInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                            .pApplicationName = applicationName.data(),
                                            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                            .pEngineName        = "terrain-engine",
                                            .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
                                            .apiVersion         = kTargetApiVersion};

    // Chaining the messenger here captures diagnostics emitted during instance creation itself.
    const VkDebugUtilsMessengerCreateInfoEXT messengerInfo =
        MessengerCreateInfo(info.verboseValidation);
    const b8_t withMessenger = enabledExtensions.Contains(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    const VkInstanceCreateInfo instanceInfo{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = withMessenger ? &messengerInfo : nullptr,
        .pApplicationInfo        = &applicationInfo,
        .enabledLayerCount       = enabledLayers.Count(),
        .ppEnabledLayerNames     = enabledLayers.Data(),
        .enabledExtensionCount   = enabledExtensions.Count(),
        .ppEnabledExtensionNames = enabledExtensions.Data()};
    VK_TRY(vkCreateInstance(&instanceInfo, nullptr, &m_instance));

    LOG_INFO("Vulkan instance ready: api 1.4, {} layer(s), {} extension(s)",
             enabledLayers.Count(), enabledExtensions.Count());
    return {};
}

Status Context::CreateMessenger(const ContextCreateInfo& info) {
    if (!info.enableValidation) {
        return {};
    }
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (create == nullptr) {
        LOG_WARN("VK_EXT_debug_utils is not available, validation messages are off");
        return {};
    }
    const VkDebugUtilsMessengerCreateInfoEXT messengerInfo =
        MessengerCreateInfo(info.verboseValidation);
    VK_TRY(create(m_instance, &messengerInfo, nullptr, &m_messenger));
    LOG_INFO("Vulkan debug messenger ready (verbose: {})", info.verboseValidation);
    return {};
}

Status Context::CreateDevice(const ContextCreateInfo& info) {
    RequestSet extensions;
    if (info.mode == RunMode::Graphics) {
        extensions.Add(VK_KHR_SWAPCHAIN_EXTENSION_NAME, true);
    }
    // Calibrated timestamps let Tracy align GPU zones with the CPU timeline.
    extensions.Add(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, false);

    u32_t extensionCount = 0;
    VK_TRY(vkEnumerateDeviceExtensionProperties(m_physical.handle, nullptr, &extensionCount,
                                                nullptr));
    std::array<VkExtensionProperties, 512> available{};
    extensionCount =
        extensionCount < available.size() ? extensionCount : static_cast<u32_t>(available.size());
    VK_TRY(vkEnumerateDeviceExtensionProperties(m_physical.handle, nullptr, &extensionCount,
                                                available.data()));

    EnabledNames enabled;
    if (Status status = Resolve(
            extensions, available, extensionCount,
            [](const VkExtensionProperties& p) { return p.extensionName; }, "device extension",
            enabled);
        !status) {
        return status;
    }
    m_calibratedTimestamps = enabled.Contains(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

    // Core features the engine relies on (spec section 5).
    VkPhysicalDeviceVulkan14Features features14{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &features14};
    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &features13};
    VkPhysicalDeviceFeatures2 features2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                        .pNext = &features12};
    vkGetPhysicalDeviceFeatures2(m_physical.handle, &features2);

    struct RequiredFeature {
        const char* name;
        VkBool32    supported;
    };
    const RequiredFeature required[] = {
        {"synchronization2", features13.synchronization2},
        {"maintenance4", features13.maintenance4},
        {"timelineSemaphore", features12.timelineSemaphore},
        {"bufferDeviceAddress", features12.bufferDeviceAddress},
        {"scalarBlockLayout", features12.scalarBlockLayout},
        // Buffer references in the kernel interface are 64-bit addresses, and the workgroup size
        // comes from specialization constants, which needs LocalSizeId from maintenance4.
        {"shaderInt64", features2.features.shaderInt64},
    };
    for (const RequiredFeature& feature : required) {
        if (feature.supported != VK_TRUE) {
            ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Vulkan,
                        "device {} does not support {}", m_physical.Name(), feature.name);
        }
    }

    VkPhysicalDeviceVulkan14Features enable14{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceVulkan13Features enable13{
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext            = &enable14,
        .synchronization2 = VK_TRUE,
        .maintenance4     = VK_TRUE};
    VkPhysicalDeviceVulkan12Features enable12{
        .sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext               = &enable13,
        .scalarBlockLayout   = VK_TRUE,
        .timelineSemaphore   = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceFeatures2 enableFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                             .pNext = &enable12};
    enableFeatures.features.shaderInt64 = VK_TRUE;

    constexpr f32_t                     kPriority = 1.0f;
    std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
    u32_t                                  queueCount = 0;
    queueInfos[queueCount++] = VkDeviceQueueCreateInfo{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = m_physical.families.compute,
        .queueCount       = 1,
        .pQueuePriorities = &kPriority};
    if (info.mode == RunMode::Graphics
        && m_physical.families.graphics != m_physical.families.compute) {
        queueInfos[queueCount++] = VkDeviceQueueCreateInfo{
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = m_physical.families.graphics,
            .queueCount       = 1,
            .pQueuePriorities = &kPriority};
    }

    const VkDeviceCreateInfo deviceInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                        .pNext = &enableFeatures,
                                        .queueCreateInfoCount    = queueCount,
                                        .pQueueCreateInfos       = queueInfos.data(),
                                        .enabledExtensionCount   = enabled.Count(),
                                        .ppEnabledExtensionNames = enabled.Data()};
    VK_TRY(vkCreateDevice(m_physical.handle, &deviceInfo, nullptr, &m_device));

    if (Status status = m_compute.Create(m_instance, m_device, m_physical.handle,
                                         m_physical.families.compute, 0, "compute",
                                         m_calibratedTimestamps);
        !status) {
        return status;
    }
    if (info.mode == RunMode::Graphics) {
        if (m_physical.families.graphics == m_physical.families.compute) {
            LOG_INFO("graphics and compute share queue family {}", m_physical.families.compute);
        } else if (Status status = m_graphics.Create(m_instance, m_device, m_physical.handle,
                                                     m_physical.families.graphics, 0, "graphics",
                                                     m_calibratedTimestamps);
                   !status) {
            return status;
        }
    }

    LOG_INFO("Vulkan device ready ({} queue family/families, calibrated timestamps: {})",
             queueCount, m_calibratedTimestamps);
    return {};
}

Status Context::WaitIdle() {
    if (m_device == VK_NULL_HANDLE) {
        return {};
    }
    VK_TRY(vkDeviceWaitIdle(m_device));
    return {};
}

} // namespace engine::vulkan
