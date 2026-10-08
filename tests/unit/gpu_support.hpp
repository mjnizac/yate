#pragma once

// Shared GPU plumbing for the tests that need a device: dispatch one kernel into a section slot,
// copy the result into the readback ring and wait for it.

#include "test_support.hpp"

#include <engine/memory/general.hpp>
#include <engine/terrain/compiler.hpp>
#include <engine/terrain/evaluator.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/vulkan/buffer_pool.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/pipeline.hpp>

#include <algorithm>
#include <cstring>
#include <memory_resource>
#include <vector>

namespace test {

inline constexpr engine::u64_t kGpuTimeoutNanoseconds = 5ull * 1000 * 1000 * 1000;

/// Fails the suite if the Vulkan debug messenger reported anything at error severity.
///
/// Without this a validation error only shows up as a log line, which is easy to filter out and easy
/// to miss: a real one sat in the output for a while because every test still passed. Calling this at
/// the end of a GPU test makes the whole suite a validation run.
inline void CheckNoValidationErrors() {
    ++g_checks;
    const engine::u64_t errors = engine::vulkan::ValidationErrorCount();
    if (errors != 0) {
        ++g_failures;
        std::printf("FAIL the Vulkan debug messenger reported %llu error(s); see the log above\n",
                    static_cast<unsigned long long>(errors));
    }
}

/// A rectangular f32 field, indexed the way a section buffer is: x fastest, then z, then y.
struct Field {
    engine::u32_t                          width  = 0;
    engine::u32_t                          height = 0;
    engine::u32_t                          depth  = 1;
    std::pmr::vector<engine::f32_t>        samples{&engine::memory::General().Resource()};

    void Resize(engine::u32_t w, engine::u32_t d, engine::u32_t h, engine::u32_t components) {
        width  = w;
        depth  = d;
        height = h;
        samples.assign(static_cast<engine::usize_t>(w) * d * h * components, 0.0f);
    }
};

/// Evaluates `graph` over `extent`-sized sections covering a `width x depth x height` region, and
/// stitches the interiors into one field.
///
/// Mirrors the exporter's loop: one submission per section, every section dispatched over its full
/// extent with only the useful interior kept, so an edge section computes exactly what an interior one
/// does.
[[nodiscard]] inline engine::Result<Field> Evaluate(
    engine::vulkan::Context& context, engine::terrain::KernelLibrary& kernels,
    const engine::terrain::Graph& graph, engine::terrain::SectionExtent extent, engine::u32_t width,
    engine::u32_t depth, engine::u32_t height, engine::f32_t resolution,
    std::array<engine::i32_t, 3> origin, engine::u32_t seed) {
    using namespace engine;
    using namespace engine::vulkan;
    using namespace engine::terrain;

    Result<CompiledGraph> compiled =
        Compile(graph, CompileOptions{.extent = extent, .resolution = resolution});
    if (!compiled) {
        return std::unexpected(compiled.error());
    }
    Result<SectionResources> resources = SectionResources::Create(context, *compiled);
    if (!resources) {
        return std::unexpected(resources.error());
    }
    Result<DispatchTimers> timers = DispatchTimers::Create(context, compiled->stats.dispatchCount);
    if (!timers) {
        return std::unexpected(timers.error());
    }

    const CompiledOutput& output     = compiled->outputs[0];
    const u8_t            components = output.mapping.components;
    // A requested output is still an ordinary value, so it carries whatever halo propagation gave it: a
    // script whose height is also read by a gradient gets a padded buffer. Reading it with a stride of
    // `extent.x` shears the image, which is how a real export bug stayed hidden here too.
    const u32_t halo   = output.halo;
    const u32_t padded = extent.x + 2 * halo;
    const u64_t sectionBytes = ValueSize(output.mapping, extent, halo);

    Field field;
    field.Resize(width, depth, height, components);

    Queue& queue = context.ComputeQueue();
    for (u32_t originY = 0; originY < depth; originY += extent.y) {
        for (u32_t originZ = 0; originZ < height; originZ += extent.z) {
            for (u32_t originX = 0; originX < width; originX += extent.x) {
                Result<VkCommandBuffer> commands = queue.BeginOneShot();
                if (!commands) {
                    return std::unexpected(commands.error());
                }

                const SectionJob job{
                    .origin = {origin[0] + static_cast<i32_t>(originX),
                               origin[1] + static_cast<i32_t>(originY),
                               origin[2] + static_cast<i32_t>(originZ)},
                    .extent = extent,
                    .seed   = seed};
                if (Status recorded = RecordSection(queue, kernels, *compiled, *resources, *timers,
                                                    *commands, job);
                    !recorded) {
                    return std::unexpected(recorded.error());
                }

                const SectionSlot& slot = resources->Slot(output.buffer);
                ComputeToTransferBarrier(*commands, slot.buffer, slot.offset, slot.size);
                Result<VkDeviceSize> readback = context.Readback().Reserve(sectionBytes, 16);
                if (!readback) {
                    return std::unexpected(readback.error());
                }
                const VkBufferCopy copy{
                    .srcOffset = slot.offset, .dstOffset = *readback, .size = sectionBytes};
                vkCmdCopyBuffer(*commands, slot.buffer, context.Readback().GetBuffer().handle, 1,
                                &copy);

                Result<u64_t> submitted = queue.EndAndSubmit(*commands);
                if (!submitted) {
                    return std::unexpected(submitted.error());
                }
                if (Status waited = queue.WaitTimeline(*submitted, kGpuTimeoutNanoseconds);
                    !waited) {
                    return std::unexpected(waited.error());
                }
                if (Status invalidated = context.Memory().InvalidateBuffer(
                        context.Readback().GetBuffer(), *readback, sectionBytes);
                    !invalidated) {
                    return std::unexpected(invalidated.error());
                }
                const auto* chunk =
                    static_cast<const f32_t*>(context.Readback().MappedAt(*readback));
                const auto* samples =
                    chunk + (static_cast<u64_t>(halo) * padded + halo) * components;

                // Copy only the part of the section that lies inside the region. An edge section
                // computes a full extent and the overhang is discarded, which is the behaviour the
                // seam test has to see through.
                const u32_t columns = std::min(extent.x, width - originX);
                const u32_t rows    = std::min(extent.z, height - originZ);
                const u32_t layers  = std::min(extent.y, depth - originY);
                for (u32_t layer = 0; layer < layers; ++layer) {
                    for (u32_t row = 0; row < rows; ++row) {
                        const u64_t source =
                            ((static_cast<u64_t>(layer) * padded + row) * padded) * components;
                        const u64_t target =
                            (((static_cast<u64_t>(originY) + layer) * height + originZ + row)
                                 * width
                             + originX)
                            * components;
                        std::memcpy(field.samples.data() + target, samples + source,
                                    static_cast<usize_t>(columns) * components * sizeof(f32_t));
                    }
                }
                context.Readback().ReleaseOldest();
            }
        }
    }
    return field;
}

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
