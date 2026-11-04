// `terrain_viewer`: the interactive front end, which runs in Graphics mode.
//
// Thin by design, like `terrain_export`: parse arguments, create the application, push layers, run
// (spec section 6). Everything it does lives in the engine.

#include <engine/application.hpp>
#include <engine/cli_parse.hpp>
#include <engine/engine.hpp>
#include <engine/log.hpp>
#include <engine/render/renderer.hpp>
#include <engine/terrain/export.hpp>
#include <engine/window_layer.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace engine;

namespace {

void PrintUsage() {
    std::fputs("terrain_viewer - interactive terrain preview\n"
               "\n"
               "Usage: terrain_viewer [options]\n"
               "\n"
               "Options:\n"
               "  --script <file>        terrain script to preview and watch (required)\n"
               "  --seed <n>             random seed (default 0)\n"
               "  --ring <n>             tiles kept on each side of the camera (default 3)\n"
               "  --max-level <n>        rings of detail beyond the finest, each a level coarser (default 0)\n"
               "  --detail <n>           use n levels finer than the distance calls for (default 0)\n"
               "  --tiles-per-frame <n>  tiles evaluated per frame while moving (default 2)\n"
               "  --resolution <meters>  meters per sample (default 2.0)\n"
               "  --section <n>          samples per tile side (default 128)\n"
               "  --grid <n>             drawn vertices per tile side (default 128)\n"
               "  --param key=value      script parameter, repeatable\n"
               "  --shapes <file.json>   drawn shapes (GeoJSON) the script reads as params.shapes,\n"
               "                         watched and reloaded like the script\n"
               "  --view yaw,pitch,dist  start the orbit camera here: degrees, degrees, metres\n"
               "  --target x,y,z         the point the camera orbits (default 0,0,0)\n"
               "  --bounds minx,minz,maxx,maxz  evaluate and draw only the tiles that overlap the map;\n"
               "                         whole tiles, so the edge is exact when 127 x resolution divides it\n"
               "  --water                draw water at the water level (off by default)\n"
               "  --sun az,el            sun azimuth (clockwise from -z, as terrain_metrics) and\n"
               "                         elevation in degrees (default 126,31.7)\n"
               "  --no-panels            draw no panels, so a screenshot shows the terrain alone\n"
               "  --plan                 orthographic view straight down, +x right and +z down as in an\n"
               "                         exported heightmap; framed on --bounds unless --view is given\n"
               "  --show <output>        paint an R2->R1 output over the terrain, a ramp over its range\n"
               "  --show-ids             paint the shown output one colour a whole value (ids, biomes)\n"
               "  --follow               re-run the script as the camera moves, with params.bounds round\n"
               "                         it: for a world built only where it is looked at\n"
               "  --map-scale <f>        draw the map f times wider, heights and water level unchanged\n"
               "  --drift <m/s>          move the camera along +x by itself, to measure streaming in motion\n"
               "  --stream-ms <ms>       milliseconds of a frame tiles may take beyond the bound (default 4)\n"
               "  --spin <deg>           turn the camera that many degrees a frame by itself, to compare frames\n"
               "  --no-vsync             draw as fast as possible, not once a vertical blank (to measure)\n"
               "  --no-cull              draw every resident tile, culled or not (to check the culling)\n"
               "  --water-level <m>      height of the water surface (default 0)\n"
               "  --water-color r,g,b    water colour, each 0 to 1 (default 0.10,0.28,0.38)\n"
               "  --water-opacity <f>    0 to 1, how much the water replaces the terrain (default 0.75)\n"
               "  --width <n>            window width in pixels (default 1600)\n"
               "  --height <n>           window height in pixels (default 900)\n"
               "  --frames <n>           present n frames and exit; 0 runs until closed\n"
               "  --screenshot <file>    write the last frame as a PNG (implies --frames 90)\n"
               "  --shot yaw,pitch,dist=<file>  move the camera there, wait for the ring to stream\n"
               "                         in and write a PNG; repeatable, exits after the last one\n"
               "  --device <uuid>        force a physical device by its 32-hex-character UUID\n"
               "  --log-format <text|json>  log encoding (default text)\n"
               "  --log-level <level>    trace, debug, info, warn, error or off (default info)\n"
               "  --help                 print this message\n"
               "\n"
               "Left-drag turns, middle-drag pans, the wheel zooms, F switches between orbit\n"
               "and fly, WASDQE moves in fly mode and shift goes faster. R reloads the\n"
               "script, which also happens on its own when the file changes, and T toggles\n"
               "wireframe.\n"
               "\n"
               "--frames is what makes the viewer testable without a human: it opens the window,\n"
               "presents that many frames and exits with the usual exit codes.\n",
               stderr);
}

/// One `--shot`: a camera view and the file its frame goes to. The target is `--target`'s.
struct Shot {
    ViewerLayer::View view;
    std::string_view  path;
};

struct Options {
    WindowDesc            window;
    ViewerLayer::Settings viewer;
    terrain::Params       params;
    std::string_view      deviceUuid;
    log::Format           logFormat = log::Format::Text;
    log::Level            logLevel  = log::kCompileLevel;
    /// Zero runs until the window is closed.
    u64_t frames = 0;
    /// Where the last frame is written as a PNG. Empty means no screenshot.
    std::string_view screenshot;
    std::array<Shot, 16> shots{};
    usize_t              shotCount = 0;
};

/// Takes the `--shot` screenshots in order: moves the camera, waits until the ring has streamed in for a
/// few frames in a row, captures the next frame and stops the application after the last one. One
/// process for every view, so the script is loaded once.
class ShotsLayer final : public Layer {
public:
    ShotsLayer(Application& application, WindowLayer& window, ViewerLayer& viewer, const Options& options)
        : Layer(application), m_window(window), m_viewer(viewer), m_options(options) {}

    [[nodiscard]] const char* Name() const noexcept override { return "ShotsLayer"; }

    void OnUpdate() override {
        // A couple of frames after the last capture is requested, so it is presented and written.
        if (m_next >= m_options.shotCount) {
            if (++m_tail > 2) {
                App().Stop();
            }
            return;
        }
        Shot shot = m_options.shots[m_next];
        shot.view.target = m_options.viewer.view.target;
        if (m_frames++ == 0) {
            m_viewer.SetView(shot.view);
        }
        m_settled = m_viewer.MissingTiles() == 0 ? m_settled + 1 : 0;
        if (m_settled < kSettledFrames && m_frames < kMaxFrames) {
            return;
        }
        if (m_frames >= kMaxFrames) {
            std::fprintf(stderr, "terrain_viewer: %s taken before the ring finished streaming\n", shot.path.data());
        }
        if (!m_window.CaptureNextFrame(shot.path.data())) {
            App().AddExit(ExitCode::FailedToInitializeEngine);
        }
        ++m_next;
        m_frames  = 0;
        m_settled = 0;
    }

private:
    static constexpr u32_t kSettledFrames = 3;
    static constexpr u32_t kMaxFrames     = 900;

    WindowLayer&   m_window;
    ViewerLayer&   m_viewer;
    const Options& m_options;
    usize_t        m_next    = 0;
    u32_t          m_frames  = 0;
    u32_t          m_settled = 0;
    u32_t          m_tail    = 0;
};

/// Parses `N` finite numbers separated by commas.
template <usize_t N>
[[nodiscard]] Result<std::array<f32_t, N>> ParseNumbers(std::string_view option, std::string_view text) {
    std::array<f32_t, N> out{};
    for (usize_t i = 0; i < out.size(); ++i) {
        const usize_t          comma = text.find(',');
        const std::string_view part  = text.substr(0, comma);
        if ((comma == std::string_view::npos) != (i + 1 == out.size())) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "{} expects {} numbers "
                        "separated by commas, got {}", option, N, text);
        }
        Result<f64_t> value = cli::ParseFinite(option, part);
        if (!value) {
            return std::unexpected(value.error());
        }
        out[i] = static_cast<f32_t>(*value);
        if (comma != std::string_view::npos) {
            text.remove_prefix(comma + 1);
        }
    }
    return out;
}

/// Parses `r,g,b`, each a finite number in `[0, 1]`.
[[nodiscard]] Result<std::array<f32_t, 3>> ParseColour(std::string_view option,
                                                       std::string_view text) {
    std::array<f32_t, 3> colour{};
    for (usize_t i = 0; i < colour.size(); ++i) {
        const usize_t        comma = text.find(',');
        const std::string_view part = text.substr(0, comma);
        if ((comma == std::string_view::npos) != (i + 1 == colour.size())) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                        "{} expects r,g,b, got {}", option, text);
        }
        Result<f64_t> value = cli::ParseFinite(option, part);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (*value < 0.0 || *value > 1.0) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                        "{} components are in [0, 1], got {}", option, part);
        }
        colour[i] = static_cast<f32_t>(*value);
        if (comma != std::string_view::npos) {
            text.remove_prefix(comma + 1);
        }
    }
    return colour;
}

/// Parses a whole number into `target`, rejecting trailing text and anything over `maximum`.
template <typename T>
[[nodiscard]] Status ParseInto(std::string_view option, std::string_view text, T& target,
                               u64_t maximum = 0xFFFFFFFFull) {
    Result<u64_t> value = cli::ParseUnsigned(option, text, maximum);
    if (!value) {
        return std::unexpected(value.error());
    }
    target = static_cast<T>(*value);
    return {};
}

[[nodiscard]] Result<Options> ParseArguments(int argc, char** argv) {
    Options options;
    options.window.title = "YATE terrain viewer";
    // Evenly paced motion by default; `--no-vsync` draws as fast as it can, for measuring.
    options.window.vsync = true;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "usage requested");
        }
        // The options that are switches rather than pairs: water is off unless asked for, and so is
        // following the camera.
        if (argument == "--water") {
            options.viewer.water.enabled = true;
            continue;
        }
        if (argument == "--show-ids") {
            options.viewer.showIds = true;
            continue;
        }
        if (argument == "--plan") {
            options.viewer.plan = true;
            continue;
        }
        if (argument == "--no-panels") {
            options.viewer.panels = false;
            continue;
        }
        if (argument == "--no-cull") {
            options.viewer.cull = false;
            continue;
        }
        if (argument == "--no-vsync") {
            options.window.vsync = false;
            continue;
        }
        if (argument == "--follow") {
            options.viewer.follow = true;
            continue;
        }
        if (i + 1 >= argc) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "{} needs a value", argument);
        }
        const std::string_view value = argv[++i];

        if (argument == "--script") {
            options.viewer.script = value;
        } else if (argument == "--shapes") {
            options.viewer.shapes = value;
        } else if (argument == "--seed") {
            if (Status ok = ParseInto(argument, value, options.viewer.seed, ~0ull); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--ring") {
            if (Status ok = ParseInto(argument, value, options.viewer.ringRadius, 16); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--detail") {
            if (Status ok = ParseInto(argument, value, options.viewer.detail, 8); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--max-level") {
            if (Status ok = ParseInto(argument, value, options.viewer.maxLevel, 16); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--spin") {
            char*       end     = nullptr;
            const f64_t degrees = std::strtod(std::string(value).c_str(), &end);
            if (end == nullptr || *end != 0) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "--spin needs degrees a frame, got {}", value);
            }
            options.viewer.spin = static_cast<f32_t>(degrees);
        } else if (argument == "--stream-ms") {
            Result<f64_t> milliseconds = cli::ParsePositive(argument, value);
            if (!milliseconds) {
                return std::unexpected(milliseconds.error());
            }
            options.viewer.streamMilliseconds = static_cast<f32_t>(*milliseconds);
        } else if (argument == "--drift") {
            char*       end   = nullptr;
            const f64_t speed = std::strtod(std::string(value).c_str(), &end);
            if (end == nullptr || *end != 0) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "--drift needs metres a second, got {}", value);
            }
            options.viewer.drift = static_cast<f32_t>(speed);
        } else if (argument == "--map-scale") {
            Result<f64_t> scale = cli::ParsePositive(argument, value);
            if (!scale) {
                return std::unexpected(scale.error());
            }
            options.viewer.mapScale = static_cast<f32_t>(*scale);
        } else if (argument == "--tiles-per-frame") {
            if (Status ok = ParseInto(argument, value, options.viewer.tilesPerFrame, 4096); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--resolution") {
            Result<f64_t> resolution = cli::ParsePositive(argument, value);
            if (!resolution) {
                return std::unexpected(resolution.error());
            }
            options.viewer.resolution = *resolution;
        } else if (argument == "--section") {
            if (Status ok = ParseInto(argument, value, options.viewer.sectionSize, 1u << 16); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--grid") {
            if (Status ok = ParseInto(argument, value, options.viewer.gridVertices, 1u << 16); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--param") {
            if (!options.params.Assign(value)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--param expects key=value, got {}", value);
            }
        } else if (argument == "--width") {
            if (Status ok = ParseInto(argument, value, options.window.width, 16384); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--height") {
            if (Status ok = ParseInto(argument, value, options.window.height, 16384); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--frames") {
            if (Status ok = ParseInto(argument, value, options.frames, ~0ull); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--view") {
            Result<std::array<f32_t, 3>> view = ParseNumbers<3>(argument, value);
            if (!view) {
                return std::unexpected(view.error());
            }
            if (!((*view)[2] > 0.0f)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--view needs a distance greater than zero, got {}", value);
            }
            options.viewer.view.enabled  = true;
            options.viewer.view.yaw      = (*view)[0];
            options.viewer.view.pitch    = (*view)[1];
            options.viewer.view.distance = (*view)[2];
        } else if (argument == "--target") {
            Result<std::array<f32_t, 3>> target = ParseNumbers<3>(argument, value);
            if (!target) {
                return std::unexpected(target.error());
            }
            options.viewer.view.target = *target;
        } else if (argument == "--bounds") {
            Result<std::array<f32_t, 4>> bounds = ParseNumbers<4>(argument, value);
            if (!bounds) {
                return std::unexpected(bounds.error());
            }
            if (!((*bounds)[0] < (*bounds)[2] && (*bounds)[1] < (*bounds)[3])) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--bounds expects minx,minz,maxx,maxz with min under max, got {}", value);
            }
            options.viewer.bounds  = *bounds;
            options.viewer.bounded = true;
        } else if (argument == "--show") {
            options.viewer.show = value;
        } else if (argument == "--sun") {
            Result<std::array<f32_t, 2>> sun = ParseNumbers<2>(argument, value);
            if (!sun) {
                return std::unexpected(sun.error());
            }
            options.viewer.sun = *sun;
        } else if (argument == "--water-level") {
            Result<f64_t> level = cli::ParseFinite(argument, value);
            if (!level) {
                return std::unexpected(level.error());
            }
            options.viewer.water.level = static_cast<f32_t>(*level);
        } else if (argument == "--water-color") {
            Result<std::array<f32_t, 3>> colour = ParseColour(argument, value);
            if (!colour) {
                return std::unexpected(colour.error());
            }
            options.viewer.water.colour = *colour;
        } else if (argument == "--water-opacity") {
            Result<f64_t> opacity = cli::ParseFinite(argument, value);
            if (!opacity) {
                return std::unexpected(opacity.error());
            }
            if (*opacity < 0.0 || *opacity > 1.0) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--water-opacity is in [0, 1], got {}", value);
            }
            options.viewer.water.opacity = static_cast<f32_t>(*opacity);
        } else if (argument == "--screenshot") {
            options.screenshot = value;
        } else if (argument == "--shot") {
            const usize_t equals = value.find('=');
            if (equals == std::string_view::npos || equals + 1 == value.size()) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--shot expects yaw,pitch,distance=file.png, got {}", value);
            }
            if (options.shotCount == options.shots.size()) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "at most {} --shot views",
                            options.shots.size());
            }
            Result<std::array<f32_t, 3>> view = ParseNumbers<3>(argument, value.substr(0, equals));
            if (!view) {
                return std::unexpected(view.error());
            }
            if (!((*view)[2] > 0.0f)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--shot needs a distance greater than zero, got {}", value);
            }
            options.shots[options.shotCount++] =
                Shot{.view = {.enabled = true, .yaw = (*view)[0], .pitch = (*view)[1], .distance = (*view)[2]},
                     .path = value.substr(equals + 1)};
        } else if (argument == "--device") {
            options.deviceUuid = value;
        } else if (argument == "--log-format") {
            if (value == "json") {
                options.logFormat = log::Format::Json;
            } else if (value != "text") {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--log-format expects text or json, got {}", value);
            }
        } else if (argument == "--log-level") {
            if (!log::ParseLevel(value, options.logLevel)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--log-level expects trace, debug, info, warn, error or off, got {}", value);
            }
        } else {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "unknown option {}",
                        argument);
        }
    }

    if (options.window.width == 0 || options.window.height == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "window is {}x{}",
                    options.window.width, options.window.height);
    }
    if (options.viewer.script.empty()) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "--script is required");
    }
    if (!(options.viewer.resolution > 0.0)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--resolution must be positive, got {}", options.viewer.resolution);
    }
    if (options.viewer.ringRadius > 16) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--ring is {}, which would be a {}x{} ring of tiles", options.viewer.ringRadius,
                    2 * options.viewer.ringRadius + 1, 2 * options.viewer.ringRadius + 1);
    }
    if (options.viewer.maxLevel > 8) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--max-level is {}, which would make one tile cover {}x its finest area",
                    options.viewer.maxLevel, 1u << (2 * options.viewer.maxLevel));
    }
    if (options.viewer.tilesPerFrame == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--tiles-per-frame must be at least 1, or the ring would never fill");
    }
    if (options.viewer.sectionSize < 2 || !IsPowerOfTwo(options.viewer.sectionSize)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--section must be a power of two of at least 2, got {}",
                    options.viewer.sectionSize);
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const Result<Options> options = ParseArguments(argc, argv);
    if (!options) {
        std::fprintf(stderr, "%s\n", options.error().Format().data());
        return static_cast<int>(ExitCode::FailedToInitializeEngine);
    }

    const Result<Application*> application =
        engine::init(AppInfo{.mode       = RunMode::Graphics,
                             .name       = "terrain_viewer",
                             .logLevel   = options->logLevel,
                             .logFormat  = options->logFormat,
                             .deviceUuid = options->deviceUuid,
                             .window     = options->window});
    if (!application) {
        std::fprintf(stderr, "%s\n", application.error().Format().data());
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        return static_cast<int>(ExitCode::FailedToInitializeEngine);
    }

    WindowLayer& window = (*application)->PushLayer<WindowLayer>();
    // A screenshot needs a last frame to be the last one, and it needs the ring to have streamed in,
    // so asking for one without a frame count picks a count rather than refusing.
    const u64_t frames =
        !options->screenshot.empty() && options->frames == 0 ? 90 : options->frames;
    window.StopAfter(frames);
    if (!options->screenshot.empty()) {
        window.ScreenshotOnLastFrame(options->screenshot.data());
    }

    // The settings hold a pointer to the parameters, which live in `options` for the whole run. Bound
    // here rather than in the parser, because the parser returns its result by value and the address
    // would be the dead temporary's.
    ViewerLayer::Settings viewerSettings = options->viewer;
    viewerSettings.params                = &options->params;
    // Pushed above the window layer, which owns the frame; the viewer registers a recorder with it.
    ViewerLayer& viewer = (*application)->PushLayer<ViewerLayer>(viewerSettings);
    if (options->shotCount > 0) {
        (*application)->PushLayer<ShotsLayer>(window, viewer, *options);
    }

    (*application)->Run();
    const ExitCode exit = (*application)->Exit();

    if (Status closed = engine::shutdown(*application); !closed) {
        std::fprintf(stderr, "%s\n", closed.error().Format().data());
        return static_cast<int>(exit | ExitCode::FailedToShutdown);
    }
    return static_cast<int>(exit);
}
