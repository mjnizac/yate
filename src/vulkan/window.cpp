#include <engine/vulkan/window.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>

// Real casing, so the project builds on a case-sensitive filesystem (spec section 3). GLFW must see
// that Vulkan is already included, or it declares its own prototypes.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <array>
#include <utility>

namespace engine::vulkan {
namespace {

/// Whether GLFW is up. One flag rather than a reference count: `engine::init` initializes it once and
/// `engine::shutdown` terminates it once, after every window is destroyed.
b8_t g_windowSystemReady = false;

/// GLFW reports errors through a callback and then returns a failure from the call that failed. The
/// description is only valid for the duration of the callback, so it is copied into a slot the caller
/// reads afterwards.
///
/// Nothing is thrown from here: this is a C callback (spec section 3).
std::array<char, 256> g_lastError{};
int                   g_lastErrorCode = 0;

void OnGlfwError(int code, const char* description) noexcept {
    g_lastErrorCode = code;
    detail::CopyBounded(g_lastError, description != nullptr ? description : "no description");
    LOG_WARN("GLFW error {}: {}", code, g_lastError.data());
}

/// The last error GLFW reported, or a generic message when it reported none.
[[nodiscard]] std::string_view LastError() {
    return g_lastErrorCode != 0 ? std::string_view{g_lastError.data()} : "no error reported";
}

[[nodiscard]] GLFWwindow* AsWindow(void* handle) { return static_cast<GLFWwindow*>(handle); }

} // namespace

Status Window::InitializeWindowSystem() {
    if (g_windowSystemReady) {
        return {};
    }
    glfwSetErrorCallback(OnGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Platform, "could not initialize GLFW: {}",
                    LastError());
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        glfwTerminate();
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Platform,
                    "GLFW reports no Vulkan loader on this system, so Graphics mode cannot run");
    }
    g_windowSystemReady = true;
    LOG_INFO("window system ready (GLFW {})", glfwGetVersionString());
    return {};
}

void Window::ShutdownWindowSystem() noexcept {
    if (!g_windowSystemReady) {
        return;
    }
    glfwTerminate();
    g_windowSystemReady = false;
}

Result<std::pair<const char**, u32_t>> Window::RequiredInstanceExtensions() {
    if (!g_windowSystemReady) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Init,
                    "the window system must be initialized before its extensions are queried");
    }
    u32_t              count      = 0;
    const char** const extensions = glfwGetRequiredInstanceExtensions(&count);
    if (extensions == nullptr) {
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Init,
                    "GLFW reports no instance extensions for presentation: {}", LastError());
    }
    return std::pair<const char**, u32_t>{extensions, count};
}

Result<Window> Window::Create(VkInstance instance, const WindowDesc& desc) {
    if (!g_windowSystemReady) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Init,
                    "the window system must be initialized before a window is created");
    }
    if (desc.width == 0 || desc.height == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "window is {}x{}", desc.width,
                    desc.height);
    }

    // No OpenGL context, and hidden until the first frame has been presented (spec section 6).
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

    std::array<char, 128> title{};
    detail::CopyBounded(title, desc.title);

    GLFWwindow* handle = glfwCreateWindow(static_cast<int>(desc.width),
                                          static_cast<int>(desc.height), title.data(), nullptr,
                                          nullptr);
    if (handle == nullptr) {
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Init, "could not create a window: {}",
                    LastError());
    }

    Window window;
    window.m_handle   = handle;
    window.m_instance = instance;
    if (const VkResult result =
            glfwCreateWindowSurface(instance, handle, nullptr, &window.m_surface);
        result != VK_SUCCESS) {
        glfwDestroyWindow(handle);
        window.m_handle = nullptr;
        ENGINE_FAIL(ErrorCode::VulkanError, ErrorStage::Init,
                    "could not create a window surface: {} ({})", ResultName(result), LastError());
    }

    LOG_INFO("window '{}' created, {}x{}, surface ready", desc.title, desc.width, desc.height);
    return window;
}

Window::~Window() { Release(); }

void Window::Release() noexcept {
    // The surface belongs to the instance and must go first: destroying the window does not destroy it.
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_handle != nullptr) {
        glfwDestroyWindow(AsWindow(m_handle));
        m_handle = nullptr;
    }
    m_instance = VK_NULL_HANDLE;
}

Window::Window(Window&& other) noexcept { *this = std::move(other); }

Window& Window::operator=(Window&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    m_handle         = other.m_handle;
    m_instance       = other.m_instance;
    m_surface        = other.m_surface;
    other.m_handle   = nullptr;
    other.m_instance = VK_NULL_HANDLE;
    other.m_surface  = VK_NULL_HANDLE;
    return *this;
}

std::array<u32_t, 2> Window::FramebufferSize() const noexcept {
    if (m_handle == nullptr) {
        return {0, 0};
    }
    int width  = 0;
    int height = 0;
    glfwGetFramebufferSize(AsWindow(m_handle), &width, &height);
    return {static_cast<u32_t>(width < 0 ? 0 : width), static_cast<u32_t>(height < 0 ? 0 : height)};
}

b8_t Window::ShouldClose() const noexcept {
    return m_handle == nullptr || glfwWindowShouldClose(AsWindow(m_handle)) == GLFW_TRUE;
}

void Window::PollEvents() {
    if (g_windowSystemReady) {
        glfwPollEvents();
    }
}

void Window::Show() {
    if (m_handle != nullptr) {
        glfwShowWindow(AsWindow(m_handle));
    }
}

b8_t Window::WaitWhileMinimized() const {
    while (true) {
        const std::array<u32_t, 2> size = FramebufferSize();
        if (size[0] != 0 && size[1] != 0) {
            return true;
        }
        if (ShouldClose()) {
            return false;
        }
        // Blocking rather than spinning: a minimized window has nothing to do until something happens.
        glfwWaitEvents();
    }
}

} // namespace engine::vulkan
