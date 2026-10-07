#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <array>

namespace engine::vulkan {
class Window;
}

namespace engine::render {

/// Keys the viewer reacts to, named by what they do rather than by where they are.
///
/// An abstraction over GLFW's codes, as the spec asks, and the reason is not portability: it is that the
/// rest of the engine should not have to know that `GLFW_KEY_LEFT_SHIFT` and `GLFW_KEY_RIGHT_SHIFT` are
/// two different keys that mean one thing, or that a scroll wheel is not a key. The translation happens
/// once, in `input.cpp`, which is the only file that includes GLFW besides the window itself.
enum class Key : u32_t {
    MoveForward = 0,
    MoveBack,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    Faster,
    ToggleMode,
    Reload,
    Wireframe,
    Count,
};

enum class MouseButton : u32_t { Left = 0, Right, Middle, Count };

/// A frame's worth of input: what is held, what was just pressed, and how much the pointer moved.
///
/// Edge state as well as level state, because "is held" and "was just pressed" are different questions
/// and deriving one from the other at every call site is how a toggle ends up firing every frame.
class Input {
public:
    /// Samples the window. Call once per frame, after events have been polled.
    void Update(const vulkan::Window& window);

    [[nodiscard]] b8_t Held(Key key) const noexcept;
    [[nodiscard]] b8_t Pressed(Key key) const noexcept;
    [[nodiscard]] b8_t Held(MouseButton button) const noexcept;

    /// Pointer movement in pixels since the previous frame. Zero on the first frame, so a window that
    /// opens under the cursor does not jerk the camera.
    [[nodiscard]] f32_t CursorDeltaX() const noexcept { return m_cursorDeltaX; }
    [[nodiscard]] f32_t CursorDeltaY() const noexcept { return m_cursorDeltaY; }

    /// Scroll ticks since the previous frame.
    [[nodiscard]] f32_t ScrollDelta() const noexcept;

private:
    static constexpr usize_t kKeyCount    = static_cast<usize_t>(Key::Count);
    static constexpr usize_t kButtonCount = static_cast<usize_t>(MouseButton::Count);

    std::array<b8_t, kKeyCount>      m_held{};
    std::array<b8_t, kKeyCount>      m_wasHeld{};
    std::array<b8_t, kButtonCount>   m_buttons{};
    f32_t                            m_cursorX      = 0.0f;
    f32_t                            m_cursorY      = 0.0f;
    f32_t                            m_cursorDeltaX = 0.0f;
    f32_t                            m_cursorDeltaY = 0.0f;
    b8_t                             m_hasCursor    = false;
};

} // namespace engine::render

#endif // IS_ENGINE
