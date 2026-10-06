// terrain_export: batch evaluation in Headless mode. Must run on a machine with no display.

#include <engine/prelude.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

using namespace engine;

/// Everything the CLI can set. Mirrors the invocation documented in spec section 12.
struct ExportJob {
    std::string_view script;
    std::string_view outputDirectory = "out/";
    std::string_view deviceUuid;
    u64_t            seed       = 0;
    f64_t            minX       = 0.0;
    f64_t            minZ       = 0.0;
    f64_t            maxX       = 1024.0;
    f64_t            maxZ       = 1024.0;
    f64_t            resolution = 1.0;
    u32_t            sectionSize = 512;
    b8_t             tiles       = false;
    log::Format      logFormat   = log::Format::Text;
};

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
        "  --help                 print this message\n",
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

/// Returns the value of an option that takes an argument, or an empty view when it is missing.
[[nodiscard]] std::string_view TakeValue(int argc, char** argv, int& index, std::string_view name) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "terrain_export: %.*s needs a value\n", static_cast<int>(name.size()),
                     name.data());
        return {};
    }
    return argv[++index];
}

[[nodiscard]] Result<ExportJob> ParseArguments(int argc, char** argv) {
    ExportJob job;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "usage requested");
        }
        if (argument == "--tiles") {
            job.tiles = true;
            continue;
        }

        const std::string_view value = TakeValue(argc, argv, i, argument);
        if (value.empty()) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "{} needs a value",
                        argument);
        }

        if (argument == "--script") {
            job.script = value;
        } else if (argument == "--out") {
            job.outputDirectory = value;
        } else if (argument == "--device") {
            job.deviceUuid = value;
        } else if (argument == "--seed") {
            job.seed = std::strtoull(value.data(), nullptr, 10);
        } else if (argument == "--resolution") {
            job.resolution = std::strtod(value.data(), nullptr);
        } else if (argument == "--section") {
            job.sectionSize = static_cast<u32_t>(std::strtoul(value.data(), nullptr, 10));
        } else if (argument == "--min") {
            if (!ParsePair(value, job.minX, job.minZ)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--min expects <x>,<z>, got {}", value);
            }
        } else if (argument == "--max") {
            if (!ParsePair(value, job.maxX, job.maxZ)) {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--max expects <x>,<z>, got {}", value);
            }
        } else if (argument == "--log-format") {
            if (value == "json") {
                job.logFormat = log::Format::Json;
            } else if (value == "text") {
                job.logFormat = log::Format::Text;
            } else {
                ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                            "--log-format expects text or json, got {}", value);
            }
        } else if (argument == "--param") {
            // Script parameters are forwarded once the Lua runtime exists (milestone 6).
            std::fprintf(stderr, "terrain_export: --param is ignored until milestone 6\n");
        } else {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "unknown option {}",
                        argument);
        }
    }

    if (job.script.empty()) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init, "--script is required");
    }
    if (job.sectionSize == 0 || !IsPowerOfTwo(job.sectionSize)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--section must be a power of two, got {}", job.sectionSize);
    }
    if (job.resolution <= 0.0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--resolution must be positive, got {}", job.resolution);
    }
    if (job.maxX <= job.minX || job.maxZ <= job.minZ) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Init,
                    "--max must be greater than --min on both axes");
    }
    return job;
}

/// Logs the job and stops the application. The evaluation and export stages replace its body in
/// milestones 4 and 7; keeping the loop real means the layer stack and Tracy zones are exercised
/// from the start.
class ExportLayer final : public Layer {
public:
    ExportLayer(Application& application, const ExportJob& job) noexcept
        : Layer(application), m_job(job) {}

    [[nodiscard]] const char* Name() const noexcept override { return "ExportLayer"; }

    void OnUpdate() override {
        LOG_INFO("job: script={} seed={} bounds=({},{})..({},{}) resolution={} section={} out={}",
                 m_job.script, m_job.seed, m_job.minX, m_job.minZ, m_job.maxX, m_job.maxZ,
                 m_job.resolution, m_job.sectionSize, m_job.outputDirectory);

        const Error error = MakeError(ErrorCode::Unsupported, ErrorStage::Export,
                                      "evaluation and PNG export are not implemented yet; "
                                      "see docs/status.md (milestone 4)");
        App().Fail(error, ExitCode::FailedToExport);
    }

private:
    ExportJob m_job;
};

} // namespace

int main(int argc, char** argv) {
    const Result<ExportJob> job = ParseArguments(argc, argv);
    if (!job) {
        std::fprintf(stderr, "terrain_export: %s\n", job.error().Format().data());
        return static_cast<int>(ExitCode::FailedToLoadScript);
    }

    const Result<Application*> application = engine::init(AppInfo{.mode = RunMode::Headless,
                                                                 .name = "terrain_export",
                                                                 .logFormat = job->logFormat,
                                                                 .deviceUuid = job->deviceUuid});
    if (!application) {
        std::fprintf(stderr, "terrain_export: %s\n", application.error().Format().data());
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        return static_cast<int>(ExitCode::FailedToInitializeEngine);
    }

    (*application)->PushLayer<ExportLayer>(*job);
    (*application)->Run();

    ExitCode exit = (*application)->Exit();
    if (const Status closed = engine::shutdown(*application); !closed) {
        std::fprintf(stderr, "terrain_export: %s\n", closed.error().Format().data());
        exit |= ExitCode::FailedToShutdown;
    }
    return static_cast<int>(exit);
}
