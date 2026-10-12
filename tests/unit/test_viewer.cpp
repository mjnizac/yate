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
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <spng.h>

using namespace engine;

namespace {

// --- Looking at the frame -----------------------------------------------------------------------
//
// Everything above asserts the step *before* a pixel: tiles evaluated, tiles past the frustum, draws
// recorded. Four defects reached a person's eyes with all of that green -- a depth clear against the
// wrong convention that discarded every fragment, a one-interval gap along every tile border, a
// hardcoded resolution -- because nothing here ever read a pixel. This reads some.

struct Frame {
    u32_t             width  = 0;
    u32_t             height = 0;
    std::vector<u8_t> rgb;
};

/// Decodes the 8-bit RGB PNG `CaptureNextFrame` writes.
[[nodiscard]] Frame DecodeFrame(const std::string& path) {
    Frame         frame;
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> file((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
    if (file.empty()) {
        return frame;
    }
    spng_ctx* context = spng_ctx_new(0);
    spng_set_png_buffer(context, file.data(), file.size());
    spng_ihdr header{};
    usize_t   size = 0;
    if (spng_get_ihdr(context, &header) == 0
        && spng_decoded_image_size(context, SPNG_FMT_RGB8, &size) == 0) {
        frame.rgb.resize(size);
        if (spng_decode_image(context, frame.rgb.data(), size, SPNG_FMT_RGB8, 0) == 0) {
            frame.width  = header.width;
            frame.height = header.height;
        } else {
            frame.rgb.clear();
        }
    }
    spng_ctx_free(context);
    return frame;
}

/// The window's clear colour as it lands in an sRGB framebuffer: (0.05, 0.06, 0.09) linear.
constexpr std::array<int, 3> kClearSrgb{63, 69, 85};

/// Fraction of pixels that are not the clear colour, i.e. have terrain on them.
[[nodiscard]] f64_t CoveredFraction(const Frame& frame) {
    usize_t covered = 0;
    const usize_t count = static_cast<usize_t>(frame.width) * frame.height;
    for (usize_t i = 0; i < count; ++i) {
        int worst = 0;
        for (usize_t c = 0; c < 3; ++c) {
            const int diff = std::abs(static_cast<int>(frame.rgb[i * 3 + c]) - kClearSrgb[c]);
            worst          = diff > worst ? diff : worst;
        }
        covered += worst > 6 ? 1 : 0;
    }
    return count == 0 ? 0.0 : static_cast<f64_t>(covered) / static_cast<f64_t>(count);
}

/// Mean absolute difference per channel between two frames of the same size, in 8-bit codes.
[[nodiscard]] f64_t MeanDifference(const Frame& a, const Frame& b) {
    if (a.rgb.size() != b.rgb.size() || a.rgb.empty()) {
        return 0.0;
    }
    f64_t total = 0.0;
    for (usize_t i = 0; i < a.rgb.size(); ++i) {
        total += std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
    }
    return total / static_cast<f64_t>(a.rgb.size());
}

/// The part of the frame the panels never reach: the right of `kClearOfPanelsX` and below
/// `kClearOfPanelsY`, as fractions. The panels sit at the top left and their water section changes with
/// the water settings, so a comparison of what the *terrain* did has to look past them.
constexpr f64_t kClearOfPanelsX = 0.72;
constexpr f64_t kClearOfPanelsY = 0.62;

/// Like `MeanDifference`, over the part of the frame the panels do not cover.
[[nodiscard]] f64_t MeanDifferenceClear(const Frame& a, const Frame& b) {
    if (a.rgb.size() != b.rgb.size() || a.rgb.empty()) {
        return 0.0;
    }
    const u32_t first = static_cast<u32_t>(static_cast<f64_t>(a.width) * kClearOfPanelsX);
    const u32_t top   = static_cast<u32_t>(static_cast<f64_t>(a.height) * kClearOfPanelsY);
    f64_t       total = 0.0;
    usize_t     count = 0;
    for (u32_t y = top; y < a.height; ++y) {
        for (u32_t x = first; x < a.width; ++x) {
            for (usize_t c = 0; c < 3; ++c) {
                const usize_t i = (static_cast<usize_t>(y) * a.width + x) * 3 + c;
                total += std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
                ++count;
            }
        }
    }
    return count == 0 ? 0.0 : total / static_cast<f64_t>(count);
}

/// Largest spread of any channel among the pixels clear of the panels that are not the clear colour, and how many such pixels there were. A flat surface lit by one sun is one colour, so
/// a spread above a few codes means something is not constant: two surfaces fighting for the pixel, or
/// a pattern.
struct Spread {
    int     widest = 0;
    usize_t pixels = 0;
};

[[nodiscard]] Spread ClearSpread(const Frame& frame) {
    Spread               out;
    std::array<int, 3>   low{255, 255, 255};
    std::array<int, 3>   high{0, 0, 0};
    const u32_t first = static_cast<u32_t>(static_cast<f64_t>(frame.width) * kClearOfPanelsX);
    const u32_t top   = static_cast<u32_t>(static_cast<f64_t>(frame.height) * kClearOfPanelsY);
    for (u32_t y = top; y < frame.height; ++y) {
        for (u32_t x = first; x < frame.width; ++x) {
            const usize_t i = (static_cast<usize_t>(y) * frame.width + x) * 3;
            int           worst = 0;
            for (usize_t c = 0; c < 3; ++c) {
                worst = std::max(worst, std::abs(static_cast<int>(frame.rgb[i + c]) - kClearSrgb[c]));
            }
            if (worst <= 6) {
                continue;
            }
            ++out.pixels;
            for (usize_t c = 0; c < 3; ++c) {
                low[c]  = std::min(low[c], static_cast<int>(frame.rgb[i + c]));
                high[c] = std::max(high[c], static_cast<int>(frame.rgb[i + c]));
            }
        }
    }
    for (usize_t c = 0; c < 3; ++c) {
        out.widest = std::max(out.widest, high[c] - low[c]);
    }
    return out;
}

/// `MeanDifferenceClear` restricted to the rows from `top` to `bottom`, as fractions of the height.
[[nodiscard]] f64_t MeanDifferenceBand(const Frame& a, const Frame& b, f64_t top, f64_t bottom) {
    if (a.rgb.size() != b.rgb.size() || a.rgb.empty()) {
        return 0.0;
    }
    const u32_t first = static_cast<u32_t>(static_cast<f64_t>(a.width) * kClearOfPanelsX);
    const u32_t from  = static_cast<u32_t>(static_cast<f64_t>(a.height) * top);
    const u32_t to    = static_cast<u32_t>(static_cast<f64_t>(a.height) * bottom);
    f64_t       total = 0.0;
    usize_t     count = 0;
    for (u32_t y = from; y < to; ++y) {
        for (u32_t x = first; x < a.width; ++x) {
            for (usize_t c = 0; c < 3; ++c) {
                const usize_t i = (static_cast<usize_t>(y) * a.width + x) * 3 + c;
                total += std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
                ++count;
            }
        }
    }
    return count == 0 ? 0.0 : total / static_cast<f64_t>(count);
}

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
            settings.script        = scriptPath;
            settings.resolution    = 4.0;
            settings.sectionSize   = 32;
            settings.gridVertices  = 32;
            settings.ringRadius    = 2;
            settings.tilesPerFrame = 4;
            settings.params        = &params;

            ViewerLayer& viewer = (*application)->PushLayer<ViewerLayer>(settings);
            // A radius of two is a 5x5 ring, filled completely before the first frame.
            CHECK_EQ(viewer.TileCount(), u32_t{25});
            CHECK_EQ(viewer.MissingTiles(), u32_t{0});
            CHECK_EQ(viewer.TilesEvaluated(), u64_t{25});
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

            test::Section("the frame has terrain on it, and it moves with the camera");
            const std::string shotA = std::string(kDirectory) + "/frame_a.png";
            const std::string shotB = std::string(kDirectory) + "/frame_b.png";

            CHECK(window.CaptureNextFrame(shotA.c_str()));
            window.StopAfter(window.FramesPresented() + 2);
            (*application)->Run();
            const Frame frameA = DecodeFrame(shotA);
            CHECK(frameA.width > 0);
            if (frameA.width > 0) {
                const f64_t covered = CoveredFraction(frameA);
                std::printf("     %.1f%% of the frame is not the clear colour\n", covered * 100.0);
                // A viewer that draws every tile and rejects every fragment presents the clear colour
                // everywhere except under the UI panels. Measured in this window, with the depth clear
                // against the wrong convention: 41.9% (all of it panel). With terrain: 77.1%. The
                // threshold sits between them, and the movement check below is the second witness.
                CHECK(covered > 0.60);
            }

            // Move the camera sideways and up: the picture has to change, or it is not following it.
            viewer.MoveCameraTo(900.0f, 1500.0f, 900.0f);
            CHECK(window.CaptureNextFrame(shotB.c_str()));
            window.StopAfter(window.FramesPresented() + 3);
            (*application)->Run();
            const Frame frameB = DecodeFrame(shotB);
            CHECK_EQ(frameB.width, frameA.width);
            const f64_t moved = MeanDifference(frameA, frameB);
            std::printf("     mean per-channel change after moving the camera: %.2f codes\n", moved);
            CHECK(moved > 1.0);

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
            CHECK_EQ(viewer.TileCount(), u32_t{25});
            CHECK_EQ(viewer.MissingTiles(), u32_t{0});

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
            CHECK(viewer.TileCount() == 25);

            // And it still draws, which is the actual promise: a bad edit must not blank the window.
            window.StopAfter(window.FramesPresented() + kFrames);
            (*application)->Run();
            CHECK(viewer.TilesDrawn() > 0);

            test::Section("the ring follows the camera");
            // Restored first, so the reload below has something that compiles.
            CHECK(WriteFile(scriptPath, kScript));
            viewer.RequestReload();
            window.StopAfter(window.FramesPresented() + 2);
            (*application)->Run();
            viewer.WaitForReload();
            CHECK(viewer.LastError().empty());

            const u64_t evaluatedBefore = viewer.TilesEvaluated();
            const u32_t levelBefore     = viewer.Level();

            // Far enough to leave the ring entirely: a 32-sample tile at 4 m is 128 m, and the ring is
            // two tiles on each side, so twenty tiles along x is well outside it. Every tile must be
            // re-evaluated at the new centre, and none of the old ones may still be resident.
            viewer.MoveCameraTo(20.0f * 128.0f, 200.0f, 0.0f);
            window.StopAfter(window.FramesPresented() + 40);
            (*application)->Run();

            std::printf("     %llu tile(s) evaluated after moving, level %u -> %u\n",
                        static_cast<unsigned long long>(viewer.TilesEvaluated() - evaluatedBefore),
                        levelBefore, viewer.Level());
            CHECK(viewer.TilesEvaluated() > evaluatedBefore);
            CHECK_EQ(viewer.TileCount(), u32_t{25});
            CHECK_EQ(viewer.MissingTiles(), u32_t{0});
            CHECK(viewer.TilesDrawn() > 0);

            test::Section("distance picks the level");
            // In fly mode the level follows the height above the ground, which is the measure that
            // makes climbing coarsen the terrain. In orbit it follows the orbit radius instead, and
            // placing the eye would move the target with it and change nothing.
            viewer.SetFlyMode(true);
            viewer.MoveCameraTo(0.0f, 20000.0f, 0.0f);
            window.StopAfter(window.FramesPresented() + 60);
            (*application)->Run();
            std::printf("     level %u at 20 km, spacing x%u\n", viewer.Level(),
                        1u << viewer.Level());
            CHECK(viewer.Level() > 0);
            CHECK_EQ(viewer.MissingTiles(), u32_t{0});

            test::Section("water is shaded on the terrain, by the terrain, and costs no evaluation");
            {
                const auto flat = [&](f64_t height) {
                    return WriteFile(scriptPath,
                                     "local function main()\n"
                                     "    return { height = { value = Terrain.Const{ value = "
                                         + std::to_string(height)
                                         + " }, range = { -100, 100 } } }\n"
                                           "end\nreturn main\n");
                };
                const auto reloadFlat = [&](f64_t height) {
                    CHECK(flat(height));
                    viewer.RequestReload();
                    window.StopAfter(window.FramesPresented() + 2);
                    (*application)->Run();
                    viewer.WaitForReload();
                    CHECK(viewer.LastError().empty());
                };
                const auto shoot = [&](const char* name) {
                    const std::string path = std::string(kDirectory) + "/" + name + ".png";
                    CHECK(window.CaptureNextFrame(path.c_str()));
                    window.StopAfter(window.FramesPresented() + 3);
                    (*application)->Run();
                    return DecodeFrame(path);
                };

                reloadFlat(-50.0);
                viewer.SetFlyMode(true);
                viewer.MoveCameraTo(0.0f, 120.0f, -400.0f);
                window.StopAfter(window.FramesPresented() + 40);
                (*application)->Run();

                const u64_t evaluated = viewer.TilesEvaluated();
                ViewerLayer::Water water;
                water.enabled = false;
                viewer.SetWater(water);
                const Frame dry = shoot("water_dry");
                water.enabled = true;
                viewer.SetWater(water);
                const Frame wet = shoot("water_wet");
                CHECK(dry.width > 0);
                CHECK(wet.width > 0);

                // Everything below the level is tinted, and the tint is far from the noise floor.
                const f64_t tint = MeanDifferenceClear(dry, wet);
                std::printf("     terrain 50 m under the level: %.2f codes of tint\n", tint);
                CHECK(tint > 8.0);

                // The level moved, not the terrain: water well below the floor leaves the frame as it was.
                water.level = -500.0f;
                viewer.SetWater(water);
                const Frame deep = shoot("water_low_level");
                const f64_t gone = MeanDifferenceClear(dry, deep);
                std::printf("     level 450 m below the terrain: %.3f codes of change\n", gone);
                CHECK(gone < 0.5);

                // Switching it on, off and moving it evaluated nothing.
                CHECK_EQ(viewer.TilesEvaluated(), evaluated);

                test::Section("a surface exactly at the water level does not fight it");
                // The case a drawn plane fails: terrain and water at the same depth over every pixel. With
                // the water shaded by the terrain there is one surface, so the frame is one colour.
                reloadFlat(0.0);
                water.level = 0.0f;
                viewer.SetWater(water);

                const Frame levelA = shoot("water_at_level_a");
                const Spread spread = ClearSpread(levelA);
                std::printf("     at the level: %zu pixel(s), widest channel spread %d code(s)\n",
                            spread.pixels, spread.widest);
                CHECK(spread.pixels > 1000);
                CHECK(spread.widest <= 3);

                // And it stays that way when the camera moves a centimetre, which is what makes a z-fight
                // flicker rather than sit still.
                viewer.MoveCameraTo(0.01f, 120.0f, -400.0f);
                const Frame levelB = shoot("water_at_level_b");
                const f64_t flicker = MeanDifferenceClear(levelA, levelB);
                std::printf("     moved 1 cm: %.3f codes of change\n", flicker);
                CHECK(flicker < 0.5);
                CHECK(ClearSpread(levelB).widest <= 3);

                test::Section("terrain above the level is left alone");
                reloadFlat(50.0);
                water.level = 0.0f;
                viewer.SetWater(water);
                const Frame highWet = shoot("water_high_wet");
                water.enabled = false;
                viewer.SetWater(water);
                const Frame highDry = shoot("water_high_dry");
                const f64_t same = MeanDifferenceClear(highDry, highWet);
                std::printf("     terrain 50 m over the level: %.3f codes of change\n", same);
                CHECK(same < 0.5);

                test::Section("a camera under the surface sees the terrain through it");
                reloadFlat(-50.0);
                water.enabled = true;
                water.level   = 2000.0f;
                viewer.SetWater(water);
                const Frame under = shoot("water_camera_under");
                CHECK(under.width > 0);
                // Tinted all over rather than a colour from nowhere: still terrain, still covering the frame.
                CHECK(CoveredFraction(under) > 0.5);
                CHECK(MeanDifferenceClear(dry, under) > 8.0);

                test::Section("nearer terrain hides farther terrain");
                // A crater: a floor 50 m under the water with a steep wall round it, seen from inside.
                // Behind the wall, further away, is more floor, and that floor is under water, so it is
                // tinted. Looking at the wall the frame must show the wall. A depth test that lets the
                // farther fragment win paints the far floor over it, which is what "the mountains go
                // transparent when you tilt the view" was.
                const std::string crater =
                    "local function main()\n"
                    "    local c = Terrain.Coords{}\n"
                    "    local r2 = c.x * c.x + c.z * c.z\n"
                    "    local wall = Curves.Smoothstep{ input = r2, min = 19600, max = 22500 }\n"
                    "               - Curves.Smoothstep{ input = r2, min = 25600, max = 32400 }\n"
                    "    return { height = { value = wall * 350.0 - 50.0, range = { -100, 400 } } }\n"
                    "end\nreturn main\n";
                CHECK(WriteFile(scriptPath, crater));
                viewer.RequestReload();
                window.StopAfter(window.FramesPresented() + 2);
                (*application)->Run();
                viewer.WaitForReload();
                CHECK(viewer.LastError().empty());
                water.enabled = true;
                water.level   = 0.0f;
                viewer.SetWater(water);
                viewer.SetFlyMode(true);
                viewer.MoveCameraTo(0.0f, 120.0f, 0.0f);
                window.StopAfter(window.FramesPresented() + 40);
                (*application)->Run();

                const Frame wallWet = shoot("depth_crater_wet");
                water.enabled       = false;
                viewer.SetWater(water);
                const Frame wallDry = shoot("depth_crater_dry");
                // The wall is above the water, so it is the same frame with the water on and off. Far
                // floor painted over it differs between the two by the whole tint.
                const f64_t differs = MeanDifferenceBand(wallWet, wallDry, 0.08, 0.45);
                std::printf("     water on against off over the wall: %.2f codes\n", differs);
                CHECK(differs < 2.0);
            }
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
