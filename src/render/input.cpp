#include <engine/render/input.hpp>

#include <engine/vulkan/window.hpp>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace engine::render {
namespace {

/// The one place GLFW key codes appear. A key that means one thing on two physical keys lists both.
struct Binding {
    Key                  key;
    std::array<int, 2>   codes;
};

constexpr std::array<Binding, static_cast<usize_t>(Key::Count)> kBindings{{
    {Key::MoveForward, {GLFW_KEY_W, GLFW_KEY_UP}},
    {Key::MoveBack, {GLFW_KEY_S, GLFW_KEY_DOWN}},
    {Key::MoveLeft, {GLFW_KEY_A, GLFW_KEY_LEFT}},
    {Key::MoveRight, {GLFW_KEY_D, GLFW_KEY_RIGHT}},
    {Key::MoveUp, {GLFW_KEY_E, GLFW_KEY_SPACE}},
    {Key::MoveDown, {GLFW_KEY_Q, GLFW_KEY_LEFT_CONTROL}},
    {Key::Faster, {GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT}},
    {Key::ToggleMode, {GLFW_KEY_F, GLFW_KEY_UNKNOWN}},
    {Key::Reload, {GLFW_KEY_R, GLFW_KEY_UNKNOWN}},
    {Key::Wireframe, {GLFW_KEY_T, GLFW_KEY_UNKNOWN}},
}};

constexpr std::array<int, static_cast<usize_t>(MouseButton::Count)> kButtons{
    GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT, GLFW_MOUSE_BUTTON_MIDDLE};

/// Scroll arrives as a callback, not as a state, so it is accumulated between frames.
///
/// File-scope because GLFW callbacks are C functions with no user pointer available at registration time
/// without owning the window's user pointer, which the window does not hand out. One viewer, one scroll
/// accumulator; a second window would need this keyed by handle, which is in the todo list along with
/// the second window.
f64_t g_scrollAccumulator = 0.0;
b8_t  g_scrollRegistered  = false;

void OnScroll(GLFWwindow* /*window*/, double /*x*/, double y) noexcept {
    g_scrollAccumulator += y;
}

} // namespace

void Input::Update(const vulkan::Window& window) {
    auto* handle = static_cast<GLFWwindow*>(window.Handle());
    if (handle == nullptr) {
        return;
    }
    if (!g_scrollRegistered) {
        glfwSetScrollCallback(handle, OnScroll);
        g_scrollRegistered = true;
    }

    m_wasHeld = m_held;
    for (const Binding& binding : kBindings) {
        b8_t down = false;
        for (const int code : binding.codes) {
            if (code != GLFW_KEY_UNKNOWN && glfwGetKey(handle, code) == GLFW_PRESS) {
                down = true;
            }
        }
        m_held[static_cast<usize_t>(binding.key)] = down;
    }

    for (usize_t i = 0; i < kButtonCount; ++i) {
        m_buttons[i] = glfwGetMouseButton(handle, kButtons[i]) == GLFW_PRESS;
    }

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(handle, &x, &y);
    const f32_t currentX = static_cast<f32_t>(x);
    const f32_t currentY = static_cast<f32_t>(y);
    // The first sample only establishes where the pointer is. Reporting a delta from zero would jerk the
    // camera by however far the window happens to be from the top-left of the screen.
    m_cursorDeltaX = m_hasCursor ? currentX - m_cursorX : 0.0f;
    m_cursorDeltaY = m_hasCursor ? currentY - m_cursorY : 0.0f;
    m_cursorX      = currentX;
    m_cursorY      = currentY;
    m_hasCursor    = true;
}

b8_t Input::Held(Key key) const noexcept {
    const usize_t index = static_cast<usize_t>(key);
    return index < kKeyCount && m_held[index];
}

b8_t Input::Pressed(Key key) const noexcept {
    const usize_t index = static_cast<usize_t>(key);
    return index < kKeyCount && m_held[index] && !m_wasHeld[index];
}

b8_t Input::Held(MouseButton button) const noexcept {
    const usize_t index = static_cast<usize_t>(button);
    return index < kButtonCount && m_buttons[index];
}

f32_t Input::ScrollDelta() const noexcept {
    const f32_t ticks   = static_cast<f32_t>(g_scrollAccumulator);
    g_scrollAccumulator = 0.0;
    return ticks;
}

} // namespace engine::render
