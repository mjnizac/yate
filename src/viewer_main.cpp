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
               "  --max-level <n>        coarsest level the ring may use (default 4)\n"
               "  --tiles-per-frame <n>  tiles evaluated per frame while moving (default 2)\n"
               "  --resolution <meters>  meters per sample (default 2.0)\n"
               "  --section <n>          samples per tile side (default 128)\n"
               "  --grid <n>             drawn vertices per tile side (default 128)\n"
               "  --param key=value      script parameter, repeatable\n"
               "  --water                draw water at the water level (off by default)\n"
               "  --water-level <m>      height of the water surface (default 0)\n"
               "  --water-color r,g,b    water colour, each 0 to 1 (default 0.10,0.28,0.38)\n"
               "  --water-opacity <f>    0 to 1, how much the water replaces the terrain (default 0.75)\n"
               "  --width <n>            window width in pixels (default 1600)\n"
               "  --height <n>           window height in pixels (default 900)\n"
               "  --frames <n>           present n frames and exit; 0 runs until closed\n"
               "  --device <uuid>        force a physical device by its 32-hex-character UUID\n"
               "  --log-format <text|json>  log encoding (default text)\n"
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

struct Options {
    WindowDesc            window;
    ViewerLayer::Settings viewer;
    terrain::Params       params;
    std::string_view      deviceUuid;
    log::Format           logFormat = log::Format::Text;
    /// Zero runs until the window is closed.
    u64_t frames = 0;
    /// Where the last frame is written as a PNG. Empty means no screenshot.
    std::string_view screenshot;
};

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

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "usage requested");
        }
        // The only option that is a switch rather than a pair: water is off unless asked for.
        if (argument == "--water") {
            options.viewer.water.enabled = true;
            continue;
        }
        if (i + 1 >= argc) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "{} needs a value", argument);
        }
        const std::string_view value = argv[++i];

        if (argument == "--script") {
            options.viewer.script = value;
        } else if (argument == "--seed") {
            if (Status ok = ParseInto(argument, value, options.viewer.seed, ~0ull); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--ring") {
            if (Status ok = ParseInto(argument, value, options.viewer.ringRadius, 16); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (argument == "--max-level") {
            if (Status ok = ParseInto(argument, value, options.viewer.maxLevel, 16); !ok) {
                return std::unexpected(ok.error());
            }
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
        } else if (argument == "--device") {
            options.deviceUuid = value;
        } else if (argument == "--log-format") {
            if (value == "json") {
                options.logFormat = log::Format::Json;
            } else if (value != "text") {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--log-format expects text or json, got {}", value);
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
    (*application)->PushLayer<ViewerLayer>(viewerSettings);

    (*application)->Run();
    const ExitCode exit = (*application)->Exit();

    if (Status closed = engine::shutdown(*application); !closed) {
        std::fprintf(stderr, "%s\n", closed.error().Format().data());
        return static_cast<int>(exit | ExitCode::FailedToShutdown);
    }
    return static_cast<int>(exit);
}
