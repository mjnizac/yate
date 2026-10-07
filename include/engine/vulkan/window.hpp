#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/engine.hpp>
#    include <engine/error.hpp>
#    include <engine/vulkan/vk.hpp>

#    include <array>

namespace engine::vulkan {

/// A GLFW window and its `VkSurfaceKHR`.
///
/// The surface has to exist before the physical device is chosen, because presentation support is part
/// of what makes a device acceptable in Graphics mode (spec section 5). So a window is created first
/// and hidden, and the instance it belongs to is handed over at construction: the surface outlives
/// everything except the instance, and it has to be destroyed before it.
///
/// GLFW is initialized once by `engine::init` and terminated once by `engine::shutdown`, after every
/// window is gone. Nothing here initializes it, and in Headless mode none of this is ever reached.
class Window {
public:
    Window() = default;
    ~Window();

    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    ENGINE_NO_COPY(Window);

    /// Creates a hidden window and its surface. `instance` must outlive the window.
    [[nodiscard]] static Result<Window> Create(VkInstance instance, const WindowDesc& desc);

    [[nodiscard]] VkSurfaceKHR Surface() const noexcept { return m_surface; }

    /// Framebuffer size in pixels, which is not the window size on a scaled display. Zero on both axes
    /// while the window is minimized, which is the signal to skip the frame rather than to resize to
    /// nothing.
    [[nodiscard]] std::array<u32_t, 2> FramebufferSize() const noexcept;

    /// True once the user has asked the window to close.
    [[nodiscard]] b8_t ShouldClose() const noexcept;

    /// Drains the event queue for every window in the process. Called once per frame.
    static void PollEvents();

    /// Makes the window visible. Deferred until the first frame has been presented, so the user never
    /// sees an unpainted white rectangle.
    void Show();

    [[nodiscard]] b8_t IsValid() const noexcept { return m_handle != nullptr; }

    /// Opaque `GLFWwindow*`, so this header does not pull GLFW in. Only `window.cpp` and the input
    /// layer dereference it.
    [[nodiscard]] void* Handle() const noexcept { return m_handle; }

    /// Blocks until the window has a non-zero framebuffer, draining events while it waits. Returns
    /// false if the window was closed while minimized.
    [[nodiscard]] b8_t WaitWhileMinimized() const;

    // --- Process-wide GLFW lifetime ---------------------------------------------------------------

    /// Initializes GLFW. Idempotent, and never called in Headless mode.
    [[nodiscard]] static Status InitializeWindowSystem();

    /// Terminates GLFW. Safe to call when it was never initialized.
    static void ShutdownWindowSystem() noexcept;

    /// Instance extensions GLFW needs for presentation on this platform, as a null-terminated array of
    /// names owned by GLFW.
    [[nodiscard]] static Result<std::pair<const char**, u32_t>> RequiredInstanceExtensions();

private:
    void Release() noexcept;

    void*        m_handle   = nullptr;
    VkInstance   m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface  = VK_NULL_HANDLE;
};

} // namespace engine::vulkan

#endif // IS_ENGINE
