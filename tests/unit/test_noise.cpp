// The fBm kernel's gradient channel must be the exact derivative of its value channel.
//
// The engine relies on analytic gradients everywhere a slope, a normal or a curvature is needed, so
// a wrong derivative would silently corrupt every downstream op instead of failing loudly. This
// test dispatches both channels over one section and compares the analytic gradient against a
// central difference of the value channel, which is an independent estimate of the same quantity.

#include "gpu_support.hpp"

#include <engine/application.hpp>
#include <engine/engine.hpp>
#include <engine/memory/general.hpp>
#include <engine/terrain/mapping.hpp>

#include <cmath>
#include <cstring>
#include <memory_resource>
#include <vector>

using namespace engine;
using namespace engine::vulkan;
using engine::terrain::ClassOf;
using engine::terrain::Domain;
using engine::terrain::Mapping;
using engine::terrain::SampleCount;
using engine::terrain::SectionExtent;
using engine::terrain::ValueSize;

namespace {

constexpr u32_t kExtent = 64;
/// One ring of padding, so a central difference is available for every interior sample.
constexpr u32_t kHalo = 1;

constexpr i32_t kOriginX = 12345;
constexpr i32_t kOriginZ = -6789;

/// The truncation error of a central difference goes as h^2, so a fine sample spacing is what makes
/// this a tight test rather than a loose one. At 0.05 m the highest octave still gets about 250
/// samples per wavelength, and the value differences stay far above f32 rounding noise.
constexpr f32_t kResolution = 0.05f;
constexpr f32_t kFrequency  = 0.01f;
constexpr u32_t kOctaves    = 4;
constexpr f32_t kLacunarity = 2.0f;
constexpr f32_t kGain       = 0.5f;
constexpr f32_t kAmplitude  = 1.0f;

/// Tolerance as a fraction of the RMS gradient magnitude over the section. Comparing against a
/// global scale rather than the per-sample magnitude avoids the degenerate case where the two
/// gradient components nearly cancel and any absolute error looks enormous in relative terms.
constexpr f64_t kRmsTolerance = 0.01;

void PackFloat(KernelPushConstants& constants, usize_t word, f32_t value) {
    std::memcpy(&constants.params[word], &value, sizeof(f32_t));
}

KernelPushConstants MakeConstants(u32_t channelMask) {
    KernelPushConstants constants{};
    constants.origin      = {kOriginX, 0, kOriginZ};
    constants.extent      = {kExtent, 1, kExtent};
    constants.halo        = kHalo;
    constants.domain      = 2;
    constants.channelMask = channelMask;
    constants.seed        = 0x5EED;
    PackFloat(constants, 0, kFrequency);
    constants.params[1] = kOctaves;
    PackFloat(constants, 2, kLacunarity);
    PackFloat(constants, 3, kGain);
    PackFloat(constants, 4, kAmplitude);
    PackFloat(constants, 5, 0.0f);
    PackFloat(constants, 6, kResolution);
    return constants;
}

} // namespace

int main() {
    const Result<Application*> application =
        engine::init(AppInfo{.mode = RunMode::Headless, .name = "test_noise"});
    if (!application) {
        std::printf("FAIL engine::init: %s\n", application.error().Format().data());
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        return 1;
    }
    Context& context = VulkanContext(**application);

    const int result = [&]() -> int {
        const SectionExtent extent{.x = kExtent, .y = 1, .z = kExtent};
        const Mapping       valueMapping{Domain::R2, 1};
        const Mapping       gradientMapping{Domain::R2, 2};
        const u64_t         samples      = SampleCount(valueMapping, extent, kHalo);
        const u64_t         valueBytes   = ValueSize(valueMapping, extent, kHalo);
        const u64_t         gradientBytes = ValueSize(gradientMapping, extent, kHalo);
        const u32_t         padded       = kExtent + 2 * kHalo;

        Result<ComputePipeline> fbm = ComputePipeline::Create(
            context.Device(), "ops/fbm.comp.spv", WorkgroupSpecialization(2),
            context.Pipelines().Handle());
        REQUIRE_OK(fbm);

        // The value and the gradient go to separate slots, so a single dispatch writing both can
        // be compared channel against channel.
        Result<SectionSlot> valueSlot =
            context.Sections().Acquire(ClassOf(valueMapping, extent, kHalo), valueBytes);
        REQUIRE_OK(valueSlot);
        Result<SectionSlot> gradientSlot =
            context.Sections().Acquire(ClassOf(gradientMapping, extent, kHalo), gradientBytes);
        REQUIRE_OK(gradientSlot);

        test::Section("channel mask");
        // A channel no consumer asks for is not written: filling the gradient slot with a sentinel
        // and dispatching with only the value channel must leave the sentinel in place.
        {
            Result<ComputePipeline> constant = ComputePipeline::Create(
                context.Device(), "ops/constant.comp.spv", WorkgroupSpecialization(2),
                context.Pipelines().Handle());
            REQUIRE_OK(constant);

            KernelPushConstants fill{};
            fill.origin      = {kOriginX, 0, kOriginZ};
            fill.extent      = {kExtent, 1, kExtent};
            fill.halo        = kHalo;
            fill.domain      = 2;
            fill.channelMask = 1;
            fill.outputs[0]  = gradientSlot->address;
            PackFloat(fill, 0, -777.0f);
            PackFloat(fill, 1, -777.0f);
            fill.params[4] = 2; // two components

            KernelPushConstants valueOnly = MakeConstants(0x1);
            valueOnly.outputs[0]          = valueSlot->address;
            valueOnly.outputs[1]          = gradientSlot->address;

            Result<test::Readback> filled =
                test::RunKernel(context, *constant, *gradientSlot, fill, gradientBytes);
            REQUIRE_OK(filled);
            context.Readback().ReleaseOldest();

            Result<test::Readback> after =
                test::RunKernel(context, *fbm, *gradientSlot, valueOnly, gradientBytes);
            REQUIRE_OK(after);
            u32_t overwritten = 0;
            for (u64_t i = 0; i < samples * 2; ++i) {
                if (after->data[i] != -777.0f) {
                    ++overwritten;
                }
            }
            CHECK_EQ(overwritten, u32_t{0});
            context.Readback().ReleaseOldest();
        }

        test::Section("analytic gradient against a central difference");
        KernelPushConstants both = MakeConstants(0x3);
        both.outputs[0]          = valueSlot->address;
        both.outputs[1]          = gradientSlot->address;

        Result<test::Readback> values =
            test::RunKernel(context, *fbm, *valueSlot, both, valueBytes);
        REQUIRE_OK(values);
        // Copy out of the ring: the gradient read below reuses it.
        std::pmr::vector<f32_t> height(samples, &memory::General().Resource());
        std::memcpy(height.data(), values->data, static_cast<usize_t>(valueBytes));
        context.Readback().ReleaseOldest();

        Result<test::Readback> gradients =
            test::RunKernel(context, *fbm, *gradientSlot, both, gradientBytes);
        REQUIRE_OK(gradients);

        const f32_t* gradient = gradients->data;

        const auto centralDifference = [&](u64_t index, u32_t axisStride) {
            return (static_cast<f64_t>(height[index + axisStride])
                    - static_cast<f64_t>(height[index - axisStride]))
                   / (2.0 * kResolution);
        };

        // First pass: the RMS gradient magnitude, which sets the scale the errors are judged at.
        f64_t sumSquares = 0.0;
        u32_t checked     = 0;
        for (u32_t localZ = kHalo; localZ + kHalo < padded; ++localZ) {
            for (u32_t localX = kHalo; localX + kHalo < padded; ++localX) {
                const u64_t index = u64_t{localZ} * padded + localX;
                const f64_t gx    = gradient[index * 2];
                const f64_t gz    = gradient[index * 2 + 1];
                sumSquares += gx * gx + gz * gz;
                ++checked;
            }
        }
        const f64_t rms     = std::sqrt(sumSquares / static_cast<f64_t>(checked));
        const f64_t allowed = kRmsTolerance * rms;

        u32_t mismatches = 0;
        f64_t worst      = 0.0;
        for (u32_t localZ = kHalo; localZ + kHalo < padded; ++localZ) {
            for (u32_t localX = kHalo; localX + kHalo < padded; ++localX) {
                const u64_t index = u64_t{localZ} * padded + localX;

                const f64_t analyticX = gradient[index * 2];
                const f64_t analyticZ = gradient[index * 2 + 1];
                const f64_t errorX    = std::fabs(analyticX - centralDifference(index, 1));
                const f64_t errorZ    = std::fabs(analyticZ - centralDifference(index, padded));
                const f64_t error     = errorX > errorZ ? errorX : errorZ;

                if (error > allowed) {
                    if (mismatches == 0) {
                        std::printf("     first mismatch at (%u,%u): analytic (%g,%g), error %g, "
                                    "allowed %g\n",
                                    localX, localZ, analyticX, analyticZ, error, allowed);
                    }
                    ++mismatches;
                }
                if (error > worst) {
                    worst = error;
                }
            }
        }

        std::printf("     %u interior samples, RMS gradient %g, worst error %g (%.4f%% of RMS)\n",
                    checked, rms, worst, rms > 0.0 ? worst / rms * 100.0 : 0.0);
        CHECK(rms > 0.0);
        CHECK_EQ(mismatches, u32_t{0});

        test::Section("value range");
        // The declared scale is meant to keep a unit-amplitude fBm inside roughly [-1, 1]. A gross
        // break here means ENGINE_SIMPLEX2_SCALE is wrong and every declared output range is off.
        f32_t minimum = height[0];
        f32_t maximum = height[0];
        for (u64_t i = 1; i < samples; ++i) {
            minimum = height[i] < minimum ? height[i] : minimum;
            maximum = height[i] > maximum ? height[i] : maximum;
        }
        std::printf("     value range over the section: [%g, %g]\n",
                    static_cast<f64_t>(minimum), static_cast<f64_t>(maximum));
        CHECK(minimum > -1.5f);
        CHECK(maximum < 1.5f);
        CHECK(maximum > minimum);

        context.Readback().ReleaseOldest();
        context.Sections().Release(*gradientSlot);
        context.Sections().Release(*valueSlot);
        CHECK_EQ(context.Sections().UsedBytes(), VkDeviceSize{0});
        return 0;
    }();

    if (const Status closed = engine::shutdown(*application); !closed) {
        std::printf("FAIL engine::shutdown: %s\n", closed.error().Format().data());
        return 1;
    }
    return result != 0 ? result : test::Summary("test_noise");
}
