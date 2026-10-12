// terrain_export: batch evaluation in Headless mode. Must run on a machine with no display.

#include <engine/cli_parse.hpp>
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
        "  --min <x>[,<y>],<z>    world-space minimum (default 0,0)\n"
        "  --max <x>[,<y>],<z>    world-space maximum (default 1024,1024)\n"
        "  --resolution <meters>  meters per pixel (default 1.0)\n"
        "  --section <n>          section size in samples, a power of two (default 512)\n"
        "  --max-samples <n>      refuse a job over this many samples (default 2^40)\n"
        "  --out <dir>            output directory (default out/)\n"
        "  --tiles                write one file per tile instead of one stitched image\n"
        "  --png-level <0-9>      zlib level for PNG outputs (default 2)\n"
        "  --png-filter <f>       PNG row filter: none, up or all (default up)\n"
        "  --param key=value      script parameter, repeatable\n"
        "  --device <uuid>        force a physical device by its 32-hex-character UUID\n"
        "  --log-format <text|json>  log encoding (default text)\n"
        "  --help                 print this message\n"
        "\n"
        "The script defines main(params) and returns a table of outputs. params carries seed,\n"
        "bounds, resolution and every --param, with a value that reads as a number pushed as\n"
        "one. The domain comes from what the script builds, so an R3 graph also needs a y\n"
        "component on --min and --max. See docs/lua_api.md.\n",
        stderr);
}

/// Parses `<x>,<z>` or `<x>,<y>,<z>`. The three-component form is what an `R3` export needs, and the
/// two-component form stays exactly as the spec documents it. Every component must be a finite number
/// and the whole token must be consumed, so `1,2,3,4`, `1,` and `1e400,0` are all errors.
[[nodiscard]] Status ParseBound(std::string_view option, std::string_view text, f64_t& x, f64_t& y,
                                f64_t& z) {
    std::array<f64_t, 3> values{};
    usize_t              count = 0;
    usize_t              start = 0;
    while (true) {
        if (count == 3) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                        "{} expects <x>,<z> or <x>,<y>,<z>, got {}", option, text);
        }
        const usize_t    comma = text.find(',', start);
        std::string_view part =
            comma == std::string_view::npos ? text.substr(start) : text.substr(start, comma - start);
        Result<f64_t> value = cli::ParseFinite(option, part);
        if (!value) {
            return std::unexpected(value.error());
        }
        values[count++] = *value;
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    if (count == 2) {
        x = values[0];
        z = values[1];
        return {};
    }
    if (count == 3) {
        x = values[0];
        y = values[1];
        z = values[2];
        return {};
    }
    ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                "{} expects <x>,<z> or <x>,<y>,<z>, got {}", option, text);
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
            Result<u64_t> seed = cli::ParseUnsigned(argument, value);
            if (!seed) {
                return std::unexpected(seed.error());
            }
            options.job.seed = *seed;
        } else if (argument == "--resolution") {
            Result<f64_t> resolution = cli::ParsePositive(argument, value);
            if (!resolution) {
                return std::unexpected(resolution.error());
            }
            options.job.resolution = *resolution;
        } else if (argument == "--section") {
            Result<u64_t> section = cli::ParseUnsigned(argument, value, 1u << 16);
            if (!section) {
                return std::unexpected(section.error());
            }
            options.job.sectionSize = static_cast<u32_t>(*section);
        } else if (argument == "--max-samples") {
            Result<u64_t> samples = cli::ParseUnsigned(argument, value);
            if (!samples) {
                return std::unexpected(samples.error());
            }
            options.job.maxSamples = *samples;
        } else if (argument == "--min") {
            if (Status bound = ParseBound(argument, value, options.job.minX, options.job.minY,
                                          options.job.minZ);
                !bound) {
                return std::unexpected(bound.error());
            }
        } else if (argument == "--max") {
            if (Status bound = ParseBound(argument, value, options.job.maxX, options.job.maxY,
                                          options.job.maxZ);
                !bound) {
                return std::unexpected(bound.error());
            }
        } else if (argument == "--param") {
            if (!options.job.params.Assign(value)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--param expects key=value, got {}", value);
            }
        } else if (argument == "--png-level") {
            Result<u64_t> level = cli::ParseUnsigned(argument, value, 9);
            if (!level) {
                return std::unexpected(level.error());
            }
            options.job.png.level = static_cast<u32_t>(*level);
        } else if (argument == "--png-filter") {
            Result<terrain::PngFilter> filter = terrain::ParsePngFilter(value);
            if (!filter) {
                return std::unexpected(filter.error());
            }
            options.job.png.filter = *filter;
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
