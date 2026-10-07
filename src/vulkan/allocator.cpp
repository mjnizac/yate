// Single translation unit that compiles the VMA implementation.
#define VMA_IMPLEMENTATION

#include <engine/vulkan/allocator.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/vulkan/context.hpp>

#include <array>
#include <cstdio>
#include <utility>

namespace engine::vulkan {

namespace {

constexpr const char* kCategoryNames[] = {
    "VRAM/Section buffers R2", "VRAM/Section buffers R3", "VRAM/Intermediate",
    "VRAM/Staging",            "VRAM/Readback",           "VRAM/Viewer",
    "VRAM/Uniforms",
};
static_assert(ArrayCount(kCategoryNames) == static_cast<usize_t>(VramCategory::Count));

constexpr const char* kDeviceMemoryPoolName = "VRAM/Device memory";

/// Heap usage above this fraction of the budget makes the evaluator back off
/// (spec section 7.4 rule 6).
constexpr f64_t kPressureThreshold = 0.90;

/// Device-memory accounting for the whole process.
///
/// VMA copies `VmaDeviceMemoryCallbacks` including `pUserData`, so the callbacks must not point
/// at a movable object. There is exactly one VMA allocator per process, so the reserved-block
/// bookkeeping lives here, at a stable address, together with its Tracy pool.
struct DeviceMemoryTracker {
    memory::TracyPool pool{kDeviceMemoryPoolName};
    u64_t             reserved = 0;
};

DeviceMemoryTracker& Tracker() {
    static DeviceMemoryTracker tracker;
    return tracker;
}

void VKAPI_PTR OnDeviceMemoryAllocate(VmaAllocator, u32_t memoryType, VkDeviceMemory memory,
                                      VkDeviceSize size, void*) {
    DeviceMemoryTracker& tracker = Tracker();
    tracker.reserved += size;
    tracker.pool.BeginBlock(memory, size);
    LOG_TRACE("VkDeviceMemory block of {} KiB reserved on memory type {}", size / 1024,
              memoryType);
}

void VKAPI_PTR OnDeviceMemoryFree(VmaAllocator, u32_t, VkDeviceMemory memory, VkDeviceSize size,
                                  void*) {
    DeviceMemoryTracker& tracker = Tracker();
    tracker.reserved -= size;
    tracker.pool.EndBlock(memory);
}

/// Process-wide Tracy pool of a category. Names must stay at one address for the session, so
/// each pool is a function-local static that is never destroyed.
memory::TracyPool& CategoryPool(VramCategory category) {
    switch (category) {
        case VramCategory::SectionBuffersR2: {
            static memory::TracyPool pool(kCategoryNames[0]);
            return pool;
        }
        case VramCategory::SectionBuffersR3: {
            static memory::TracyPool pool(kCategoryNames[1]);
            return pool;
        }
        case VramCategory::Intermediate: {
            static memory::TracyPool pool(kCategoryNames[2]);
            return pool;
        }
        case VramCategory::Staging: {
            static memory::TracyPool pool(kCategoryNames[3]);
            return pool;
        }
        case VramCategory::Readback: {
            static memory::TracyPool pool(kCategoryNames[4]);
            return pool;
        }
        case VramCategory::Viewer: {
            static memory::TracyPool pool(kCategoryNames[5]);
            return pool;
        }
        case VramCategory::Uniforms:
        case VramCategory::Count:
            break;
    }
    static memory::TracyPool uniforms(kCategoryNames[6]);
    return uniforms;
}

/// Per-heap plot names, each kept at a stable address.
const char* HeapPlotName(u32_t heap, b8_t budget) {
    static std::array<std::array<char, 40>, 2 * VK_MAX_MEMORY_HEAPS> names{};
    const usize_t index = heap * 2 + (budget ? 1u : 0u);
    if (names[index][0] == 0) {
        std::snprintf(names[index].data(), names[index].size(), "VRAM heap %u %s", heap,
                      budget ? "budget" : "usage");
    }
    return names[index].data();
}

} // namespace

const char* ToString(VramCategory category) noexcept {
    const usize_t index = static_cast<usize_t>(category);
    return index < ArrayCount(kCategoryNames) ? kCategoryNames[index] : "VRAM/Unknown";
}

u64_t Allocator::ReservedDeviceMemory() noexcept { return Tracker().reserved; }

Allocator::~Allocator() {
    if (m_allocator == nullptr) {
        return;
    }
    for (usize_t i = 0; i < kCategoryCount; ++i) {
        if (m_categoryBytes[i] != 0) {
            LOG_ERROR("{} leaked {} bytes at shutdown", kCategoryNames[i], m_categoryBytes[i]);
        }
        ENGINE_ASSERT(m_categoryBytes[i] == 0, "{} destroyed with live allocations",
                      kCategoryNames[i]);
    }
    vmaDestroyAllocator(m_allocator);
    m_allocator = nullptr;
}

Allocator::Allocator(Allocator&& other) noexcept { *this = std::move(other); }

Allocator& Allocator::operator=(Allocator&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    this->~Allocator();
    m_allocator     = other.m_allocator;
    m_device        = other.m_device;
    m_categoryPools    = other.m_categoryPools;
    m_categoryBytes    = other.m_categoryBytes;
    m_categoryReserved = other.m_categoryReserved;
    m_categoryPeak     = other.m_categoryPeak;
    m_underPressure    = other.m_underPressure;
    other.m_allocator  = nullptr;
    other.m_device     = VK_NULL_HANDLE;
    other.m_categoryBytes.fill(0);
    other.m_categoryReserved.fill(0);
    return *this;
}

Result<Allocator> Allocator::Create(Context& context) {
    Allocator allocator;
    allocator.m_device = context.Device();
    for (usize_t i = 0; i < kCategoryCount; ++i) {
        allocator.m_categoryPools[i] = &CategoryPool(static_cast<VramCategory>(i));
    }
    Tracker(); // Create the device-memory pool before the first callback can fire.

    const VmaDeviceMemoryCallbacks callbacks{.pfnAllocate = &OnDeviceMemoryAllocate,
                                             .pfnFree     = &OnDeviceMemoryFree,
                                             .pUserData   = nullptr};
    const VmaAllocatorCreateInfo   info{
          .flags                  = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
          .physicalDevice         = context.PhysicalDevice(),
          .device                 = context.Device(),
          .pDeviceMemoryCallbacks = &callbacks,
          .instance               = context.Instance(),
          .vulkanApiVersion       = kTargetApiVersion};
    VK_TRY(vmaCreateAllocator(&info, &allocator.m_allocator));

    LOG_INFO("VMA allocator ready, {} VRAM categories tracked in Tracy", kCategoryCount);
    return allocator;
}

Result<Buffer> Allocator::CreateBuffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Vulkan,
                    "cannot create a zero-sized buffer in {}", ToString(desc.category));
    }

    // Buffer device address is always requested: kernels receive their inputs and outputs as
    // addresses in push constants, so no descriptor sets are needed (spec section 9, stage 5).
    const VkBufferCreateInfo bufferInfo{
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = desc.size,
        .usage       = desc.usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};

    VmaAllocationCreateFlags flags = 0;
    if (desc.hostVisible) {
        flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
        flags |= desc.randomAccess ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                   : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }

    const VmaAllocationCreateInfo allocationInfo{
        .flags = flags,
        .usage = desc.hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                  : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};

    Buffer            buffer{};
    VmaAllocationInfo resultInfo{};
    VK_TRY(vmaCreateBuffer(m_allocator, &bufferInfo, &allocationInfo, &buffer.handle,
                           &buffer.allocation, &resultInfo));

    const VkBufferDeviceAddressInfo addressInfo{
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer.handle};
    buffer.address   = vkGetBufferDeviceAddress(m_device, &addressInfo);
    buffer.size      = desc.size;
    buffer.mapped    = resultInfo.pMappedData;
    buffer.category  = desc.category;
    buffer.poolBlock = desc.poolBlock;

    // A GPU sub-allocation has no CPU address, so the VmaAllocation handle is the Tracy key.
    const usize_t index = static_cast<usize_t>(desc.category);
    m_categoryReserved[index] += desc.size;
    m_categoryPools[index]->BeginBlock(buffer.allocation, desc.size);
    if (!desc.poolBlock) {
        m_categoryBytes[index] += desc.size;
        TrackPeak(index);
        m_categoryPools[index]->Acquire(buffer.allocation, desc.size);
    }
    m_categoryPools[index]->PlotUnused(m_categoryReserved[index], m_categoryBytes[index]);
    return buffer;
}

void Allocator::DestroyBuffer(Buffer& buffer) noexcept {
    if (!buffer.IsValid()) {
        return;
    }
    const usize_t index = static_cast<usize_t>(buffer.category);
    if (!buffer.poolBlock) {
        m_categoryPools[index]->Release(buffer.allocation);
        m_categoryBytes[index] -= buffer.size;
    }
    m_categoryPools[index]->EndBlock(buffer.allocation);
    m_categoryReserved[index] -= buffer.size;
    m_categoryPools[index]->PlotUnused(m_categoryReserved[index], m_categoryBytes[index]);
    vmaDestroyBuffer(m_allocator, buffer.handle, buffer.allocation);
    buffer = Buffer{};
}

void Allocator::TrackPeak(usize_t index) noexcept {
    if (m_categoryBytes[index] > m_categoryPeak[index]) {
        m_categoryPeak[index] = m_categoryBytes[index];
    }
}

void Allocator::ReportSubAllocation(VramCategory category, const void* key, u64_t size) noexcept {
    const usize_t index = static_cast<usize_t>(category);
    m_categoryBytes[index] += size;
    TrackPeak(index);
    m_categoryPools[index]->Acquire(key, size);
    m_categoryPools[index]->PlotUnused(m_categoryReserved[index], m_categoryBytes[index]);
}

void Allocator::ReleaseSubAllocation(VramCategory category, const void* key, u64_t size) noexcept {
    const usize_t index = static_cast<usize_t>(category);
    m_categoryPools[index]->Release(key);
    m_categoryBytes[index] -= size;
    m_categoryPools[index]->PlotUnused(m_categoryReserved[index], m_categoryBytes[index]);
}

u64_t Allocator::CategoryReserved(VramCategory category) const noexcept {
    const usize_t index = static_cast<usize_t>(category);
    return index < kCategoryCount ? m_categoryReserved[index] : 0;
}

u64_t Allocator::CategoryPeak(VramCategory category) const noexcept {
    const usize_t index = static_cast<usize_t>(category);
    return index < kCategoryCount ? m_categoryPeak[index] : 0;
}

void Allocator::UpdateCategoryPlots(VramCategory category) const noexcept {
    const usize_t index = static_cast<usize_t>(category);
    if (index >= kCategoryCount) {
        return;
    }
    m_categoryPools[index]->PlotUsed(m_categoryBytes[index]);
    m_categoryPools[index]->PlotUnused(m_categoryReserved[index], m_categoryBytes[index]);
}

Status Allocator::FlushBuffer(const Buffer& buffer, VkDeviceSize offset, VkDeviceSize size) {
    VK_TRY(vmaFlushAllocation(m_allocator, buffer.allocation, offset, size));
    return {};
}

Status Allocator::InvalidateBuffer(const Buffer& buffer, VkDeviceSize offset, VkDeviceSize size) {
    VK_TRY(vmaInvalidateAllocation(m_allocator, buffer.allocation, offset, size));
    return {};
}

u64_t Allocator::CategoryBytes(VramCategory category) const noexcept {
    const usize_t index = static_cast<usize_t>(category);
    return index < kCategoryCount ? m_categoryBytes[index] : 0;
}

Allocator::HeapBudget Allocator::DeviceLocalBudget() const {
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(m_allocator, budgets.data());

    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(m_allocator, &properties);
    if (properties == nullptr) {
        return {};
    }

    HeapBudget largest;
    for (u32_t heap = 0; heap < properties->memoryHeapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
        const b8_t deviceLocal = (properties->memoryHeaps[heap].flags
                                  & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                                 != 0;
        if (deviceLocal && budgets[heap].budget > largest.budget) {
            largest = HeapBudget{budgets[heap].usage, budgets[heap].budget};
        }
    }
    return largest;
}

b8_t Allocator::UpdateBudgets() {
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(m_allocator, budgets.data());

    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(m_allocator, &properties);
    const u32_t heapCount = properties != nullptr ? properties->memoryHeapCount : 0;

    b8_t pressure = false;
    for (u32_t heap = 0; heap < heapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
        const VmaBudget& budget = budgets[heap];
        memory::PlotMemory(HeapPlotName(heap, false), budget.usage);
        memory::PlotMemory(HeapPlotName(heap, true), budget.budget);
        if (budget.budget == 0) {
            continue;
        }
        const f64_t fraction =
            static_cast<f64_t>(budget.usage) / static_cast<f64_t>(budget.budget);
        if (fraction > kPressureThreshold) {
            pressure = true;
            if (!m_underPressure) {
                LOG_WARN("VRAM heap {} at {:.1f}% of its budget ({} / {} MiB), reducing sections "
                         "in flight",
                         heap, fraction * 100.0, budget.usage / (1024 * 1024),
                         budget.budget / (1024 * 1024));
            }
        }
    }

    for (usize_t i = 0; i < kCategoryCount; ++i) {
        UpdateCategoryPlots(static_cast<VramCategory>(i));
    }
    m_underPressure = pressure;
    return !pressure;
}

} // namespace engine::vulkan
