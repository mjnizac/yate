#include <engine/render/renderer.hpp>

#include <engine/application.hpp>
#include <engine/log.hpp>
#include <engine/lua.hpp>
#include <engine/memory/general.hpp>
#include <engine/platform.hpp>
#include <engine/render/camera.hpp>
#include <engine/render/input.hpp>
#include <engine/render/ui.hpp>
#include <engine/terrain/compiler.hpp>
#include <engine/terrain/evaluator.hpp>
#include <engine/terrain/export.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/pipeline.hpp>
#include <engine/vulkan/swapchain.hpp>
#include <engine/vulkan/window.hpp>
#include <engine/window_layer.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory_resource>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace engine {
namespace {

using namespace engine::render;
using terrain::CompiledGraph;
using terrain::Domain;
using terrain::Graph;
using terrain::Mapping;
using terrain::SectionExtent;
using terrain::Value;

constexpr u64_t kGpuTimeoutNanoseconds = 5ull * 1000 * 1000 * 1000;

/// Debounce for the file watcher (spec section 11). An editor writing a file produces several
/// modification events, and reloading on the first one reads a half-written script.
constexpr f64_t kReloadDebounceSeconds = 0.150;

/// How often the script's modification time is checked. Polling, because no OS watch API is needed for
/// one file and a cross-platform one would be the only thing in the engine that needed it.
constexpr f64_t kWatchIntervalSeconds = 0.25;

/// Frames the ideal level must disagree with the active one before the ring switches.
///
/// A level change evicts and re-evaluates every tile, so acting on a single frame's measurement would
/// make the viewer stutter whenever the camera hovered near a threshold. A quarter of a second at sixty
/// frames is long enough that only a deliberate move crosses it.
constexpr u32_t kLevelChangeFrames = 15;

/// Largest tile side the preview accepts.
///
/// Set by the row-by-row interior copy: one `VkBufferCopy` per row, in a fixed-size array so building a
/// preview allocates nothing. 1024 samples at 2 m is a two-kilometre tile, which is well past what a
/// preview wants to evaluate in one go anyway.
constexpr u32_t kMaxPreviewSamples = 1024;

/// Which piece of terrain a tile holds.
///
/// Coordinates are in tile units *at its level*, so the same `x` and `z` mean a different patch of world
/// at a different level. The level is part of the key for exactly that reason.
struct TileKey {
    i32_t x     = 0;
    i32_t z     = 0;
    u32_t level = 0;

    [[nodiscard]] b8_t operator==(const TileKey&) const noexcept = default;
};

/// One evaluated piece of terrain, resident on the GPU.
///
/// Height and normals live in their own section-pool slots rather than in the evaluator's reusable
/// buffers, because the evaluator reuses one set for every section and the viewer needs them all at once.
struct Tile {
    TileKey             key;
    vulkan::SectionSlot height;
    vulkan::SectionSlot normals;
    /// World position of the tile's first sample.
    std::array<f32_t, 3> origin{};
    /// Metres between samples at this tile's level.
    f32_t spacing = 1.0f;
    u32_t samples = 0;
};

/// Six frustum planes as `ax + by + cz + d >= 0` inside the volume.
struct Frustum {
    std::array<std::array<f32_t, 4>, 6> planes{};

    /// True when any part of the box could be visible. Conservative: a box that straddles a plane counts
    /// as inside, which is the only safe direction to be wrong in.
    [[nodiscard]] b8_t Intersects(std::array<f32_t, 3> minimum,
                                  std::array<f32_t, 3> maximum) const noexcept {
        for (const std::array<f32_t, 4>& plane : planes) {
            // The box corner furthest along the plane normal. If even that one is outside, the whole box
            // is, which is the standard test and the only one that needs no per-corner loop.
            const f32_t x = plane[0] >= 0.0f ? maximum[0] : minimum[0];
            const f32_t y = plane[1] >= 0.0f ? maximum[1] : minimum[1];
            const f32_t z = plane[2] >= 0.0f ? maximum[2] : minimum[2];
            if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

/// Extracts the frustum planes from a view-projection matrix.
///
/// The rows of the matrix combined with the clip-space bounds give the planes directly, which is why no
/// inverse is needed. The matrix is column-major, so row `r` is `m[0][r], m[1][r], m[2][r], m[3][r]`.
/// With reversed depth the near plane is `w - z >= 0` and the far plane is `z >= 0`, which is the one
/// place the reversal has to be remembered outside the projection itself.
[[nodiscard]] Frustum FrustumOf(const Mat4& viewProjection) {
    const auto row = [&viewProjection](usize_t r) {
        return std::array<f32_t, 4>{viewProjection.m[0][r], viewProjection.m[1][r],
                                    viewProjection.m[2][r], viewProjection.m[3][r]};
    };
    const std::array<f32_t, 4> x = row(0);
    const std::array<f32_t, 4> y = row(1);
    const std::array<f32_t, 4> z = row(2);
    const std::array<f32_t, 4> w = row(3);

    const auto add = [](const std::array<f32_t, 4>& a, const std::array<f32_t, 4>& b) {
        return std::array<f32_t, 4>{a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]};
    };
    const auto subtract = [](const std::array<f32_t, 4>& a, const std::array<f32_t, 4>& b) {
        return std::array<f32_t, 4>{a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]};
    };
    const auto normalize = [](std::array<f32_t, 4> plane) {
        const f32_t length =
            std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        if (length > 1e-12f) {
            const f32_t inverse = 1.0f / length;
            plane[0] *= inverse;
            plane[1] *= inverse;
            plane[2] *= inverse;
            plane[3] *= inverse;
        }
        return plane;
    };

    Frustum frustum;
    frustum.planes[0] = normalize(add(w, x));      // left
    frustum.planes[1] = normalize(subtract(w, x)); // right
    frustum.planes[2] = normalize(add(w, y));      // bottom
    frustum.planes[3] = normalize(subtract(w, y)); // top
    frustum.planes[4] = normalize(subtract(w, z)); // near, reversed depth
    frustum.planes[5] = normalize(z);              // far, reversed depth
    return frustum;
}

/// A graph the script produced, plus what the viewer needs to draw it.
struct LoadedGraph {
    Graph graph;
    /// Index of the height output and of the normals output in the graph's output list.
    usize_t heightOutput  = 0;
    usize_t normalsOutput = 0;
    f32_t   heightMinimum = 0.0f;
    f32_t   heightMaximum = 0.0f;

    /// Empties it for reuse. A `Graph` is neither copyable nor movable, by design: it owns
    /// fixed-capacity node storage and handles point into it by index, so assigning one would quietly
    /// invalidate every handle anyone still held. Clearing in place is the only way to reuse it, and the
    /// only way that keeps the 150 KiB of storage allocated once instead of per reload.
    void Reset() {
        graph.Clear();
        heightOutput  = 0;
        normalsOutput = 0;
        heightMinimum = 0.0f;
        heightMaximum = 0.0f;
    }
};

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
        if (!foundHeight && output.value.mapping == Mapping{Domain::R2, 1}) {
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
    return {};
}

/// Everything needed to evaluate one tile, rebuilt on a load and on a level change.
///
/// The compiled graph, the evaluator's shared buffers and the query pool used to live inside the function
/// that built the whole preview, because the whole preview was built at once. Tiles are now evaluated a
/// couple at a time as the camera moves, so these have to outlive a frame.
struct Terrain {
    /// `CompiledGraph` has no default constructor: its vectors need a memory resource, which is the
    /// engine's way of making sure nothing allocates from the global default by accident. So this one
    /// names the resource too, and is assigned from a compile rather than built empty.
    terrain::CompiledGraph    compiled{memory::General().Resource()};
    terrain::SectionResources resources;
    terrain::DispatchTimers   timers;
    u32_t                     samples = 0;
    /// Level this was compiled for, and the sample spacing that implies.
    u32_t level   = 0;
    f32_t spacing = 1.0f;
    /// Index into `compiled.outputs`.
    usize_t heightOutput  = 0;
    usize_t normalsOutput = 0;
    /// Halo each output buffer carries, which is not always zero: a value some other op reads with a
    /// radius keeps that halo even when it is also a requested output.
    u32_t heightHalo  = 0;
    u32_t normalsHalo = 0;
    /// Bytes one tight tile takes, with no halo.
    u64_t heightBytes   = 0;
    u64_t normalsBytes  = 0;
    f32_t heightMinimum = 0.0f;
    f32_t heightMaximum = 0.0f;
    b8_t  valid         = false;
};

/// The resident tiles.
struct TileCache {
    std::pmr::vector<Tile> tiles{&memory::General().Resource()};
    u64_t                  evaluated = 0;

    void Release(vulkan::Context& context) {
        for (Tile& tile : tiles) {
            context.Sections().Release(tile.height);
            context.Sections().Release(tile.normals);
        }
        tiles.clear();
    }

    [[nodiscard]] b8_t Holds(const TileKey& key) const {
        for (const Tile& tile : tiles) {
            if (tile.key == key) {
                return true;
            }
        }
        return false;
    }
};

/// Sample spacing of a level. Doubling per level, so a level's lattice is a subset of the one below it.
[[nodiscard]] f32_t SpacingOf(f64_t baseResolution, u32_t level) {
    return static_cast<f32_t>(baseResolution) * static_cast<f32_t>(1u << level);
}

/// Level whose tiles are about the right size for what the camera can see.
///
/// One level for the whole ring, not one per tile. Per-tile levels are the usual answer and they bring
/// the usual problem: two levels meeting on screen sample the terrain at different spacings, so their
/// shared edge does not line up and the crack has to be hidden with skirts. This engine's whole claim is
/// that section borders are bit-identical, and shipping a renderer that visibly cracks between levels
/// would contradict it. With one global level nothing ever meets a different level: the ring switches as
/// a whole, which is a transition in time rather than a seam in space.
///
/// The target is a tile spanning about half the viewing distance, which keeps the visible ring a few
/// tiles across whatever the altitude.
[[nodiscard]] u32_t IdealLevel(f32_t distance, u32_t samples, f64_t baseResolution, u32_t maxLevel) {
    const f32_t wanted = distance * 0.5f / static_cast<f32_t>(std::max(samples, 1u));
    const f32_t ratio  = wanted / std::max(static_cast<f32_t>(baseResolution), 1e-6f);
    if (!(ratio > 1.0f)) {
        return 0;
    }
    const u32_t level = static_cast<u32_t>(std::log2(ratio));
    return std::min(level, maxLevel);
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

    built.heightOutput  = loaded.heightOutput;
    built.normalsOutput = loaded.normalsOutput;
    built.heightMinimum = loaded.heightMinimum;
    built.heightMaximum = loaded.heightMaximum;

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

/// Evaluates one tile into freshly acquired slots.
///
/// Synchronous: submit and wait. A tile of the sample script takes about a millisecond, and the frame
/// loop only ever does a couple per frame, so the cost is bounded and visible rather than hidden behind a
/// queue nothing watches.
[[nodiscard]] Result<Tile> EvaluateTile(vulkan::Context& context, terrain::KernelLibrary& kernels,
                                        Terrain& built, const TileKey& key, u32_t seed) {
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

    Result<vulkan::SectionSlot> height = context.Sections().Acquire(
        terrain::ClassOf(heightOutput.mapping, extent, 0), built.heightBytes);
    if (!height) {
        return std::unexpected(height.error());
    }
    tile.height = *height;
    Result<vulkan::SectionSlot> normals = context.Sections().Acquire(
        terrain::ClassOf(normalsOutput.mapping, extent, 0), built.normalsBytes);
    if (!normals) {
        context.Sections().Release(tile.height);
        return std::unexpected(normals.error());
    }
    tile.normals = *normals;

    const auto fail = [&](const Error& error) {
        context.Sections().Release(tile.height);
        context.Sections().Release(tile.normals);
        return std::unexpected(error);
    };

    vulkan::Queue&          queue    = context.ComputeQueue();
    Result<VkCommandBuffer> commands = queue.BeginOneShot();
    if (!commands) {
        return fail(commands.error());
    }

    const terrain::SectionJob job{.origin = {originX, 0, originZ},
                                  .extent = extent,
                                  .seed   = seed};
    if (Status recorded = terrain::RecordSection(queue, kernels, built.compiled, built.resources,
                                                built.timers, *commands, job);
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
    };
    const std::array<CopyPlan, 2> copies{{{&heightOutput, &tile.height, built.heightHalo},
                                          {&normalsOutput, &tile.normals, built.normalsHalo}}};
    for (const CopyPlan& plan : copies) {
        const vulkan::SectionSlot& source     = built.resources.Slot(plan.output->buffer);
        const u32_t                components = plan.output->mapping.components;
        const u64_t rowBytes = static_cast<u64_t>(samples) * components * sizeof(f32_t);
        const u64_t sourceStride =
            static_cast<u64_t>(samples + 2 * plan.halo) * components * sizeof(f32_t);

        vulkan::ComputeToTransferBarrier(*commands, source.buffer, source.offset, source.size);

        if (plan.halo == 0) {
            const VkBufferCopy copy{.srcOffset = source.offset,
                                    .dstOffset = plan.target->offset,
                                    .size      = rowBytes * samples};
            vkCmdCopyBuffer(*commands, source.buffer, plan.target->buffer, 1, &copy);
            continue;
        }
        // One region per interior row. `kMaxPreviewSamples` bounds the array so this allocates nothing.
        std::array<VkBufferCopy, kMaxPreviewSamples> regions{};
        for (u32_t row = 0; row < samples; ++row) {
            const u64_t sourceRow = static_cast<u64_t>(row + plan.halo) * sourceStride
                                    + static_cast<u64_t>(plan.halo) * components * sizeof(f32_t);
            regions[row] = VkBufferCopy{.srcOffset = source.offset + sourceRow,
                                        .dstOffset = plan.target->offset + row * rowBytes,
                                        .size      = rowBytes};
        }
        vkCmdCopyBuffer(*commands, source.buffer, plan.target->buffer, samples, regions.data());
    }

    Result<u64_t> submitted = queue.EndAndSubmit(*commands);
    if (!submitted) {
        return fail(submitted.error());
    }
    if (Status waited = queue.WaitTimeline(*submitted, kGpuTimeoutNanoseconds); !waited) {
        return fail(waited.error());
    }
    return tile;
}

/// A reload running on a worker thread.
///
/// Only the CPU half happens there: running the script and compiling the graph. Creating GPU resources
/// stays on the main thread, because the queue and the section pool are not synchronized for concurrent
/// use and making them so would be a large change to serve one feature. The split matches what the spec
/// asks for either way: the previous graph keeps rendering until the new one is *ready*, and ready means
/// it compiled.
struct Reload {
    std::thread        worker;
    std::atomic<b8_t>  finished{false};
    std::atomic<b8_t>  running{false};
    LoadedGraph*       loaded = nullptr;
    Error              error;
    b8_t               failed = false;
};

[[nodiscard]] f64_t NowSeconds() {
    return std::chrono::duration<f64_t>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Modification time of `path` in seconds, or zero when it cannot be read.
///
/// `std::filesystem` rather than a platform call: it is standard, and a missing file is a normal state
/// here, not an error, because a script can be saved by an editor that deletes and recreates it.
[[nodiscard]] f64_t FileModifiedSeconds(std::string_view path) {
    std::error_code                code;
    const std::filesystem::path    file{std::string(path)};
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, code);
    if (code) {
        return 0.0;
    }
    return std::chrono::duration<f64_t>(time.time_since_epoch()).count();
}

} // namespace

struct ViewerLayer::State {
    Settings              settings;
    std::array<char, 512> scriptPath{};
    terrain::Params       params;

    vulkan::Context*        context  = nullptr;
    terrain::KernelLibrary* kernels  = nullptr;
    vulkan::Window*         window   = nullptr;
    vulkan::Swapchain*      swapchain = nullptr;
    WindowLayer*            windowLayer = nullptr;

    vulkan::GraphicsPipeline pipeline;
    /// The same pipeline in line mode. Worth having: it is the only way to see what the distance LOD
    /// actually chose, which is otherwise invisible in a filled image.
    vulkan::GraphicsPipeline wirePipeline;
    b8_t                     wireframe = false;
    Camera                   camera;
    Input                    input;
    Ui                       ui;
    /// Smoothed, because a per-frame figure flickers too fast to read.
    f32_t framesPerSecond   = 0.0f;
    f32_t frameMilliseconds = 0.0f;

    Terrain   terrain;
    TileCache cache;
    /// Level the ring is at, and how long the ideal level has disagreed with it.
    u32_t level             = 0;
    u32_t disagreeingFrames = 0;
    /// Tiles missing from the ring at the end of the last streaming pass. Zero means the ring is
    /// complete, which is what a test can wait for.
    u32_t missingTiles = 0;
    /// Kept so a reload can be compiled against the same node storage without reallocating it.
    LoadedGraph current;
    LoadedGraph pending;

    Reload reload;

    u64_t loadCount  = 0;
    u32_t tilesDrawn = 0;

    std::array<char, Error::kMaxFormat> lastError{};
    b8_t                                hasError = false;

    f64_t lastWatchSeconds    = 0.0;
    f64_t lastModifiedSeconds = 0.0;
    f64_t pendingSinceSeconds = 0.0;
    b8_t  reloadRequested     = false;
};

namespace {

/// Runs the script and resolves its outputs. CPU only, safe on a worker thread.
[[nodiscard]] Status LoadScript(LoadedGraph& loaded, std::string_view path, u64_t seed,
                                const terrain::Params& params) {
    const lua::ScriptEnvironment environment{.seed       = seed,
                                             .minX       = 0.0,
                                             .minY       = 0.0,
                                             .minZ       = 0.0,
                                             .maxX       = 0.0,
                                             .maxY       = 0.0,
                                             .maxZ       = 0.0,
                                             .resolution = 1.0,
                                             .params     = &params};
    if (Status ran = lua::RunScript(loaded.graph, path, environment); !ran) {
        return ran;
    }
    return ResolveOutputs(loaded);
}

} // namespace

// --- ViewerLayer ------------------------------------------------------------------------------

ViewerLayer::ViewerLayer(Application& application, const Settings& settings)
    : Layer(application) {
    void* storage = memory::General().Allocate(sizeof(State), alignof(State));
    if (storage == nullptr) {
        LOG_ERROR("could not allocate the viewer state");
        return;
    }
    m_state           = ::new (storage) State{};
    m_state->settings = settings;
    detail::CopyBounded(m_state->scriptPath, settings.script);
    m_state->settings.script = std::string_view{m_state->scriptPath.data()};
    if (settings.params != nullptr) {
        m_state->params = *settings.params;
    }
    m_state->settings.params = &m_state->params;
    // At most as many drawn vertices per side as there are samples: a finer grid than the data would
    // interpolate nothing and cost triangles.
    m_state->settings.gridVertices =
        std::min(std::max(settings.gridVertices, 2u), std::max(settings.sectionSize, 2u));
}

ViewerLayer::~ViewerLayer() {
    if (m_state == nullptr) {
        return;
    }
    if (m_state->reload.worker.joinable()) {
        m_state->reload.worker.join();
    }
    m_state->~State();
    memory::General().Free(m_state);
    m_state = nullptr;
}

void ViewerLayer::OnAttach() {
    if (m_state == nullptr) {
        App().Stop();
        return;
    }
    ApplicationState& state = StateOf(App());
    if (state.window == nullptr || state.swapchain == nullptr) {
        LOG_ERROR("ViewerLayer needs a window; push a WindowLayer first");
        App().Stop();
        return;
    }
    m_state->context   = state.vulkan;
    m_state->kernels   = &KernelsOf(App());
    m_state->window    = state.window;
    m_state->swapchain = state.swapchain;

    // The frame belongs to the window layer, so this layer asks to be called inside its render pass.
    // Found by walking the stack rather than taken as a constructor argument, so the executable does not
    // have to thread one layer into another.
    for (usize_t i = 0; i < state.layers.size(); ++i) {
        if (auto* candidate = dynamic_cast<WindowLayer*>(state.layers[i].layer);
            candidate != nullptr) {
            m_state->windowLayer = candidate;
            break;
        }
    }
    if (m_state->windowLayer == nullptr) {
        LOG_ERROR("ViewerLayer found no WindowLayer below it");
        App().Stop();
        return;
    }

    Result<vulkan::GraphicsPipeline> pipeline = vulkan::GraphicsPipeline::Create(
        m_state->context->Device(), m_state->swapchain->Pass().Handle(), "viewer/terrain.vert.spv",
        "viewer/terrain.frag.spv", m_state->context->Pipelines().Handle());
    if (!pipeline) {
        LOG_ERROR("could not create the terrain pipeline: {}", pipeline.error().Format().data());
        App().Stop();
        return;
    }
    m_state->pipeline = std::move(*pipeline);

    // The same shaders in line mode, and only when the device offers line rasterization. Asking anyway
    // and taking the failure would be a validation error rather than a clean refusal, which is a worse
    // outcome than not having a wireframe.
    if (!m_state->context->Info().fillModeNonSolid) {
        LOG_INFO("no wireframe: the device does not support fillModeNonSolid");
    } else if (Result<vulkan::GraphicsPipeline> wire = vulkan::GraphicsPipeline::Create(
            m_state->context->Device(), m_state->swapchain->Pass().Handle(),
            "viewer/terrain.vert.spv", "viewer/terrain.frag.spv",
            m_state->context->Pipelines().Handle(), vulkan::PolygonMode::Line);
               wire) {
        m_state->wirePipeline = std::move(*wire);
    } else {
        LOG_WARN("wireframe is unavailable: {}", wire.error().Format().data());
    }

    // First load is synchronous: there is nothing to keep rendering yet, so there is no reason to defer
    // it and every reason to fail here rather than after the window has opened on an empty scene.
    if (Status loaded = LoadScript(m_state->current, m_state->settings.script,
                                   m_state->settings.seed, m_state->params);
        !loaded) {
        detail::CopyBounded(m_state->lastError, std::string_view{loaded.error().Format().data()});
        m_state->hasError = true;
        LOG_ERROR("{}", m_state->lastError.data());
        App().Stop();
        return;
    }
    // Framed before anything is evaluated, because the camera is what decides which tiles to evaluate
    // and at what spacing. The radius it is framed on is the ring's own radius in world units.
    const f32_t ringRadius = static_cast<f32_t>(m_state->settings.sectionSize)
                             * static_cast<f32_t>(m_state->settings.resolution)
                             * static_cast<f32_t>(m_state->settings.ringRadius + 1);
    m_state->camera.Frame(Vec3{0.0f, 0.0f, 0.0f}, ringRadius);
    m_state->level = IdealLevel(m_state->camera.ViewDistance(0.0f), m_state->settings.sectionSize,
                                m_state->settings.resolution, m_state->settings.maxLevel);

    Result<Terrain> terrain = BuildTerrain(*m_state->context, m_state->current, m_state->settings,
                                           m_state->level);
    if (!terrain) {
        detail::CopyBounded(m_state->lastError, std::string_view{terrain.error().Format().data()});
        m_state->hasError = true;
        LOG_ERROR("{}", m_state->lastError.data());
        App().Stop();
        return;
    }
    m_state->terrain = std::move(*terrain);
    ++m_state->loadCount;

    // The first ring is filled before the window opens, so the viewer never shows a hole it could have
    // avoided. After that, streaming is bounded per frame.
    StreamTiles(~0u);

    m_state->lastModifiedSeconds = FileModifiedSeconds(m_state->settings.script);
    m_state->lastWatchSeconds    = NowSeconds();

    // Before the UI: its backend installs a scroll callback that chains to whatever was there first, so
    // installing this one afterwards would replace it and the panels would stop scrolling.
    if (Status attached = m_state->input.Attach(*m_state->window); !attached) {
        LOG_WARN("the scroll wheel is unavailable: {}", attached.error().Format().data());
    }

    if (Status ui = m_state->ui.Initialize(*m_state->context, *m_state->window,
                                           m_state->swapchain->Pass().Handle(),
                                           static_cast<u32_t>(m_state->swapchain->ImageCount()));
        !ui) {
        // Not fatal: a viewer without panels still shows terrain, and losing the whole preview because
        // the UI backend failed would be the wrong trade.
        LOG_WARN("the UI is unavailable: {}", ui.error().Format().data());
    }

    m_state->windowLayer->SetRecorder(
        [](const WindowLayer::FrameContext& frame, void* user) {
            static_cast<ViewerLayer*>(user)->Record(frame);
        },
        this);

    LOG_INFO("viewer ready: orbit and fly with the mouse, F switches mode, R reloads the script");
}

void ViewerLayer::OnDetach() {
    if (m_state == nullptr) {
        return;
    }
    // Stop being called into first, so nothing records against resources that are about to go.
    if (m_state->windowLayer != nullptr) {
        m_state->windowLayer->SetRecorder(nullptr, nullptr);
    }
    WaitForReload();

    if (m_state->context != nullptr) {
        if (Status idle = m_state->context->WaitIdle(); !idle) {
            LOG_WARN("could not wait for the device before releasing the viewer: {}",
                     idle.error().Format().data());
        }
    }
    // An idle device is not enough. The last frame the window layer recorded still *names* this layer's
    // pipeline and the UI's buffers and descriptor sets, and a command buffer keeps referencing what it
    // mentions until it is reset. Layers detach in reverse push order, so this one tears down while that
    // recording is still alive; resetting it is what releases the references. See
    // `Swapchain::ResetFrames`.
    if (m_state->swapchain != nullptr) {
        if (Status reset = m_state->swapchain->ResetFrames(); !reset) {
            LOG_WARN("could not reset the frame commands before releasing the viewer: {}",
                     reset.error().Format().data());
        }
    }

    m_state->ui.Shutdown();
    if (m_state->context != nullptr) {
        m_state->cache.Release(*m_state->context);
    }
    m_state->pipeline     = vulkan::GraphicsPipeline{};
    m_state->wirePipeline = vulkan::GraphicsPipeline{};
}

void ViewerLayer::OnUpdate() {
#ifdef TRACY_ENABLE
    ZoneScopedN("ViewerLayer::OnUpdate");
#endif
    if (m_state == nullptr || m_state->windowLayer == nullptr) {
        return;
    }
    UpdateInput();
    UpdateLevel();
    StreamTiles(m_state->settings.tilesPerFrame);
    UpdatePanels();
    UpdateReload();
}

void ViewerLayer::UpdateInput() {
    State&      state = *m_state;
    const f64_t delta = state.windowLayer->DeltaSeconds();

    state.input.Update(*state.window);

    // A panel under the pointer takes the pointer. Without this, dragging a slider also turns the
    // camera, which is the single most irritating bug an immediate-mode UI can have.
    //
    // These flags come from the panels built during the *previous* frame's recording, because that is
    // when the UI frame is laid out. One frame of latency on "is the cursor over a panel", which is
    // inherent to an immediate-mode UI and invisible at any frame rate worth using.
    const b8_t uiHasMouse    = state.ui.WantsMouse();
    const b8_t uiHasKeyboard = state.ui.WantsKeyboard();

    if (!uiHasKeyboard && state.input.Pressed(Key::ToggleMode)) {
        const b8_t toFly = state.camera.Mode() == CameraMode::Orbit;
        state.camera.SetMode(toFly ? CameraMode::Fly : CameraMode::Orbit);
        LOG_INFO("camera mode: {}", toFly ? "fly" : "orbit");
    }
    if (!uiHasKeyboard && state.input.Pressed(Key::Reload)) {
        RequestReload();
    }
    if (!uiHasKeyboard && state.input.Pressed(Key::Wireframe)) {
        state.wireframe = !state.wireframe;
    }

    // Left drag turns, middle drag pans, wheel zooms. Dragging rather than captured-cursor look, because
    // the viewer also wants a usable pointer for the parameter panel.
    if (!uiHasMouse && state.input.Held(MouseButton::Left)) {
        state.camera.Turn(state.input.CursorDeltaX(), state.input.CursorDeltaY());
    }
    if (!uiHasMouse && state.input.Held(MouseButton::Middle)) {
        state.camera.Pan(state.input.CursorDeltaX(), state.input.CursorDeltaY());
    }
    // Read either way, so a tick spent over a panel is consumed rather than applied on the next frame.
    const f32_t scroll = state.input.ScrollDelta();
    if (!uiHasMouse && scroll != 0.0f) {
        state.camera.Zoom(scroll);
    }

    const f32_t forward = (state.input.Held(Key::MoveForward) ? 1.0f : 0.0f)
                          - (state.input.Held(Key::MoveBack) ? 1.0f : 0.0f);
    const f32_t right = (state.input.Held(Key::MoveRight) ? 1.0f : 0.0f)
                        - (state.input.Held(Key::MoveLeft) ? 1.0f : 0.0f);
    const f32_t up = (state.input.Held(Key::MoveUp) ? 1.0f : 0.0f)
                     - (state.input.Held(Key::MoveDown) ? 1.0f : 0.0f);
    if (!uiHasKeyboard && (forward != 0.0f || right != 0.0f || up != 0.0f)) {
        const f32_t boost = state.input.Held(Key::Faster) ? 5.0f : 1.0f;
        state.camera.Move(forward * boost, right * boost, up * boost,
                          static_cast<f32_t>(delta));
    }
}

void ViewerLayer::UpdatePanels() {
    State&      state = *m_state;
    const f64_t delta = state.windowLayer->DeltaSeconds();
    if (delta > 0.0) {
        // Exponential smoothing. A per-frame figure changes faster than it can be read.
        const f32_t instant = static_cast<f32_t>(1.0 / delta);
        state.framesPerSecond =
            state.framesPerSecond == 0.0f ? instant : state.framesPerSecond * 0.9f + instant * 0.1f;
        const f32_t milliseconds = static_cast<f32_t>(delta * 1000.0);
        state.frameMilliseconds  = state.frameMilliseconds == 0.0f
                                       ? milliseconds
                                       : state.frameMilliseconds * 0.9f + milliseconds * 0.1f;
    }
}

/// Builds and records the panels.
///
/// The whole UI frame lives here, including `NewFrame`, rather than being split between the layer update
/// and the recorder. The layer stack is `WindowLayer` then `ViewerLayer`, and the window layer owns the
/// frame: it calls the recorder from inside its own update, which runs *before* this layer's. Building
/// the UI in `OnUpdate` therefore put `NewFrame` after the `Render` that consumes it, and ImGui said so
/// on the first frame. Keeping the pair together removes the ordering question instead of answering it.
void ViewerLayer::RecordPanels(VkCommandBuffer commands) {
    State& state = *m_state;
    if (!state.ui.IsValid()) {
        return;
    }
    state.ui.Begin();

    const Vec3 eye = state.camera.Position();
    Ui::Panels panels;
    panels.scriptPath        = state.scriptPath.data();
    panels.error             = state.hasError ? state.lastError.data() : "";
    panels.cameraMode        = state.camera.Mode() == CameraMode::Orbit ? "orbit camera (F to fly)"
                                                                       : "fly camera (F to orbit)";
    panels.tilesDrawn        = state.tilesDrawn;
    panels.tileCount         = static_cast<u32_t>(state.cache.tiles.size());
    panels.level             = state.level;
    panels.spacing           = state.terrain.spacing;
    panels.missingTiles      = state.missingTiles;
    panels.wireframe         = state.wireframe;
    panels.loadCount         = state.loadCount;
    panels.framesPerSecond   = state.framesPerSecond;
    panels.frameMilliseconds = state.frameMilliseconds;
    panels.eye               = {eye.x, eye.y, eye.z};
    panels.params            = &state.params;

    // Editing a parameter reloads: the script is what turns a parameter into terrain, so there is no
    // shortcut that updates push constants instead. The spec's "only numeric parameters changed, so no
    // pipeline is created" holds anyway, because the pipeline map is keyed on op and variant and a
    // changed number produces the same variants.
    if (state.ui.Draw(panels)) {
        RequestReload();
    }

    state.ui.Record(commands);
}

void ViewerLayer::UpdateReload() {
    State&      state = *m_state;
    const f64_t now   = NowSeconds();

    // Finish a reload that completed on the worker. The GPU half runs here, on the thread that owns the
    // queue and the section pool.
    if (state.reload.finished.load()) {
        state.reload.worker.join();
        state.reload.finished.store(false);
        state.reload.running.store(false);

        if (state.reload.failed) {
            detail::CopyBounded(state.lastError,
                                std::string_view{state.reload.error.Format().data()});
            state.hasError = true;
            LOG_ERROR("reload failed, keeping the previous terrain: {}", state.lastError.data());
        } else {
            Result<Terrain> terrain =
                BuildTerrain(*state.context, state.pending, state.settings, state.level);
            if (!terrain) {
                detail::CopyBounded(state.lastError,
                                    std::string_view{terrain.error().Format().data()});
                state.hasError = true;
                LOG_ERROR("reload compiled but could not be prepared, keeping the previous "
                          "terrain: {}",
                          state.lastError.data());
            } else {
                // Swapped only once the new terrain exists, so a failure anywhere above leaves the old
                // one on screen untouched (spec section 11).
                if (Status idle = state.context->WaitIdle(); !idle) {
                    LOG_WARN("could not wait for the device before swapping the terrain: {}",
                             idle.error().Format().data());
                }
                state.cache.Release(*state.context);
                state.terrain = std::move(*terrain);
                ++state.loadCount;
                state.hasError     = false;
                state.lastError[0] = 0;
                // Refilled immediately rather than over the next frames: a reload is a deliberate act
                // and showing a half-built ring afterwards would look like a failure.
                StreamTiles(~0u);
                LOG_INFO("script reloaded ({} load(s))", state.loadCount);
            }
        }
    }

    if (state.reload.running.load()) {
        return;
    }

    // Poll, with a debounce. An editor writes a file in several steps, and reloading on the first one
    // reads a half-written script; waiting for the modification time to stop changing avoids reporting a
    // syntax error the user never made.
    if (now - state.lastWatchSeconds >= kWatchIntervalSeconds) {
        state.lastWatchSeconds       = now;
        const f64_t modified         = FileModifiedSeconds(state.settings.script);
        if (modified != 0.0 && modified != state.lastModifiedSeconds) {
            state.lastModifiedSeconds = modified;
            state.pendingSinceSeconds = now;
        }
    }

    const b8_t debounced =
        state.pendingSinceSeconds != 0.0 && now - state.pendingSinceSeconds >= kReloadDebounceSeconds;
    if (!state.reloadRequested && !debounced) {
        return;
    }
    state.reloadRequested     = false;
    state.pendingSinceSeconds = 0.0;

    state.reload.running.store(true);
    state.reload.failed = false;
    state.reload.worker = std::thread([&state]() {
        platform::SetThreadName("engine-reload");
        // Emptied every time, so a script that fails halfway cannot leave half its nodes behind.
        state.pending.Reset();
        if (Status loaded = LoadScript(state.pending, state.settings.script, state.settings.seed,
                                       state.params);
            !loaded) {
            state.reload.error  = loaded.error();
            state.reload.failed = true;
        }
        state.reload.finished.store(true);
    });
}

void ViewerLayer::Record(const WindowLayer::FrameContext& frame) {
#ifdef TRACY_ENABLE
    ZoneScopedN("viewer: record tiles");
#endif
    State& state = *m_state;
    if (state.cache.tiles.empty() || !state.pipeline.IsValid()) {
        state.tilesDrawn = 0;
        RecordPanels(frame.commands);
        return;
    }

    const f32_t aspect = frame.extent.height != 0
                             ? static_cast<f32_t>(frame.extent.width)
                                   / static_cast<f32_t>(frame.extent.height)
                             : 1.0f;
    const Mat4    viewProjection = state.camera.ViewProjection(aspect);
    const Frustum frustum        = FrustumOf(viewProjection);

    vulkan::TerrainPushConstants constants{};
    constants.viewProjection = viewProjection.m;
    constants.samples        = state.terrain.samples;

    const vulkan::GraphicsPipeline& pipeline =
        state.wireframe && state.wirePipeline.IsValid() ? state.wirePipeline : state.pipeline;

    b8_t  bound = false;
    u32_t drawn = 0;
    for (const Tile& tile : state.cache.tiles) {
        // The tile's bounding box: its footprint, and the height range the script declared. Using the
        // declared range rather than the real one means the box is never too small, which is the only
        // direction that would cull something visible.
        const f32_t side = static_cast<f32_t>(tile.samples - 1) * tile.spacing;
        const std::array<f32_t, 3> minimum{tile.origin[0], state.terrain.heightMinimum,
                                           tile.origin[2]};
        const std::array<f32_t, 3> maximum{tile.origin[0] + side, state.terrain.heightMaximum,
                                           tile.origin[2] + side};
        if (!frustum.Intersects(minimum, maximum)) {
            continue;
        }

        // One grid density for every tile, for the same reason there is one evaluation level: two
        // densities meeting along a shared edge is a T-junction. The coarse side spans its edge with a
        // chord between every nth sample while the fine side follows each one, so the surfaces part
        // company and the background shows through as a hairline crack. Distance is already handled,
        // and handled once, by the level that chooses the sample spacing.
        const u32_t gridVertices = std::min(state.settings.gridVertices, tile.samples);
        const u32_t quadsPerSide = gridVertices - 1;
        const u32_t vertexCount  = quadsPerSide * quadsPerSide * 6;

        constants.resolution   = tile.spacing;
        constants.gridVertices = gridVertices;
        constants.tileOrigin   = tile.origin;
        constants.heights      = tile.height.address;
        constants.normals      = tile.normals.address;

        if (!bound) {
            pipeline.Bind(frame.commands, frame.extent, constants);
            bound = true;
        } else {
            pipeline.Push(frame.commands, constants);
        }
        vkCmdDraw(frame.commands, vertexCount, 1, 0, 0);
        ++drawn;
    }
    state.tilesDrawn = drawn;

    // Panels last, so they sit on top of the terrain. Same render pass, no depth test in the UI
    // pipeline, so ordering inside the pass is all that is needed.
    RecordPanels(frame.commands);
}

/// Brings the ring of tiles around the camera up to date.
///
/// `budget` tiles are evaluated at most, nearest first. The bound is what keeps a frame from stalling
/// while the camera moves: a hole at the edge of the ring for a few frames is a far better failure than
/// a hitch every time the camera crosses a tile border. `~0u` means "take as long as it needs", which is
/// what a load and a reload use, because showing a half-built ring right after a deliberate act would
/// look like a failure rather than like streaming.
void ViewerLayer::StreamTiles(u32_t budget) {
#ifdef TRACY_ENABLE
    ZoneScopedN("viewer: stream tiles");
#endif
    State& state = *m_state;
    if (!state.terrain.valid) {
        return;
    }

    const i32_t radius   = static_cast<i32_t>(state.settings.ringRadius);
    // `samples - 1`, matching the origins a key maps to: a tile covers that many intervals.
    const f32_t tileSide = static_cast<f32_t>(state.terrain.samples - 1) * state.terrain.spacing;
    // Centred on what the camera is about rather than on the eye: see `Camera::Focus`.
    const Vec3 focus = state.camera.Focus();

    // The tile the focus is over. Floor division, not truncation: truncating rounds towards zero and
    // would make the ring jump by one tile every time it crossed an axis.
    const auto floorDiv = [](f32_t value, f32_t size) {
        return static_cast<i32_t>(std::floor(value / size));
    };
    const i32_t centreX = floorDiv(focus.x, tileSide);
    const i32_t centreZ = floorDiv(focus.z, tileSide);

    // Evict what is no longer wanted. Done before evaluating, so the slots come back to the pool in time
    // to be reused by the tiles that replace them.
    for (usize_t i = state.cache.tiles.size(); i-- > 0;) {
        const Tile& tile = state.cache.tiles[i];
        const b8_t  wanted = tile.key.level == state.level
                            && std::abs(tile.key.x - centreX) <= radius
                            && std::abs(tile.key.z - centreZ) <= radius;
        if (wanted) {
            continue;
        }
        state.context->Sections().Release(state.cache.tiles[i].height);
        state.context->Sections().Release(state.cache.tiles[i].normals);
        state.cache.tiles.erase(state.cache.tiles.begin() + static_cast<isize_t>(i));
    }

    // Evaluate what is missing, nearest first, so a hole appears at the edge of the view rather than
    // under the camera.
    u32_t missing = 0;
    u32_t spent   = 0;
    for (i32_t ring = 0; ring <= radius; ++ring) {
        for (i32_t z = centreZ - ring; z <= centreZ + ring; ++z) {
            for (i32_t x = centreX - ring; x <= centreX + ring; ++x) {
                // Only the new border of this ring; the inside was covered by a smaller one.
                if (ring != 0 && std::abs(x - centreX) != ring && std::abs(z - centreZ) != ring) {
                    continue;
                }
                const TileKey key{.x = x, .z = z, .level = state.level};
                if (state.cache.Holds(key)) {
                    continue;
                }
                if (spent >= budget) {
                    ++missing;
                    continue;
                }
                Result<Tile> tile = EvaluateTile(*state.context, *state.kernels, state.terrain, key,
                                                 static_cast<u32_t>(state.settings.seed));
                if (!tile) {
                    detail::CopyBounded(state.lastError,
                                        std::string_view{tile.error().Format().data()});
                    state.hasError = true;
                    LOG_ERROR("could not evaluate a tile: {}", state.lastError.data());
                    // Giving up on the rest of this pass rather than failing the same way N more times.
                    state.missingTiles = missing + 1;
                    return;
                }
                state.cache.tiles.push_back(*tile);
                ++state.cache.evaluated;
                ++spent;
            }
        }
    }
    state.missingTiles = missing;
}

/// Picks the level the ring should be at, with hysteresis.
///
/// A level change evicts and re-evaluates every tile, so it must not happen because the camera drifted a
/// metre across a threshold. The ideal level has to disagree for a while before it is acted on, which
/// turns a boundary into a dead band without needing one.
void ViewerLayer::UpdateLevel() {
    State&      state = *m_state;
    const f32_t ground = (state.terrain.heightMinimum + state.terrain.heightMaximum) * 0.5f;
    const u32_t ideal   = IdealLevel(state.camera.ViewDistance(ground), state.settings.sectionSize,
                                   state.settings.resolution, state.settings.maxLevel);
    if (ideal == state.level) {
        state.disagreeingFrames = 0;
        return;
    }
    if (++state.disagreeingFrames < kLevelChangeFrames) {
        return;
    }
    state.disagreeingFrames = 0;

    Result<Terrain> terrain =
        BuildTerrain(*state.context, state.current, state.settings, ideal);
    if (!terrain) {
        LOG_WARN("could not switch to level {}, staying at {}: {}", ideal, state.level,
                 terrain.error().Format().data());
        return;
    }
    if (Status idle = state.context->WaitIdle(); !idle) {
        LOG_WARN("could not wait for the device before changing level: {}",
                 idle.error().Format().data());
    }
    // Every resident tile is at the old spacing, so none of them survives a level change. That is the
    // cost of one global level, and it is paid rarely: the ideal level has to disagree for
    // `kLevelChangeFrames` frames first.
    state.cache.Release(*state.context);
    state.terrain = std::move(*terrain);
    state.level   = ideal;
    StreamTiles(~0u);
    LOG_INFO("level {} at {} m per sample", state.level, state.terrain.spacing);
}

u32_t ViewerLayer::TilesDrawn() const noexcept {
    return m_state != nullptr ? m_state->tilesDrawn : 0;
}

u32_t ViewerLayer::TileCount() const noexcept {
    return m_state != nullptr ? static_cast<u32_t>(m_state->cache.tiles.size()) : 0;
}

u32_t ViewerLayer::MissingTiles() const noexcept {
    return m_state != nullptr ? m_state->missingTiles : 0;
}

u32_t ViewerLayer::Level() const noexcept { return m_state != nullptr ? m_state->level : 0; }

u64_t ViewerLayer::TilesEvaluated() const noexcept {
    return m_state != nullptr ? m_state->cache.evaluated : 0;
}

u64_t ViewerLayer::LoadCount() const noexcept {
    return m_state != nullptr ? m_state->loadCount : 0;
}

std::string_view ViewerLayer::LastError() const noexcept {
    if (m_state == nullptr || !m_state->hasError) {
        return {};
    }
    return std::string_view{m_state->lastError.data()};
}

void ViewerLayer::MoveCameraTo(f32_t x, f32_t y, f32_t z) noexcept {
    if (m_state != nullptr) {
        m_state->camera.PlaceAt(Vec3{x, y, z});
    }
}

void ViewerLayer::SetFlyMode(b8_t fly) noexcept {
    if (m_state != nullptr) {
        m_state->camera.SetMode(fly ? CameraMode::Fly : CameraMode::Orbit);
    }
}

void ViewerLayer::RequestReload() noexcept {
    if (m_state != nullptr) {
        m_state->reloadRequested = true;
    }
}

void ViewerLayer::WaitForReload() {
    if (m_state == nullptr) {
        return;
    }
    // Two steps: wait for the worker, then run the GPU half, which only the owning thread may do.
    if (m_state->reload.worker.joinable() && !m_state->reload.finished.load()) {
        while (!m_state->reload.finished.load()) {
            std::this_thread::yield();
        }
    }
    if (m_state->reload.finished.load()) {
        UpdateReload();
    }
}

} // namespace engine
