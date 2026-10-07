// `terrain_viewer`: the interactive front end, which runs in Graphics mode.
//
// Thin by design, like `terrain_export`: parse arguments, create the application, push layers, run
// (spec section 6). Everything it does lives in the engine.

#include <engine/application.hpp>
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
               "  --extent <meters>      side of the previewed square (default 2048)\n"
               "  --resolution <meters>  meters per sample (default 2.0)\n"
               "  --section <n>          samples per tile side (default 128)\n"
               "  --grid <n>             drawn vertices per tile side (default 128)\n"
               "  --param key=value      script parameter, repeatable\n"
               "  --width <n>            window width in pixels (default 1600)\n"
               "  --height <n>           window height in pixels (default 900)\n"
               "  --frames <n>           present n frames and exit; 0 runs until closed\n"
               "  --device <uuid>        force a physical device by its 32-hex-character UUID\n"
               "  --log-format <text|json>  log encoding (default text)\n"
               "  --help                 print this message\n"
               "\n"
               "Left-drag turns, middle-drag pans, the wheel zooms, F switches between orbit\n"
               "and fly, WASDQE moves in fly mode and shift goes faster. R reloads the\n"
               "script, which also happens on its own when the file changes.\n"
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
};

[[nodiscard]] Result<Options> ParseArguments(int argc, char** argv) {
    Options options;
    options.window.title = "YATE terrain viewer";

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "usage requested");
        }
        if (i + 1 >= argc) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "{} needs a value", argument);
        }
        const std::string_view value = argv[++i];

        if (argument == "--script") {
            options.viewer.script = value;
        } else if (argument == "--seed") {
            options.viewer.seed = std::strtoull(value.data(), nullptr, 10);
        } else if (argument == "--extent") {
            options.viewer.extent = std::strtod(value.data(), nullptr);
        } else if (argument == "--resolution") {
            options.viewer.resolution = std::strtod(value.data(), nullptr);
        } else if (argument == "--section") {
            options.viewer.sectionSize =
                static_cast<u32_t>(std::strtoul(value.data(), nullptr, 10));
        } else if (argument == "--grid") {
            options.viewer.gridVertices =
                static_cast<u32_t>(std::strtoul(value.data(), nullptr, 10));
        } else if (argument == "--param") {
            if (!options.params.Assign(value)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--param expects key=value, got {}", value);
            }
        } else if (argument == "--width") {
            options.window.width = static_cast<u32_t>(std::strtoul(value.data(), nullptr, 10));
        } else if (argument == "--height") {
            options.window.height = static_cast<u32_t>(std::strtoul(value.data(), nullptr, 10));
        } else if (argument == "--frames") {
            options.frames = std::strtoull(value.data(), nullptr, 10);
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
    if (!(options.viewer.extent > 0.0)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--extent must be positive, got {}", options.viewer.extent);
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
    window.StopAfter(options->frames);

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
