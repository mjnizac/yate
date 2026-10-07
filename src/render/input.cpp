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
/// Keyed by window handle rather than kept in one variable: GLFW callbacks are C functions and the
/// window's user pointer belongs to whoever else wants it, so a small table is the way to stay correct
/// with more than one window without fighting over that pointer. Four entries, scanned linearly, because
/// the count is a handful and a map would allocate behind the engine's back.
constexpr usize_t kMaxScrollSources = 4;

struct ScrollSource {
    GLFWwindow* window      = nullptr;
    f64_t       accumulated = 0.0;
};

std::array<ScrollSource, kMaxScrollSources> g_scroll{};

[[nodiscard]] ScrollSource* FindScroll(GLFWwindow* window) {
    for (ScrollSource& source : g_scroll) {
        if (source.window == window) {
            return &source;
        }
    }
    return nullptr;
}

/// Nothing is thrown or logged from here: this is a C callback (spec section 3).
void OnScroll(GLFWwindow* window, double /*x*/, double y) noexcept {
    if (ScrollSource* source = FindScroll(window); source != nullptr) {
        source->accumulated += y;
    }
}

} // namespace

Status Input::Attach(const vulkan::Window& window) {
    auto* handle = static_cast<GLFWwindow*>(window.Handle());
    if (handle == nullptr) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "the input system needs an open window");
    }
    if (FindScroll(handle) != nullptr) {
        m_window = handle;
        return {};
    }
    for (ScrollSource& source : g_scroll) {
        if (source.window == nullptr) {
            source.window      = handle;
            source.accumulated = 0.0;
            glfwSetScrollCallback(handle, OnScroll);
            m_window = handle;
            return {};
        }
    }
    ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Init,
                "at most {} window(s) can report scrolling", kMaxScrollSources);
}

void Input::Update(const vulkan::Window& window) {
    auto* handle = static_cast<GLFWwindow*>(window.Handle());
    if (handle == nullptr) {
        return;
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
    ScrollSource* source = FindScroll(static_cast<GLFWwindow*>(m_window));
    if (source == nullptr) {
        return 0.0f;
    }
    // Consumed on read, so a tick is applied exactly once whoever reads it.
    const f32_t ticks   = static_cast<f32_t>(source->accumulated);
    source->accumulated = 0.0;
    return ticks;
}

} // namespace engine::render
