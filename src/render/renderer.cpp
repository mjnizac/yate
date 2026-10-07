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

/// Drawn grid density by distance, as a fraction of the tile's sample count.
///
/// The spec asks for "a resolution appropriate to camera distance". This is the render half of that: a
/// tile twenty tile-widths away contributes a handful of pixels per quad, so drawing it at full density
/// spends triangles on detail no one can see. The *evaluation* half, re-evaluating distant tiles at a
/// coarser sample spacing, is a streaming problem and is listed in docs/todo.md.
///
/// Powers of two, so a coarser grid lands on a subset of the same samples and neighbouring tiles at
/// different levels still meet along their shared edge.
[[nodiscard]] u32_t GridVerticesFor(u32_t full, f32_t distance, f32_t tileSide) {
    const f32_t tiles = tileSide > 1e-6f ? distance / tileSide : 0.0f;
    u32_t       divisor = 1;
    if (tiles > 16.0f) {
        divisor = 8;
    } else if (tiles > 8.0f) {
        divisor = 4;
    } else if (tiles > 4.0f) {
        divisor = 2;
    }
    const u32_t quads = std::max((full - 1) / divisor, 1u);
    return quads + 1;
}

/// Largest tile side the preview accepts.
///
/// Set by the row-by-row interior copy: one `VkBufferCopy` per row, in a fixed-size array so building a
/// preview allocates nothing. 1024 samples at 2 m is a two-kilometre tile, which is well past what a
/// preview wants to evaluate in one go anyway.
constexpr u32_t kMaxPreviewSamples = 1024;

/// One evaluated piece of terrain, resident on the GPU.
///
/// Height and normals live in their own section-pool slots rather than in the evaluator's reusable
/// buffers, because the evaluator reuses one set for every section and the viewer needs them all at once.
struct Tile {
    vulkan::SectionSlot  height;
    vulkan::SectionSlot  normals;
    /// World position of the tile's first sample.
    std::array<f32_t, 3> origin{};
    /// Samples per side.
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

/// The resident preview: the tiles, and the buffers the evaluator used to make them.
struct Preview {
    std::pmr::vector<Tile> tiles{&memory::General().Resource()};
    u32_t                  samplesPerTile = 0;
    f32_t                  resolution     = 1.0f;
    f32_t                  heightMinimum  = 0.0f;
    f32_t                  heightMaximum  = 0.0f;
    /// Centre and radius of everything, for framing the camera.
    Vec3  centre{};
    f32_t radius = 1.0f;

    void Release(vulkan::Context& context) {
        for (Tile& tile : tiles) {
            context.Sections().Release(tile.height);
            context.Sections().Release(tile.normals);
        }
        tiles.clear();
    }
};

/// Evaluates the graph tile by tile into resident slots.
///
/// One submission per tile, waited on before the next: the evaluator's buffers are shared between tiles,
/// so tile k+1 would overwrite what tile k's copy is still reading. Building a preview happens once and
/// on reload, so a few milliseconds of serialization costs nothing worth the complexity of pipelining it.
[[nodiscard]] Result<Preview> BuildPreview(vulkan::Context& context, terrain::KernelLibrary& kernels,
                                           const LoadedGraph&            loaded,
                                           const ViewerLayer::Settings& settings) {
#ifdef TRACY_ENABLE
    ZoneScopedN("viewer: build preview");
#endif
    const u32_t samples = settings.sectionSize;
    if (samples < 2 || samples > kMaxPreviewSamples) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Evaluate,
                    "a preview tile of {} samples is outside [2, {}]", samples,
                    kMaxPreviewSamples);
    }
    const SectionExtent extent{.x = samples, .y = 1, .z = samples};

    Result<CompiledGraph> compiled = terrain::Compile(
        loaded.graph,
        terrain::CompileOptions{.extent     = extent,
                                .resolution = static_cast<f32_t>(settings.resolution)});
    if (!compiled) {
        return std::unexpected(compiled.error());
    }
    Result<terrain::SectionResources> resources =
        terrain::SectionResources::Create(context, *compiled);
    if (!resources) {
        return std::unexpected(resources.error());
    }
    Result<terrain::DispatchTimers> timers =
        terrain::DispatchTimers::Create(context, compiled->stats.dispatchCount);
    if (!timers) {
        return std::unexpected(timers.error());
    }

    const terrain::CompiledOutput& heightOutput  = compiled->outputs[loaded.heightOutput];
    const terrain::CompiledOutput& normalsOutput = compiled->outputs[loaded.normalsOutput];

    // The resident tiles are tight: exactly `samples x samples`, no halo. An output buffer is *not*
    // necessarily tight, which was an assumption worth getting wrong once. A value that some other op
    // reads with a radius carries that halo even when it is also a requested output, and in a script
    // that takes the gradient of its own height the height buffer ends up with the whole erosion chain's
    // halo: 37 samples on each side, a 202x202 buffer where the tile wants 128x128.
    //
    // So the copy takes the interior row by row rather than the buffer wholesale. The alternative is
    // passing the halo to the vertex shader and indexing around it, which spreads the padding into the
    // renderer for no gain: the tiles are built once and read every frame.
    const u32_t heightHalo  = compiled->buffers[heightOutput.buffer].halo;
    const u32_t normalsHalo = compiled->buffers[normalsOutput.buffer].halo;
    const u64_t heightBytes = terrain::ValueSize(heightOutput.mapping, extent, 0);
    const u64_t normalsBytes = terrain::ValueSize(normalsOutput.mapping, extent, 0);

    // Tiles across the previewed square. Integer sample counts, so the tiles meet exactly.
    const i32_t totalSamples =
        static_cast<i32_t>(settings.extent / settings.resolution);
    const i32_t tilesPerSide = std::max(totalSamples / static_cast<i32_t>(samples), 1);
    const i32_t firstSample  = -tilesPerSide * static_cast<i32_t>(samples) / 2;

    Preview preview;
    preview.samplesPerTile = samples;
    preview.resolution     = static_cast<f32_t>(settings.resolution);
    preview.heightMinimum  = loaded.heightMinimum;
    preview.heightMaximum  = loaded.heightMaximum;
    preview.tiles.reserve(static_cast<usize_t>(tilesPerSide) * static_cast<usize_t>(tilesPerSide));

    vulkan::Queue& queue = context.ComputeQueue();

    for (i32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ) {
        for (i32_t tileX = 0; tileX < tilesPerSide; ++tileX) {
            const i32_t originX = firstSample + tileX * static_cast<i32_t>(samples);
            const i32_t originZ = firstSample + tileZ * static_cast<i32_t>(samples);

            Tile tile;
            tile.samples = samples;
            tile.origin  = {static_cast<f32_t>(originX) * preview.resolution, 0.0f,
                            static_cast<f32_t>(originZ) * preview.resolution};

            Result<vulkan::SectionSlot> height = context.Sections().Acquire(
                terrain::ClassOf(heightOutput.mapping, extent, 0), heightBytes);
            if (!height) {
                preview.Release(context);
                return std::unexpected(height.error());
            }
            tile.height = *height;
            Result<vulkan::SectionSlot> normals = context.Sections().Acquire(
                terrain::ClassOf(normalsOutput.mapping, extent, 0), normalsBytes);
            if (!normals) {
                context.Sections().Release(tile.height);
                preview.Release(context);
                return std::unexpected(normals.error());
            }
            tile.normals = *normals;

            Result<VkCommandBuffer> commands = queue.BeginOneShot();
            if (!commands) {
                preview.Release(context);
                return std::unexpected(commands.error());
            }

            const terrain::SectionJob job{.origin = {originX, 0, originZ},
                                          .extent = extent,
                                          .seed   = static_cast<u32_t>(settings.seed)};
            if (Status recorded = terrain::RecordSection(queue, kernels, *compiled, *resources,
                                                         *timers, *commands, job);
                !recorded) {
                preview.Release(context);
                return std::unexpected(recorded.error());
            }

            // Copied rather than aliased: the evaluator's output buffers are reused by the next tile.
            struct CopyPlan {
                const terrain::CompiledOutput* output;
                const vulkan::SectionSlot*     target;
                u32_t                          halo;
            };
            const std::array<CopyPlan, 2> copies{
                {{&heightOutput, &tile.height, heightHalo},
                 {&normalsOutput, &tile.normals, normalsHalo}}};

            for (const CopyPlan& plan : copies) {
                const vulkan::SectionSlot& source     = resources->Slot(plan.output->buffer);
                const u32_t                components = plan.output->mapping.components;
                const u64_t                rowBytes =
                    static_cast<u64_t>(samples) * components * sizeof(f32_t);
                const u64_t sourceStride =
                    static_cast<u64_t>(samples + 2 * plan.halo) * components * sizeof(f32_t);

                vulkan::ComputeToTransferBarrier(*commands, source.buffer, source.offset,
                                                 source.size);

                if (plan.halo == 0) {
                    const VkBufferCopy copy{.srcOffset = source.offset,
                                            .dstOffset = plan.target->offset,
                                            .size      = rowBytes * samples};
                    vkCmdCopyBuffer(*commands, source.buffer, plan.target->buffer, 1, &copy);
                    continue;
                }

                // One region per interior row. `kMaxPreviewSamples` bounds the array so this allocates
                // nothing; a larger tile than that is refused up front.
                std::array<VkBufferCopy, kMaxPreviewSamples> regions{};
                for (u32_t row = 0; row < samples; ++row) {
                    const u64_t sourceRow = static_cast<u64_t>(row + plan.halo) * sourceStride
                                            + static_cast<u64_t>(plan.halo) * components
                                                  * sizeof(f32_t);
                    regions[row] = VkBufferCopy{.srcOffset = source.offset + sourceRow,
                                                .dstOffset = plan.target->offset + row * rowBytes,
                                                .size      = rowBytes};
                }
                vkCmdCopyBuffer(*commands, source.buffer, plan.target->buffer, samples,
                                regions.data());
            }

            Result<u64_t> submitted = queue.EndAndSubmit(*commands);
            if (!submitted) {
                preview.Release(context);
                return std::unexpected(submitted.error());
            }
            if (Status waited = queue.WaitTimeline(*submitted, kGpuTimeoutNanoseconds); !waited) {
                preview.Release(context);
                return std::unexpected(waited.error());
            }

            preview.tiles.push_back(tile);
        }
    }

    const f32_t half = static_cast<f32_t>(tilesPerSide * static_cast<i32_t>(samples)) * 0.5f
                       * preview.resolution;
    preview.centre = Vec3{0.0f, (loaded.heightMinimum + loaded.heightMaximum) * 0.5f, 0.0f};
    preview.radius = std::max(half, 1.0f);

    LOG_INFO("preview ready: {} tile(s) of {} samples at {} m/sample, {} dispatch(es) per tile",
             preview.tiles.size(), samples, preview.resolution, compiled->stats.dispatchCount);
    return preview;
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
    Camera                   camera;
    Input                    input;
    Ui                       ui;
    /// Smoothed, because a per-frame figure flickers too fast to read.
    f32_t framesPerSecond   = 0.0f;
    f32_t frameMilliseconds = 0.0f;

    Preview preview;
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
    Result<Preview> preview =
        BuildPreview(*m_state->context, *m_state->kernels, m_state->current, m_state->settings);
    if (!preview) {
        detail::CopyBounded(m_state->lastError, std::string_view{preview.error().Format().data()});
        m_state->hasError = true;
        LOG_ERROR("{}", m_state->lastError.data());
        App().Stop();
        return;
    }
    m_state->preview = std::move(*preview);
    ++m_state->loadCount;

    m_state->camera.Frame(m_state->preview.centre, m_state->preview.radius);
    m_state->lastModifiedSeconds = FileModifiedSeconds(m_state->settings.script);
    m_state->lastWatchSeconds    = NowSeconds();

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
        m_state->preview.Release(*m_state->context);
    }
    m_state->pipeline = vulkan::GraphicsPipeline{};
}

void ViewerLayer::OnUpdate() {
#ifdef TRACY_ENABLE
    ZoneScopedN("ViewerLayer::OnUpdate");
#endif
    if (m_state == nullptr || m_state->windowLayer == nullptr) {
        return;
    }
    UpdateInput();
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
    panels.tileCount         = static_cast<u32_t>(state.preview.tiles.size());
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
            Result<Preview> preview = BuildPreview(*state.context, *state.kernels, state.pending,
                                                   state.settings);
            if (!preview) {
                detail::CopyBounded(state.lastError,
                                    std::string_view{preview.error().Format().data()});
                state.hasError = true;
                LOG_ERROR("reload compiled but could not be evaluated, keeping the previous "
                          "terrain: {}",
                          state.lastError.data());
            } else {
                // Swapped only once the new preview exists, so a failure anywhere above leaves the old
                // one on screen untouched (spec section 11).
                if (Status idle = state.context->WaitIdle(); !idle) {
                    LOG_WARN("could not wait for the device before swapping the preview: {}",
                             idle.error().Format().data());
                }
                state.preview.Release(*state.context);
                state.preview = std::move(*preview);
                ++state.loadCount;
                state.hasError    = false;
                state.lastError[0] = 0;
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
    if (state.preview.tiles.empty() || !state.pipeline.IsValid()) {
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
    constants.resolution     = state.preview.resolution;
    constants.samples        = state.preview.samplesPerTile;

    const Vec3  eye      = state.camera.Position();
    const f32_t tileSide = static_cast<f32_t>(state.preview.samplesPerTile - 1)
                           * state.preview.resolution;

    b8_t  bound = false;
    u32_t drawn = 0;
    for (const Tile& tile : state.preview.tiles) {
        // The tile's bounding box: its footprint, and the height range the script declared. Using the
        // declared range rather than the real one means the box is never too small, which is the only
        // direction that would cull something visible.
        const f32_t side = static_cast<f32_t>(tile.samples - 1) * state.preview.resolution;
        const std::array<f32_t, 3> minimum{tile.origin[0], state.preview.heightMinimum,
                                           tile.origin[2]};
        const std::array<f32_t, 3> maximum{tile.origin[0] + side, state.preview.heightMaximum,
                                           tile.origin[2] + side};
        if (!frustum.Intersects(minimum, maximum)) {
            continue;
        }

        // Grid density from the distance to the tile's centre, so a far tile costs a fraction of the
        // triangles. The centre rather than the nearest corner: using the nearest point makes the level
        // change as the camera slides along a tile edge, which flickers.
        const Vec3  centre{(minimum[0] + maximum[0]) * 0.5f, (minimum[1] + maximum[1]) * 0.5f,
                          (minimum[2] + maximum[2]) * 0.5f};
        const f32_t distance     = Length(centre - eye);
        const u32_t gridVertices =
            GridVerticesFor(state.settings.gridVertices, distance, tileSide);
        const u32_t quadsPerSide = gridVertices - 1;
        const u32_t vertexCount  = quadsPerSide * quadsPerSide * 6;

        constants.gridVertices = gridVertices;
        constants.tileOrigin   = tile.origin;
        constants.heights      = tile.height.address;
        constants.normals      = tile.normals.address;

        if (!bound) {
            state.pipeline.Bind(frame.commands, frame.extent, constants);
            bound = true;
        } else {
            state.pipeline.Push(frame.commands, constants);
        }
        vkCmdDraw(frame.commands, vertexCount, 1, 0, 0);
        ++drawn;
    }
    state.tilesDrawn = drawn;

    // Panels last, so they sit on top of the terrain. Same render pass, no depth test in the UI
    // pipeline, so ordering inside the pass is all that is needed.
    RecordPanels(frame.commands);
}

u32_t ViewerLayer::TilesDrawn() const noexcept {
    return m_state != nullptr ? m_state->tilesDrawn : 0;
}

u32_t ViewerLayer::TileCount() const noexcept {
    return m_state != nullptr ? static_cast<u32_t>(m_state->preview.tiles.size()) : 0;
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
