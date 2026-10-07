// Milestone 3 acceptance: a compute kernel writes a section buffer that is read back correctly,
// and every VRAM pool is accounted for.
//
// The `coords` kernel is used on purpose: its output pins down the sample layout, the halo offset
// and the integer section origin at the same time, so an indexing mistake cannot pass unnoticed.

#include "gpu_support.hpp"

#include <engine/application.hpp>
#include <engine/engine.hpp>
#include <engine/log.hpp>
#include <engine/terrain/mapping.hpp>
#include <engine/vulkan/buffer_pool.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/pipeline.hpp>

#include <cstring>

using namespace engine;
using namespace engine::vulkan;
using engine::terrain::ClassOf;
using engine::terrain::Domain;
using engine::terrain::Mapping;
using engine::terrain::SampleCount;
using engine::terrain::SectionExtent;
using engine::terrain::ValueSize;

namespace {

/// Deliberately far from the origin and not a multiple of the section size, so a kernel that
/// derived positions from floats or ignored the halo would be caught.
constexpr i32_t kOriginX = 1 << 20;
constexpr i32_t kOriginZ = -3 * 1024 - 7;
constexpr u32_t kExtent  = 16;
constexpr u32_t kHalo    = 2;

constexpr u64_t kTimeoutNanoseconds = 5ull * 1000 * 1000 * 1000;

} // namespace

int main() {
    const Result<Application*> application =
        engine::init(AppInfo{.mode = RunMode::Headless, .name = "test_compute_roundtrip"});
    if (!application) {
        std::printf("FAIL engine::init: %s\n", application.error().Format().data());
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        return 1;
    }
    Context& context = VulkanContext(**application);

    const int result = [&]() -> int {
        test::Section("device and pools");
        CHECK(context.Device() != VK_NULL_HANDLE);
        CHECK(context.ComputeQueue().IsValid());
        CHECK(context.Memory().IsValid());
        // Staging and readback rings are persistently mapped.
        CHECK(context.Staging().GetBuffer().mapped != nullptr);
        CHECK(context.Readback().GetBuffer().mapped != nullptr);
        CHECK(context.Memory().CategoryBytes(VramCategory::Staging)
              == context.Staging().Capacity());
        CHECK(context.Memory().CategoryBytes(VramCategory::Readback)
              == context.Readback().Capacity());
        // Nothing is reserved for section buffers before the first slot is taken.
        CHECK_EQ(context.Sections().UsedBytes(), VkDeviceSize{0});
        CHECK_EQ(context.Sections().BlockCount(), usize_t{0});

        test::Section("section slot");
        const SectionExtent extent{.x = kExtent, .y = 1, .z = kExtent};
        const Mapping       coordsMapping{Domain::R2, 2};
        const u64_t         coordsBytes = ValueSize(coordsMapping, extent, kHalo);
        const u64_t         samples     = SampleCount(coordsMapping, extent, kHalo);
        CHECK_EQ(samples, u64_t{(kExtent + 2 * kHalo) * (kExtent + 2 * kHalo)});
        CHECK_EQ(coordsBytes, samples * 2 * 4);

        Result<SectionSlot> slot =
            context.Sections().Acquire(ClassOf(coordsMapping, extent, kHalo), coordsBytes);
        REQUIRE_OK(slot);
        CHECK(slot->IsValid());
        CHECK(slot->address != 0);
        CHECK(slot->size >= coordsBytes);
        CHECK_EQ(slot->offset % kSectionSlotAlignment, VkDeviceSize{0});
        // The pool grew by one R2 block, which counts as reserved but not yet as used.
        CHECK_EQ(context.Sections().BlockCount(), usize_t{1});
        CHECK_EQ(context.Sections().UsedBytes(), slot->size);
        CHECK(context.Memory().CategoryReserved(VramCategory::SectionBuffersR2)
              > context.Memory().CategoryBytes(VramCategory::SectionBuffersR2));
        CHECK_EQ(context.Memory().CategoryBytes(VramCategory::SectionBuffersR3), u64_t{0});

        test::Section("coords kernel round-trip");
        Result<ComputePipeline> coords = ComputePipeline::Create(
            context.Device(), "ops/coords.comp.spv", WorkgroupSpecialization(2),
            context.Pipelines().Handle());
        REQUIRE_OK(coords);

        KernelPushConstants constants{};
        constants.origin      = {kOriginX, 0, kOriginZ};
        constants.extent      = {kExtent, 1, kExtent};
        constants.halo        = kHalo;
        constants.SetDomain(2);
        constants.SetChannelMask(1);
        constants.outputs[0]  = slot->address;

        Result<test::Readback> coordsRead =
            test::RunKernel(context, *coords, *slot, constants, coordsBytes);
        REQUIRE_OK(coordsRead);
        const f32_t* values = coordsRead->data;
        const u32_t padded    = kExtent + 2 * kHalo;
        u32_t       mismatches = 0;
        for (u32_t localZ = 0; localZ < padded; ++localZ) {
            for (u32_t localX = 0; localX < padded; ++localX) {
                const u64_t index    = u64_t{localZ} * padded + localX;
                const f32_t expectedX =
                    static_cast<f32_t>(kOriginX - static_cast<i32_t>(kHalo)
                                       + static_cast<i32_t>(localX));
                const f32_t expectedZ =
                    static_cast<f32_t>(kOriginZ - static_cast<i32_t>(kHalo)
                                       + static_cast<i32_t>(localZ));
                if (values[index * 2] != expectedX || values[index * 2 + 1] != expectedZ) {
                    if (mismatches == 0) {
                        std::printf("     first mismatch at (%u,%u): got (%f,%f) want (%f,%f)\n",
                                    localX, localZ, static_cast<f64_t>(values[index * 2]),
                                    static_cast<f64_t>(values[index * 2 + 1]),
                                    static_cast<f64_t>(expectedX), static_cast<f64_t>(expectedZ));
                    }
                    ++mismatches;
                }
            }
        }
        CHECK_EQ(mismatches, u32_t{0});
        context.Readback().ReleaseOldest();

        test::Section("constant kernel and channel count");
        Result<ComputePipeline> constant = ComputePipeline::Create(
            context.Device(), "ops/constant.comp.spv", WorkgroupSpecialization(2),
            context.Pipelines().Handle());
        REQUIRE_OK(constant);

        KernelPushConstants fill{};
        fill.origin      = {kOriginX, 0, kOriginZ};
        fill.extent      = {kExtent, 1, kExtent};
        fill.halo        = kHalo;
        fill.SetDomain(2);
        fill.SetChannelMask(1);
        fill.outputs[0]  = slot->address;
        const f32_t wanted = -1234.5f;
        std::memcpy(&fill.params[0], &wanted, sizeof(f32_t));
        fill.params[4] = 1; // component count

        const u64_t        scalarBytes = ValueSize(Mapping{Domain::R2, 1}, extent, kHalo);
        Result<test::Readback> fillRead =
            test::RunKernel(context, *constant, *slot, fill, scalarBytes);
        REQUIRE_OK(fillRead);
        const f32_t* filled = fillRead->data;
        u32_t wrong = 0;
        for (u64_t i = 0; i < samples; ++i) {
            if (filled[i] != wanted) {
                ++wrong;
            }
        }
        CHECK_EQ(wrong, u32_t{0});
        context.Readback().ReleaseOldest();
        CHECK_EQ(context.Readback().ChunkCount(), usize_t{0});
        CHECK_EQ(context.Readback().LiveBytes(), VkDeviceSize{0});

        test::Section("teardown accounting");
        context.Sections().Release(*slot);
        CHECK(!slot->IsValid());
        CHECK_EQ(context.Sections().UsedBytes(), VkDeviceSize{0});
        CHECK_EQ(context.Memory().CategoryBytes(VramCategory::SectionBuffersR2), u64_t{0});

        // An empty block is only released on an explicit trim.
        CHECK_EQ(context.Sections().BlockCount(), usize_t{1});
        context.Sections().Trim();
        CHECK_EQ(context.Sections().BlockCount(), usize_t{0});
        CHECK_EQ(context.Memory().CategoryReserved(VramCategory::SectionBuffersR2), u64_t{0});
        CHECK(Allocator::ReservedDeviceMemory() > 0);

        return 0;
    }();

    if (const Status closed = engine::shutdown(*application); !closed) {
        std::printf("FAIL engine::shutdown: %s\n", closed.error().Format().data());
        return 1;
    }
    test::CheckNoValidationErrors();
    return result != 0 ? result : test::Summary("test_compute_roundtrip");
}
