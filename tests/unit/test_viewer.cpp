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
#include <engine/platform.hpp>
#include <engine/render/renderer.hpp>
#include <engine/terrain/export.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/swapchain.hpp>
#include <engine/vulkan/window.hpp>
#include <engine/window_layer.hpp>

#include <array>
#include <cstdio>
#include <string>

using namespace engine;

namespace {

/// Matches `SKIP_RETURN_CODE` in tests/CMakeLists.txt.
constexpr int kSkip = 77;

constexpr u64_t kFrames = 12;

constexpr const char* kDirectory = "out/test_viewer";

/// A script small enough to compile and evaluate in a test, with both outputs the viewer wants.
constexpr const char* kScript = R"(local function main(params)
    local base = Noises.fBm{
        frequency = 0.004,
        octaves   = 4,
        amplitude = params.amplitude or 80.0,
    }
    return {
        height  = { value = base.value, range = { -120, 120 } },
        normals = { value = Terrain.Normals{ input = base.gradient } },
    }
end
return main
)";

/// Only a height, so the viewer has to synthesize the normals itself.
constexpr const char* kHeightOnlyScript = R"(local function main()
    local base = Noises.fBm{ frequency = 0.004, octaves = 3, amplitude = 60.0 }
    return { height = { value = base.value, range = { -90, 90 } } }
end
return main
)";

[[nodiscard]] b8_t WriteFile(const std::string& path, std::string_view text) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const usize_t written = std::fwrite(text.data(), 1, text.size(), file);
    (void)std::fclose(file);
    return written == text.size();
}

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
    // MakeDirectory does not create parents, and ctest runs from the build directory.
    if (Status made = platform::MakeDirectory("out"); !made) {
        std::printf("FAIL could not create out/: %s\n", made.error().Format().data());
    }
    if (Status made = platform::MakeDirectory(kDirectory); !made) {
        std::printf("FAIL could not create %s: %s\n", kDirectory, made.error().Format().data());
    }

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

        test::Section("the viewer evaluates a script and draws its tiles");
        {
            const std::string scriptPath = std::string(kDirectory) + "/preview.lua";
            CHECK(WriteFile(scriptPath, kScript));

            terrain::Params params;
            CHECK(params.Set("amplitude", "80"));

            ViewerLayer::Settings settings;
            settings.script      = scriptPath;
            settings.extent      = 512.0;
            settings.resolution  = 4.0;
            settings.sectionSize = 32;
            settings.gridVertices = 32;
            settings.params      = &params;

            ViewerLayer& viewer = (*application)->PushLayer<ViewerLayer>(settings);
            // 512 m at 4 m/sample is 128 samples, which is 4x4 tiles of 32.
            CHECK_EQ(viewer.TileCount(), u32_t{16});
            CHECK_EQ(viewer.LoadCount(), u64_t{1});
            CHECK(viewer.LastError().empty());

            const u64_t before = window.FramesPresented();
            window.StopAfter(before + kFrames);
            (*application)->Run();

            // The camera is framed on the preview when it loads, so every tile should be in front of it.
            // Asserting *something* was drawn is the part that matters: a culling bug that rejected
            // everything would otherwise look exactly like a successful empty frame, which is how this
            // whole class of error hides.
            std::printf("     %u of %u tile(s) drawn\n", viewer.TilesDrawn(), viewer.TileCount());
            CHECK(viewer.TilesDrawn() > 0);
            CHECK(viewer.TilesDrawn() <= viewer.TileCount());

            test::Section("a reload picks up a changed script");
            CHECK(WriteFile(scriptPath, kHeightOnlyScript));
            viewer.RequestReload();
            window.StopAfter(window.FramesPresented() + 2);
            (*application)->Run();
            viewer.WaitForReload();
            CHECK_EQ(viewer.LoadCount(), u64_t{2});
            // No normals output in that script, so the viewer appended a measured gradient and the
            // normals from it rather than refusing to show it.
            CHECK(viewer.LastError().empty());
            CHECK_EQ(viewer.TileCount(), u32_t{16});

            test::Section("a broken script keeps the previous terrain on screen");
            CHECK(WriteFile(scriptPath, "local function main( return {} end\nreturn main\n"));
            viewer.RequestReload();
            window.StopAfter(window.FramesPresented() + 2);
            (*application)->Run();
            viewer.WaitForReload();
            // Still two loads: the third one failed, so nothing was swapped.
            CHECK_EQ(viewer.LoadCount(), u64_t{2});
            CHECK(!viewer.LastError().empty());
            std::printf("     reload rejected with: %.*s\n",
                        static_cast<int>(viewer.LastError().size()), viewer.LastError().data());
            CHECK(viewer.TileCount() == 16);

            // And it still draws, which is the actual promise: a bad edit must not blank the window.
            window.StopAfter(window.FramesPresented() + kFrames);
            (*application)->Run();
            CHECK(viewer.TilesDrawn() > 0);
        }

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
