// The viewer's presentation path, end to end (milestone 9).
//
// Opening a window and presenting is the one part of the engine a test cannot judge by looking, so what
// this checks is everything *around* the picture: that the window system comes up, that a surface is
// created before the device is chosen, that the swapchain matches the surface, that frames are acquired
// and presented without a validation error, and that a resize rebuilds everything without leaking or
// tripping over a semaphore. Whether the terrain looks right is a human's job; whether the frame loop is
// correct is this test's.
//
// It skips rather than fails when there is no display, so the suite stays meaningful on a headless
// machine. ctest is told which code means "skipped".

#include "test_support.hpp"

#include <engine/application.hpp>
#include <engine/engine.hpp>
#include <engine/log.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/swapchain.hpp>
#include <engine/vulkan/window.hpp>
#include <engine/window_layer.hpp>

#include <array>
#include <cstdio>

using namespace engine;

namespace {

/// Matches `SKIP_RETURN_CODE` in tests/CMakeLists.txt.
constexpr int kSkip = 77;

constexpr u64_t kFrames = 12;

/// True when the failure is "there is no window system here" rather than a real defect.
///
/// A headless container has no display, and GLFW says so from `glfwInit`. That is not something the
/// engine can fix and not something this test should fail for, so it is told apart from everything else
/// by the code and stage the engine already reports.
[[nodiscard]] b8_t IsMissingDisplay(const Error& error) {
    return error.code == ErrorCode::Unsupported
           && (error.stage == ErrorStage::Platform || error.stage == ErrorStage::Init);
}

} // namespace

int main() {
    const Result<Application*> application =
        engine::init(AppInfo{.mode   = RunMode::Graphics,
                             .name   = "test_viewer",
                             .window = WindowDesc{.title     = "test_viewer",
                                                  .width     = 480,
                                                  .height    = 320,
                                                  .resizable = true}});
    if (!application) {
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        if (IsMissingDisplay(application.error())) {
            std::printf("SKIP no window system available: %s\n",
                        application.error().Format().data());
            return kSkip;
        }
        std::printf("FAIL engine::init: %s\n", application.error().Format().data());
        return 1;
    }

    vulkan::Context& context = VulkanContext(**application);

    const int result = [&]() -> int {
        test::Section("Graphics mode brings up a window, a surface and a swapchain");
        ApplicationState& state = StateOf(**application);
        CHECK(state.window != nullptr);
        CHECK(state.swapchain != nullptr);
        if (state.window == nullptr || state.swapchain == nullptr) {
            return 1;
        }
        CHECK(state.window->Surface() != VK_NULL_HANDLE);
        CHECK(state.swapchain->IsValid());
        // At least double buffered, or there would be nothing to show while a frame is drawn.
        CHECK(state.swapchain->ImageCount() >= 2);
        CHECK(state.swapchain->Extent().width != 0);
        CHECK(state.swapchain->Extent().height != 0);
        CHECK(state.swapchain->Pass().IsValid());
        // A graphics queue exists only in Graphics mode, and presentation needs it.
        CHECK(context.GraphicsQueue().IsValid());

        test::Section("frames are acquired and presented");
        WindowLayer& window = (*application)->PushLayer<WindowLayer>();
        window.StopAfter(kFrames);
        (*application)->Run();

        // More than `kFrames` is fine: a frame that found the swapchain out of date is counted and then
        // repeated, which is the behaviour, not a defect.
        CHECK(window.FramesPresented() >= kFrames);
        // Compared as the underlying integer: `ExitCode` is a bit-flag enum with no formatter, and
        // CHECK_EQ prints both sides.
        CHECK_EQ(static_cast<i32_t>((*application)->Exit()), static_cast<i32_t>(ExitCode::Success));

        test::Section("a resize rebuilds the swapchain");
        // Driven directly rather than by resizing the window: a test cannot drag a corner, and this is
        // the code path a drag would reach. Doing it twice, to different sizes, also covers handing the
        // old swapchain to the new one as `oldSwapchain`.
        REQUIRE_OK(context.WaitIdle());
        const std::array<std::array<u32_t, 2>, 3> sizes{{{640, 400}, {300, 700}, {480, 320}}};
        for (const std::array<u32_t, 2>& size : sizes) {
            REQUIRE_OK(state.swapchain->Recreate(size[0], size[1]));
            CHECK(state.swapchain->IsValid());
            CHECK(state.swapchain->ImageCount() >= 2);
            // The surface decides the final extent, so the request is not guaranteed; what must hold is
            // that it is never zero, because no swapchain can have a zero extent.
            CHECK(state.swapchain->Extent().width != 0);
            CHECK(state.swapchain->Extent().height != 0);
        }

        test::Section("and the loop still runs afterwards");
        window.StopAfter(window.FramesPresented() + kFrames);
        (*application)->Run();
        CHECK(window.FramesPresented() >= 2 * kFrames);

        // The whole point: none of the above may have produced a validation error.
        ++test::g_checks;
        if (const u64_t errors = vulkan::ValidationErrorCount(); errors != 0) {
            ++test::g_failures;
            std::printf("FAIL the Vulkan debug messenger reported %llu error(s)\n",
                        static_cast<unsigned long long>(errors));
        }
        return 0;
    }();

    if (Status closed = engine::shutdown(*application); !closed) {
        std::printf("FAIL engine::shutdown: %s\n", closed.error().Format().data());
        return 1;
    }
    return result != 0 ? result : test::Summary("test_viewer");
}
