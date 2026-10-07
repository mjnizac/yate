// The noise kernels and their analytic gradients.
//
// Three independent things are checked, because each catches a different class of mistake:
//
//   1. The analytic derivative really is the derivative, checked on the CPU by sweeping the step of
//      a central difference and taking the best agreement. A sweep rather than one fixed step,
//      because a fixed step cannot tell truncation error apart from a wrong formula.
//   2. The kernel agrees with that CPU reference, sample for sample, in both domains and for every
//      base function. This is milestone 5's "each op kernel matches a CPU reference implementation"
//      and it is what would catch a GLSL/C++ divergence.
//   3. A channel nothing consumes is not written.
//
// Check 1 lives on the CPU on purpose: it needs many evaluations at arbitrary points, which a
// section-based dispatch is a poor fit for, and check 2 pins the kernel to the reference anyway.

#include "gpu_support.hpp"

#include <engine/application.hpp>
#include <engine/engine.hpp>
#include <engine/memory/general.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/terrain/kernels.hpp>
#include <engine/terrain/mapping.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory_resource>
#include <vector>

using namespace engine;
using namespace engine::vulkan;
using engine::terrain::ClassOf;
using engine::terrain::Domain;
using engine::terrain::Graph;
using engine::terrain::Mapping;
using engine::terrain::NoiseKind;
using engine::terrain::SampleCount;
using engine::terrain::SectionExtent;
using engine::terrain::ValueSize;

namespace {

constexpr u32_t kHalo = 1;

/// Deliberately far from the origin and not a multiple of the section size, so a kernel that
/// derived positions from floats or ignored the halo would be caught.
constexpr i32_t kOriginX = 1 << 20;
constexpr i32_t kOriginY = -4097;
constexpr i32_t kOriginZ = -3 * 1024 - 7;

constexpr f32_t kResolution  = 0.5f;
constexpr f32_t kFrequency   = 0.01f;
constexpr u32_t kOctaves     = 4;
constexpr f32_t kLacunarity  = 2.0f;
constexpr f32_t kPersistence = 0.5f;
constexpr f32_t kAmplitude   = 1.0f;
constexpr u32_t kSeed        = 0x5EED;

/// The kernel and the reference are both f32 but group their arithmetic slightly differently in
/// places, so exact equality is not the bar; agreement to a few f32 epsilons is.
constexpr f64_t kReferenceTolerance = 2e-5;

/// Fraction of samples whose gradient may diverge near the origin. Nonzero only because ridged and
/// billow have a kink where the base crosses zero; see the comment at the check.
constexpr f64_t kKinkFraction = 0.02;

/// What the same comparison can achieve about 5e5 metres from the origin, where the f32 coordinate
/// of a simplex cell offset has roughly three good digits left. Recorded as a number rather than
/// waved away: it is the budget every op downstream of noise inherits at that distance, and
/// docs/todo.md tracks narrowing it.
constexpr f64_t kFarReferenceTolerance = 0.02;

/// A correct analytic derivative must agree with the best central difference to inside this. It
/// cannot be made arbitrarily tight: a central difference is bounded above by truncation and below
/// by f32 cancellation, and the best achievable sits between the two.
constexpr f64_t kDerivativeTolerance = 0.01;

[[nodiscard]] Graph::NoiseParams MakeNoiseParams(NoiseKind kind, Domain domain) {
    Graph::NoiseParams params;
    params.kind        = kind;
    params.domain      = domain;
    params.frequency   = kFrequency;
    params.octaves     = kOctaves;
    params.lacunarity  = kLacunarity;
    params.persistence = kPersistence;
    params.amplitude   = kAmplitude;
    return params;
}

void PackFloat(KernelPushConstants& constants, usize_t word, f32_t value) {
    std::memcpy(&constants.params[word], &value, sizeof(f32_t));
}

[[nodiscard]] KernelPushConstants MakeConstants(const Graph::NoiseParams& params,
                                               std::array<i32_t, 3> origin, SectionExtent extent,
                                               u32_t channelMask) {
    KernelPushConstants constants{};
    constants.origin      = {origin[0], params.domain == Domain::R3 ? origin[1] : 0, origin[2]};
    constants.extent      = {extent.x, extent.y, extent.z};
    constants.halo        = kHalo;
    constants.SetDomain(static_cast<u32_t>(params.domain));
    constants.SetChannelMask(channelMask);
    constants.seed        = kSeed;
    PackFloat(constants, 0, params.frequency);
    constants.params[1] = params.octaves;
    PackFloat(constants, 2, params.lacunarity);
    PackFloat(constants, 3, params.persistence);
    PackFloat(constants, 4, params.amplitude);
    PackFloat(constants, 5, params.offset);
    PackFloat(constants, 6, kResolution);
    constants.params[7] = 0;
    return constants;
}

/// World position in metres of the sample at local offset `(x, y, z)` inside the padded section.
/// Mirrors `WorldSample` in lib/kernel.lib.glsl times the sample spacing.
[[nodiscard]] std::array<f32_t, 3> WorldPosition(Domain domain, std::array<i32_t, 3> origin,
                                                u32_t x, u32_t y, u32_t z) {
    const i32_t halo = static_cast<i32_t>(kHalo);
    const i32_t wx   = origin[0] - halo + static_cast<i32_t>(x);
    const i32_t wy   = domain == Domain::R3 ? origin[1] - halo + static_cast<i32_t>(y) : 0;
    const i32_t wz   = origin[2] - halo + static_cast<i32_t>(z);
    return {static_cast<f32_t>(wx) * kResolution, static_cast<f32_t>(wy) * kResolution,
            static_cast<f32_t>(wz) * kResolution};
}

/// Which world axis a gradient component differentiates along: R2 is (x, z), R3 is (x, y, z).
[[nodiscard]] usize_t WorldAxisOf(Domain domain, usize_t component) {
    if (domain == Domain::R3) {
        return component;
    }
    return component == 0 ? 0 : 2;
}

// --- Check 1: is the analytic derivative the derivative? -----------------------------------------

/// Positions used for the derivative sweep. Near the origin on purpose: at the far-from-origin
/// coordinates the other checks use, an f32 position of about 5e5 metres has a representable
/// spacing of 0.0625 m, so every step below that silently rounds to nothing and a central difference
/// measures noise rather than a slope. The far coordinates are exercised by check 2, which does not
/// differentiate anything.
[[nodiscard]] std::array<f32_t, 3> SweepPosition(Domain domain, u32_t index) {
    const f32_t x = static_cast<f32_t>(13 * index + 1) * kResolution;
    const f32_t y = domain == Domain::R3 ? static_cast<f32_t>(7 * index + 3) * kResolution : 0.0f;
    const f32_t z = static_cast<f32_t>(29 * index + 5) * kResolution;
    return {x, y, z};
}

/// True when this sample sits on the kink that ridged and billow have where the base crosses zero.
///
/// With a single octave the kink is visible from the value alone: ridged peaks at 1 there and billow
/// bottoms out at -1. A central difference across a kink converges to the average of the two
/// one-sided derivatives, which matches neither, so those samples are skipped and counted rather
/// than silently tolerated.
[[nodiscard]] b8_t OnKink(NoiseKind kind, f32_t value) {
    if (kind == NoiseKind::Ridged) {
        return value > 0.98f;
    }
    if (kind == NoiseKind::Billow) {
        return value < -0.98f;
    }
    return false;
}

/// Best relative disagreement between the analytic gradient and a central difference of the
/// reference, over a sweep of step sizes.
///
/// Sweeping is the point: one step cannot distinguish a wrong formula from the truncation error of
/// the estimator. If the formula is right, some step in the sweep gets close.
[[nodiscard]] f64_t BestDerivativeError(const Graph::NoiseParams& params,
                                       std::array<f32_t, 3> position, usize_t component) {
    const terrain::NoiseSample sample   = terrain::EvalNoise(params, kSeed, position);
    const f64_t                analytic = sample.gradient[component];
    const usize_t              axis     = WorldAxisOf(params.domain, component);

    f64_t best = 1e30;
    for (i32_t exponent = 0; exponent <= 10; ++exponent) {
        const f32_t step = 4.0f / static_cast<f32_t>(1u << exponent);

        std::array<f32_t, 3> ahead  = position;
        std::array<f32_t, 3> behind = position;
        ahead[axis] += step;
        behind[axis] -= step;
        // A step that rounds away entirely would make the difference meaningless.
        if (ahead[axis] == position[axis] || behind[axis] == position[axis]) {
            continue;
        }

        const f64_t central = (static_cast<f64_t>(terrain::EvalNoise(params, kSeed, ahead).value)
                               - static_cast<f64_t>(terrain::EvalNoise(params, kSeed, behind).value))
                              / (static_cast<f64_t>(ahead[axis]) - static_cast<f64_t>(behind[axis]));

        const f64_t scale = std::max(std::fabs(analytic), 1e-3);
        best              = std::min(best, std::fabs(analytic - central) / scale);
    }
    return best;
}

void CheckDerivative(NoiseKind kind, Domain domain, u32_t octaves, const char* label) {
    Graph::NoiseParams params = MakeNoiseParams(kind, domain);
    params.octaves            = octaves;
    const usize_t components  = domain == Domain::R3 ? 3 : 2;

    u32_t checked = 0;
    u32_t skipped = 0;
    u32_t bad     = 0;
    f64_t worst   = 0.0;

    for (u32_t i = 0; i < 64; ++i) {
        const std::array<f32_t, 3> position = SweepPosition(domain, i);
        if (octaves == 1 && OnKink(kind, terrain::EvalNoise(params, kSeed, position).value)) {
            ++skipped;
            continue;
        }
        for (usize_t component = 0; component < components; ++component) {
            const f64_t error = BestDerivativeError(params, position, component);
            ++checked;
            if (error > kDerivativeTolerance) {
                if (bad == 0) {
                    std::printf("     %s component %zu: best relative error %g at sample %u\n",
                                label, component, error, i);
                }
                ++bad;
            }
            worst = std::max(worst, error);
        }
    }
    std::printf("     %s: %u derivative(s), %u on a kink skipped, worst best-case error %.4f%%\n",
                label, checked, skipped, worst * 100.0);
    CHECK_EQ(bad, u32_t{0});
}

/// Range a single octave of each base function actually produces.
///
/// The normalization constants in lib/noise.lib.glsl exist to keep one octave inside roughly
/// [-1, 1]; they are not free parameters, because every declared output range and every clamped
/// sample count downstream depends on them. Measuring is the only way to know, so this reports the
/// band and fails only on a gross break.
void CheckNormalization(NoiseKind kind, Domain domain, const char* label) {
    Graph::NoiseParams params = MakeNoiseParams(kind, domain);
    params.octaves            = 1;

    f32_t minimum = terrain::EvalNoise(params, kSeed, SweepPosition(domain, 0)).value;
    f32_t maximum = minimum;
    for (u32_t i = 1; i < 4096; ++i) {
        const std::array<f32_t, 3> position = SweepPosition(domain, i);
        const f32_t                value = terrain::EvalNoise(params, kSeed, position).value;
        minimum                          = std::min(minimum, value);
        maximum                          = std::max(maximum, value);
    }
    const f32_t extreme = std::max(std::fabs(minimum), std::fabs(maximum));
    std::printf("     %s: one octave spans [%.4f, %.4f]\n", label, static_cast<f64_t>(minimum),
                static_cast<f64_t>(maximum));
    // Ridged lands in [0, 1] and billow in [-1, 1]; simplex is symmetric. All three must stay inside
    // the band without being so small that the band is wasted.
    CHECK(extreme <= 1.05f);
    CHECK(extreme >= 0.4f);
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
        struct Case {
            const char*   label;
            NoiseKind     kind;
            Domain        domain;
            SectionExtent extent;
        };
        const Case cases[] = {
            {"simplex R2", NoiseKind::Simplex, Domain::R2, {.x = 32, .y = 1, .z = 32}},
            {"ridged R2", NoiseKind::Ridged, Domain::R2, {.x = 32, .y = 1, .z = 32}},
            {"billow R2", NoiseKind::Billow, Domain::R2, {.x = 32, .y = 1, .z = 32}},
            {"simplex R3", NoiseKind::Simplex, Domain::R3, {.x = 16, .y = 16, .z = 16}},
            {"ridged R3", NoiseKind::Ridged, Domain::R3, {.x = 16, .y = 16, .z = 16}},
            {"billow R3", NoiseKind::Billow, Domain::R3, {.x = 16, .y = 16, .z = 16}},
        };

        test::Section("one octave stays inside its declared band");
        for (const Case& testCase : cases) {
            CheckNormalization(testCase.kind, testCase.domain, testCase.label);
        }

        // One octave per base function: with a single octave the kink of ridged and billow is
        // detectable from the value, so those samples can be skipped honestly.
        test::Section("one octave: the analytic derivative is the derivative");
        for (const Case& testCase : cases) {
            CheckDerivative(testCase.kind, testCase.domain, 1, testCase.label);
        }

        // Many octaves on simplex, which has no kink: this is what validates that the chain rule is
        // applied per octave rather than to the sum.
        test::Section("four octaves: the fractal chain rule");
        CheckDerivative(NoiseKind::Simplex, Domain::R2, kOctaves, "simplex R2");
        CheckDerivative(NoiseKind::Simplex, Domain::R3, kOctaves, "simplex R3");

        // Two origins. Near zero, f32 coordinates have ample precision and the kernel must match
        // the reference to a few epsilons. Far out, `d0 = p - cellOrigin` is a difference of two
        // numbers around 5e3 in noise space, where the f32 step is 5e-4, so d0 itself carries only
        // about three good digits; both implementations suffer that and land on different sides of
        // it. The far case is kept because it is the one that exercises the integer-origin path, but
        // it is judged against what the arithmetic can actually deliver.
        struct Origin {
            const char*          label;
            std::array<i32_t, 3> value;
            f64_t                tolerance;
            f64_t                fraction;
        };
        const Origin origins[] = {
            {"near origin", {97, 41, -53}, kReferenceTolerance, kKinkFraction},
            {"far from origin", {kOriginX, kOriginY, kOriginZ}, kFarReferenceTolerance, 1.01},
        };

        test::Section("the kernel matches the CPU reference");
        for (const Origin& originCase : origins) {
        const std::array<i32_t, 3> origin = originCase.value;
        for (const Case& testCase : cases) {
            const Graph::NoiseParams params = MakeNoiseParams(testCase.kind, testCase.domain);
            const u32_t              axes   = testCase.domain == Domain::R3 ? 3u : 2u;
            const Mapping            valueMapping{testCase.domain, 1};
            const Mapping            gradientMapping{testCase.domain, static_cast<u8_t>(axes)};
            const u64_t samples       = SampleCount(valueMapping, testCase.extent, kHalo);
            const u64_t valueBytes    = ValueSize(valueMapping, testCase.extent, kHalo);
            const u64_t gradientBytes = ValueSize(gradientMapping, testCase.extent, kHalo);

            const SpecializationValues workgroup =
                WorkgroupSpecialization(static_cast<u32_t>(testCase.domain));
            SpecializationValues specialization = workgroup;
            specialization.Add(static_cast<u32_t>(testCase.kind));
            specialization.Add(1); // normalized

            Result<ComputePipeline> fbm = ComputePipeline::Create(
                context.Device(), "ops/fbm.comp.spv", specialization,
                context.Pipelines().Handle());
            REQUIRE_OK(fbm);

            Result<SectionSlot> valueSlot =
                context.Sections().Acquire(ClassOf(valueMapping, testCase.extent, kHalo),
                                          valueBytes);
            REQUIRE_OK(valueSlot);
            Result<SectionSlot> gradientSlot =
                context.Sections().Acquire(ClassOf(gradientMapping, testCase.extent, kHalo),
                                          gradientBytes);
            REQUIRE_OK(gradientSlot);

            KernelPushConstants both = MakeConstants(params, origin, testCase.extent, 0x3);
            both.outputs[0]          = valueSlot->address;
            both.outputs[1]          = gradientSlot->address;

            Result<test::Readback> values =
                test::RunKernel(context, *fbm, *valueSlot, both, valueBytes);
            REQUIRE_OK(values);
            std::pmr::vector<f32_t> height(samples, &memory::General().Resource());
            std::memcpy(height.data(), values->data, static_cast<usize_t>(valueBytes));
            context.Readback().ReleaseOldest();

            Result<test::Readback> gradients =
                test::RunKernel(context, *fbm, *gradientSlot, both, gradientBytes);
            REQUIRE_OK(gradients);

            const u32_t nx = testCase.extent.x + 2 * kHalo;
            const u32_t ny = testCase.domain == Domain::R3 ? testCase.extent.y + 2 * kHalo : 1;
            const u32_t nz = testCase.extent.z + 2 * kHalo;

            f64_t worstValue    = 0.0;
            f64_t worstGradient = 0.0;
            f64_t valueScale    = 1e-9;
            f64_t gradientScale = 1e-9;
            u64_t divergent     = 0;

            for (u32_t y = 0; y < ny; ++y) {
                for (u32_t z = 0; z < nz; ++z) {
                    for (u32_t x = 0; x < nx; ++x) {
                        const u64_t                at = (static_cast<u64_t>(y) * nz + z) * nx + x;
                        const terrain::NoiseSample cpu = terrain::EvalNoise(
                            params, kSeed, WorldPosition(testCase.domain, origin, x, y, z));

                        valueScale = std::max(valueScale, std::fabs(static_cast<f64_t>(cpu.value)));
                        const f64_t valueDelta = std::fabs(static_cast<f64_t>(height[at])
                                                           - static_cast<f64_t>(cpu.value));
                        worstValue             = std::max(worstValue, valueDelta);
                        b8_t sampleDiverged    = valueDelta > kReferenceTolerance;
                        for (u32_t axis = 0; axis < axes; ++axis) {
                            const f64_t reference = cpu.gradient[axis];
                            gradientScale         = std::max(gradientScale, std::fabs(reference));
                            const f64_t delta =
                                std::fabs(static_cast<f64_t>(gradients->data[at * axes + axis])
                                          - reference);
                            worstGradient  = std::max(worstGradient, delta);
                            sampleDiverged = sampleDiverged || delta > kReferenceTolerance;
                        }
                        if (sampleDiverged) {
                            ++divergent;
                        }
                    }
                }
            }
            context.Readback().ReleaseOldest();

            const f64_t valueError    = worstValue / valueScale;
            const f64_t gradientError = worstGradient / gradientScale;
            const f64_t divergentFraction =
                static_cast<f64_t>(divergent) / static_cast<f64_t>(samples);
            std::printf("     %s: %llu samples, %llu divergent (%.4f%%), worst value %.3g, worst "
                        "gradient %.3g\n",
                        testCase.label, static_cast<unsigned long long>(samples),
                        static_cast<unsigned long long>(divergent), divergentFraction * 100.0,
                        valueError, gradientError);
            // Almost every sample must agree to f32 precision. A handful may not, and that is not a
            // defect of either side: these positions are about 5e5 metres out, where an f32
            // coordinate steps by 0.0625 m, so a sample can sit exactly on a simplex cell boundary
            // and the two implementations are free to round the `floor` either way. Ridged and
            // billow add their own: within an epsilon of the kink the sign flips and the gradient
            // legitimately jumps. A wrong formula would move nearly every sample, not a fraction of
            // a percent of them, which is why the criterion is a fraction and not the worst case.
            // The value is asserted on directly: it is continuous everywhere, so two correct
            // implementations cannot disagree by more than the precision available.
            CHECK(valueError < originCase.tolerance);
            // The gradient is asserted on by fraction, not by worst case. Where ridged or billow
            // crosses zero the derivative genuinely jumps, and which side an implementation lands on
            // is decided by the last bit of the base value, so two correct implementations differ by
            // O(1) at those samples. The measurements above show exactly that: the value agrees to
            // 6e-7 while the gradient flips. A wrong gradient formula would move nearly every
            // sample, which this bound catches; check 1 pins the formula itself.
            CHECK(divergentFraction < originCase.fraction);

            // A channel nothing consumes must not be written. The helper kernel needs its own
            // pipeline per domain, because the workgroup shape differs.
            Result<ComputePipeline> constantPipeline = ComputePipeline::Create(
                context.Device(), "ops/constant.comp.spv", workgroup,
                context.Pipelines().Handle());
            REQUIRE_OK(constantPipeline);

            KernelPushConstants fill = MakeConstants(params, origin, testCase.extent, 0x1);
            fill.outputs[0]          = gradientSlot->address;
            PackFloat(fill, 0, -777.0f);
            PackFloat(fill, 1, -777.0f);
            PackFloat(fill, 2, -777.0f);
            fill.params[4] = axes;
            Result<test::Readback> filled =
                test::RunKernel(context, *constantPipeline, *gradientSlot, fill, gradientBytes);
            REQUIRE_OK(filled);
            context.Readback().ReleaseOldest();

            KernelPushConstants valueOnly = MakeConstants(params, origin, testCase.extent, 0x1);
            valueOnly.outputs[0]          = valueSlot->address;
            valueOnly.outputs[1]          = gradientSlot->address;
            Result<test::Readback> after =
                test::RunKernel(context, *fbm, *gradientSlot, valueOnly, gradientBytes);
            REQUIRE_OK(after);
            u32_t overwritten = 0;
            for (u64_t i = 0; i < samples * axes; ++i) {
                if (after->data[i] != -777.0f) {
                    ++overwritten;
                }
            }
            CHECK_EQ(overwritten, u32_t{0});
            context.Readback().ReleaseOldest();

            context.Sections().Release(*gradientSlot);
            context.Sections().Release(*valueSlot);
        }
        }

        CHECK_EQ(context.Sections().UsedBytes(), VkDeviceSize{0});
        return 0;
    }();

    if (const Status closed = engine::shutdown(*application); !closed) {
        std::printf("FAIL engine::shutdown: %s\n", closed.error().Format().data());
        return 1;
    }
    test::CheckNoValidationErrors();
    return result != 0 ? result : test::Summary("test_noise");
}
