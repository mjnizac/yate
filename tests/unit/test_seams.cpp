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
#include <engine/terrain/evaluator.hpp>
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

[[nodiscard]] SourceLocation At(u32_t line) { return SourceLocation{"test_seams", line}; }

/// A rectangular f32 field, indexed the way a section buffer is: x fastest, then z, then y.
struct Field {
    u32_t                   width  = 0;
    u32_t                   height = 0;
    u32_t                   depth  = 1;
    std::pmr::vector<f32_t> samples{&memory::General().Resource()};

    void Resize(u32_t w, u32_t d, u32_t h, u32_t components) {
        width  = w;
        depth  = d;
        height = h;
        samples.assign(static_cast<usize_t>(w) * d * h * components, 0.0f);
    }
};

/// Evaluates `graph` over `extent`-sized sections covering a `width x depth x height` region, and
/// stitches the interiors into one field.
///
/// Mirrors the exporter's loop: one submission per section, every section dispatched over its full
/// extent with only the useful interior kept, so an edge section computes exactly what an interior one
/// does.
[[nodiscard]] Result<Field> Evaluate(Context& context, KernelLibrary& kernels, const Graph& graph,
                                     SectionExtent extent, u32_t width, u32_t depth, u32_t height,
                                     f32_t resolution) {
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
    // Output buffers always have halo zero, because nothing reads them with a radius, so a section's
    // rows are exactly `extent` wide.
    const u64_t sectionBytes =
        ValueSize(output.mapping, extent, compiled->buffers[output.buffer].halo);

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
                    .origin = {kOriginX + static_cast<i32_t>(originX),
                               kOriginY + static_cast<i32_t>(originY),
                               kOriginZ + static_cast<i32_t>(originZ)},
                    .extent = extent,
                    .seed   = kSeed};
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
                if (Status waited =
                        queue.WaitTimeline(*submitted, test::kGpuTimeoutNanoseconds);
                    !waited) {
                    return std::unexpected(waited.error());
                }
                if (Status invalidated = context.Memory().InvalidateBuffer(
                        context.Readback().GetBuffer(), *readback, sectionBytes);
                    !invalidated) {
                    return std::unexpected(invalidated.error());
                }
                const auto* samples =
                    static_cast<const f32_t*>(context.Readback().MappedAt(*readback));

                // Copy only the part of the section that lies inside the region. An edge section
                // computes a full extent and the overhang is discarded, which is the behaviour the
                // seam test has to see through.
                const u32_t columns = std::min(extent.x, width - originX);
                const u32_t rows    = std::min(extent.z, height - originZ);
                const u32_t layers  = std::min(extent.y, depth - originY);
                for (u32_t layer = 0; layer < layers; ++layer) {
                    for (u32_t row = 0; row < rows; ++row) {
                        const u64_t source =
                            ((static_cast<u64_t>(layer) * extent.z + row) * extent.x) * components;
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

/// Runs `graph` whole and tiled over the same region and demands the same bytes.
void CheckSeams(Context& context, KernelLibrary& kernels, const char* label, const Graph& graph,
                SectionExtent whole, SectionExtent tiled) {
    const u32_t width  = whole.x;
    const u32_t depth  = whole.y;
    const u32_t height = whole.z;

    Result<Field> single = Evaluate(context, kernels, graph, whole, width, depth, height, 1.0f);
    REQUIRE_OK_VOID(single);
    Result<Field> split = Evaluate(context, kernels, graph, tiled, width, depth, height, 1.0f);
    REQUIRE_OK_VOID(split);

    const u8_t components =
        static_cast<u8_t>(single->samples.size() / (static_cast<u64_t>(width) * depth * height));
    Compare(label, *single, *split, components);
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
        {
            Graph noise;
            REQUIRE_OK(BuildNoiseGraph(noise, Domain::R2));
            CheckSeams(context, kernels, "noise 128x128 against 4x4 of 32", noise,
                       SectionExtent{.x = 128, .y = 1, .z = 128},
                       SectionExtent{.x = 32, .y = 1, .z = 32});

            // A region that is not a whole number of sections, so every edge section has overhang
            // that must be discarded rather than written.
            Result<Field> single = Evaluate(context, kernels, noise,
                                            SectionExtent{.x = 128, .y = 1, .z = 128}, 100, 1, 70,
                                            1.0f);
            REQUIRE_OK(single);
            Result<Field> split = Evaluate(context, kernels, noise,
                                           SectionExtent{.x = 32, .y = 1, .z = 32}, 100, 1, 70,
                                           1.0f);
            REQUIRE_OK(split);
            Compare("noise 100x70 against 32-wide sections", *single, *split, 1);
        }

        test::Section("a halo does not create a seam");
        for (const u32_t radius : {1u, 3u, 8u}) {
            Graph blurred;
            REQUIRE_OK(BuildBlurredGraph(blurred, Domain::R2, radius));
            char label[96];
            std::snprintf(label, sizeof(label), "blur radius %u, 128x128 against 4x4 of 32", radius);
            CheckSeams(context, kernels, label, blurred,
                       SectionExtent{.x = 128, .y = 1, .z = 128},
                       SectionExtent{.x = 32, .y = 1, .z = 32});
        }

        test::Section("a multi-dispatch graph does not create a seam");
        {
            Graph blended;
            REQUIRE_OK(BuildBlendGraph(blended));
            CheckSeams(context, kernels, "noise + slope mask + blend", blended,
                       SectionExtent{.x = 128, .y = 1, .z = 128},
                       SectionExtent{.x = 32, .y = 1, .z = 32});
        }

        test::Section("R3 bricks agree with one brick");
        {
            Graph volume;
            REQUIRE_OK(BuildNoiseGraph(volume, Domain::R3));
            CheckSeams(context, kernels, "R3 noise 32^3 against 4x4x4 of 8", volume,
                       SectionExtent{.x = 32, .y = 32, .z = 32},
                       SectionExtent{.x = 8, .y = 8, .z = 8});

            Graph blurredVolume;
            REQUIRE_OK(BuildBlurredGraph(blurredVolume, Domain::R3, 2));
            CheckSeams(context, kernels, "R3 blur radius 2, 32^3 against 4x4x4 of 8",
                       blurredVolume, SectionExtent{.x = 32, .y = 32, .z = 32},
                       SectionExtent{.x = 8, .y = 8, .z = 8});
        }

        test::Section("the same evaluation twice gives the same bytes");
        {
            Graph blended;
            REQUIRE_OK(BuildBlendGraph(blended));
            const SectionExtent extent{.x = 64, .y = 1, .z = 64};
            Result<Field> first = Evaluate(context, kernels, blended, extent, 128, 1, 128, 1.0f);
            REQUIRE_OK(first);
            Result<Field> second = Evaluate(context, kernels, blended, extent, 128, 1, 128, 1.0f);
            REQUIRE_OK(second);
            Compare("two runs of the same graph", *first, *second, 1);
        }

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
