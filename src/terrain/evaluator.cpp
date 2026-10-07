#include <engine/terrain/evaluator.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/pipeline.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace engine::terrain {

// --- SectionResources -----------------------------------------------------------------------------

Result<SectionResources> SectionResources::Create(vulkan::Context&     context,
                                                 const CompiledGraph& compiled) {
    SectionResources resources;
    resources.m_pool  = &context.Sections();
    resources.m_slots = std::pmr::vector<vulkan::SectionSlot>(&memory::General().Resource());
    resources.m_slots.reserve(compiled.buffers.size());

    for (const PlannedBuffer& planned : compiled.buffers) {
        Result<vulkan::SectionSlot> slot = resources.m_pool->Acquire(planned.sizeClass, planned.bytes);
        if (!slot) {
            return std::unexpected(slot.error());
        }
        resources.m_totalBytes += slot->size;
        resources.m_slots.push_back(*slot);
    }

    LOG_INFO("section resources ready: {} slot(s), {} KiB (plan said {} KiB)",
             resources.m_slots.size(), resources.m_totalBytes / 1024,
             compiled.stats.peakSectionBytes / 1024);
    return resources;
}

SectionResources::~SectionResources() { Release(); }

void SectionResources::Release() noexcept {
    if (m_pool != nullptr) {
        for (vulkan::SectionSlot& slot : m_slots) {
            m_pool->Release(slot);
        }
        m_pool = nullptr;
    }
    m_slots.clear();
    m_totalBytes = 0;
}

SectionResources::SectionResources(SectionResources&& other) noexcept { *this = std::move(other); }

SectionResources& SectionResources::operator=(SectionResources&& other) noexcept {
    if (this != &other) {
        Release();
        m_pool       = other.m_pool;
        m_slots      = std::move(other.m_slots);
        m_totalBytes = other.m_totalBytes;
        other.m_pool = nullptr;
        other.m_totalBytes = 0;
    }
    return *this;
}

const vulkan::SectionSlot& SectionResources::Slot(u32_t buffer) const {
    ENGINE_ASSERT(buffer < m_slots.size(), "buffer {} is out of range ({} slots)", buffer,
                  m_slots.size());
    return m_slots[buffer < m_slots.size() ? buffer : 0];
}

// --- DispatchTimers -------------------------------------------------------------------------------

Result<DispatchTimers> DispatchTimers::Create(vulkan::Context& context, u32_t dispatchCount) {
    if (dispatchCount == 0) {
        return DispatchTimers{};
    }
    DispatchTimers timers;
    timers.m_device = context.Device();
    timers.m_count  = dispatchCount;
    timers.m_period = context.Info().timestampPeriod;

    const VkQueryPoolCreateInfo info{.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType  = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 2 * dispatchCount};
    VK_TRY(vkCreateQueryPool(timers.m_device, &info, nullptr, &timers.m_pool));
    return timers;
}

DispatchTimers::~DispatchTimers() { Release(); }

void DispatchTimers::Release() noexcept {
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    m_device = VK_NULL_HANDLE;
    m_count  = 0;
}

DispatchTimers::DispatchTimers(DispatchTimers&& other) noexcept { *this = std::move(other); }

DispatchTimers& DispatchTimers::operator=(DispatchTimers&& other) noexcept {
    if (this != &other) {
        Release();
        m_device       = other.m_device;
        m_pool         = other.m_pool;
        m_count        = other.m_count;
        m_period       = other.m_period;
        other.m_device = VK_NULL_HANDLE;
        other.m_pool   = VK_NULL_HANDLE;
        other.m_count  = 0;
    }
    return *this;
}

void DispatchTimers::Reset(VkCommandBuffer commands) const {
    if (m_pool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commands, m_pool, 0, 2 * m_count);
    }
}

void DispatchTimers::Begin(VkCommandBuffer commands, u32_t dispatch) const {
    if (m_pool != VK_NULL_HANDLE && dispatch < m_count) {
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m_pool,
                             2 * dispatch);
    }
}

void DispatchTimers::End(VkCommandBuffer commands, u32_t dispatch) const {
    if (m_pool != VK_NULL_HANDLE && dispatch < m_count) {
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m_pool,
                             2 * dispatch + 1);
    }
}

void DispatchTimers::Accumulate(std::pmr::vector<f64_t>& totals) const {
    if (m_pool == VK_NULL_HANDLE) {
        return;
    }
    std::pmr::vector<u64_t> ticks(2 * m_count, &memory::General().Resource());
    if (vkGetQueryPoolResults(m_device, m_pool, 0, 2 * m_count,
                              ticks.size() * sizeof(u64_t), ticks.data(), sizeof(u64_t),
                              VK_QUERY_RESULT_64_BIT)
        != VK_SUCCESS) {
        return;
    }
    if (totals.size() < m_count) {
        totals.resize(m_count, 0.0);
    }
    for (u32_t i = 0; i < m_count; ++i) {
        if (ticks[2 * i + 1] > ticks[2 * i]) {
            totals[i] += static_cast<f64_t>(ticks[2 * i + 1] - ticks[2 * i])
                         * static_cast<f64_t>(m_period) * 1e-6;
        }
    }
}

// --- Recording ------------------------------------------------------------------------------------

namespace {

/// Hazard tracking per buffer, so a barrier is emitted only where two dispatches really depend on
/// each other rather than between every pair.
struct BufferState {
    b8_t writtenSinceBarrier = false;
    b8_t readSinceBarrier    = false;
};

/// Emits one dependency covering every buffer this dispatch has a hazard on.
void EmitBarriers(VkCommandBuffer commands, const Dispatch& dispatch,
                  const SectionResources& resources, std::pmr::vector<BufferState>& state) {
    std::array<VkBufferMemoryBarrier2, kMaxNodeInputs + kMaxNodeChannels> barriers{};
    u32_t                                                                count = 0;

    const auto push = [&](u32_t buffer) {
        const vulkan::SectionSlot& slot = resources.Slot(buffer);
        barriers[count++] = VkBufferMemoryBarrier2{
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                             | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                             | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer              = slot.buffer,
            .offset              = slot.offset,
            .size                = slot.size};
    };

    // Read-after-write: an input this dispatch reads was written by an earlier one.
    for (u8_t i = 0; i < dispatch.inputCount; ++i) {
        const u32_t buffer = dispatch.inputBuffers[i];
        if (buffer != kInvalidBuffer && state[buffer].writtenSinceBarrier) {
            push(buffer);
        }
    }
    // Write-after-read: an output slot this dispatch overwrites is one an earlier dispatch read,
    // which happens as soon as the planner starts reusing buffers.
    for (u8_t channel = 0; channel < kMaxNodeChannels; ++channel) {
        const u32_t buffer = dispatch.outputBuffers[channel];
        if (buffer != kInvalidBuffer
            && (state[buffer].readSinceBarrier || state[buffer].writtenSinceBarrier)) {
            push(buffer);
        }
    }

    if (count == 0) {
        return;
    }
    const VkDependencyInfo dependency{.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .bufferMemoryBarrierCount = count,
                                      .pBufferMemoryBarriers    = barriers.data()};
    vkCmdPipelineBarrier2(commands, &dependency);

    for (BufferState& entry : state) {
        entry.readSinceBarrier    = false;
        entry.writtenSinceBarrier = false;
    }
}

} // namespace

Status RecordSection(vulkan::Queue& queue, KernelLibrary& kernels, const CompiledGraph& compiled,
                     const SectionResources& resources, const DispatchTimers& timers,
                     VkCommandBuffer commands, const SectionJob& job) {
#ifdef TRACY_ENABLE
    ZoneScopedN("terrain::RecordSection");
#endif
    timers.Reset(commands);

    std::pmr::vector<BufferState> state(compiled.buffers.size(), &memory::General().Resource());

    for (usize_t i = 0; i < compiled.dispatches.size(); ++i) {
        const Dispatch& dispatch = compiled.dispatches[i];
        const Domain    domain   = dispatch.channels[0].domain;

        Result<const vulkan::ComputePipeline*> pipeline =
            kernels.Get(dispatch.kind, dispatch.variant, domain);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }

        EmitBarriers(commands, dispatch, resources, state);

        vulkan::KernelPushConstants constants{};
        constants.origin      = job.origin;
        constants.extent      = {job.extent.x, job.extent.y, job.extent.z};
        constants.halo        = dispatch.halo;
        constants.domain      = static_cast<u32_t>(domain);
        constants.channelMask = dispatch.channelMask;
        constants.seed        = job.seed;
        constants.params      = dispatch.params;
        for (u8_t input = 0; input < dispatch.inputCount; ++input) {
            const u32_t buffer = dispatch.inputBuffers[input];
            if (buffer == kInvalidBuffer) {
                return std::unexpected(MakeScriptError(
                    ErrorCode::InternalError, ErrorStage::Evaluate, dispatch.location.file,
                    dispatch.location.line, "{} input {} was not assigned a buffer",
                    ToString(dispatch.kind), input));
            }
            constants.inputs[input] = resources.Slot(buffer).address;
        }
        for (u8_t channel = 0; channel < kMaxNodeChannels; ++channel) {
            const u32_t buffer = dispatch.outputBuffers[channel];
            if (buffer != kInvalidBuffer) {
                constants.outputs[channel] = resources.Slot(buffer).address;
            }
        }

#ifdef TRACY_ENABLE
        // Labelled with the op and its script line, on the CPU timeline and on the GPU one. The GPU
        // zones are the measurement that decides whether kernel fusion is worth building: they say
        // how much of a section is spent in pointwise dispatches (spec sections 9 and 13).
        std::array<char, 64> zoneName{};
        std::snprintf(zoneName.data(), zoneName.size(), "%s:%u", ToString(dispatch.kind),
                      dispatch.location.line);
        ZoneTransientN(dispatchZone, zoneName.data(), true);
        TracyVkZoneTransient(queue.TracyContext(), gpuZone, commands, zoneName.data(), true);
#else
        (void)queue;
#endif

        timers.Begin(commands, static_cast<u32_t>(i));
        (*pipeline)->Bind(commands, constants);
        const std::array<u32_t, 3> groups =
            vulkan::DispatchSize(constants.domain, constants.extent, constants.halo);
        vkCmdDispatch(commands, groups[0], groups[1], groups[2]);
        timers.End(commands, static_cast<u32_t>(i));

        for (u8_t input = 0; input < dispatch.inputCount; ++input) {
            const u32_t buffer = dispatch.inputBuffers[input];
            if (buffer != kInvalidBuffer) {
                state[buffer].readSinceBarrier = true;
            }
        }
        for (u8_t channel = 0; channel < kMaxNodeChannels; ++channel) {
            const u32_t buffer = dispatch.outputBuffers[channel];
            if (buffer != kInvalidBuffer) {
                state[buffer].writtenSinceBarrier = true;
            }
        }
    }
    return {};
}

} // namespace engine::terrain
