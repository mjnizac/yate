#pragma once

// Shared GPU plumbing for the tests that need a device: dispatch one kernel into a section slot,
// copy the result into the readback ring and wait for it.

#include "test_support.hpp"

#include <engine/vulkan/buffer_pool.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/pipeline.hpp>

#include <cstring>

namespace test {

inline constexpr engine::u64_t kGpuTimeoutNanoseconds = 5ull * 1000 * 1000 * 1000;

/// A readback window: the mapped samples plus the ring offset the caller must release.
struct Readback {
    const engine::f32_t* data   = nullptr;
    VkDeviceSize         offset = 0;
};

/// Records a dispatch of `pipeline` writing into `slot`, copies `byteCount` bytes into the
/// readback ring, submits, waits for the timeline and invalidates the host range.
[[nodiscard]] inline engine::Result<Readback> RunKernel(engine::vulkan::Context&     context,
                                                        const engine::vulkan::ComputePipeline& pipeline,
                                                        const engine::vulkan::SectionSlot&     slot,
                                                        const engine::vulkan::KernelPushConstants& constants,
                                                        VkDeviceSize byteCount) {
    using namespace engine;
    using namespace engine::vulkan;

    Result<VkDeviceSize> readbackOffset = context.Readback().Reserve(byteCount, 16);
    if (!readbackOffset) {
        return std::unexpected(readbackOffset.error());
    }

    Queue&                  queue    = context.ComputeQueue();
    Result<VkCommandBuffer> commands = queue.BeginOneShot();
    if (!commands) {
        return std::unexpected(commands.error());
    }

    pipeline.Bind(*commands, constants);
    const std::array<u32_t, 3> groups =
        DispatchSize(constants.Domain(), constants.extent, constants.halo);
    vkCmdDispatch(*commands, groups[0], groups[1], groups[2]);

    ComputeToTransferBarrier(*commands, slot.buffer, slot.offset, slot.size);

    const VkBufferCopy copy{
        .srcOffset = slot.offset, .dstOffset = *readbackOffset, .size = byteCount};
    vkCmdCopyBuffer(*commands, slot.buffer, context.Readback().GetBuffer().handle, 1, &copy);

    Result<u64_t> submitted = queue.EndAndSubmit(*commands);
    if (!submitted) {
        return std::unexpected(submitted.error());
    }
    if (Status waited = queue.WaitTimeline(*submitted, kGpuTimeoutNanoseconds); !waited) {
        return std::unexpected(waited.error());
    }
    if (Status invalidated = context.Memory().InvalidateBuffer(context.Readback().GetBuffer(),
                                                               *readbackOffset, byteCount);
        !invalidated) {
        return std::unexpected(invalidated.error());
    }

    return Readback{
        .data   = static_cast<const f32_t*>(context.Readback().MappedAt(*readbackOffset)),
        .offset = *readbackOffset};
}

/// Uploads `bytes` of `data` into `slot` through the staging ring.
///
/// Letting a test choose the exact contents of an input is what makes a kernel comparison worth
/// anything: generated inputs from another kernel would only prove the two agree with each other.
[[nodiscard]] inline engine::Status UploadToSlot(engine::vulkan::Context&           context,
                                                const engine::vulkan::SectionSlot& slot,
                                                const void* data, VkDeviceSize bytes) {
    using namespace engine;
    using namespace engine::vulkan;

    Result<VkDeviceSize> stagingOffset = context.Staging().Reserve(bytes, 16);
    if (!stagingOffset) {
        return std::unexpected(stagingOffset.error());
    }
    std::memcpy(context.Staging().MappedAt(*stagingOffset), data, static_cast<usize_t>(bytes));
    if (Status flushed =
            context.Memory().FlushBuffer(context.Staging().GetBuffer(), *stagingOffset, bytes);
        !flushed) {
        return std::unexpected(flushed.error());
    }

    Queue&                  queue    = context.ComputeQueue();
    Result<VkCommandBuffer> commands = queue.BeginOneShot();
    if (!commands) {
        return std::unexpected(commands.error());
    }
    const VkBufferCopy copy{
        .srcOffset = *stagingOffset, .dstOffset = slot.offset, .size = bytes};
    vkCmdCopyBuffer(*commands, context.Staging().GetBuffer().handle, slot.buffer, 1, &copy);

    Result<u64_t> submitted = queue.EndAndSubmit(*commands);
    if (!submitted) {
        return std::unexpected(submitted.error());
    }
    if (Status waited = queue.WaitTimeline(*submitted, kGpuTimeoutNanoseconds); !waited) {
        return std::unexpected(waited.error());
    }
    context.Staging().ReleaseOldest();
    return {};
}

} // namespace test
