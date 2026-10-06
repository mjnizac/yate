// terrain_export: batch evaluation in Headless mode. Must run on a machine with no display.

#include <engine/prelude.hpp>
#include <engine/terrain/export.hpp>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

using namespace engine;
using engine::terrain::ExportJob;
using engine::terrain::ExportSummary;

void PrintUsage() {
    std::fputs(
        "usage: terrain_export --script <file.lua> [options]\n"
        "\n"
        "  --script <file>        terrain script to evaluate (required)\n"
        "  --seed <n>             random seed (default 0)\n"
        "  --min <x>,<z>          world-space minimum (default 0,0)\n"
        "  --max <x>,<z>          world-space maximum (default 1024,1024)\n"
        "  --resolution <meters>  meters per pixel (default 1.0)\n"
        "  --section <n>          section size in samples, a power of two (default 512)\n"
        "  --out <dir>            output directory (default out/)\n"
        "  --tiles                write one file per tile instead of one stitched image\n"
        "  --param key=value      script parameter, repeatable\n"
        "  --device <uuid>        force a physical device by its 32-hex-character UUID\n"
        "  --log-format <text|json>  log encoding (default text)\n"
        "  --help                 print this message\n"
        "\n"
        "Until the Lua runtime lands (milestone 6) the graph is a single fBm node driven by\n"
        "--param: frequency, octaves, lacunarity, persistence, amplitude, offset,\n"
        "range=<min>,<max>, normalize=0 and normals=1.\n"
        "and normals=1.\n",
        stderr);
}

/// Parses `<a>,<b>` into two doubles.
[[nodiscard]] b8_t ParsePair(std::string_view text, f64_t& first, f64_t& second) {
    const usize_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    std::array<char, 64> buffer{};
    detail::CopyBounded(buffer, text.substr(0, comma));
    char* end = nullptr;
    first     = std::strtod(buffer.data(), &end);
    if (end == buffer.data()) {
        return false;
    }
    detail::CopyBounded(buffer, text.substr(comma + 1));
    second = std::strtod(buffer.data(), &end);
    return end != buffer.data();
}

struct Options {
    ExportJob        job;
    std::string_view deviceUuid;
    log::Format      logFormat = log::Format::Text;
};

[[nodiscard]] Result<Options> ParseArguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "usage requested");
        }
        if (argument == "--tiles") {
            options.job.tiles = true;
            continue;
        }
        if (i + 1 >= argc) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "{} needs a value", argument);
        }
        const std::string_view value = argv[++i];

        if (argument == "--script") {
            options.job.script = value;
        } else if (argument == "--out") {
            options.job.outputDirectory = value;
        } else if (argument == "--device") {
            options.deviceUuid = value;
        } else if (argument == "--seed") {
            options.job.seed = std::strtoull(value.data(), nullptr, 10);
        } else if (argument == "--resolution") {
            options.job.resolution = std::strtod(value.data(), nullptr);
        } else if (argument == "--section") {
            options.job.sectionSize = static_cast<u32_t>(std::strtoul(value.data(), nullptr, 10));
        } else if (argument == "--min") {
            if (!ParsePair(value, options.job.minX, options.job.minZ)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--min expects <x>,<z>, got {}", value);
            }
        } else if (argument == "--max") {
            if (!ParsePair(value, options.job.maxX, options.job.maxZ)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--max expects <x>,<z>, got {}", value);
            }
        } else if (argument == "--param") {
            if (!options.job.params.Assign(value)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--param expects key=value, got {}", value);
            }
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

    if (options.job.script.empty()) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "--script is required");
    }
    return options;
}

/// Runs the job once and stops the application. Section pipelining arrives in milestone 7; the
/// layer stack is what will carry it.
class ExportLayer final : public Layer {
public:
    ExportLayer(Application& application, const ExportJob& job) noexcept
        : Layer(application), m_job(job) {}

    [[nodiscard]] const char* Name() const noexcept override { return "ExportLayer"; }

    void OnUpdate() override {
        const Result<ExportSummary> summary = terrain::RunExport(App(), m_job);
        if (!summary) {
            App().Fail(summary.error(), ExitCode::FailedToExport);
            return;
        }
        for (usize_t i = 0; i < summary->fileCount; ++i) {
            LOG_INFO("wrote {}", summary->files[i].data());
        }
        LOG_INFO("wrote {}", summary->metadataFile.data());
        App().Stop();
    }

private:
    ExportJob m_job;
};

} // namespace

int main(int argc, char** argv) {
    const Result<Options> options = ParseArguments(argc, argv);
    if (!options) {
        std::fprintf(stderr, "terrain_export: %s\n", options.error().Format().data());
        return static_cast<int>(ExitCode::FailedToLoadScript);
    }

    const Result<Application*> application =
        engine::init(AppInfo{.mode       = RunMode::Headless,
                             .name       = "terrain_export",
                             .logFormat  = options->logFormat,
                             .deviceUuid = options->deviceUuid});
    if (!application) {
        std::fprintf(stderr, "terrain_export: %s\n", application.error().Format().data());
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        return static_cast<int>(ExitCode::FailedToInitializeEngine);
    }

    (*application)->PushLayer<ExportLayer>(options->job);
    (*application)->Run();

    ExitCode exit = (*application)->Exit();
    if (const Status closed = engine::shutdown(*application); !closed) {
        std::fprintf(stderr, "terrain_export: %s\n", closed.error().Format().data());
        exit |= ExitCode::FailedToShutdown;
    }
    return static_cast<int>(exit);
}
