#include <engine/render/tiles.hpp>

#include <engine/log.hpp>
#include <engine/lua.hpp>
#include <engine/terrain/export.hpp>
#include <engine/vulkan/pipeline.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

using terrain::Domain;
using terrain::Graph;
using terrain::Mapping;
using terrain::SectionExtent;
using terrain::Value;

constexpr u64_t kGpuTimeoutNanoseconds = 5ull * 1000 * 1000 * 1000;

/// Most dispatches recorded into one command buffer before it is submitted and waited on.
///
/// Each dispatch opens a Tracy GPU zone, two timestamps in a pool of 64K that Tracy only recycles from
/// results the GPU has finished. A whole ring recorded at once wrapped the pool onto timestamps nobody had
/// reset, and Tracy's collection then wrote twice its result buffer: a heap overrun that crashed the
/// viewer on shutdown. This keeps what is outstanding at a chunk plus a tile, far inside both bounds.
constexpr usize_t kMaxDispatchesPerSubmit = 4096;

/// Largest tile side the preview accepts.
///
/// Set by the row-by-row interior copy: one `VkBufferCopy` per row, in a fixed-size array so building a
/// preview allocates nothing. 1024 samples at 2 m is a two-kilometre tile, which is well past what a
/// preview wants to evaluate in one go anyway.
constexpr u32_t kMaxPreviewSamples = 1024;

/// The water outputs a script may give, in the order the tile's height slot carries them after the height.
constexpr std::array<std::string_view, 3> kWaterOutputs{"lakes", "rivers", "sea"};
/// What a tile holds where its script has no water output: far under any terrain, so nothing is drawn.
/// The bits of -1e6f, for `vkCmdFillBuffer`.
constexpr u32_t kNoWaterBits = 0xC9742400u;

/// Finds the height output and makes sure a normals output exists.
///
/// A script written for the exporter may produce only a height. Rather than refusing it, the viewer
/// appends the two nodes it needs: a measured gradient and the normals from it. That is the one place in
/// the engine where a gradient is taken by finite differences, and the viewer is the right consumer for
/// it, because the field it is looking at has already been through blends and erosion and has no closed
/// form left.
[[nodiscard]] Status ResolveOutputs(LoadedGraph& loaded) {
    Graph&  graph        = loaded.graph;
    b8_t    foundHeight  = false;
    b8_t    foundNormals = false;

    for (usize_t i = 0; i < graph.OutputCount(); ++i) {
        const terrain::OutputRequest& output = graph.Output(i);
        const std::string_view        name{output.name.data()};
        // The output named "height" wins; failing that the first R2->R1 that is not the shadow or water.
        const b8_t named = name == "height";
        const auto plane = std::find(kWaterOutputs.begin(), kWaterOutputs.end(), name);
        const b8_t water = plane != kWaterOutputs.end();
        if (water && output.value.mapping == Mapping{Domain::R2, 1}) {
            loaded.waterOutputs[static_cast<usize_t>(plane - kWaterOutputs.begin())] = i;
        }
        if ((named || (!foundHeight && name != "shadow" && !water))
            && output.value.mapping == Mapping{Domain::R2, 1}) {
            loaded.heightOutput  = i;
            loaded.heightMinimum = output.rangeMin;
            loaded.heightMaximum = output.rangeMax;
            foundHeight          = true;
        }
        if (name == "normals" && output.value.mapping == Mapping{Domain::R2, 3}) {
            loaded.normalsOutput = i;
            foundNormals         = true;
        }
    }

    if (!foundHeight) {
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Script,
                    "the viewer needs an R2->R1 height output; this script produces none");
    }

    if (!foundNormals) {
        const Value height = graph.Output(loaded.heightOutput).value;
        Result<Value> gradient = graph.AddGradient(height, terrain::SourceLocation{"<viewer>", 0});
        if (!gradient) {
            return std::unexpected(gradient.error());
        }
        Result<Value> normals =
            graph.AddNormals(*gradient, 1.0f, terrain::SourceLocation{"<viewer>", 0});
        if (!normals) {
            return std::unexpected(normals.error());
        }
        loaded.normalsOutput = graph.OutputCount();
        if (Status requested = graph.RequestOutput("normals", *normals, -1.0f, 1.0f); !requested) {
            return requested;
        }
        // `RequestOutput` keeps outputs in the order they were requested, so the height index is still
        // valid; only a new entry was appended.
    }

    // A script may say where the sun does not reach, as an R2->R1 output named "shadow" in [0, 1]. The
    // viewer has no room left in its push constants for another buffer, so the shadow travels in the
    // length of the normal: the normals the viewer reads are scaled to `0.25 + 0.75 * shadow`, and
    // terrain.frag.glsl reads the length back before normalising. Exports are untouched.
    for (usize_t i = 0; i < graph.OutputCount(); ++i) {
        const terrain::OutputRequest& output = graph.Output(i);
        if (std::string_view{output.name.data()} != "shadow"
            || !(output.value.mapping == Mapping{Domain::R2, 1})) {
            continue;
        }
        const terrain::SourceLocation here{"<viewer>", 0};
        Result<Value> quarter = graph.AddConst(Mapping{Domain::R2, 1}, {0.25f, 0, 0, 0}, here);
        Result<Value> scale   = graph.AddConst(Mapping{Domain::R2, 1}, {0.75f, 0, 0, 0}, here);
        if (!quarter || !scale) {
            return std::unexpected((quarter ? scale : quarter).error());
        }
        Result<Value> scaled = graph.AddArith(terrain::ArithOp::Multiply, output.value, *scale, here);
        if (!scaled) {
            return std::unexpected(scaled.error());
        }
        Result<Value> factor = graph.AddArith(terrain::ArithOp::Add, *scaled, *quarter, here);
        if (!factor) {
            return std::unexpected(factor.error());
        }
        Result<Value> lit = graph.AddArith(terrain::ArithOp::Multiply,
                                           graph.Output(loaded.normalsOutput).value, *factor, here);
        if (!lit) {
            return std::unexpected(lit.error());
        }
        loaded.normalsOutput = graph.OutputCount();
        if (Status requested = graph.RequestOutput("normals_lit", *lit, -1.0f, 1.0f); !requested) {
            return requested;
        }
        break;
    }
    return {};
}

/// Records one tile into `commands`, in freshly acquired slots: its section, then the copies of its
/// outputs into the slots, then a barrier, so a next tile recorded after it may reuse the evaluator's
/// buffers. Submitting is the caller's (`EvaluateTiles`).
[[nodiscard]] Result<Tile> RecordTile(vulkan::Context& context, terrain::KernelLibrary& kernels,
                                      Terrain& built, const TileKey& key, u32_t seed, VkCommandBuffer commands) {
#ifdef TRACY_ENABLE
    ZoneScopedN("viewer: evaluate tile");
#endif
    const u32_t         samples = built.samples;
    const SectionExtent extent{.x = samples, .y = 1, .z = samples};

    // Sample coordinates are in this level's units, which is why the compiled graph carries this level's
    // spacing: the kernels build a world position as `(origin + local) * resolution`, so a coarser level
    // is a coarser resolution over the same integer lattice rather than a stride through a finer one.
    // Origins step by `samples - 1`, not by `samples`, so a tile's last sample *is* its neighbour's
    // first one. A tile of N samples spans N-1 intervals; stepping by N would leave one interval
    // undrawn between every pair of tiles, which is the hairline of background that showed along every
    // tile border. The shared sample is the same world position in both tiles and the evaluator is
    // position-based, so the two surfaces meet exactly rather than merely closely.
    //
    // The level lattices still nest: a coarse origin is a multiple of the coarse spacing, which is a
    // multiple of the fine one, so changing level does not shift the terrain sideways.
    const i32_t originX = key.x * static_cast<i32_t>(samples - 1);
    const i32_t originZ = key.z * static_cast<i32_t>(samples - 1);

    Tile tile;
    tile.key     = key;
    tile.samples = samples;
    tile.spacing = built.spacing;
    tile.origin  = {static_cast<f32_t>(originX) * built.spacing, 0.0f,
                    static_cast<f32_t>(originZ) * built.spacing};

    const terrain::CompiledOutput& heightOutput  = built.compiled.outputs[built.heightOutput];
    const terrain::CompiledOutput& normalsOutput = built.compiled.outputs[built.normalsOutput];

    // The height slot is four planes: the height, then the lake, river and sea surfaces. The push
    // constants have no room for another address, so the water travels with the height it is drawn over.
    Result<vulkan::SectionSlot> height = context.Sections().Acquire(
        terrain::ClassOf(Mapping{Domain::R2, 4}, extent, 0), 4 * built.heightBytes);
    if (!height) {
        return std::unexpected(height.error());
    }
    tile.height = *height;
    // The shown output, if any, is a fourth plane after the normals' three.
    const b8_t                  shown   = built.shownOutput != kNoOutput;
    Result<vulkan::SectionSlot> normals = context.Sections().Acquire(
        terrain::ClassOf(shown ? Mapping{Domain::R2, 4} : normalsOutput.mapping, extent, 0),
        built.normalsBytes + (shown ? built.heightBytes : 0));
    if (!normals) {
        context.Sections().Release(tile.height);
        return std::unexpected(normals.error());
    }
    tile.normals = *normals;
    tile.shown   = shown;

    const auto fail = [&](const Error& error) {
        context.Sections().Release(tile.height);
        context.Sections().Release(tile.normals);
        return std::unexpected(error);
    };

    vulkan::Queue&          queue    = context.ComputeQueue();

    const terrain::SectionJob job{.origin = {originX, 0, originZ},
                                  .extent = extent,
                                  .seed   = seed};
    if (Status recorded = terrain::RecordSection(queue, kernels, built.compiled, built.resources,
                                                built.timers, commands, job);
        !recorded) {
        return fail(recorded.error());
    }

    // Copied rather than aliased: the evaluator's output buffers are reused by the next tile. Row by row
    // when the source carries a halo, because the resident tile is tight.
    //
    // An output buffer is not necessarily tight, which was an assumption worth getting wrong once. A
    // value some other op reads with a radius keeps that halo even when it is also a requested output,
    // and in a script that takes the gradient of its own height the height buffer ends up with the whole
    // erosion chain's halo: a 202x202 buffer where the tile wants 128x128.
    struct CopyPlan {
        const terrain::CompiledOutput* output;
        const vulkan::SectionSlot*     target;
        u32_t                          halo;
        u64_t                          offset;
    };
    std::array<CopyPlan, 6> copies{{{&heightOutput, &tile.height, built.heightHalo, 0},
                                    {&normalsOutput, &tile.normals, built.normalsHalo, 0},
                                    {nullptr, &tile.height, 0, built.heightBytes},
                                    {nullptr, &tile.height, 0, 2 * built.heightBytes},
                                    {nullptr, &tile.height, 0, 3 * built.heightBytes},
                                    {nullptr, nullptr, 0, built.normalsBytes}}};
    for (usize_t w = 0; w < kWaterOutputs.size(); ++w) {
        if (built.waterOutputs[w] != kNoOutput) {
            copies[2 + w].output = &built.compiled.outputs[built.waterOutputs[w]];
            copies[2 + w].halo   = copies[2 + w].output->halo;
        }
    }
    if (shown) {
        copies[5] = {&built.compiled.outputs[built.shownOutput], &tile.normals,
                     built.compiled.outputs[built.shownOutput].halo, built.normalsBytes};
    }
    for (const CopyPlan& plan : copies) {
        if (plan.target == nullptr) {
            continue;
        }
        if (plan.output == nullptr) {
            vkCmdFillBuffer(commands, plan.target->buffer, plan.target->offset + plan.offset,
                            built.heightBytes, kNoWaterBits);
            continue;
        }
        const vulkan::SectionSlot& source     = built.resources.Slot(plan.output->buffer);
        const u32_t                components = plan.output->mapping.components;
        const u64_t rowBytes = static_cast<u64_t>(samples) * components * sizeof(f32_t);
        const u64_t sourceStride =
            static_cast<u64_t>(samples + 2 * plan.halo) * components * sizeof(f32_t);

        vulkan::ComputeToTransferBarrier(commands, source.buffer, source.offset, source.size);

        if (plan.halo == 0) {
            const VkBufferCopy copy{.srcOffset = source.offset,
                                    .dstOffset = plan.target->offset + plan.offset,
                                    .size      = rowBytes * samples};
            vkCmdCopyBuffer(commands, source.buffer, plan.target->buffer, 1, &copy);
            continue;
        }
        // One region per interior row. `kMaxPreviewSamples` bounds the array so this allocates nothing.
        std::array<VkBufferCopy, kMaxPreviewSamples> regions{};
        for (u32_t row = 0; row < samples; ++row) {
            const u64_t sourceRow = static_cast<u64_t>(row + plan.halo) * sourceStride
                                    + static_cast<u64_t>(plan.halo) * components * sizeof(f32_t);
            regions[row] = VkBufferCopy{.srcOffset = source.offset + sourceRow,
                                        .dstOffset = plan.target->offset + plan.offset + row * rowBytes,
                                        .size      = rowBytes};
        }
        vkCmdCopyBuffer(commands, source.buffer, plan.target->buffer, samples, regions.data());
    }

    // The next tile in this command buffer writes the buffers these copies read: it waits for them.
    const VkMemoryBarrier reuse{.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &reuse, 0, nullptr, 0, nullptr);
    return tile;
}

} // namespace

/// Runs the script and resolves its outputs. CPU only, safe on a worker thread.
/// `resolution` is the *level's* sample spacing, not the one on the command line.
///
/// It used to be hardcoded to 1.0, which quietly made the viewer a liar: a script that derives
/// anything from `params.resolution` — a talus angle, metres out of `Terrain.Coords`, a blur radius —
/// got one answer here and a different one from the exporter, so the preview showed terrain the export
/// would never produce. Since the spacing changes with the level, the script is re-run when the level
/// changes; that is why this takes the spacing rather than reading it from the settings.
/// `centre` and `half` are the ground the ring covers, given to the script as `params.bounds`.
[[nodiscard]] Status LoadScript(LoadedGraph& loaded, std::string_view path, std::string_view shapes,
                                u64_t seed, const terrain::Params& params, f64_t resolution,
                                const f32_t (&centre)[2], f32_t half) {
    const lua::ScriptEnvironment environment{.seed       = seed,
                                             .minX       = centre[0] - half,
                                             .minY       = 0.0,
                                             .minZ       = centre[1] - half,
                                             .maxX       = centre[0] + half,
                                             .maxY       = 0.0,
                                             .maxZ       = centre[1] + half,
                                             .resolution = resolution,
                                             .params     = &params,
                                             .shapes     = shapes};
    if (Status ran = lua::RunScript(loaded.graph, path, environment); !ran) {
        return ran;
    }
    return ResolveOutputs(loaded);
}

/// Builds the compiled graph and the evaluator's resources for one level.
///
/// Recompiled per level rather than overriding the resolution at dispatch time, because the sample
/// spacing is baked into the parameter block of every op that needs a world position. A compile is 0.05
/// ms and a level change already evicts every tile, so there is nothing to save by being clever; and the
/// pipeline map is keyed on op and variant, so no pipeline is created either.
[[nodiscard]] Result<Terrain> BuildTerrain(vulkan::Context& context, const LoadedGraph& loaded,
                                           const ViewerLayer::Settings& settings, u32_t level) {
#ifdef TRACY_ENABLE
    ZoneScopedN("viewer: compile terrain");
#endif
    const u32_t samples = settings.sectionSize;
    if (samples < 2 || samples > kMaxPreviewSamples) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Evaluate,
                    "a preview tile of {} samples is outside [2, {}]", samples,
                    kMaxPreviewSamples);
    }
    const SectionExtent extent{.x = samples, .y = 1, .z = samples};

    Terrain built;
    built.samples = samples;
    built.level   = level;
    built.spacing = SpacingOf(settings.resolution, level);

    Result<terrain::CompiledGraph> compiled = terrain::Compile(
        loaded.graph,
        terrain::CompileOptions{.extent = extent, .resolution = built.spacing});
    if (!compiled) {
        return std::unexpected(compiled.error());
    }
    built.compiled = std::move(*compiled);

    Result<terrain::SectionResources> resources =
        terrain::SectionResources::Create(context, built.compiled);
    if (!resources) {
        return std::unexpected(resources.error());
    }
    built.resources = std::move(*resources);

    Result<terrain::DispatchTimers> timers =
        terrain::DispatchTimers::Create(context, built.compiled.stats.dispatchCount);
    if (!timers) {
        return std::unexpected(timers.error());
    }
    built.timers = std::move(*timers);

    {
        const Graph::Look look = loaded.graph.GetLook();
        const f32_t       wind = std::fmod(std::fmod(look.wind, 360.0f) + 360.0f, 360.0f);
        built.look = (look.palette & 0xFFu) | (static_cast<u32_t>(wind) << 8)
                   | ((static_cast<u32_t>(look.snow) & 0x7FFFu) << 17);
    }
    built.heightOutput  = loaded.heightOutput;
    built.normalsOutput = loaded.normalsOutput;
    built.waterOutputs  = loaded.waterOutputs;
    built.heightMinimum = loaded.heightMinimum;
    built.heightMaximum = loaded.heightMaximum;
    // The shown output, by name among the R2->R1 ones. A name the script does not give shows nothing.
    for (usize_t i = 0; i < loaded.graph.OutputCount() && !settings.show.empty(); ++i) {
        const terrain::OutputRequest& output = loaded.graph.Output(i);
        if (std::string_view{output.name.data()} == settings.show && output.value.mapping == Mapping{Domain::R2, 1}) {
            built.shownOutput  = i;
            built.shownMinimum = output.rangeMin;
            built.shownMaximum = output.rangeMax > output.rangeMin ? output.rangeMax : output.rangeMin + 1.0f;
        }
    }
    if (!settings.show.empty() && built.shownOutput == kNoOutput && level == 0) {
        LOG_WARN("nothing to show: the script has no R2->R1 output named {}", settings.show);
    }

    const terrain::CompiledOutput& height  = built.compiled.outputs[built.heightOutput];
    const terrain::CompiledOutput& normals = built.compiled.outputs[built.normalsOutput];
    built.heightHalo   = height.halo;
    built.normalsHalo  = normals.halo;
    built.heightBytes  = terrain::ValueSize(height.mapping, extent, 0);
    built.normalsBytes = terrain::ValueSize(normals.mapping, extent, 0);
    built.valid        = true;

    LOG_INFO("terrain ready at level {}: {} m per sample, {} dispatch(es) per tile of {} samples",
             level, built.spacing, built.compiled.stats.dispatchCount, samples);
    return built;
}

/// Evaluates `keys` into freshly acquired slots, appended to `out`, in as few command buffers as
/// `kMaxDispatchesPerSubmit` allows: one submit and one wait per chunk, not one per tile. A tile's own
/// work is a fraction of a millisecond on the GPU; the round trip to submit it and wait was most of what
/// it cost.
[[nodiscard]] Status EvaluateTiles(vulkan::Context& context, terrain::KernelLibrary& kernels, Terrain& built,
                                   std::span<const TileKey> keys, u32_t seed, std::pmr::vector<Tile>& out) {
#ifdef TRACY_ENABLE
    ZoneScopedN("viewer: evaluate tiles");
#endif
    const usize_t first = out.size();
    const auto    fail  = [&](const Error& error) -> Status {
        for (usize_t i = first; i < out.size(); ++i) {
            context.Sections().Release(out[i].height);
            context.Sections().Release(out[i].normals);
        }
        out.resize(first);
        return std::unexpected(error);
    };
    vulkan::Queue&          queue    = context.ComputeQueue();
    const auto submitAndWait = [&](VkCommandBuffer buffer) -> Status {
        Result<u64_t> submitted = queue.EndAndSubmit(buffer);
        if (!submitted) {
            return std::unexpected(submitted.error());
        }
        return queue.WaitTimeline(*submitted, kGpuTimeoutNanoseconds);
    };
    Result<VkCommandBuffer> commands = queue.BeginOneShot();
    if (!commands) {
        return std::unexpected(commands.error());
    }
    const usize_t dispatches = built.compiled.dispatches.size();
    usize_t       recorded   = 0;
    for (const TileKey& key : keys) {
        if (recorded > 0 && recorded + dispatches > kMaxDispatchesPerSubmit) {
            if (Status done = submitAndWait(*commands); !done) {
                return fail(done.error());
            }
            commands = queue.BeginOneShot();
            if (!commands) {
                return fail(commands.error());
            }
            recorded = 0;
        }
        recorded += dispatches;
        Result<Tile> tile = RecordTile(context, kernels, built, key, seed, *commands);
        if (!tile) {
            (void)queue.EndAndSubmit(*commands);
            return fail(tile.error());
        }
        out.push_back(*tile);
    }
    if (Status done = submitAndWait(*commands); !done) {
        return fail(done.error());
    }
    return {};
}

} // namespace engine::render
