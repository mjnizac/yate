// Seams and determinism (spec section 10, milestone 7 acceptance).
//
// Two properties, both of which the engine either has exactly or does not have at all.
//
//  1. **Seams.** A region evaluated as one section and the same region evaluated as N x N sections
//     must produce bit-identical interiors. Not "close": identical. Anything less and a 16k export
//     shows section borders, which is the failure mode that is hardest to find by looking.
//  2. **Determinism.** The same graph, seed and bounds evaluated twice must give the same bytes.
//
// The comparison is on f32 samples read straight out of the section buffers, not on encoded PNGs,
// because a 16-bit quantization would hide exactly the one-LSB differences these tests exist to find.
//
// The tests drive the evaluator the way the exporter does, one section at a time through
// `RecordSection`, so what is under test is the real path and not a simplified copy of it.

#include "gpu_support.hpp"

#include <engine/application.hpp>
#include <engine/engine.hpp>
#include <engine/memory/general.hpp>
#include <engine/terrain/compiler.hpp>
#include <engine/terrain/graph.hpp>

#include <cstring>
#include <memory_resource>
#include <vector>

using namespace engine;
using namespace engine::vulkan;
using namespace engine::terrain;

namespace {

/// Deliberately away from the origin and not a multiple of any section size under test, so a kernel
/// that quietly worked in section-local coordinates would disagree between the two layouts.
constexpr i32_t kOriginX = 1031;
constexpr i32_t kOriginY = -517;
constexpr i32_t kOriginZ = -2049;
constexpr u32_t kSeed    = 0x5EA11234u;

/// Passed to every evaluation, so both layouts see the same world.
constexpr std::array<i32_t, 3> kOrigin{kOriginX, kOriginY, kOriginZ};

[[nodiscard]] SourceLocation At(u32_t line) { return SourceLocation{"test_seams", line}; }

using test::Evaluate;
using test::Field;

/// Reports the first differing sample and how many differ, because "they differ" is not actionable
/// and the position says immediately whether the break is on a section border.
void Compare(const char* label, const Field& a, const Field& b, u32_t components) {
    ++test::g_checks;
    if (a.samples.size() != b.samples.size()) {
        ++test::g_failures;
        std::printf("FAIL %s: %zu samples against %zu\n", label, a.samples.size(),
                    b.samples.size());
        return;
    }

    u64_t   differing = 0;
    usize_t first     = 0;
    for (usize_t i = 0; i < a.samples.size(); ++i) {
        // Bit comparison, not an epsilon: the claim is that the two layouts produce the same bytes.
        if (std::memcmp(&a.samples[i], &b.samples[i], sizeof(f32_t)) != 0) {
            if (differing == 0) {
                first = i;
            }
            ++differing;
        }
    }

    if (differing == 0) {
        std::printf("     %s: %zu sample(s) identical\n", label, a.samples.size());
        return;
    }

    ++test::g_failures;
    const usize_t sample = first / components;
    const u32_t   x      = static_cast<u32_t>(sample % a.width);
    const u32_t   z      = static_cast<u32_t>((sample / a.width) % a.height);
    const u32_t   y      = static_cast<u32_t>(sample / (static_cast<u64_t>(a.width) * a.height));
    std::printf("FAIL %s: %llu of %zu sample(s) differ, first at (%u, %u, %u): %.9g against %.9g\n",
                label, static_cast<unsigned long long>(differing), a.samples.size(), x, y, z,
                static_cast<f64_t>(a.samples[first]), static_cast<f64_t>(b.samples[first]));
}

// --- Graphs under test --------------------------------------------------------------------------

/// Fractal noise alone: the simplest thing that can have a seam, and the one every other op inherits
/// its coordinates from.
[[nodiscard]] Status BuildNoiseGraph(Graph& graph, Domain domain) {
    Graph::NoiseParams noise;
    noise.domain    = domain;
    noise.frequency = 0.01f;
    noise.octaves   = 4;
    noise.amplitude = 1.0f;
    Result<Value> value = graph.AddNoise(noise, At(1));
    if (!value) {
        return std::unexpected(value.error());
    }
    return graph.RequestOutput("height", *value, -1.0f, 1.0f);
}

/// A blur over noise: the case with a halo, which is where a seam would actually come from. The blur
/// reads 3 samples around each output sample, so the noise has to be evaluated over a padded section
/// and the padding has to agree with what the neighbouring section computes in its interior.
[[nodiscard]] Status BuildBlurredGraph(Graph& graph, Domain domain, u32_t radius) {
    Graph::NoiseParams noise;
    noise.domain    = domain;
    noise.frequency = 0.02f;
    noise.octaves   = 3;
    noise.amplitude = 1.0f;
    Result<Value> value = graph.AddNoise(noise, At(2));
    if (!value) {
        return std::unexpected(value.error());
    }
    Result<Value> blurred = graph.AddBlur(*value, radius, At(3));
    if (!blurred) {
        return std::unexpected(blurred.error());
    }
    return graph.RequestOutput("height", *blurred, -1.0f, 1.0f);
}

/// Noise, a slope mask over its analytic gradient, and the two blended. Several dispatches with
/// barriers between them, so the test also covers the case where a seam could come from a buffer
/// being reused across sections without being fully rewritten.
[[nodiscard]] Status BuildBlendGraph(Graph& graph) {
    Graph::NoiseParams noise;
    noise.domain    = Domain::R2;
    noise.frequency = 0.008f;
    noise.octaves   = 4;
    noise.amplitude = 1.0f;
    Result<Value> value = graph.AddNoise(noise, At(4));
    if (!value) {
        return std::unexpected(value.error());
    }
    Result<Value> gradient = graph.Channel(*value, 1);
    if (!gradient) {
        return std::unexpected(gradient.error());
    }
    Result<Value> mask = graph.AddSlopeMask(*gradient, 0.1f, 0.5f, At(5));
    if (!mask) {
        return std::unexpected(mask.error());
    }
    Result<Value> other = graph.AddCurve(CurveOp::Power, *value, 2.0f, 0.0f, At(6));
    if (!other) {
        return std::unexpected(other.error());
    }
    Result<Value> blended = graph.AddBlend(*value, *other, *mask, At(7));
    if (!blended) {
        return std::unexpected(blended.error());
    }
    return graph.RequestOutput("height", *blended, -1.0f, 1.0f);
}

/// Noise run through thermal erosion. The case that matters most: an iterative op accumulates one
/// cell of influence per iteration, so its halo is its iteration count and every one of those extra
/// samples has to agree with what the neighbouring section computes in its own interior. An
/// off-by-one in the state halo shows up here and nowhere else.
[[nodiscard]] Status BuildErodedGraph(Graph& graph, Domain domain, u32_t iterations) {
    Graph::NoiseParams noise;
    noise.domain    = domain;
    noise.frequency = 0.03f;
    noise.octaves   = 3;
    noise.amplitude = 1.0f;
    Result<Value> value = graph.AddNoise(noise, At(8));
    if (!value) {
        return std::unexpected(value.error());
    }
    Graph::ThermalParams thermal;
    thermal.iterations = iterations;
    thermal.talus      = 0.01f;
    thermal.strength   = 0.4f;
    Result<Value> eroded = graph.AddThermalErosion(*value, thermal, At(9));
    if (!eroded) {
        return std::unexpected(eroded.error());
    }
    return graph.RequestOutput("height", *eroded, -1.0f, 1.0f);
}

/// Noise run through hydraulic erosion. Harder than the thermal case in one specific way: the state
/// the iterations exchange is three components wide while the node produces one, so the first and last
/// iterations read and write different shapes from the ones in between. A mistake in that indexing
/// would most likely show up as a seam rather than as garbage.
[[nodiscard]] Status BuildHydraulicGraph(Graph& graph, u32_t iterations) {
    Graph::NoiseParams noise;
    noise.frequency = 0.03f;
    noise.octaves   = 3;
    noise.amplitude = 1.0f;
    Result<Value> value = graph.AddNoise(noise, At(10));
    if (!value) {
        return std::unexpected(value.error());
    }
    Graph::HydraulicParams hydraulic;
    hydraulic.iterations = iterations;
    Result<Value> eroded = graph.AddHydraulicErosion(*value, hydraulic, At(11));
    if (!eroded) {
        return std::unexpected(eroded.error());
    }
    return graph.RequestOutput("height", *eroded, -1.0f, 1.0f);
}

/// Runs `graph` whole and tiled over the same region and demands the same bytes.
void CheckSeams(Context& context, KernelLibrary& kernels, const char* label, const Graph& graph,
                SectionExtent whole, SectionExtent tiled) {
    const u32_t width  = whole.x;
    const u32_t depth  = whole.y;
    const u32_t height = whole.z;

    Result<Field> single = Evaluate(context, kernels, graph, whole, width, depth, height, 1.0f, kOrigin, kSeed);
    REQUIRE_OK_VOID(single);
    Result<Field> split = Evaluate(context, kernels, graph, tiled, width, depth, height, 1.0f, kOrigin, kSeed);
    REQUIRE_OK_VOID(split);

    const u8_t components =
        static_cast<u8_t>(single->samples.size() / (static_cast<u64_t>(width) * depth * height));
    Compare(label, *single, *split, components);
}

/// Each check builds its graph in its own frame.
///
/// Not a matter of taste: a `Graph` keeps fixed-capacity storage for 1024 nodes, so it is around 150 KiB,
/// and nine of them in one function overflowed the default 1 MiB stack in Debug. One graph per frame,
/// and a loop reuses its one slot.
void CheckNoiseSeams(Context& context, KernelLibrary& kernels) {
    Graph noise;
    REQUIRE_OK_VOID(BuildNoiseGraph(noise, Domain::R2));
    CheckSeams(context, kernels, "noise 128x128 against 4x4 of 32", noise,
               SectionExtent{.x = 128, .y = 1, .z = 128}, SectionExtent{.x = 32, .y = 1, .z = 32});
}

/// A region that is not a whole number of sections, so every edge section has overhang that must be
/// discarded rather than written.
void CheckPartialSectionSeams(Context& context, KernelLibrary& kernels) {
    Graph noise;
    REQUIRE_OK_VOID(BuildNoiseGraph(noise, Domain::R2));
    Result<Field> single = Evaluate(context, kernels, noise,
                                   SectionExtent{.x = 128, .y = 1, .z = 128}, 100, 1, 70, 1.0f,
                                   kOrigin, kSeed);
    REQUIRE_OK_VOID(single);
    Result<Field> split = Evaluate(context, kernels, noise, SectionExtent{.x = 32, .y = 1, .z = 32},
                                   100, 1, 70, 1.0f, kOrigin, kSeed);
    REQUIRE_OK_VOID(split);
    Compare("noise 100x70 against 32-wide sections", *single, *split, 1);
}

void CheckBlurSeams(Context& context, KernelLibrary& kernels) {
    for (const u32_t radius : {1u, 3u, 8u}) {
        Graph blurred;
        REQUIRE_OK_VOID(BuildBlurredGraph(blurred, Domain::R2, radius));
        char label[96];
        std::snprintf(label, sizeof(label), "blur radius %u, 128x128 against 4x4 of 32", radius);
        CheckSeams(context, kernels, label, blurred, SectionExtent{.x = 128, .y = 1, .z = 128},
                   SectionExtent{.x = 32, .y = 1, .z = 32});
    }
}

void CheckBlendSeams(Context& context, KernelLibrary& kernels) {
    Graph blended;
    REQUIRE_OK_VOID(BuildBlendGraph(blended));
    CheckSeams(context, kernels, "noise + slope mask + blend", blended,
               SectionExtent{.x = 128, .y = 1, .z = 128}, SectionExtent{.x = 32, .y = 1, .z = 32});
}

void CheckVolumeSeams(Context& context, KernelLibrary& kernels) {
    Graph volume;
    REQUIRE_OK_VOID(BuildNoiseGraph(volume, Domain::R3));
    CheckSeams(context, kernels, "R3 noise 32^3 against 4x4x4 of 8", volume,
               SectionExtent{.x = 32, .y = 32, .z = 32}, SectionExtent{.x = 8, .y = 8, .z = 8});
}

void CheckVolumeBlurSeams(Context& context, KernelLibrary& kernels) {
    Graph blurredVolume;
    REQUIRE_OK_VOID(BuildBlurredGraph(blurredVolume, Domain::R3, 2));
    CheckSeams(context, kernels, "R3 blur radius 2, 32^3 against 4x4x4 of 8", blurredVolume,
               SectionExtent{.x = 32, .y = 32, .z = 32}, SectionExtent{.x = 8, .y = 8, .z = 8});
}

/// One, two and three iterations cover every shape the ping-pong takes: no scratch buffer, one, and the
/// alternating pair. Larger counts then check that the state halo keeps up.
void CheckThermalSeams(Context& context, KernelLibrary& kernels) {
    for (const u32_t iterations : {1u, 2u, 3u, 8u, 24u}) {
        Graph eroded;
        REQUIRE_OK_VOID(BuildErodedGraph(eroded, Domain::R2, iterations));
        char label[112];
        std::snprintf(label, sizeof(label),
                      "thermal erosion, %u iteration(s), 128x128 against 4x4 of 32", iterations);
        CheckSeams(context, kernels, label, eroded, SectionExtent{.x = 128, .y = 1, .z = 128},
                   SectionExtent{.x = 32, .y = 1, .z = 32});
    }
}

void CheckThermalVolumeSeams(Context& context, KernelLibrary& kernels) {
    Graph erodedVolume;
    REQUIRE_OK_VOID(BuildErodedGraph(erodedVolume, Domain::R3, 4));
    CheckSeams(context, kernels, "R3 thermal erosion, 4 iterations, 32^3 against 4x4x4 of 8",
               erodedVolume, SectionExtent{.x = 32, .y = 32, .z = 32},
               SectionExtent{.x = 8, .y = 8, .z = 8});
}

void CheckHydraulicSeams(Context& context, KernelLibrary& kernels) {
    for (const u32_t iterations : {1u, 2u, 3u, 12u}) {
        Graph eroded;
        REQUIRE_OK_VOID(BuildHydraulicGraph(eroded, iterations));
        char label[112];
        std::snprintf(label, sizeof(label),
                      "hydraulic erosion, %u iteration(s), 128x128 against 4x4 of 32", iterations);
        CheckSeams(context, kernels, label, eroded, SectionExtent{.x = 128, .y = 1, .z = 128},
                   SectionExtent{.x = 32, .y = 1, .z = 32});
    }
}

/// Two radius-carrying ops in series, neighbourhood and iterative.
///
/// The gap this fills: every other case here is one such op on its own, and one op on its own never
/// shares a buffer slot with anything. Chained, the planner hands the second op a slot the first one
/// had already finished with — and a reused slot keeps the `mapping`, `halo` and `bytes` of the value
/// it was *created* for, so reading an output back with `PlannedBuffer::halo` strided it by a halo
/// that belonged to some unrelated earlier value. Both the stride and which slots get reused depend on
/// the section size, which is exactly what a seam check varies, so one 128-sample section disagreed with
/// 4x4 sections of 32 over 13800 of 16384 samples, deterministically. `CompiledOutput::halo` now carries
/// the halo of the dispatch that wrote the value.
///
/// The height is requested first so that it is what gets compared; the gradient variants give the
/// chain a nonzero halo at its own output, which is what makes every radius below it stack.
void CheckChainedOpSeams(Context& context, KernelLibrary& kernels) {
    struct Variant {
        const char* label;
        u32_t       first;
        u32_t       second;
        b8_t        gradient;
    };
    const Variant blurs[] = {
        {"blur(1) then blur(1)", 1, 1, false},
        {"blur(3) then blur(3)", 3, 3, false},
        {"blur(3) then blur(3), gradient on top", 3, 3, true},
        {"blur(2) then blur(7), gradient on top", 2, 7, true},
    };
    for (const Variant& variant : blurs) {
        Graph              graph;
        Graph::NoiseParams noise;
        noise.frequency = 0.02f;
        noise.octaves   = 3;
        noise.amplitude = 1.0f;
        Result<Value> value = graph.AddNoise(noise, At(12));
        REQUIRE_OK_VOID(value);

        Result<Value> once = graph.AddBlur(*value, variant.first, At(18));
        REQUIRE_OK_VOID(once);
        Result<Value> twice = graph.AddBlur(*once, variant.second, At(19));
        REQUIRE_OK_VOID(twice);

        REQUIRE_OK_VOID(graph.RequestOutput("height", *twice, -1.0f, 1.0f));
        if (variant.gradient) {
            Result<Value> gradient = graph.AddGradient(*twice, At(15));
            REQUIRE_OK_VOID(gradient);
            REQUIRE_OK_VOID(graph.RequestOutput("gradient", *gradient, -1.0f, 1.0f));
        }

        CheckSeams(context, kernels, variant.label, graph,
                   SectionExtent{.x = 128, .y = 1, .z = 128},
                   SectionExtent{.x = 32, .y = 1, .z = 32});
    }

    // Iterative chains: a thermal pass over a blur, over another thermal pass, and over hydraulic
    // erosion, which is the one producer with more than one channel.
    struct Chain {
        const char* label;
        enum { Blur, Thermal, Hydraulic } producer;
        u32_t iterations;
    };
    const Chain chains[] = {
        {"blur(5) then thermal(5)", Chain::Blur, 5},
        {"thermal(5) then thermal(1)", Chain::Thermal, 1},
        {"thermal(5) then thermal(5)", Chain::Thermal, 5},
        {"hydraulic(6) then thermal(5)", Chain::Hydraulic, 5},
    };
    for (const Chain& chain : chains) {
        Graph              graph;
        Graph::NoiseParams noise;
        noise.frequency = 0.02f;
        noise.octaves   = 3;
        noise.amplitude = 1.0f;
        Result<Value> value = graph.AddNoise(noise, At(12));
        REQUIRE_OK_VOID(value);

        Graph::ThermalParams thermal;
        thermal.iterations = 5;
        thermal.talus      = 0.01f;
        thermal.strength   = 0.4f;

        Result<Value> producer = value;
        if (chain.producer == Chain::Blur) {
            producer = graph.AddBlur(*value, 5, At(18));
        } else if (chain.producer == Chain::Thermal) {
            producer = graph.AddThermalErosion(*value, thermal, At(16));
        } else {
            Graph::HydraulicParams hydraulic;
            hydraulic.iterations = 6;
            producer             = graph.AddHydraulicErosion(*value, hydraulic, At(20));
        }
        REQUIRE_OK_VOID(producer);

        Graph::ThermalParams consumer = thermal;
        consumer.iterations           = chain.iterations;
        Result<Value> settled         = graph.AddThermalErosion(*producer, consumer, At(17));
        REQUIRE_OK_VOID(settled);

        REQUIRE_OK_VOID(graph.RequestOutput("height", *settled, -1.0f, 1.0f));
        Result<Value> gradient = graph.AddGradient(*settled, At(15));
        REQUIRE_OK_VOID(gradient);
        REQUIRE_OK_VOID(graph.RequestOutput("gradient", *gradient, -1.0f, 1.0f));

        CheckSeams(context, kernels, chain.label, graph,
                   SectionExtent{.x = 128, .y = 1, .z = 128},
                   SectionExtent{.x = 32, .y = 1, .z = 32});
    }
}

void CheckDeterminism(Context& context, KernelLibrary& kernels) {
    Graph blended;
    REQUIRE_OK_VOID(BuildBlendGraph(blended));
    const SectionExtent extent{.x = 64, .y = 1, .z = 64};
    Result<Field>       first =
        Evaluate(context, kernels, blended, extent, 128, 1, 128, 1.0f, kOrigin, kSeed);
    REQUIRE_OK_VOID(first);
    Result<Field> second =
        Evaluate(context, kernels, blended, extent, 128, 1, 128, 1.0f, kOrigin, kSeed);
    REQUIRE_OK_VOID(second);
    Compare("two runs of the same graph", *first, *second, 1);
}

} // namespace

int main() {
    const Result<Application*> application =
        engine::init(AppInfo{.mode = RunMode::Headless, .name = "test_seams"});
    if (!application) {
        std::printf("FAIL engine::init: %s\n", application.error().Format().data());
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        return 1;
    }
    Context&       context = VulkanContext(**application);
    KernelLibrary& kernels = KernelsOf(**application);

    const int result = [&]() -> int {
        test::Section("R2 tiles agree with one section");
        CheckNoiseSeams(context, kernels);
        CheckPartialSectionSeams(context, kernels);

        test::Section("a halo does not create a seam");
        CheckBlurSeams(context, kernels);

        test::Section("a multi-dispatch graph does not create a seam");
        CheckBlendSeams(context, kernels);

        test::Section("R3 bricks agree with one brick");
        CheckVolumeSeams(context, kernels);
        CheckVolumeBlurSeams(context, kernels);

        test::Section("an iterative op does not create a seam");
        CheckThermalSeams(context, kernels);
        CheckThermalVolumeSeams(context, kernels);
        CheckHydraulicSeams(context, kernels);
        CheckChainedOpSeams(context, kernels);

        test::Section("the same evaluation twice gives the same bytes");
        CheckDeterminism(context, kernels);

        test::CheckNoValidationErrors();
        return 0;
    }();

    const Status closed = engine::shutdown(*application);
    if (!closed) {
        std::printf("FAIL engine::shutdown: %s\n", closed.error().Format().data());
        return 1;
    }
    return result != 0 ? result : test::Summary("test_seams");
}
