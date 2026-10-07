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
        /// Side of the previewed square, in metres, centred on the world origin.
        f64_t extent = 2048.0;
        /// Metres per sample. The preview is deliberately coarser than an export.
        f64_t resolution = 2.0;
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
    /// Tiles the preview holds, drawn or not.
    [[nodiscard]] u32_t TileCount() const noexcept;
    /// Times the script has been loaded, including the first. Incremented by a successful reload.
    [[nodiscard]] u64_t LoadCount() const noexcept;
    /// The last error the script or the compiler produced, or an empty view when the preview is valid.
    ///
    /// Shown on screen by the viewer, and checked by the tests. A failed reload leaves the previous
    /// preview on screen and this set.
    [[nodiscard]] std::string_view LastError() const noexcept;

    /// Forces a reload on the next update, as a file-time change would.
    void RequestReload() noexcept;

    /// Blocks until a reload in flight has finished. For tests, which cannot wait on a frame count.
    void WaitForReload();

private:
    struct State;

    void UpdateInput();
    void UpdatePanels();
    void UpdateReload();
#ifdef IS_ENGINE
    /// Records this frame's tiles inside the window layer's render pass.
    void Record(const WindowLayer::FrameContext& frame);
    void RecordPanels(VkCommandBuffer commands);
#endif

    State* m_state = nullptr;
};

} // namespace engine
