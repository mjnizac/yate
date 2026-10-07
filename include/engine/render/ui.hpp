#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/vulkan/vk.hpp>

namespace engine::terrain {
class Params;
}

namespace engine::vulkan {
class Context;
class Window;
} // namespace engine::vulkan

namespace engine::render {

/// The viewer's panels.
///
/// No UI-library type appears here, the same arrangement `lua.hpp` has with Sol2: the whole of Dear
/// ImGui is confined to `ui.cpp`, so replacing it would touch one file.
class Ui {
public:
    Ui() = default;
    ~Ui();

    ENGINE_NO_COPY(Ui);
    ENGINE_NO_MOVE(Ui);

    /// `renderPass` is the pass the panels are drawn in, so they must be recorded inside it.
    [[nodiscard]] Status Initialize(vulkan::Context& context, const vulkan::Window& window,
                                   VkRenderPass renderPass, u32_t imageCount);
    void                 Shutdown();

    [[nodiscard]] b8_t IsValid() const noexcept;

    /// What the panels show. Filled by the caller each frame; `params` is edited in place.
    struct Panels {
        const char* scriptPath = "";
        /// Null-terminated, empty when there is nothing wrong.
        const char* error      = "";
        const char* cameraMode = "";
        u32_t       tilesDrawn = 0;
        u32_t       tileCount  = 0;
        u64_t       loadCount  = 0;
        f32_t       framesPerSecond   = 0.0f;
        f32_t       frameMilliseconds = 0.0f;
        std::array<f32_t, 3> eye{};
        /// Edited in place when the user drags a value. Null hides the section.
        terrain::Params* params = nullptr;
    };

    /// Starts a frame. Must be paired with `Record`.
    void Begin();

    /// Lays the panels out. Returns true when the user changed a parameter or asked for a reload.
    [[nodiscard]] b8_t Draw(Panels& panels);

    /// Records the panels into `commands`, which must be inside the render pass given to `Initialize`.
    void Record(VkCommandBuffer commands);

    /// True while the pointer or the keyboard is over a panel, so the camera should ignore them.
    [[nodiscard]] b8_t WantsMouse() const;
    [[nodiscard]] b8_t WantsKeyboard() const;

private:
    struct State;

    State* m_state = nullptr;
};

} // namespace engine::render

#endif // IS_ENGINE
