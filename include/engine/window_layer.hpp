#pragma once

#include <engine/common.hpp>
#include <engine/layer.hpp>

namespace engine {

/// Owns the frame: acquires a swapchain image, runs the render pass, presents.
///
/// The first `WindowLayer` adopts the main window `engine::init` created, because that window's surface
/// had to exist before the physical device was chosen (spec section 6). A second one would need its own
/// window and a presentation check against the same device; that is not built yet and says so rather
/// than failing obscurely.
///
/// What this layer does *not* do is draw anything. It begins the render pass and ends it; the terrain
/// renderer is a separate layer pushed above it that records into the command buffer this one owns. A
/// viewer with nothing above it is a valid, empty window, which is exactly what the smoke test runs.
class ENGINE_API WindowLayer final : public Layer {
public:
    explicit WindowLayer(Application& application) noexcept;
    ~WindowLayer() override;

    ENGINE_NO_COPY(WindowLayer);
    ENGINE_NO_MOVE(WindowLayer);

    [[nodiscard]] const char* Name() const noexcept override { return "WindowLayer"; }

    void OnAttach() override;
    void OnUpdate() override;
    void OnDetach() override;

    /// Frames presented since the window opened. Zero until the first successful present, which is what
    /// the smoke test watches to know the whole path worked.
    [[nodiscard]] u64_t FramesPresented() const noexcept;

    /// Stops the application after this many presented frames. Zero, the default, runs until the user
    /// closes the window. This is what makes the viewer testable without a human.
    void StopAfter(u64_t frames) noexcept;

private:
    /// Defined in `window_layer.cpp`, so this header stays free of Vulkan: it is one of the few an
    /// executable includes.
    struct State;

    State* m_state = nullptr;
};

} // namespace engine
