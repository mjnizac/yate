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
        /// Coarsest level the ring may switch to. Each level doubles the sample spacing and so quadruples
        /// the area one tile covers.
        u32_t maxLevel = 4;
        /// Tiles evaluated per frame while the camera moves. The bound is what keeps a frame from
        /// stalling; a hole at the edge of the ring for a few frames is the better failure.
        u32_t tilesPerFrame = 2;
        /// Samples per side of one tile. Tiles are the unit of evaluation and of culling.
        u32_t sectionSize = 128;
        /// Vertices per side of the drawn grid. At most `sectionSize`; lower trades detail for
        /// triangles.
        u32_t gridVertices = 128;
        /// User parameters the script receives, as `--param` does for the exporter.
        const terrain::Params* params = nullptr;
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
    /// Tiles the ring wants that are not resident yet. Zero means streaming has caught up.
    [[nodiscard]] u32_t MissingTiles() const noexcept;
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

    /// Blocks until a reload in flight has finished. For tests, which cannot wait on a frame count.
    void WaitForReload();

private:
    struct State;

    void UpdateInput();
    void UpdateLevel();
    void UpdatePanels();
    void UpdateReload();
    /// Evaluates at most `budget` missing tiles of the ring, and evicts what left it.
    void StreamTiles(u32_t budget);
#ifdef IS_ENGINE
    /// Records this frame's tiles inside the window layer's render pass.
    void Record(const WindowLayer::FrameContext& frame);
    void RecordPanels(VkCommandBuffer commands);
#endif

    State* m_state = nullptr;
};

} // namespace engine
