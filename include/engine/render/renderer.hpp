#pragma once

#include <engine/common.hpp>
#include <engine/layer.hpp>

#ifdef IS_ENGINE
#    include <engine/window_layer.hpp>
#endif

#include <string_view>

namespace engine::terrain {
class Params;
}

namespace engine {

/// The viewer: evaluates a terrain script into resident GPU tiles and draws them as a displaced grid.
///
/// Pushed above a `WindowLayer`, which owns the frame; this layer registers a recorder with it and draws
/// inside its render pass. Nothing is uploaded: the vertex shader reads the compute output through a
/// buffer device address, exactly as the kernels pass buffers to each other, so what the evaluator
/// produced is drawn where it lies.
class ENGINE_API ViewerLayer final : public Layer {
public:
    /// Water for the preview. Lives in the viewer, not in the graph: the terrain defines an elevation
    /// and the viewer draws a level over it, so changing any of this never re-evaluates a tile and a
    /// reload keeps it.
    struct Water {
        /// Off by default, so a script that has never been shown with water looks as it always did.
        b8_t enabled = false;
        /// Which bodies are drawn: the sea at `level`, and the script's `lakes` and `rivers` outputs,
        /// which are water surfaces of their own.
        b8_t sea    = true;
        b8_t lakes  = true;
        b8_t rivers = true;
        /// Height of the sea in world metres. Sea level is zero.
        f32_t level = 0.0f;
        /// Linear RGB of the water body.
        std::array<f32_t, 3> colour{0.10f, 0.28f, 0.38f};
        /// How much of the terrain's own colour the water replaces at depth, in `[0, 1]`.
        f32_t opacity = 0.75f;
    };

    /// Where the orbit camera starts. Off by default, which frames the ring as before.
    struct View {
        b8_t                 enabled = false;
        /// Degrees. 0 looks along +z and 90 along +x; a negative pitch looks down.
        f32_t                yaw   = 0.0f;
        f32_t                pitch = -35.0f;
        f32_t                distance = 4000.0f;
        std::array<f32_t, 3> target{};
    };

    struct Settings {
        /// Script to evaluate. Reloaded when its modification time changes.
        std::string_view script;
        u64_t            seed = 0;
        /// Metres per sample at the finest level. A coarser level doubles it.
        f64_t resolution = 2.0;
        /// Tiles kept on each side of the camera's own tile, so the ring is `2r + 1` across.
        ///
        /// Three gives a 7x7 ring, which at 128 samples and 2 m is about 1.8 km across: enough that the
        /// edge is past the far plane at ground level and the ring is never visibly square.
        u32_t ringRadius = 3;
        /// Rings of detail beyond the finest (`--max-level`, "Detail rings" in the panel): each a level
        /// coarser, twice the spacing and twice as far. 0 is the finest ring alone.
        u32_t maxLevel = 0;
        /// Levels finer than the distance heuristic picks; 0 is the heuristic. For close inspection.
        u32_t detail = 0;
        /// Tiles evaluated per frame while the camera moves, at least; then more while `streamMilliseconds`
        /// of the frame are not spent. The time bound is what keeps a frame from stalling (a tile of a heavy
        /// script takes milliseconds) and lets a light one fill its ring at once; a hole at the edge of the
        /// ring for a few frames is the better failure.
        u32_t tilesPerFrame = 2;
        f32_t streamMilliseconds = 4.0f;
        /// Re-run the script when the camera's focus moves a quarter of the ring away from where it was
        /// last run, so a script that builds only round `params.bounds` keeps up. Off by default: any
        /// other script would re-run for nothing.
        b8_t follow = false;
        /// With `bounded`, only the tiles that overlap `bounds` (min x, min z, max x, max z in metres) are
        /// evaluated and drawn (`--bounds`): a map of a given size instead of an endless ring. Whole tiles,
        /// so the edge falls on the bounds when they are multiples of a tile's side.
        b8_t                 bounded = false;
        std::array<f32_t, 4> bounds{};
        /// How much wider than the script built it the map is drawn: x and z are multiplied by it, the
        /// heights and the water level are not, so distances grow and the sea stays at 0. The tiles are
        /// still evaluated at the script's own positions.
        f32_t mapScale = 1.0f;
        /// Metres a second the camera moves along +x by itself (`--drift`): a benchmark of streaming in
        /// motion, reproducible without a hand on the mouse. 0 leaves it still.
        f32_t drift = 0.0f;
        /// Degrees of yaw the camera turns a frame by itself (`--spin`): a turn that is the same every run,
        /// for comparing frames. 0 leaves it still.
        f32_t spin = 0.0f;
        /// Tiles outside the view are not drawn; off (`--no-cull`) every resident tile is, to check that
        /// culling never hides one that shows.
        b8_t cull = true;
        /// Samples per side of one tile. Tiles are the unit of evaluation and of culling.
        u32_t sectionSize = 128;
        /// Vertices per side of the drawn grid. At most `sectionSize`; lower trades detail for
        /// triangles.
        u32_t gridVertices = 128;
        /// User parameters the script receives, as `--param` does for the exporter.
        const terrain::Params* params = nullptr;
        /// Drawn shapes (GeoJSON) the script reads as `params.shapes`; empty for none. Watched like the
        /// script.
        std::string_view       shapes;
        Water                  water;
        View                   view;
        /// Where the sun is (`--sun`, "Sun" in the panel), in degrees: azimuth clockwise from -z (the
        /// top of a hillshade, as `terrain_metrics --sun` counts it) and elevation over the horizon.
        /// Viewer state: changing it never re-evaluates a tile.
        std::array<f32_t, 2> sun{126.03f, 31.70f};
        /// Draw the panels; off (`--no-panels`) a screenshot shows the terrain alone.
        b8_t panels = true;
        /// Plan view (`--plan`, "Plan view" in the panel): orthographic, straight down, +x right and +z
        /// down as in an exported heightmap, so a shot lines up with `hillshade.png` and a reference map.
        /// The orbit camera's target is the centre and its distance the zoom; with `--bounds` and no
        /// `--view` it starts framed on the bounds.
        b8_t plan = false;
        /// An R2->R1 output painted over the terrain, still lit (`--show`, "Show" in the panel): a
        /// colour ramp over the range the script declared for it, or with `showIds` one colour a whole
        /// value, for ids such as biomes. Empty shows none. Changing it re-copies the tiles, no script run.
        std::string_view show;
        b8_t             showIds = false;
        /// A PNG laid over the terrain as a tracing aid (`--guide`): a reference map or outline, its first
        /// row at min z as in an exported heightmap, stretched over `guideBounds` (`--guide-bounds`,
        /// else `bounds`). `guideOpacity` is how much of it shows ("Guide" in the panel).
        std::string_view     guide;
        std::array<f32_t, 4> guideBounds{};
        b8_t                 guideBounded = false;
        f32_t                guideOpacity = 0.5f;
    };

    ViewerLayer(Application& application, const Settings& settings);
    ~ViewerLayer() override;

    ENGINE_NO_COPY(ViewerLayer);
    ENGINE_NO_MOVE(ViewerLayer);

    [[nodiscard]] const char* Name() const noexcept override { return "ViewerLayer"; }

    void OnAttach() override;
    void OnUpdate() override;
    void OnDetach() override;

    /// Tiles drawn in the last frame, after culling. What the culling is judged by.
    [[nodiscard]] u32_t TilesDrawn() const noexcept;
    /// Tiles resident right now, drawn or not.
    [[nodiscard]] u32_t TileCount() const noexcept;
    /// Tiles the ring wants that are not resident yet, plus one while a level change is being built.
    /// Zero means streaming has caught up.
    [[nodiscard]] u32_t MissingTiles() const noexcept;
    /// Moves the orbit camera, as `Settings::view` does at start. The ring follows on later frames;
    /// `MissingTiles` says when it has caught up.
    void SetView(const View& view) noexcept;
    /// Level the ring is at. Sample spacing is `resolution * 2^level`.
    [[nodiscard]] u32_t Level() const noexcept;
    /// Tiles evaluated since the last load, which is what says streaming is doing anything.
    [[nodiscard]] u64_t TilesEvaluated() const noexcept;
    /// Times the script has been loaded, including the first. Incremented by a successful reload.
    [[nodiscard]] u64_t LoadCount() const noexcept;
    /// The last error the script or the compiler produced, or an empty view when the preview is valid.
    ///
    /// Shown on screen by the viewer, and checked by the tests. A failed reload leaves the previous
    /// preview on screen and this set.
    [[nodiscard]] std::string_view LastError() const noexcept;

    /// Forces a reload on the next update, as a file-time change would.
    void RequestReload() noexcept;

    /// Puts the camera here, in world metres.
    ///
    /// Exists because a test cannot fly: the only way to check that the ring follows the camera is to
    /// move the camera. It is also the hook a "go to coordinates" command would use.
    void MoveCameraTo(f32_t x, f32_t y, f32_t z) noexcept;

    /// Switches between fly and orbit without moving the eye. The `F` key does the same thing.
    void SetFlyMode(b8_t fly) noexcept;

    /// Replaces the water settings. Takes effect on the next frame and costs no evaluation.
    void SetWater(const Water& water) noexcept;

    /// Blocks until a reload in flight has finished. For tests, which cannot wait on a frame count.
    void WaitForReload();

private:
    struct State;

    void UpdateInput();
    void UpdateLevel();
    void UpdatePanels();
    void UpdateReload();
    void ReshowTiles();
    /// Evaluates at most `budget` missing tiles of the ring, and evicts what left it.
    void StreamTiles(u32_t budget, f64_t deadline = 1e300);
#ifdef IS_ENGINE
    /// Records this frame's tiles inside the window layer's render pass.
    void Record(const WindowLayer::FrameContext& frame);
    void RecordPanels(VkCommandBuffer commands);
#endif

    State* m_state = nullptr;
};

} // namespace engine
