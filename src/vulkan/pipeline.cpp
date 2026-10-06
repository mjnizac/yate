#include <engine/vulkan/pipeline.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/platform.hpp>

#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <utility>
#include <vector>

namespace engine::vulkan {

namespace {

constexpr const char* kShaderSubdirectory = "shaders/";

/// Builds `<executable dir>/<subdirectory><relative>` into `out`.
void ResolvePath(std::array<char, 512>& out, const char* subdirectory,
                 std::string_view relative) {
    const std::string_view root = platform::ExecutableDirectory();
    std::snprintf(out.data(), out.size(), "%.*s%s%.*s", static_cast<int>(root.size()), root.data(),
                  subdirectory, static_cast<int>(relative.size()), relative.data());
}

/// Reads a whole binary file into `out`. SPIR-V and pipeline caches are both read this way.
[[nodiscard]] b8_t ReadBinaryFile(const char* path, std::pmr::vector<u8_t>& out) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<usize_t>(size));
    const usize_t read = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (read != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace

// --- PipelineCache ----------------------------------------------------------------------------

Result<PipelineCache> PipelineCache::Create(VkDevice device, std::string_view fileName) {
    PipelineCache cache;
    cache.m_device = device;
    detail::CopyBounded(cache.m_fileName, fileName);

    std::array<char, 512> path{};
    ResolvePath(path, "", fileName);

    std::pmr::vector<u8_t> initial(&memory::General().Resource());
    const b8_t             loaded = ReadBinaryFile(path.data(), initial);

    const VkPipelineCacheCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                        .initialDataSize = loaded ? initial.size() : 0,
                                        .pInitialData = loaded ? initial.data() : nullptr};
    VkResult                        created = vkCreatePipelineCache(device, &info, nullptr, &cache.m_cache);
    if (created != VK_SUCCESS && loaded) {
        // A cache from another driver version is rejected; start over rather than fail.
        LOG_WARN("pipeline cache {} was rejected ({}), starting empty", fileName,
                 ResultName(created));
        const VkPipelineCacheCreateInfo empty{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        created = vkCreatePipelineCache(device, &empty, nullptr, &cache.m_cache);
    }
    if (created != VK_SUCCESS) {
        return std::unexpected(MakeVulkanError(created, "vkCreatePipelineCache"));
    }

    LOG_INFO("pipeline cache ready ({})", loaded ? "loaded from disk" : "empty");
    return cache;
}

PipelineCache::~PipelineCache() {
    if (m_cache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(m_device, m_cache, nullptr);
        m_cache = VK_NULL_HANDLE;
    }
}

PipelineCache::PipelineCache(PipelineCache&& other) noexcept { *this = std::move(other); }

PipelineCache& PipelineCache::operator=(PipelineCache&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    this->~PipelineCache();
    m_device       = other.m_device;
    m_cache        = other.m_cache;
    m_fileName     = other.m_fileName;
    other.m_cache  = VK_NULL_HANDLE;
    other.m_device = VK_NULL_HANDLE;
    return *this;
}

void PipelineCache::Save() const noexcept {
    if (m_cache == VK_NULL_HANDLE) {
        return;
    }
    usize_t size = 0;
    VK_CHECK_RETURN(, vkGetPipelineCacheData(m_device, m_cache, &size, nullptr));
    if (size == 0) {
        return;
    }
    std::pmr::vector<u8_t> data(size, &memory::General().Resource());
    VK_CHECK_RETURN(, vkGetPipelineCacheData(m_device, m_cache, &size, data.data()));

    std::array<char, 512> path{};
    ResolvePath(path, "", m_fileName.data());
    std::FILE* file = std::fopen(path.data(), "wb");
    if (file == nullptr) {
        LOG_WARN("could not write the pipeline cache to {}", path.data());
        return;
    }
    const usize_t written = std::fwrite(data.data(), 1, size, file);
    std::fclose(file);
    LOG_DEBUG("pipeline cache saved ({} of {} bytes)", written, size);
}

// --- ComputePipeline --------------------------------------------------------------------------

Result<ComputePipeline> ComputePipeline::Create(VkDevice device,
                                               std::string_view            spirvRelativePath,
                                               const SpecializationValues& specialization,
                                               VkPipelineCache             cache) {
    std::array<char, 512> path{};
    ResolvePath(path, kShaderSubdirectory, spirvRelativePath);

    std::pmr::vector<u8_t> spirv(&memory::General().Resource());
    if (!ReadBinaryFile(path.data(), spirv)) {
        ENGINE_FAIL(ErrorCode::NotFound, ErrorStage::Vulkan, "could not read the SPIR-V at {}",
                    path.data());
    }
    if (spirv.size() % 4 != 0) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Vulkan,
                    "{} is {} bytes, which is not a whole number of SPIR-V words", path.data(),
                    spirv.size());
    }

    ComputePipeline pipeline;
    pipeline.m_device = device;

    const VkShaderModuleCreateInfo moduleInfo{
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv.size(),
        .pCode    = reinterpret_cast<const u32_t*>(spirv.data())};
    VkShaderModule module = VK_NULL_HANDLE;
    VK_TRY(vkCreateShaderModule(device, &moduleInfo, nullptr, &module));

    // No descriptor sets: everything the kernel reads or writes arrives as a device address.
    const VkPushConstantRange pushRange{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                        .offset     = 0,
                                        .size       = sizeof(KernelPushConstants)};
    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pushRange};
    VK_CHECK_X(std::unexpected(MakeVulkanError(VK_ERROR_INITIALIZATION_FAILED,
                                              "vkCreatePipelineLayout")),
               vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipeline.m_layout),
               { vkDestroyShaderModule(device, module, nullptr); });

    std::array<VkSpecializationMapEntry, SpecializationValues::kMaxValues> entries{};
    for (u32_t i = 0; i < specialization.count; ++i) {
        entries[i] = VkSpecializationMapEntry{.constantID = i,
                                              .offset     = i * static_cast<u32_t>(sizeof(u32_t)),
                                              .size       = sizeof(u32_t)};
    }
    const VkSpecializationInfo specializationInfo{
        .mapEntryCount = specialization.count,
        .pMapEntries   = entries.data(),
        .dataSize      = specialization.count * sizeof(u32_t),
        .pData         = specialization.values.data()};

    const VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = VkPipelineShaderStageCreateInfo{
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
            .module              = module,
            .pName               = "main",
            .pSpecializationInfo = specialization.count > 0 ? &specializationInfo : nullptr},
        .layout = pipeline.m_layout};
    const VkResult created =
        vkCreateComputePipelines(device, cache, 1, &pipelineInfo, nullptr, &pipeline.m_pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (created != VK_SUCCESS) {
        return std::unexpected(MakeVulkanError(created, "vkCreateComputePipelines"));
    }

    LOG_DEBUG("compute pipeline ready from {}", spirvRelativePath);
    return pipeline;
}

ComputePipeline::~ComputePipeline() {
    if (m_device == VK_NULL_HANDLE) {
        return;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    m_device = VK_NULL_HANDLE;
}

ComputePipeline::ComputePipeline(ComputePipeline&& other) noexcept { *this = std::move(other); }

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    this->~ComputePipeline();
    m_device         = other.m_device;
    m_layout         = other.m_layout;
    m_pipeline       = other.m_pipeline;
    other.m_device   = VK_NULL_HANDLE;
    other.m_layout   = VK_NULL_HANDLE;
    other.m_pipeline = VK_NULL_HANDLE;
    return *this;
}

void ComputePipeline::Bind(VkCommandBuffer commands, const KernelPushConstants& constants) const {
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdPushConstants(commands, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(KernelPushConstants), &constants);
}

namespace {

void BufferBarrier(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset,
                   VkDeviceSize size, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    const VkBufferMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                         .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                         .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                         .dstStageMask  = dstStage,
                                         .dstAccessMask = dstAccess,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .buffer              = buffer,
                                         .offset              = offset,
                                         .size                = size};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .bufferMemoryBarrierCount = 1,
                                      .pBufferMemoryBarriers    = &barrier};
    vkCmdPipelineBarrier2(commands, &dependency);
}

} // namespace

void ComputeToComputeBarrier(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset,
                             VkDeviceSize size) {
    BufferBarrier(commands, buffer, offset, size, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void ComputeToTransferBarrier(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset,
                              VkDeviceSize size) {
    BufferBarrier(commands, buffer, offset, size, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
}

} // namespace engine::vulkan
