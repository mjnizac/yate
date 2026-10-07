// What erosion must do, as physical invariants rather than as a reference image (milestone 8).
//
// An iterative op has no CPU reference: running the whole scheme on the host would be a second
// implementation to keep in step, and a golden image only says "the same as last time", not "right".
// What *is* checkable is the set of properties the scheme is built on, each of which a plausible bug
// breaks:
//
//  - It changes the field. A kernel that read and wrote the same buffer index, or whose ping-pong
//    parity was wrong, would hand back the input untouched and every other check would still pass.
//  - It conserves material, in the only form that is testable here. Thermal erosion moves height
//    between neighbours rather than creating or destroying it, but what the test can read back is a
//    *window* of a larger computation, and material genuinely crosses that window's border. So the
//    total is not expected to hold; what is expected is that the change in the total comes from the
//    border and nowhere else. That has a falsifiable consequence: the drift is a perimeter effect, so
//    it must shrink like 1/L as the window grows, while a per-sample leak from a sign error or a
//    double-counted gift would stay the same fraction at any size. Measured at 64 and at 128.
//  - It reduces slope. That is what erosion *means*, and it is the direction a flipped sign would
//    reverse: the same arithmetic with `given` and `received` swapped would sharpen the terrain, pass
//    the conservation check, and look like erosion only until you measured it.
//  - It settles. More iterations may not undo the smoothing of fewer, and nothing may blow up.
//  - A talus above the steepest slope present does nothing at all, exactly.
//
// Seam behaviour is in test_seams, which is where the halo arithmetic is pinned.

#include "gpu_support.hpp"

#include <engine/application.hpp>
#include <engine/engine.hpp>
#include <engine/terrain/graph.hpp>

#include <cmath>
#include <cstdio>

using namespace engine;
using namespace engine::vulkan;
using namespace engine::terrain;
using test::Evaluate;
using test::Field;

namespace {

constexpr u32_t                kExtent = 64;
constexpr u32_t                kSeed   = 0xE1051011u;
constexpr std::array<i32_t, 3> kOrigin{311, 0, -907};

[[nodiscard]] SourceLocation At(u32_t line) { return SourceLocation{"test_erosion", line}; }

/// Noise, optionally eroded. `iterations == 0` means the bare noise, which is the baseline every
/// measurement below is taken against.
[[nodiscard]] Status BuildGraph(Graph& graph, u32_t iterations, f32_t talus, f32_t strength) {
    Graph::NoiseParams noise;
    noise.frequency = 0.04f;
    noise.octaves   = 4;
    noise.amplitude = 1.0f;
    Result<Value> value = graph.AddNoise(noise, At(1));
    if (!value) {
        return std::unexpected(value.error());
    }
    if (iterations == 0) {
        return graph.RequestOutput("height", *value, -1.0f, 1.0f);
    }

    Graph::ThermalParams thermal;
    thermal.iterations = iterations;
    thermal.talus      = talus;
    thermal.strength   = strength;
    Result<Value> eroded = graph.AddThermalErosion(*value, thermal, At(2));
    if (!eroded) {
        return std::unexpected(eroded.error());
    }
    return graph.RequestOutput("height", *eroded, -1.0f, 1.0f);
}

[[nodiscard]] Result<Field> RunAt(Context& context, KernelLibrary& kernels, u32_t iterations,
                                  f32_t talus, f32_t strength, u32_t side) {
    Graph graph;
    if (Status built = BuildGraph(graph, iterations, talus, strength); !built) {
        return std::unexpected(built.error());
    }
    return Evaluate(context, kernels, graph, SectionExtent{.x = side, .y = 1, .z = side}, side, 1,
                    side, 1.0f, kOrigin, kSeed);
}

[[nodiscard]] Result<Field> Run(Context& context, KernelLibrary& kernels, u32_t iterations,
                                f32_t talus, f32_t strength) {
    return RunAt(context, kernels, iterations, talus, strength, kExtent);
}

/// Sum of every sample, in f64 so the comparison measures the scheme and not the accumulation.
[[nodiscard]] f64_t Total(const Field& field) {
    f64_t total = 0.0;
    for (const f32_t sample : field.samples) {
        total += static_cast<f64_t>(sample);
    }
    return total;
}

/// Largest height difference between horizontally or vertically adjacent samples.
[[nodiscard]] f64_t MaximumSlope(const Field& field) {
    f64_t worst = 0.0;
    for (u32_t z = 0; z < field.height; ++z) {
        for (u32_t x = 0; x < field.width; ++x) {
            const f64_t centre =
                static_cast<f64_t>(field.samples[static_cast<usize_t>(z) * field.width + x]);
            if (x + 1 < field.width) {
                const f64_t right =
                    static_cast<f64_t>(field.samples[static_cast<usize_t>(z) * field.width + x + 1]);
                worst = std::max(worst, std::fabs(centre - right));
            }
            if (z + 1 < field.height) {
                const f64_t below = static_cast<f64_t>(
                    field.samples[static_cast<usize_t>(z + 1) * field.width + x]);
                worst = std::max(worst, std::fabs(centre - below));
            }
        }
    }
    return worst;
}

/// Sum of absolute values: a stable scale to measure drift against, unlike the signed total, which is
/// a sum of symmetric noise and sits near zero.
[[nodiscard]] f64_t Magnitude(const Field& field) {
    f64_t total = 0.0;
    for (const f32_t sample : field.samples) {
        total += std::fabs(static_cast<f64_t>(sample));
    }
    return total;
}

/// Mean absolute difference, which is how "did anything change" is measured.
[[nodiscard]] f64_t MeanDifference(const Field& a, const Field& b) {
    f64_t sum = 0.0;
    for (usize_t i = 0; i < a.samples.size(); ++i) {
        sum += std::fabs(static_cast<f64_t>(a.samples[i]) - static_cast<f64_t>(b.samples[i]));
    }
    return a.samples.empty() ? 0.0 : sum / static_cast<f64_t>(a.samples.size());
}

/// How much of the field's magnitude the total moved by, which for a conservative scheme is a measure
/// of the flux across the window border.
[[nodiscard]] Result<f64_t> BorderDrift(Context& context, KernelLibrary& kernels, u32_t iterations,
                                        u32_t side) {
    Result<Field> bare = RunAt(context, kernels, 0, 0.0f, 0.0f, side);
    if (!bare) {
        return std::unexpected(bare.error());
    }
    Result<Field> eroded = RunAt(context, kernels, iterations, 0.01f, 0.4f, side);
    if (!eroded) {
        return std::unexpected(eroded.error());
    }
    return std::fabs(Total(*eroded) - Total(*bare)) / Magnitude(*bare);
}

[[nodiscard]] b8_t AnyNotFinite(const Field& field) {
    for (const f32_t sample : field.samples) {
        if (!std::isfinite(sample)) {
            return true;
        }
    }
    return false;
}

/// The checks below each build a `Graph`, which keeps fixed-capacity storage for 1024 nodes and so is
/// around 150 KiB. Six of them on one frame overflowed the default 1 MiB stack in Debug, so each one gets
/// its own frame. `graph.hpp` has a static_assert pinning the size for the same reason.

void CheckThermalBuilderRefusals(Context& context, KernelLibrary& kernels) {
    (void)context;
    (void)kernels;

            Graph                graph;
            Result<Value>        value = graph.AddNoise(Graph::NoiseParams{}, At(3));
            REQUIRE_OK_VOID(value);
            Graph::ThermalParams params;

            // The iteration count is the halo, and the halo is one byte per slot.
            params.iterations = kMaxHalo + 1;
            CHECK(!graph.AddThermalErosion(*value, params, At(4)).has_value());
            params.iterations = 0;
            CHECK(!graph.AddThermalErosion(*value, params, At(5)).has_value());

            // Above one half the scheme oscillates instead of settling.
            params.iterations = 4;
            params.strength   = 0.75f;
            CHECK(!graph.AddThermalErosion(*value, params, At(6)).has_value());
            params.strength = 0.0f;
            CHECK(!graph.AddThermalErosion(*value, params, At(7)).has_value());

            // A vector field is not a height field.
            params.strength            = 0.25f;
            Result<Value> gradient     = graph.Channel(*value, 1);
            REQUIRE_OK_VOID(gradient);
            Result<Value> wrongMapping = graph.AddThermalErosion(*gradient, params, At(8));
            CHECK(!wrongMapping.has_value());
            if (!wrongMapping) {
                CHECK_EQ(wrongMapping.error().line, u32_t{8});
            }
        
}

void CheckChainedHaloRefused(Context& context, KernelLibrary& kernels) {
    (void)context;
    (void)kernels;

            // Two 200-iteration erosions in series need 400 samples of padding on every side, which the
            // one-byte halo cannot express. The compiler must say so rather than truncate.
            Graph                graph;
            Result<Value>        value = graph.AddNoise(Graph::NoiseParams{}, At(9));
            REQUIRE_OK_VOID(value);
            Graph::ThermalParams params;
            params.iterations = 200;
            Result<Value> once = graph.AddThermalErosion(*value, params, At(10));
            REQUIRE_OK_VOID(once);
            Result<Value> twice = graph.AddThermalErosion(*once, params, At(11));
            REQUIRE_OK_VOID(twice);
            REQUIRE_OK_VOID(graph.RequestOutput("height", *twice, -1.0f, 1.0f));

            Result<CompiledGraph> compiled = Compile(
                graph, CompileOptions{.extent = SectionExtent{.x = kExtent, .y = 1, .z = kExtent},
                                      .resolution = 1.0f});
            CHECK(!compiled.has_value());
            if (!compiled) {
                std::printf("     refused with: %s\n", compiled.error().Format().data());
                CHECK_EQ(compiled.error().line, u32_t{9});
            }
        
}

void CheckIterativeBufferPlan(Context& context, KernelLibrary& kernels) {
    (void)context;
    (void)kernels;

            Graph graph;
            REQUIRE_OK_VOID(BuildGraph(graph, 64, 0.01f, 0.4f));
            Result<CompiledGraph> compiled = Compile(
                graph, CompileOptions{.extent = SectionExtent{.x = kExtent, .y = 1, .z = kExtent},
                                      .resolution = 1.0f});
            REQUIRE_OK_VOID(compiled);
            // Noise and erosion, whatever the iteration count.
            CHECK_EQ(compiled->stats.dispatchCount, u32_t{2});
            const Dispatch& erosion = compiled->dispatches[1];
            CHECK_EQ(erosion.iterations, u32_t{64});
            CHECK_EQ(erosion.scratchCount, u8_t{2});
            // The state is computed over the dispatch halo plus the influence radius, which is what
            // makes the result exact out to the dispatch halo after the last iteration.
            CHECK_EQ(erosion.stateHalo, erosion.halo + 64);
            // The noise feeding it was given the same halo, so the first iteration reads a field of the
            // same shape as the state it writes.
            CHECK_EQ(compiled->dispatches[0].halo, erosion.stateHalo);
            // Three values ever live at once: the noise, the output, and the two scratch halves, with
            // the noise buffer freed after the first iteration reads it.
            CHECK(compiled->stats.bufferCount <= 4);
        
}

void CheckHydraulicBufferPlan(Context& context, KernelLibrary& kernels) {
    (void)context;
    (void)kernels;

            Graph                  graph;
            Result<Value>          noise = graph.AddNoise(Graph::NoiseParams{}, At(20));
            REQUIRE_OK_VOID(noise);
            Graph::HydraulicParams hydraulic;
            hydraulic.iterations = 32;
            Result<Value> carvedValue = graph.AddHydraulicErosion(*noise, hydraulic, At(21));
            REQUIRE_OK_VOID(carvedValue);
            REQUIRE_OK_VOID(graph.RequestOutput("height", *carvedValue, -1.0f, 1.0f));

            Result<CompiledGraph> compiled = Compile(
                graph, CompileOptions{.extent = SectionExtent{.x = kExtent, .y = 1, .z = kExtent},
                                      .resolution = 1.0f});
            REQUIRE_OK_VOID(compiled);
            const Dispatch& dispatch = compiled->dispatches[1];
            // The state is three components wide while the node produces one, and the compiler has to
            // size the scratch pair from the state rather than from the output. Getting that wrong would
            // under-allocate by a factor of three.
            CHECK_EQ(dispatch.stateMapping.components, u8_t{3});
            CHECK_EQ(dispatch.channels[0].components, u8_t{1});
            CHECK_EQ(compiled->buffers[dispatch.scratchBuffers[0]].mapping.components, u8_t{3});
            CHECK_EQ(compiled->buffers[dispatch.outputBuffers[0]].mapping.components, u8_t{1});
            CHECK_EQ(dispatch.stateHalo, dispatch.halo + 32);
        
}

void CheckHydraulicBuilderRefusals(Context& context, KernelLibrary& kernels) {
    (void)context;
    (void)kernels;

            Graph                  graph;
            Result<Value>          noise = graph.AddNoise(Graph::NoiseParams{}, At(25));
            REQUIRE_OK_VOID(noise);
            Graph::HydraulicParams params;

            // Above a quarter the four outflows of a cell can exceed the water it has.
            params.flowRate = 0.4f;
            CHECK(!graph.AddHydraulicErosion(*noise, params, At(26)).has_value());
            params.flowRate = 0.15f;

            params.evaporation = 1.0f;
            CHECK(!graph.AddHydraulicErosion(*noise, params, At(27)).has_value());
            params.evaporation = 0.05f;

            params.deposition = 1.5f;
            CHECK(!graph.AddHydraulicErosion(*noise, params, At(28)).has_value());
            params.deposition = 0.3f;

            params.iterations = kMaxHalo + 1;
            Result<Value> tooMany = graph.AddHydraulicErosion(*noise, params, At(29));
            CHECK(!tooMany.has_value());
            if (!tooMany) {
                CHECK_EQ(tooMany.error().line, u32_t{29});
            }
        
}

} // namespace

int main() {
    const Result<Application*> application =
        engine::init(AppInfo{.mode = RunMode::Headless, .name = "test_erosion"});
    if (!application) {
        std::printf("FAIL engine::init: %s\n", application.error().Format().data());
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        return 1;
    }
    Context&       context = VulkanContext(**application);
    KernelLibrary& kernels = KernelsOf(**application);

    const int result = [&]() -> int {
        Result<Field> bare = Run(context, kernels, 0, 0.0f, 0.0f);
        REQUIRE_OK(bare);
        (void)Total(*bare);
        const f64_t bareSlope = MaximumSlope(*bare);
        std::printf("     baseline: maximum slope %.6f\n", bareSlope);
        CHECK(bareSlope > 0.0);

        test::Section("erosion changes the field and conserves material");
        Result<Field> eroded = Run(context, kernels, 16, 0.01f, 0.4f);
        REQUIRE_OK(eroded);
        CHECK(!AnyNotFinite(*eroded));

        const f64_t difference = MeanDifference(*bare, *eroded);
        std::printf("     16 iterations: mean change %.6g\n", difference);
        // Large enough that a no-op cannot pass, small enough that it is not asserting a particular
        // amount of erosion.
        CHECK(difference > 1e-4);

        // The window is not a closed system: material crosses its border, so the total moves. What
        // must hold is that the movement is a border effect, which means it shrinks like 1/L as the
        // window grows. A per-sample leak would not.
        Result<f64_t> drift64 = BorderDrift(context, kernels, 16, 64);
        REQUIRE_OK(drift64);
        Result<f64_t> drift128 = BorderDrift(context, kernels, 16, 128);
        REQUIRE_OK(drift128);
        std::printf("     border drift: 64 -> %.4g, 128 -> %.4g, ratio %.3f\n", *drift64, *drift128,
                    *drift128 / *drift64);
        // Halving is the prediction; 0.7 leaves room for the border not being perfectly uniform while
        // still failing anything that does not shrink with the perimeter.
        CHECK(*drift128 < *drift64 * 0.7);

        test::Section("erosion reduces slope, and more of it reduces it further");
        const f64_t erodedSlope = MaximumSlope(*eroded);
        std::printf("     maximum slope %.6f against %.6f\n", erodedSlope, bareSlope);
        CHECK(erodedSlope < bareSlope);

        Result<Field> little = Run(context, kernels, 2, 0.01f, 0.4f);
        REQUIRE_OK(little);
        Result<Field> lots = Run(context, kernels, 48, 0.01f, 0.4f);
        REQUIRE_OK(lots);
        CHECK(!AnyNotFinite(*lots));
        const f64_t littleSlope = MaximumSlope(*little);
        const f64_t lotsSlope   = MaximumSlope(*lots);
        std::printf("     slope by iterations: 2 -> %.6f, 16 -> %.6f, 48 -> %.6f\n", littleSlope,
                    erodedSlope, lotsSlope);
        CHECK(littleSlope <= bareSlope);
        CHECK(erodedSlope <= littleSlope);
        CHECK(lotsSlope <= erodedSlope);
        // The same 1/L law after three times as many iterations: the flux grows with the iteration
        // count, but it is still only a border effect.
        Result<f64_t> deep64 = BorderDrift(context, kernels, 48, 64);
        REQUIRE_OK(deep64);
        Result<f64_t> deep128 = BorderDrift(context, kernels, 48, 128);
        REQUIRE_OK(deep128);
        std::printf("     border drift at 48 iterations: 64 -> %.4g, 128 -> %.4g, ratio %.3f\n",
                    *deep64, *deep128, *deep128 / *deep64);
        CHECK(*deep128 < *deep64 * 0.7);
        CHECK(*deep64 > *drift64); // More iterations move more material across the border.

        test::Section("a talus above every slope present does nothing");
        // Nothing exceeds the talus, so no material qualifies to move and the output must be the input
        // bit for bit. This is the check that the threshold is a threshold and not a scale factor.
        Result<Field> untouched = Run(context, kernels, 32, 10.0f, 0.5f);
        REQUIRE_OK(untouched);
        u64_t differing = 0;
        for (usize_t i = 0; i < bare->samples.size(); ++i) {
            if (std::memcmp(&bare->samples[i], &untouched->samples[i], sizeof(f32_t)) != 0) {
                ++differing;
            }
        }
        CHECK_EQ(differing, u64_t{0});

        test::Section("the builder refuses what the interface cannot carry");
        CheckThermalBuilderRefusals(context, kernels);

        test::Section("chained iterative ops are refused when the halo would not fit");
        CheckChainedHaloRefused(context, kernels);

        test::Section("an iterative dispatch costs two buffers, not one per iteration");
        CheckIterativeBufferPlan(context, kernels);

        test::Section("hydraulic erosion carves and keeps the terrain finite");
        CheckHydraulicBufferPlan(context, kernels);

        // The same noise the thermal checks use, so the comparison is against `bare` and the slopes are
        // real: with the default low-frequency noise the steepest step is 0.016, and erosion driven by
        // slope has almost nothing to work with.
        Graph hydraulicGraph;
        {
            Graph::NoiseParams noise;
            noise.frequency     = 0.04f;
            noise.octaves       = 4;
            noise.amplitude     = 1.0f;
            Result<Value> value = hydraulicGraph.AddNoise(noise, At(22));
            REQUIRE_OK(value);
            Graph::HydraulicParams hydraulic;
            hydraulic.iterations = 32;
            Result<Value> carvedValue =
                hydraulicGraph.AddHydraulicErosion(*value, hydraulic, At(23));
            REQUIRE_OK(carvedValue);
            REQUIRE_OK(hydraulicGraph.RequestOutput("height", *carvedValue, -1.0f, 1.0f));
        }
        Result<Field> carved =
            Evaluate(context, kernels, hydraulicGraph,
                     SectionExtent{.x = kExtent, .y = 1, .z = kExtent}, kExtent, 1, kExtent, 1.0f,
                     kOrigin, kSeed);
        REQUIRE_OK(carved);
        CHECK(!AnyNotFinite(*carved));

        const f64_t carvedChange = MeanDifference(*bare, *carved);
        std::printf("     hydraulic, 32 iterations: mean change %.6g, maximum slope %.6f against "
                    "%.6f\n",
                    carvedChange, MaximumSlope(*carved), bareSlope);
        CHECK(carvedChange > 1e-4);
        // Unlike thermal erosion, hydraulic erosion is not expected to flatten: it cuts channels, which
        // can leave the steepest single step as steep as it was. What must not happen is a blow-up, so
        // the bound is on the magnitude rather than on the slope.
        CHECK(Magnitude(*carved) < Magnitude(*bare) * 4.0);

        test::Section("hydraulic erosion refuses an unstable configuration");
        CheckHydraulicBuilderRefusals(context, kernels);

        test::CheckNoValidationErrors();
        return 0;
    }();

    const Status closed = engine::shutdown(*application);
    if (!closed) {
        std::printf("FAIL engine::shutdown: %s\n", closed.error().Format().data());
        return 1;
    }
    return result != 0 ? result : test::Summary("test_erosion");
}
