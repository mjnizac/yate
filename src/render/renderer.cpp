#include <engine/render/renderer.hpp>
#include <engine/render/tiles.hpp>

#include <engine/application.hpp>
#include <engine/log.hpp>
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
#include <cstring>
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

/// Rings of detail round the camera beyond the finest, each a level coarser: 8 is 256 times the spacing.
constexpr u32_t kMaxRings = 8;
/// The finest level the distance may pick, as the ring of tiles always did; the rings go out from it.
constexpr u32_t kMaxBase = 8;

/// Flags in the high bits of the push constants' `samples`, read by both viewer shaders.
constexpr u32_t kDrawLakes  = 1u << 31;
constexpr u32_t kDrawRivers = 1u << 30;
constexpr u32_t kDrawSea    = 1u << 29;
/// Set when the script gives the sea as an output: the shaders read it in place of the global level.
constexpr u32_t kSeaOutput  = 1u << 28;
/// Set on a tile with a coarser ring outside its own: it hangs a skirt round its edge to close the gap.
constexpr u32_t kSkirt      = 1u << 27;

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

/// Half the side of the ground the ring covers at `level`, in metres: its tiles each way and the one under
/// the focus.
[[nodiscard]] f32_t RingHalf(const ViewerLayer::Settings& settings, u32_t level) {
    const f32_t tileSide = static_cast<f32_t>(settings.sectionSize - 1) * SpacingOf(settings.resolution, level);
    return (static_cast<f32_t>(settings.ringRadius) + 0.5f) * tileSide;
}

/// A script following the camera that ran out of what it may make in one run says `pending` in its
/// report (`Lands.World`'s budget): it is run again at once, and fills the world in a few pieces a run.
[[nodiscard]] b8_t Unfinished(const LoadedGraph& loaded, const ViewerLayer::Settings& settings) {
    return settings.follow && loaded.graph.Report().find("\"pending\":true") != std::string_view::npos;
}

/// What the camera is about, in the script's metres: the map is drawn `scale` times wider than that.
[[nodiscard]] Vec3 WorldFocus(const Camera& camera, f32_t scale) noexcept {
    const Vec3 focus = camera.Focus();
    return Vec3{focus.x / scale, focus.y, focus.z / scale};
}

/// The matrix that draws the map `scale` times wider in x and z (heights unchanged).
[[nodiscard]] Mat4 MapScale(f32_t scale) noexcept {
    Mat4 result    = Mat4::Identity();
    result.m[0][0] = scale;
    result.m[2][2] = scale;
    return result;
}

/// Half the plan view's height over the camera's distance: tan of the orbit camera's half field of view
/// (60 degrees), so switching to the plan view keeps about the same ground on screen.
constexpr f32_t kPlanTangent = 0.57735027f;

/// The plan view: orthographic and straight down, +x to the right and +z down the screen (as rows run in
/// an exported heightmap), `half` metres from `centre` to the top edge. Depth runs over the heights `low`
/// to `high`, reversed like the perspective one: the highest ground is nearest.
[[nodiscard]] Mat4 PlanProjection(Vec3 centre, f32_t half, f32_t aspect, f32_t low, f32_t high) noexcept {
    const f32_t width = half * aspect;
    Mat4        result;
    result.m[0][0] = 1.0f / width;
    result.m[3][0] = -centre.x / width;
    result.m[2][1] = 1.0f / half;
    result.m[3][1] = -centre.z / half;
    result.m[1][2] = 1.0f / (high - low);
    result.m[3][2] = -low / (high - low);
    result.m[3][3] = 1.0f;
    return result;
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
///
/// `detail` asks for that many levels finer than the heuristic picks, to inspect a terrain close up
/// without the mesh rounding its edges; the ring then covers less ground, which `--ring` makes up for.
[[nodiscard]] u32_t IdealLevel(f32_t distance, u32_t samples, f64_t baseResolution, u32_t maxLevel,
                               u32_t detail) {
    const f32_t wanted = distance * 0.5f / static_cast<f32_t>(std::max(samples, 1u));
    const f32_t ratio  = wanted / std::max(static_cast<f32_t>(baseResolution), 1e-6f);
    if (!(ratio > 1.0f)) {
        return 0;
    }
    const u32_t level = static_cast<u32_t>(std::log2(ratio));
    return std::min(level > detail ? level - detail : 0u, maxLevel);
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

/// Water colour and opacity as RGBA8 in the byte order `unpackUnorm4x8` reads. Opacity is the alpha,
/// and an alpha of zero is how the shaders know there is no water.
[[nodiscard]] u32_t PackWater(const ViewerLayer::Water& water) noexcept {
    const auto byte = [](f32_t value) {
        return static_cast<u32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return byte(water.colour[0]) | (byte(water.colour[1]) << 8) | (byte(water.colour[2]) << 16)
           | (byte(water.opacity) << 24);
}

} // namespace

struct ViewerLayer::State {
    Settings              settings;
    std::array<char, 512> scriptPath{};
    std::array<char, 512> shapesPath{};
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
    /// Open water for the pixels no terrain covers. Only drawn when water is on.
    vulkan::GraphicsPipeline seaPipeline;
    /// Triangle indices of the one grid every tile is drawn with: two triangles per quad, wound
    /// counter-clockwise seen from above (which with the pipeline's front face and cull mode makes the
    /// terrain visible from above and culled from below).
    vulkan::Buffer indices;
    u32_t          indexCount = 0;
    /// One `ViewerGlobals` per frame in flight, host-visible: a frame writes its own while the GPU may
    /// still read the other's.
    vulkan::Buffer globals;
    b8_t                     wireframe = false;
    Camera                   camera;
    Input                    input;
    Ui                       ui;
    /// Smoothed, because a per-frame figure flickers too fast to read.
    f32_t framesPerSecond   = 0.0f;
    f32_t frameMilliseconds = 0.0f;

    Terrain   terrain;
    /// The coarser rings' terrains (index = level, 0 unused: that is `terrain`), compiled from the live
    /// graph the first time a ring needs one and dropped at every reload.
    std::array<Terrain, kMaxBase + kMaxRings + 1> rings;
    /// Which of `current` and `pending` holds the graph on screen; a reload loads into the other.
    LoadedGraph* live = nullptr;
    LoadedGraph* building = nullptr;
    TileCache cache;
    /// Level the ring is at, and how long the ideal level has disagreed with it.
    u32_t level             = 0;
    /// The level the next (or the running) background build is for. Equal to `level` for a plain reload.
    u32_t levelTarget       = 0;
    u32_t pendingLevel      = 0;
    u32_t disagreeingFrames = 0;
    /// The ground the script was last given as `params.bounds`: the ring round the focus, at the level
    /// it was loaded for. A focus that wanders a quarter of it away reloads, so a world built only round
    /// its bounds (an unbounded one, say) keeps up with the camera.
    f32_t boundsCentre[2] = {0.0f, 0.0f};
    f32_t boundsHalf      = 0.0f;
    f32_t pendingCentre[2] = {0.0f, 0.0f};
    /// Tiles missing from the ring at the end of the last streaming pass. Zero means the ring is
    /// complete, which is what a test can wait for.
    u32_t missingTiles = 0;
    /// Kept so a reload can be compiled against the same node storage without reallocating it.
    LoadedGraph current;
    LoadedGraph pending;

    Reload reload;

    u64_t loadCount  = 0;
    /// What a tile has been costing to evaluate, smoothed: how many fit in a frame's streaming time.
    f64_t secondsPerTile = 0.002;
    u32_t tilesDrawn = 0;

    /// What a run cost, logged when the viewer closes (`--frames` makes that a benchmark): frames and
    /// their wall time once the loop runs, the tiles evaluated and the time spent on them, and how long
    /// until the ring was first complete.
    struct Stats {
        f64_t started        = 0.0;
        u64_t frames         = 0;
        f64_t worstFrame     = 0.0;
        f64_t lastFrame      = 0.0;
        u64_t tiles          = 0;
        f64_t tileSeconds    = 0.0;
        f64_t completeAfter  = -1.0;
    } stats;

    std::array<char, Error::kMaxFormat> lastError{};
    b8_t                                hasError = false;

    f64_t lastWatchSeconds    = 0.0;
    f64_t lastModifiedSeconds = 0.0;
    f64_t pendingSinceSeconds = 0.0;
    b8_t  reloadRequested     = false;
};


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
    detail::CopyBounded(m_state->shapesPath, settings.shapes);
    m_state->settings.shapes = std::string_view{m_state->shapesPath.data()};
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
        App().AddExit(ExitCode::FailedToInitializeEngine);
        App().Stop();
        return;
    }
    ApplicationState& state = StateOf(App());
    if (state.window == nullptr || state.swapchain == nullptr) {
        LOG_ERROR("ViewerLayer needs a window; push a WindowLayer first");
        App().AddExit(ExitCode::FailedToInitializeEngine);
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
        App().AddExit(ExitCode::FailedToInitializeEngine);
        App().Stop();
        return;
    }

    Result<vulkan::GraphicsPipeline> pipeline = vulkan::GraphicsPipeline::Create(
        m_state->context->Device(), m_state->swapchain->Pass().Handle(), "viewer/terrain.vert.spv",
        "viewer/terrain.frag.spv", m_state->context->Pipelines().Handle(), vulkan::PipelineKind::Terrain,
        m_state->swapchain->Pass().Samples());
    if (!pipeline) {
        LOG_ERROR("could not create the terrain pipeline: {}", pipeline.error().Format().data());
        App().AddExit(ExitCode::FailedToInitializeEngine);
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
            m_state->context->Pipelines().Handle(), vulkan::PipelineKind::TerrainWire,
            m_state->swapchain->Pass().Samples());
               wire) {
        m_state->wirePipeline = std::move(*wire);
    } else {
        LOG_WARN("wireframe is unavailable: {}", wire.error().Format().data());
    }

    if (Result<vulkan::GraphicsPipeline> sea = vulkan::GraphicsPipeline::Create(
            m_state->context->Device(), m_state->swapchain->Pass().Handle(),
            "viewer/water.vert.spv", "viewer/water.frag.spv",
            m_state->context->Pipelines().Handle(), vulkan::PipelineKind::Sea,
            m_state->swapchain->Pass().Samples());
        sea) {
        m_state->seaPipeline = std::move(*sea);
    } else {
        LOG_WARN("open water is unavailable: {}", sea.error().Format().data());
    }

    {
        // One grid for every tile, so one index buffer. Host-visible: it is a few hundred kilobytes written once.
        // A ring of skirt vertices round the grid (see `viewer/terrain.vert.glsl`), so two more a side.
        const u32_t vertices = std::min(m_state->settings.gridVertices, m_state->settings.sectionSize) + 2;
        const u32_t quads    = vertices - 1;
        std::pmr::vector<u32_t> data(&memory::General().Resource());
        data.reserve(static_cast<usize_t>(quads) * quads * 6);
        for (u32_t z = 0; z < quads; ++z) {
            for (u32_t x = 0; x < quads; ++x) {
                const u32_t a = z * vertices + x;
                const u32_t b = a + vertices;
                for (const u32_t corner : {a, b, a + 1, a + 1, b, b + 1}) {
                    data.push_back(corner);
                }
            }
        }
        const VkDeviceSize bytes = data.size() * sizeof(u32_t);
        Result<vulkan::Buffer> indices = m_state->context->Memory().CreateBuffer(vulkan::BufferDesc{
            .size = bytes, .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            .category = vulkan::VramCategory::Viewer, .hostVisible = true});
        if (!indices) {
            LOG_ERROR("could not create the grid indices: {}", indices.error().Format().data());
            App().AddExit(ExitCode::FailedToInitializeEngine);
            App().Stop();
            return;
        }
        m_state->indices    = *indices;
        m_state->indexCount = static_cast<u32_t>(data.size());
        std::memcpy(m_state->indices.mapped, data.data(), bytes);
        if (Status flushed = m_state->context->Memory().FlushBuffer(m_state->indices, 0, bytes); !flushed) {
            LOG_WARN("could not flush the grid indices: {}", flushed.error().Format().data());
        }
        Result<vulkan::Buffer> globals = m_state->context->Memory().CreateBuffer(vulkan::BufferDesc{
            .size = sizeof(vulkan::ViewerGlobals) * vulkan::kFramesInFlight, .usage = 0,
            .category = vulkan::VramCategory::Viewer, .hostVisible = true});
        if (!globals) {
            LOG_ERROR("could not create the viewer globals: {}", globals.error().Format().data());
            App().AddExit(ExitCode::FailedToInitializeEngine);
            App().Stop();
            return;
        }
        m_state->globals = *globals;
    }

    // The camera is framed and the level chosen *before* the script runs, because the script is given
    // the level's sample spacing and `IdealLevel` needs only the camera, not the graph.
    //
    // `startRadius` is the radius of the ring *at level 0*, and it is used as a distance heuristic, not
    // as a claim about the ring that ends up on screen: the level this picks is usually above 0, where
    // the ring is several times larger than what the camera frames. That is by design. `IdealLevel`
    // wants a tile about half the viewing distance across, so the ring is always bigger than the view,
    // and there is no self-consistent "frame the whole ring" to converge to. Trying it anyway raises
    // the level by two on every pass until it hits `maxLevel`.
    const f32_t startRadius = static_cast<f32_t>(m_state->settings.sectionSize)
                              * static_cast<f32_t>(m_state->settings.resolution)
                              * static_cast<f32_t>(m_state->settings.ringRadius + 1);
    m_state->camera.Frame(Vec3{0.0f, 0.0f, 0.0f}, startRadius);
    if (const ViewerLayer::View& view = m_state->settings.view; view.enabled) {
        constexpr f32_t kDegrees = 3.14159265358979f / 180.0f;
        m_state->camera.SetView(Vec3{view.target[0], view.target[1], view.target[2]},
                                view.yaw * kDegrees, view.pitch * kDegrees, view.distance);
    } else if (const Settings& s = m_state->settings; s.plan && s.bounded) {
        // A plan view of a map starts with the whole map on screen, centred.
        const VkExtent2D extent = m_state->swapchain->Extent();
        const f32_t aspect = extent.height != 0 ? static_cast<f32_t>(extent.width) / static_cast<f32_t>(extent.height) : 1.0f;
        const f32_t half   = std::max(s.bounds[3] - s.bounds[1], (s.bounds[2] - s.bounds[0]) / aspect) * 0.5f * s.mapScale;
        m_state->camera.SetView(Vec3{(s.bounds[0] + s.bounds[2]) * 0.5f * s.mapScale, 0.0f,
                                     (s.bounds[1] + s.bounds[3]) * 0.5f * s.mapScale},
                                0.0f, -1.55f, half / kPlanTangent);
    }
    // The script runs once, at the finest spacing; the rings, from the level the distance picks out,
    // compile the same graph at theirs.
    m_state->level       = IdealLevel(m_state->camera.ViewDistance(0.0f) / m_state->settings.mapScale,
                                      m_state->settings.sectionSize, m_state->settings.resolution, kMaxBase,
                                      m_state->settings.detail);
    m_state->levelTarget = m_state->level;
    m_state->boundsCentre[0] = WorldFocus(m_state->camera, m_state->settings.mapScale).x;
    m_state->boundsCentre[1] = WorldFocus(m_state->camera, m_state->settings.mapScale).z;
    m_state->boundsHalf      = RingHalf(m_state->settings, m_state->level);

    // First load is synchronous: there is nothing to keep rendering yet, so there is no reason to defer
    // it and every reason to fail here rather than after the window has opened on an empty scene.
    if (Status loaded = LoadScript(m_state->current, m_state->settings.script, m_state->settings.shapes,
                                   m_state->settings.seed, m_state->params,
                                   SpacingOf(m_state->settings.resolution, 0),
                                   m_state->boundsCentre, m_state->boundsHalf);
        !loaded) {
        detail::CopyBounded(m_state->lastError, std::string_view{loaded.error().Format().data()});
        m_state->hasError = true;
        LOG_ERROR("{}", m_state->lastError.data());
        App().AddExit(ExitCode::FailedToLoadScript);
        App().Stop();
        return;
    }
    Result<Terrain> terrain = BuildTerrain(*m_state->context, m_state->current, m_state->settings,
                                           0);
    if (!terrain) {
        detail::CopyBounded(m_state->lastError, std::string_view{terrain.error().Format().data()});
        m_state->hasError = true;
        LOG_ERROR("{}", m_state->lastError.data());
        App().AddExit(ExitCode::FailedToCompileGraph);
        App().Stop();
        return;
    }
    m_state->terrain = std::move(*terrain);
    m_state->live    = &m_state->current;
    m_state->reloadRequested = Unfinished(*m_state->live, m_state->settings);
    ++m_state->loadCount;

    // The first ring is filled before the window opens, so the viewer never shows a hole it could have
    // avoided. After that, streaming is bounded per frame.
    const f64_t fillStart = NowSeconds();
    StreamTiles(~0u);
    m_state->stats.tiles       = m_state->cache.evaluated;
    m_state->stats.tileSeconds = NowSeconds() - fillStart;

    m_state->lastModifiedSeconds =
        FileModifiedSeconds(m_state->settings.script) + FileModifiedSeconds(m_state->settings.shapes);
    m_state->lastWatchSeconds    = NowSeconds();

    // Before the UI: its backend installs a scroll callback that chains to whatever was there first, so
    // installing this one afterwards would replace it and the panels would stop scrolling.
    if (Status attached = m_state->input.Attach(*m_state->window); !attached) {
        LOG_WARN("the scroll wheel is unavailable: {}", attached.error().Format().data());
    }

    if (Status ui = m_state->ui.Initialize(*m_state->context, *m_state->window,
                                           m_state->swapchain->Pass().Handle(),
                                           static_cast<u32_t>(m_state->swapchain->ImageCount()),
                                           m_state->swapchain->Pass().Samples());
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
    if (const State::Stats& stats = m_state->stats; stats.frames > 1) {
        const f64_t wall = stats.lastFrame - stats.started;
        LOG_INFO("viewer stats: {} frame(s) in {:.2f} s, {:.2f} ms a frame, worst {:.1f} ms; {} tile(s) in "
                 "{:.3f} s, {:.2f} ms a tile; ring complete after {:.2f} s; {} tile(s) resident",
                 stats.frames, wall, 1000.0 * wall / static_cast<f64_t>(stats.frames - 1),
                 1000.0 * stats.worstFrame, stats.tiles, stats.tileSeconds,
                 stats.tiles > 0 ? 1000.0 * stats.tileSeconds / static_cast<f64_t>(stats.tiles) : 0.0,
                 stats.completeAfter, m_state->cache.tiles.size());
    }

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
    if (m_state->context != nullptr && m_state->indices.IsValid()) {
        m_state->context->Memory().DestroyBuffer(m_state->indices);
    }
    if (m_state->context != nullptr && m_state->globals.IsValid()) {
        m_state->context->Memory().DestroyBuffer(m_state->globals);
    }
    m_state->pipeline     = vulkan::GraphicsPipeline{};
    m_state->wirePipeline = vulkan::GraphicsPipeline{};
    m_state->seaPipeline  = vulkan::GraphicsPipeline{};
}

void ViewerLayer::OnUpdate() {
#ifdef TRACY_ENABLE
    ZoneScopedN("ViewerLayer::OnUpdate");
#endif
    if (m_state == nullptr || m_state->windowLayer == nullptr) {
        return;
    }
    State::Stats& stats = m_state->stats;
    const f64_t   now   = NowSeconds();
    if (stats.frames == 0) {
        stats.started = now;
    } else {
        stats.worstFrame = std::max(stats.worstFrame, now - stats.lastFrame);
    }
    stats.lastFrame = now;
    ++stats.frames;

    UpdateInput();
    if (m_state->settings.spin != 0.0f) {
        m_state->camera.Spin(m_state->settings.spin * 3.14159265358979f / 180.0f);
    }
    if (m_state->settings.drift != 0.0f) {
        const Vec3 eye = m_state->camera.Position();
        const f32_t step = m_state->settings.drift * static_cast<f32_t>(m_state->windowLayer->DeltaSeconds());
        m_state->camera.PlaceAt(Vec3{eye.x + step, eye.y, eye.z});
    }
    UpdateLevel();
    // The bound, then as many more as fit in `streamMilliseconds` of the frame: a light script fills a ring
    // of hundreds at once, a heavy one never stalls a frame for it.
    const u64_t before = m_state->cache.evaluated;
    const f64_t streamStart = NowSeconds();
    StreamTiles(m_state->settings.tilesPerFrame,
                streamStart + 0.001 * static_cast<f64_t>(m_state->settings.streamMilliseconds));
    if (m_state->cache.evaluated != before) {
        stats.tiles += m_state->cache.evaluated - before;
        stats.tileSeconds += NowSeconds() - streamStart;
    }
    if (stats.completeAfter < 0.0 && m_state->missingTiles == 0 && !m_state->reload.running.load()) {
        stats.completeAfter = NowSeconds() - stats.started;
    }
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
    state.camera.Update(static_cast<f32_t>(delta));
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
    if (!state.ui.IsValid() || !state.settings.panels) {
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
    panels.waterEnabled      = state.settings.water.enabled;
    panels.waterSea          = state.settings.water.sea;
    panels.waterLakes        = state.settings.water.lakes;
    panels.waterRivers       = state.settings.water.rivers;
    panels.waterLevel        = state.settings.water.level;
    panels.waterColour       = state.settings.water.colour;
    panels.waterOpacity      = state.settings.water.opacity;
    panels.resolution        = static_cast<f32_t>(state.settings.resolution);
    panels.maxLevel          = static_cast<i32_t>(state.settings.maxLevel);
    panels.ringRadius        = static_cast<i32_t>(state.settings.ringRadius);
    panels.mapScale          = state.settings.mapScale;
    panels.sun               = state.settings.sun;
    panels.plan              = state.settings.plan;

    // Editing a parameter reloads: the script is what turns a parameter into terrain, so there is no
    // shortcut that updates push constants instead. The spec's "only numeric parameters changed, so no
    // pipeline is created" holds anyway, because the pipeline map is keyed on op and variant and a
    // changed number produces the same variants.
    if (state.ui.Draw(panels)) {
        RequestReload();
    }
    state.settings.water.enabled = panels.waterEnabled;
    state.settings.water.sea     = panels.waterSea;
    state.settings.water.lakes   = panels.waterLakes;
    state.settings.water.rivers  = panels.waterRivers;
    state.settings.water.level   = panels.waterLevel;
    state.settings.water.colour  = panels.waterColour;
    state.settings.water.opacity = panels.waterOpacity;
    state.settings.resolution    = panels.resolution;
    state.settings.maxLevel      = static_cast<u32_t>(panels.maxLevel);
    state.settings.ringRadius    = static_cast<u32_t>(panels.ringRadius);
    state.settings.sun           = panels.sun;
    state.settings.plan          = panels.plan;
    if (panels.mapScale != state.settings.mapScale) {
        // The camera keeps looking at the same ground: what it is about is moved with the map.
        state.camera.ScaleGround(panels.mapScale / state.settings.mapScale);
        state.settings.mapScale = panels.mapScale;
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
            state.levelTarget = state.level;
        } else {
            Result<Terrain> terrain =
                BuildTerrain(*state.context, *state.building, state.settings, 0);
            if (!terrain) {
                detail::CopyBounded(state.lastError,
                                    std::string_view{terrain.error().Format().data()});
                state.hasError = true;
                LOG_ERROR("reload compiled but could not be prepared, keeping the previous "
                          "terrain: {}",
                          state.lastError.data());
                state.levelTarget = state.level;
            } else {
                // Swapped only once the new terrain exists, so a failure anywhere above leaves the old
                // one on screen untouched (spec section 11).
                if (Status idle = state.context->WaitIdle(); !idle) {
                    LOG_WARN("could not wait for the device before swapping the terrain: {}",
                             idle.error().Format().data());
                }
                // Following the camera, the old tiles stay on screen, stale, and the ring is refreshed
                // within each frame's streaming time: a reload every few seconds must not stop the picture
                // for the quarter of a second a whole ring takes. Any other reload is a deliberate act,
                // refilled at once, since a half-built ring after it would look like a failure.
                const b8_t gradual = state.settings.follow;
                if (gradual) {
                    ++state.cache.generation;
                } else {
                    state.cache.Release(*state.context);
                }
                state.terrain = std::move(*terrain);
                state.live    = state.building;
                // The coarser rings are compiled again from the new graph when they are next needed.
                for (Terrain& ring : state.rings) {
                    ring = Terrain{};
                }
                state.boundsCentre[0] = state.pendingCentre[0];
                state.boundsCentre[1] = state.pendingCentre[1];
                state.boundsHalf      = RingHalf(state.settings, state.level);
                ++state.loadCount;
                state.hasError     = false;
                state.lastError[0] = 0;
                if (!gradual) {
                    const u64_t refilled    = state.cache.evaluated;
                    const f64_t refillStart = NowSeconds();
                    StreamTiles(~0u);
                    state.stats.tiles += state.cache.evaluated - refilled;
                    state.stats.tileSeconds += NowSeconds() - refillStart;
                }
                LOG_INFO("script reloaded ({} load(s))", state.loadCount);
                state.reloadRequested = state.reloadRequested || Unfinished(*state.live, state.settings);
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
        // The shapes file is watched with the script: a change to either changes the terrain.
        const f64_t modified = FileModifiedSeconds(state.settings.script)
                               + FileModifiedSeconds(state.settings.shapes);
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

    state.pendingLevel     = 0;
    // Loaded into the graph not on screen, which keeps drawing until the new one is built.
    state.building         = state.live == &state.current ? &state.pending : &state.current;
    state.pendingCentre[0] = WorldFocus(state.camera, state.settings.mapScale).x;
    state.pendingCentre[1] = WorldFocus(state.camera, state.settings.mapScale).z;
    state.reload.running.store(true);
    state.reload.failed = false;
    // The worker gets its own copy of the parameters: the panel may edit them on this thread meanwhile.
    state.reload.worker = std::thread([&state, params = state.params]() {
        platform::SetThreadName("engine-reload");
        // Emptied every time, so a script that fails halfway cannot leave half its nodes behind.
        state.building->Reset();
        if (Status loaded = LoadScript(*state.building, state.settings.script, state.settings.shapes,
                                       state.settings.seed, params,
                                       SpacingOf(state.settings.resolution, 0),
                                       state.pendingCentre, RingHalf(state.settings, state.pendingLevel));
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
    // The camera works in the drawn map; the tiles and the frustum test in the script's metres. The plan
    // view looks down on the camera's focus from above every height the script declared, with margin for
    // ground past the range and the skirts.
    const f32_t span  = state.terrain.heightMaximum - state.terrain.heightMinimum + 100.0f;
    const f32_t low   = state.terrain.heightMinimum - span;
    const f32_t high  = state.terrain.heightMaximum + span;
    const Vec3  focus = state.camera.Focus();
    const f32_t half  = state.camera.ViewDistance(0.5f * (low + high)) * kPlanTangent;
    const Mat4  projection = state.settings.plan ? PlanProjection(focus, half, aspect, low, high)
                                                 : state.camera.ViewProjection(aspect);
    const Mat4    viewProjection = projection * MapScale(state.settings.mapScale);
    const Frustum frustum        = FrustumOf(viewProjection);

    // Water is shaded by the terrain fragments themselves (see `terrain.frag.glsl`). A wireframe draws
    // none: its fragments are lines, and a tinted line says nothing about where the surface is.
    const ViewerLayer::Water& water = state.settings.water;
    const b8_t shadeWater           = water.enabled && !state.wireframe;
    const b8_t seaOutput            = state.terrain.waterOutputs[2] != kNoOutput;
    // In the plan view the water is looked at from straight above the focus.
    const Vec3 drawnEye = state.settings.plan ? Vec3{focus.x, high + half, focus.z} : state.camera.Position();
    const Vec3 eye{drawnEye.x / state.settings.mapScale, drawnEye.y, drawnEye.z / state.settings.mapScale};

    vulkan::TerrainPushConstants constants{};
    constants.viewProjection = viewProjection.m;
    constants.samples        = state.terrain.samples | (water.sea ? kDrawSea : 0u)
                        | (water.lakes ? kDrawLakes : 0u) | (water.rivers ? kDrawRivers : 0u)
                        | (seaOutput ? kSeaOutput : 0u);
    constants.look           = state.terrain.look;
    constants.waterLevel     = water.level;
    constants.waterColour    = shadeWater ? PackWater(water) : 0u;

    // This frame's globals, in its own slot of the buffer.
    constexpr f32_t      kDegrees = 3.14159265358979f / 180.0f;
    const f32_t          azimuth  = state.settings.sun[0] * kDegrees;
    const f32_t          altitude = state.settings.sun[1] * kDegrees;
    vulkan::ViewerGlobals globals{
        .eye = {eye.x, eye.y, eye.z},
        .sun = {std::sin(azimuth) * std::cos(altitude), std::sin(altitude), -std::cos(azimuth) * std::cos(altitude)}};
    const VkDeviceSize slot = sizeof(vulkan::ViewerGlobals) * frame.frameIndex;
    std::memcpy(static_cast<u8_t*>(state.globals.mapped) + slot, &globals, sizeof(globals));
    if (Status flushed = state.context->Memory().FlushBuffer(state.globals, slot, sizeof(globals)); !flushed) {
        LOG_WARN("could not flush the viewer globals: {}", flushed.error().Format().data());
    }
    constants.globals = state.globals.address + slot;

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
        if (state.settings.cull && !frustum.Intersects(minimum, maximum)) {
            continue;
        }

        // One grid density for every tile, for the same reason there is one evaluation level: two
        // densities meeting along a shared edge is a T-junction. The coarse side spans its edge with a
        // chord between every nth sample while the fine side follows each one, so the surfaces part
        // company and the background shows through as a hairline crack. Distance is already handled,
        // and handled once, by the level that chooses the sample spacing.
        const u32_t gridVertices = std::min(state.settings.gridVertices, tile.samples);
        const u32_t quadsPerSide = gridVertices - 1;
        // The grid and its skirt: one more quad a side.
        const u32_t vertexCount  = (quadsPerSide + 2) * (quadsPerSide + 2) * 6;

        constants.resolution   = tile.spacing;
        constants.samples      = (constants.samples & ~kSkirt)
                            | (tile.key.level < state.level + std::min(state.settings.maxLevel, kMaxRings) ? kSkirt : 0u);
        constants.gridVertices = gridVertices;
        constants.tileOrigin   = tile.origin;
        constants.heights      = tile.height.address;
        constants.normals      = tile.normals.address;

        if (!bound) {
            pipeline.Bind(frame.commands, frame.extent, constants);
            vkCmdBindIndexBuffer(frame.commands, state.indices.handle, 0, VK_INDEX_TYPE_UINT32);
            bound = true;
        } else {
            pipeline.Push(frame.commands, constants);
        }
        vkCmdDrawIndexed(frame.commands, vertexCount, 1, 0, 0, 0);
        ++drawn;
    }
    state.tilesDrawn = drawn;

    // Open water covers the ring where no terrain was drawn: holes while streaming, and the part of a
    // tile the height range culled. One quad over the footprint of every resident tile, drawn after the
    // terrain; the depth test, not the draw order, is what keeps it behind the terrain (see
    // `viewer/water.vert.glsl`).
    // Zero opacity is no water at all, the open sea included: drawn anyway it left a blue plate at the
    // edge of the ring that the terrain shading, skipped at alpha 0, did not explain.
    // A script that says where its sea is has no open sea beyond the ring: the sea is where it put it.
    if (shadeWater && water.sea && !seaOutput && water.opacity > 0.0f && state.seaPipeline.IsValid()) {
        f32_t minX = std::numeric_limits<f32_t>::max();
        f32_t minZ = minX;
        f32_t maxX = std::numeric_limits<f32_t>::lowest();
        f32_t maxZ = maxX;
        for (const Tile& tile : state.cache.tiles) {
            const f32_t side = static_cast<f32_t>(tile.samples - 1) * tile.spacing;
            minX             = std::min(minX, tile.origin[0]);
            minZ             = std::min(minZ, tile.origin[2]);
            maxX             = std::max(maxX, tile.origin[0] + side);
            maxZ             = std::max(maxZ, tile.origin[2] + side);
        }
        vulkan::SeaPushConstants sea{};
        sea.viewProjection = viewProjection.m;
        sea.eye            = globals.eye;
        sea.level          = water.level;
        sea.centre         = {(minX + maxX) * 0.5f, (minZ + maxZ) * 0.5f};
        sea.halfExtent     = {(maxX - minX) * 0.5f, (maxZ - minZ) * 0.5f};
        sea.colour         = constants.waterColour;
        state.seaPipeline.Bind(frame.commands, frame.extent, sea);
        vkCmdDraw(frame.commands, 6, 1, 0, 0);
    }

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
void ViewerLayer::StreamTiles(u32_t budget, f64_t deadline) {
#ifdef TRACY_ENABLE
    ZoneScopedN("viewer: stream tiles");
#endif
    State& state = *m_state;
    if (!state.terrain.valid) {
        return;
    }

    // Rings of detail, a geometry clipmap of tiles: the coarsest level (`maxLevel`) covers the square of
    // `ringRadius` tiles each way round the focus; below it, each level is drawn exactly over the children
    // of the coarser level's middle (its tiles within `inner` of its centre), so the levels never overlap
    // and the detail doubles towards the camera. A tile of level l covers four of level l - 1, since both
    // the spacing and the side double. Every level evaluates the one graph the script built, at its own
    // spacing, so moving between rings never re-runs the script.
    const i32_t radius = static_cast<i32_t>(state.settings.ringRadius);
    const i32_t inner  = std::max(radius / 2, 1);
    // The finest level is the one the distance picks (`state.level`), the rings go out from it.
    const u32_t base   = state.level;
    const u32_t top    = base + std::min(state.settings.maxLevel, kMaxRings);
    // Centred on what the camera is about rather than on the eye: see `Camera::Focus`.
    const Vec3 focus = WorldFocus(state.camera, state.settings.mapScale);
    const f32_t side0 = static_cast<f32_t>(state.terrain.samples - 1) * state.terrain.spacing;

    // The tile the focus is over at each level. Floor division, not truncation: truncating rounds
    // towards zero and would make the ring jump by one tile every time it crossed an axis.
    const auto floorDiv = [](f32_t value, f32_t size) {
        return static_cast<i32_t>(std::floor(value / size));
    };
    std::array<std::array<i32_t, 2>, kMaxBase + kMaxRings + 1> centre{};
    for (u32_t level = base; level <= top; ++level) {
        const f32_t side = side0 * static_cast<f32_t>(1u << level);
        centre[level]    = {floorDiv(focus.x, side), floorDiv(focus.z, side)};
    }
    const auto halve = [](i32_t value) { return value >= 0 ? value / 2 : -((1 - value) / 2); };
    // Whether a tile is drawn: inside its level's area, and not in the middle a finer level covers.
    const auto drawn = [&](const TileKey& key) {
        if (key.level > top || key.level < base) {
            return false;
        }
        const u32_t level = key.level;
        if (state.settings.bounded) {
            const f32_t side = side0 * static_cast<f32_t>(1u << level);
            const auto& b    = state.settings.bounds;
            if (static_cast<f32_t>(key.x) * side >= b[2] || static_cast<f32_t>(key.x + 1) * side <= b[0]
                || static_cast<f32_t>(key.z) * side >= b[3] || static_cast<f32_t>(key.z + 1) * side <= b[1]) {
                return false;
            }
        }
        b8_t inside;
        if (level == top) {
            inside = std::abs(key.x - centre[level][0]) <= radius && std::abs(key.z - centre[level][1]) <= radius;
        } else {
            inside = std::abs(halve(key.x) - centre[level + 1][0]) <= inner
                     && std::abs(halve(key.z) - centre[level + 1][1]) <= inner;
        }
        const b8_t middle = level > base && std::abs(key.x - centre[level][0]) <= inner
                            && std::abs(key.z - centre[level][1]) <= inner;
        return inside && !middle;
    };

    // Evict what is no longer wanted. Done before evaluating, so the slots come back to the pool in time
    // to be reused by the tiles that replace them.
    for (usize_t i = state.cache.tiles.size(); i-- > 0;) {
        if (drawn(state.cache.tiles[i].key)) {
            continue;
        }
        state.context->Sections().Release(state.cache.tiles[i].height);
        state.context->Sections().Release(state.cache.tiles[i].normals);
        state.cache.tiles.erase(state.cache.tiles.begin() + static_cast<isize_t>(i));
    }

    // Evaluate what is missing, the finest level first and each nearest first, so a hole appears at the
    // edge of the view rather than under the camera.
    u32_t missing = 0;
    u32_t spent   = 0;
    // As many tiles as the bound, or as fit in what is left of the frame's time at what a tile has been costing.
    const f64_t left    = deadline - NowSeconds();
    const u32_t allowed = budget == ~0u ? ~0u
                          : std::max(budget, static_cast<u32_t>(std::max(left, 0.0) / std::max(state.secondsPerTile, 1e-5)));
    std::pmr::vector<TileKey> batch(&memory::General().Resource());
    for (u32_t level = base; level <= top; ++level) {
        Terrain* terrain = level == 0 ? &state.terrain : &state.rings[level];
        if (!terrain->valid) {
            Result<Terrain> built = BuildTerrain(*state.context, *state.live, state.settings, level);
            if (!built) {
                detail::CopyBounded(state.lastError, std::string_view{built.error().Format().data()});
                state.hasError = true;
                LOG_ERROR("could not prepare detail ring {}: {}", level, state.lastError.data());
                state.missingTiles = missing + 1;
                return;
            }
            *terrain = std::move(*built);
        }
        // The candidates: the level's own square at the top, else the children of the coarser middle.
        i32_t x0, z0, x1, z1;
        if (level == top) {
            x0 = centre[level][0] - radius;
            x1 = centre[level][0] + radius;
            z0 = centre[level][1] - radius;
            z1 = centre[level][1] + radius;
        } else {
            x0 = 2 * (centre[level + 1][0] - inner);
            x1 = 2 * (centre[level + 1][0] + inner) + 1;
            z0 = 2 * (centre[level + 1][1] - inner);
            z1 = 2 * (centre[level + 1][1] + inner) + 1;
        }
        const i32_t reach = std::max({centre[level][0] - x0, x1 - centre[level][0], centre[level][1] - z0,
                                      z1 - centre[level][1]});
        for (i32_t ring = 0; ring <= reach; ++ring) {
            for (i32_t z = centre[level][1] - ring; z <= centre[level][1] + ring; ++z) {
                for (i32_t x = centre[level][0] - ring; x <= centre[level][0] + ring; ++x) {
                    // Only the new border of this ring; the inside was covered by a smaller one.
                    if (ring != 0 && std::abs(x - centre[level][0]) != ring && std::abs(z - centre[level][1]) != ring) {
                        continue;
                    }
                    const TileKey key{.x = x, .z = z, .level = level};
                    if (x < x0 || x > x1 || z < z0 || z > z1 || !drawn(key) || state.cache.Holds(key)) {
                        continue;
                    }
                    batch.push_back(key);
                }
            }
        }
        // What the camera faces first, then the rest, each still nearest first: turning round fills what
        // comes into view before what went out of it. A tile within two of the focus counts as faced.
        {
            const f32_t side  = side0 * static_cast<f32_t>(1u << level);
            const Vec3  ahead = state.camera.Forward();
            const auto  faced = [&](const TileKey& key) {
                const f32_t dx = (static_cast<f32_t>(key.x) + 0.5f) * side - focus.x;
                const f32_t dz = (static_cast<f32_t>(key.z) + 0.5f) * side - focus.z;
                const f32_t d  = std::sqrt(dx * dx + dz * dz);
                return d < 2.0f * side || (dx * ahead.x + dz * ahead.z) > 0.2f * d * std::sqrt(ahead.x * ahead.x + ahead.z * ahead.z);
            };
            std::stable_partition(batch.begin(), batch.end(), faced);
        }
        // The bound, then what the frame's time allows: past it the rest waits for a next frame.
        if (spent + batch.size() > allowed) {
            const usize_t keep = allowed > spent ? allowed - spent : 0;
            missing += static_cast<u32_t>(batch.size() - keep);
            batch.resize(keep);
        }
        if (batch.empty()) {
            continue;
        }
        const f64_t started = NowSeconds();
        const usize_t fresh = state.cache.tiles.size();
        if (Status evaluated = EvaluateTiles(*state.context, *state.kernels, *terrain, batch,
                                             static_cast<u32_t>(state.settings.seed), state.cache.tiles);
            !evaluated) {
            detail::CopyBounded(state.lastError, std::string_view{evaluated.error().Format().data()});
            state.hasError = true;
            LOG_ERROR("could not evaluate a tile: {}", state.lastError.data());
            // Giving up on the rest of this pass rather than failing the same way N more times.
            state.missingTiles = missing + static_cast<u32_t>(batch.size());
            return;
        }
        // What a tile costs, smoothed, for the next frame's share.
        const f64_t each     = (NowSeconds() - started) / static_cast<f64_t>(batch.size());
        state.secondsPerTile = 0.8 * state.secondsPerTile + 0.2 * each;
        // The fresh tiles are of this generation, and take over from the stale ones of their keys.
        for (usize_t i = fresh; i < state.cache.tiles.size(); ++i) {
            state.cache.tiles[i].generation = state.cache.generation;
        }
        for (const TileKey& key : batch) {
            state.cache.Retire(*state.context, key);
        }
        state.cache.evaluated += batch.size();
        spent += static_cast<u32_t>(batch.size());
        batch.clear();
    }
    state.missingTiles = missing;
}

/// Picks the finest level, with hysteresis, and keeps the script's ground up with the camera.
///
/// The finest ring's level follows the distance, as the ring of tiles always did, but a change no longer
/// re-runs the script: every level compiles the one graph the script built at its own spacing (see
/// `StreamTiles`). The ideal level has to disagree for a while before it is acted on, which turns a
/// boundary into a dead band. With `--follow` the script is given the ground round the focus
/// (`params.bounds`): a focus that wanders a quarter of it away re-runs the script there.
void ViewerLayer::UpdateLevel() {
    State&      state  = *m_state;
    const f32_t ground = (state.terrain.heightMinimum + state.terrain.heightMaximum) * 0.5f;
    const u32_t ideal  = IdealLevel(state.camera.ViewDistance(ground) / state.settings.mapScale,
                                    state.settings.sectionSize, state.settings.resolution, kMaxBase,
                                    state.settings.detail);
    if (ideal == state.level) {
        state.disagreeingFrames = 0;
    } else if (++state.disagreeingFrames >= kLevelChangeFrames) {
        state.disagreeingFrames = 0;
        state.level             = ideal;
        state.levelTarget       = ideal;
    }
    const Vec3 focus = WorldFocus(state.camera, state.settings.mapScale);
    const b8_t away  = std::max(std::abs(focus.x - state.boundsCentre[0]), std::abs(focus.z - state.boundsCentre[1]))
                      > 0.25f * state.boundsHalf;
    if (state.settings.follow && away && !state.reload.running.load()) {
        state.reloadRequested = true;
    }
}

u32_t ViewerLayer::TilesDrawn() const noexcept {
    return m_state != nullptr ? m_state->tilesDrawn : 0;
}

u32_t ViewerLayer::TileCount() const noexcept {
    return m_state != nullptr ? static_cast<u32_t>(m_state->cache.tiles.size()) : 0;
}

u32_t ViewerLayer::MissingTiles() const noexcept {
    if (m_state == nullptr) {
        return 0;
    }
    // A level change being built on the worker will replace the whole ring: until it lands, the ring is
    // not complete either.
    const b8_t changing = m_state->levelTarget != m_state->level || m_state->reload.running.load();
    return m_state->missingTiles + (changing ? 1u : 0u);
}

void ViewerLayer::SetView(const View& view) noexcept {
    if (m_state == nullptr) {
        return;
    }
    constexpr f32_t kDegrees = 3.14159265358979f / 180.0f;
    m_state->camera.SetView(Vec3{view.target[0], view.target[1], view.target[2]}, view.yaw * kDegrees,
                            view.pitch * kDegrees, view.distance);
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

void ViewerLayer::SetWater(const Water& water) noexcept {
    if (m_state != nullptr) {
        m_state->settings.water = water;
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
