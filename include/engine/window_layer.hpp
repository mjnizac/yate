#pragma once

#include <engine/common.hpp>
#include <engine/layer.hpp>

#ifdef IS_ENGINE
#    include <engine/vulkan/vk.hpp>
#endif

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

    /// Seconds since the previous frame. Zero on the first one.
    [[nodiscard]] f64_t DeltaSeconds() const noexcept;

    /// Writes the next presented frame to `path` as an 8-bit PNG, once.
    ///
    /// This is how the renderer gets checked without a human at the keyboard: everything else about a
    /// frame can be asserted from the CPU side — tiles evaluated, draws recorded, nothing culled — and
    /// none of it notices a depth test that rejects every fragment. The copy comes straight out of the
    /// presented swapchain image, so what lands in the file is what the window showed.
    ///
    /// Returns false when the surface did not allow its images to be a transfer source, which is the one
    /// case where no screenshot is possible.
    [[nodiscard]] b8_t CaptureNextFrame(const char* path) noexcept;

    /// Writes the *last* frame before `StopAfter` stops the application. Needs a nonzero `StopAfter`,
    /// because otherwise there is no last frame to pick. Later than `CaptureNextFrame` on purpose: the
    /// first frames of a viewer are a half-streamed ring, and a screenshot of those says nothing.
    void ScreenshotOnLastFrame(const char* path) noexcept;

#ifdef IS_ENGINE
    /// What a recorder is given. Engine-only, because it names Vulkan types.
    struct FrameContext {
        /// The frame's primary command buffer, inside an already-begun render pass.
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkExtent2D      extent{};
        /// Which of the frames in flight this is, for anything that keeps per-frame resources.
        u32_t frameIndex = 0;
        f64_t deltaSeconds = 0.0;
    };

    using Recorder = void (*)(const FrameContext& frame, void* user);

    /// Registers a function that records drawing commands inside this layer's render pass.
    ///
    /// The frame belongs to this layer: it acquires the image, begins the pass, ends it and presents.
    /// A layer above it cannot simply record in its own `OnUpdate`, because by then the pass would have
    /// to be either still open across layer boundaries or reopened, and both make the ownership of the
    /// command buffer ambiguous. One explicit registration keeps the frame in one place and still lets
    /// the push order be `WindowLayer` then `ViewerLayer`, as the spec describes.
    void SetRecorder(Recorder recorder, void* user) noexcept;
#endif

private:
    /// Defined in `window_layer.cpp`, so this header stays free of Vulkan: it is one of the few an
    /// executable includes.
    struct State;

    State* m_state = nullptr;
};

} // namespace engine
