// Dataset runner: golden comparison plus performance regression (spec section 14).
//
// Each case under tests/datasets/<case>/ is run, and its *decoded* samples are compared against the
// expected ones. Decoded, not file bytes: PNG encoding details may differ between zlib builds while
// the image is identical. Timings are then compared against a machine-specific baseline, which the
// run records when there is none yet.
//
//   test_datasets                          run every case
//   test_datasets --case <name>            run one case
//   test_datasets --update-golden <name>   regenerate the expected outputs of one case
//   test_datasets --update-baseline        re-record the timing baseline for this machine
//   test_datasets --list                   list the cases

#include "json_reader.hpp"

#include <engine/application.hpp>
#include <engine/engine.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/platform.hpp>
#include <engine/terrain/export.hpp>
#include <engine/terrain/output_writer.hpp>
#include <engine/vulkan/context.hpp>

#include <spng.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;
using engine::terrain::ExportJob;
using engine::terrain::ExportSummary;

namespace {

/// Where the cases live, relative to the repository root. The runner is executed from the build
/// directory, so the root is resolved from the executable location.
constexpr const char* kDatasetsRelative = "tests/datasets";
constexpr const char* kBaselinesRelative = "tests/baselines";
constexpr const char* kHistoryRelative   = "tests/history";

/// Most samples a difference image is written for. A volume gets its worst-difference numbers
/// reported instead, because there is no obvious 2D view of a 3D difference.
constexpr usize_t kMaxOutputsPerCase = 8;

constexpr f64_t kWarnSlowerFraction = 0.10;
constexpr f64_t kFailSlowerFraction = 0.25;

u32_t g_failures = 0;
u32_t g_warnings = 0;

void Fail(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::fputs("FAIL ", stdout);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    ++g_failures;
}

void Warn(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::fputs("WARN ", stdout);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    ++g_warnings;
}

/// A decoded 16-bit image.
struct Image {
    u32_t              width      = 0;
    u32_t              height     = 0;
    u32_t              components = 0;
    std::vector<u16_t> samples;

    [[nodiscard]] b8_t Matches(const Image& other) const {
        return width == other.width && height == other.height && components == other.components;
    }
};

/// Raw little-endian f32 samples, as an `R3` output is written.
[[nodiscard]] Result<std::vector<f32_t>> ReadRawVolume(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ENGINE_FAIL(ErrorCode::NotFound, ErrorStage::Export, "could not open {}", path);
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || size % static_cast<long>(sizeof(f32_t)) != 0) {
        std::fclose(file);
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export,
                    "{} is {} bytes, which is not a whole number of f32 samples", path, size);
    }
    std::vector<f32_t> samples(static_cast<usize_t>(size) / sizeof(f32_t));
    const usize_t      read = std::fread(samples.data(), sizeof(f32_t), samples.size(), file);
    std::fclose(file);
    if (read != samples.size()) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "read {} of {} samples from {}", read,
                    samples.size(), path);
    }
    return samples;
}

/// Decodes a 16-bit PNG. `SPNG_FMT_PNG` hands samples through in PNG's big-endian order, so they
/// are swapped back here, mirroring what the writer does.
[[nodiscard]] Result<Image> DecodePng(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ENGINE_FAIL(ErrorCode::NotFound, ErrorStage::Export, "could not open {}", path);
    }
    spng_ctx* context = spng_ctx_new(0);
    if (context == nullptr) {
        std::fclose(file);
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export, "could not create a libspng context");
    }
    const auto fail = [&](const char* call, int code) {
        spng_ctx_free(context);
        std::fclose(file);
        return std::unexpected(MakeError(ErrorCode::IoError, ErrorStage::Export, "{} on {}: {}",
                                        call, path, spng_strerror(code)));
    };

    if (const int code = spng_set_png_file(context, file); code != 0) {
        return fail("spng_set_png_file", code);
    }
    spng_ihdr header{};
    if (const int code = spng_get_ihdr(context, &header); code != 0) {
        return fail("spng_get_ihdr", code);
    }
    if (header.bit_depth != 16) {
        spng_ctx_free(context);
        std::fclose(file);
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Export, "{} is {} bits, expected 16", path,
                    header.bit_depth);
    }

    usize_t decodedSize = 0;
    if (const int code = spng_decoded_image_size(context, SPNG_FMT_PNG, &decodedSize); code != 0) {
        return fail("spng_decoded_image_size", code);
    }

    Image image;
    image.width      = header.width;
    image.height     = header.height;
    image.components = header.color_type == SPNG_COLOR_TYPE_TRUECOLOR ? 3u : 1u;
    image.samples.resize(decodedSize / sizeof(u16_t));

    if (const int code = spng_decode_image(context, image.samples.data(), decodedSize,
                                           SPNG_FMT_PNG, 0);
        code != 0) {
        return fail("spng_decode_image", code);
    }
    spng_ctx_free(context);
    std::fclose(file);

    for (u16_t& sample : image.samples) {
        sample = static_cast<u16_t>((sample >> 8) | (sample << 8));
    }
    return image;
}

struct Comparison {
    u64_t differingSamples = 0;
    u32_t maximumDifference = 0;
};

/// Compares decoded samples and, on a mismatch, writes a difference image so the failure is
/// inspectable rather than just a number.
[[nodiscard]] Comparison Compare(const Image& actual, const Image& expected, u32_t tolerance,
                                 const std::string& diffPath) {
    Comparison comparison;
    for (usize_t i = 0; i < actual.samples.size(); ++i) {
        const i32_t difference = std::abs(static_cast<i32_t>(actual.samples[i])
                                          - static_cast<i32_t>(expected.samples[i]));
        if (static_cast<u32_t>(difference) > tolerance) {
            ++comparison.differingSamples;
        }
        comparison.maximumDifference =
            std::max(comparison.maximumDifference, static_cast<u32_t>(difference));
    }

    if (comparison.differingSamples == 0) {
        return comparison;
    }

    // One grayscale channel holding the absolute difference, scaled to the worst case so even a
    // one-unit difference is visible.
    Result<terrain::PngWriter> writer = terrain::PngWriter::Create(
        diffPath, actual.width, actual.height, terrain::Mapping{terrain::Domain::R2, 1}, 0.0f,
        static_cast<f32_t>(comparison.maximumDifference), terrain::PngCompression{});
    if (!writer) {
        Warn("could not write the difference image: %s", writer.error().Format().data());
        return comparison;
    }
    std::vector<f32_t> row(actual.width);
    for (u32_t y = 0; y < actual.height; ++y) {
        for (u32_t x = 0; x < actual.width; ++x) {
            u32_t worst = 0;
            for (u32_t c = 0; c < actual.components; ++c) {
                const usize_t index = (static_cast<usize_t>(y) * actual.width + x)
                                          * actual.components
                                      + c;
                worst = std::max(worst, static_cast<u32_t>(std::abs(
                                            static_cast<i32_t>(actual.samples[index])
                                            - static_cast<i32_t>(expected.samples[index]))));
            }
            row[x] = static_cast<f32_t>(worst);
        }
        if (Status written = writer->WriteRow(row.data()); !written) {
            Warn("writing the difference image failed: %s", written.error().Format().data());
            return comparison;
        }
    }
    if (Status finished = writer->Finish(); !finished) {
        Warn("finishing the difference image failed: %s", finished.error().Format().data());
    }
    std::printf("     difference image written to %s\n", diffPath.c_str());
    return comparison;
}

/// Repository root, derived from the executable directory: <root>/build/<config>/bin/.
[[nodiscard]] std::string RepositoryRoot() {
    std::string directory{platform::ExecutableDirectory()};
    for (char& c : directory) {
        if (c == '\\') {
            c = '/';
        }
    }
    // Strip bin/, the configuration directory and build/.
    for (int i = 0; i < 3; ++i) {
        while (!directory.empty() && directory.back() == '/') {
            directory.pop_back();
        }
        const usize_t slash = directory.rfind('/');
        if (slash == std::string::npos) {
            break;
        }
        directory.resize(slash);
    }
    return directory + "/";
}

/// Stable identity of this machine, so timing baselines are never compared across hardware.
[[nodiscard]] std::string MachineId(vulkan::Context& context) {
    std::array<char, 512> text{};
    std::snprintf(text.data(), text.size(), "%.*s|%u|%.*s|%s",
                  static_cast<int>(context.Info().Name().size()), context.Info().Name().data(),
                  context.Info().driverVersion, static_cast<int>(platform::CpuName().size()),
                  platform::CpuName().data(),
#ifdef ENGINE_DEBUG
                  "Debug"
#else
                  "Release"
#endif
    );
    u64_t hash = 0xCBF29CE484222325ull;
    for (const char* c = text.data(); *c != '\0'; ++c) {
        hash ^= static_cast<u8_t>(*c);
        hash *= 0x100000001B3ull;
    }
    std::array<char, 17> hex{};
    std::snprintf(hex.data(), hex.size(), "%016llx", static_cast<unsigned long long>(hash));
    return std::string{hex.data()};
}

/// What a case declares.
struct Case {
    std::string name;
    std::string directory;
    ExportJob   job;
    /// Maximum absolute difference that still counts as a match: 16-bit steps for a PNG, raw units
    /// for a volume. Zero means bit-exact.
    f64_t tolerance = 0.0;
    /// File names the case produces, read from `case.json` so a case can declare any set of outputs.
    std::vector<std::string> outputs;
    /// Measured passes after one warm-up.
    u32_t passes = 5;
    f64_t warnSlower = kWarnSlowerFraction;
    f64_t failSlower = kFailSlowerFraction;
};

[[nodiscard]] Result<Case> LoadCase(const std::string& root, const std::string& name) {
    Case testCase;
    testCase.name      = name;
    testCase.directory = root + kDatasetsRelative + "/" + name + "/";

    Result<test::Json> json = test::Json::Load(testCase.directory + "case.json");
    if (!json) {
        return std::unexpected(json.error());
    }

    testCase.job.seed        = static_cast<u64_t>(json->Number("seed", 0));
    testCase.job.minX        = json->Number("bounds.min_x", 0.0);
    testCase.job.minZ        = json->Number("bounds.min_z", 0.0);
    testCase.job.maxX        = json->Number("bounds.max_x", 512.0);
    testCase.job.maxZ        = json->Number("bounds.max_z", 512.0);
    testCase.job.minY        = json->Number("bounds.min_y", 0.0);
    testCase.job.maxY        = json->Number("bounds.max_y", 256.0);
    testCase.job.resolution  = json->Number("resolution", 1.0);
    testCase.job.sectionSize = static_cast<u32_t>(json->Number("section", 256));
    testCase.tolerance       = json->Number("tolerance", 0.0);

    const usize_t outputCount = json->ArraySize("outputs");
    for (usize_t i = 0; i < outputCount; ++i) {
        std::array<char, 64> key{};
        std::snprintf(key.data(), key.size(), "outputs.%zu", i);
        const std::string_view value = json->Text(key.data());
        if (!value.empty()) {
            testCase.outputs.emplace_back(value);
        }
    }
    if (testCase.outputs.empty()) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "case {} declares no outputs", name);
    }
    testCase.passes          = static_cast<u32_t>(json->Number("performance.passes", 5.0));
    testCase.warnSlower      = json->Number("performance.warn_slower", kWarnSlowerFraction);
    testCase.failSlower      = json->Number("performance.fail_slower", kFailSlowerFraction);

    // User parameters, as a list of `key=value` strings. A list rather than an object because a
    // script now declares whatever parameters it wants, so there is no fixed set of keys to look for,
    // and `Params::Assign` already parses this form for `--param`.
    const usize_t paramCount = json->ArraySize("params");
    for (usize_t i = 0; i < paramCount; ++i) {
        std::array<char, 32> key{};
        std::snprintf(key.data(), key.size(), "params.%zu", i);
        const std::string_view assignment = json->Text(key.data());
        if (!assignment.empty() && !testCase.job.params.Assign(assignment)) {
            Warn("%s: could not read the parameter '%s'", name.c_str(),
                 std::string(assignment).c_str());
        }
    }

    // The script is what builds the graph, and it is hashed into the metadata sidecar, so a change to
    // it shows up in the golden data.
    static std::array<char, 512> scriptPath{};
    const std::string            script = testCase.directory + "script.lua";
    detail::CopyBounded(scriptPath, script);
    testCase.job.script = std::string_view{scriptPath.data()};
    return testCase;
}

/// Lists the case directories under tests/datasets/.
[[nodiscard]] std::vector<std::string> DiscoverCases(const std::string& root) {
    std::vector<std::string> names;
    // No directory iteration in the engine, and <filesystem> is enough for test tooling.
    const std::string base = root + kDatasetsRelative;
    for (const auto& entry : std::filesystem::directory_iterator(
             base, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "case.json")) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

struct PassResult {
    f64_t totalMs = 0.0;
    f64_t gpuMs   = 0.0;
};

/// Runs the case once into `outputDirectory`.
[[nodiscard]] Result<PassResult> RunOnce(Application& application, const Case& testCase,
                                        const std::string& outputDirectory) {
    ExportJob job            = testCase.job;
    job.outputDirectory      = outputDirectory;
    Result<ExportSummary> summary = terrain::RunExport(application, job);
    if (!summary) {
        return std::unexpected(summary.error());
    }
    return PassResult{.totalMs = summary->totalMs, .gpuMs = summary->gpuMs};
}

[[nodiscard]] f64_t Median(std::vector<f64_t> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

/// Compares one measured value against its baseline and reports according to the thresholds.
void CheckTiming(const Case& testCase, const char* label, f64_t measured, f64_t baseline) {
    if (baseline <= 0.0) {
        return;
    }
    const f64_t ratio = measured / baseline;
    if (ratio > 1.0 + testCase.failSlower) {
        Fail("%s: %s is %.1f%% slower than the baseline (%.2f ms vs %.2f ms)",
             testCase.name.c_str(), label, (ratio - 1.0) * 100.0, measured, baseline);
    } else if (ratio > 1.0 + testCase.warnSlower) {
        Warn("%s: %s is %.1f%% slower than the baseline (%.2f ms vs %.2f ms)",
             testCase.name.c_str(), label, (ratio - 1.0) * 100.0, measured, baseline);
    } else if (ratio < 1.0 - testCase.failSlower) {
        std::printf("NOTE %s: %s is %.1f%% faster than the baseline (%.2f ms vs %.2f ms); "
                    "consider updating it\n",
                    testCase.name.c_str(), label, (1.0 - ratio) * 100.0, measured, baseline);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string updateGolden;
    std::string onlyCase;
    b8_t        listOnly       = false;
    b8_t        updateBaseline = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--list") {
            listOnly = true;
        } else if (argument == "--update-golden" && i + 1 < argc) {
            updateGolden = argv[++i];
        } else if (argument == "--update-baseline") {
            updateBaseline = true;
        } else if (argument == "--case" && i + 1 < argc) {
            onlyCase = argv[++i];
        } else {
            std::fprintf(stderr, "test_datasets: unknown argument %s\n", argument.data());
            return 2;
        }
    }

    // The executable directory is only known once the platform module has queried it, and the
    // repository root is derived from it. Safe to call again: engine::init repeats it.
    if (Status started = platform::Init(); !started) {
        std::fprintf(stderr, "test_datasets: %s\n", started.error().Format().data());
        return 2;
    }

    const std::string root = RepositoryRoot();
    std::vector<std::string> cases;
    try {
        cases = DiscoverCases(root);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_datasets: could not list %s%s: %s\n", root.c_str(),
                     kDatasetsRelative, error.what());
        return 2;
    }

    if (listOnly) {
        for (const std::string& name : cases) {
            std::printf("%s\n", name.c_str());
        }
        return 0;
    }
    if (cases.empty()) {
        std::fprintf(stderr, "test_datasets: no cases found under %s%s\n", root.c_str(),
                     kDatasetsRelative);
        return 2;
    }

    const Result<Application*> application =
        engine::init(AppInfo{.mode = RunMode::Headless, .name = "test_datasets"});
    if (!application) {
        std::fprintf(stderr, "test_datasets: %s\n", application.error().Format().data());
        const Status closed = engine::shutdown(nullptr);
        (void)closed;
        return 1;
    }
    vulkan::Context& context  = VulkanContext(**application);
    const std::string machine = MachineId(context);
    std::printf("machine id: %s\n", machine.c_str());

    for (const std::string& name : cases) {
        if (!onlyCase.empty() && name != onlyCase) {
            continue;
        }
        if (!updateGolden.empty() && name != updateGolden) {
            continue;
        }

        std::printf("-- %s\n", name.c_str());
        Result<Case> testCase = LoadCase(root, name);
        if (!testCase) {
            Fail("%s: %s", name.c_str(), testCase.error().Format().data());
            continue;
        }

        const b8_t        updating = !updateGolden.empty();
        const std::string expected = testCase->directory + "expected";
        const std::string actual   = updating ? expected : testCase->directory + "actual";
        if (Status created = platform::MakeDirectory(actual); !created) {
            Fail("%s: %s", name.c_str(), created.error().Format().data());
            continue;
        }

        const Result<PassResult> first = RunOnce(**application, *testCase, actual);
        if (!first) {
            Fail("%s: %s", name.c_str(), first.error().Format().data());
            continue;
        }

        if (updating) {
            std::printf("     expected outputs regenerated in %s\n", expected.c_str());
            continue;
        }

        // Correctness first: decoded samples, not file bytes.
        b8_t caseFailed = false;
        for (const std::string& output : testCase->outputs) {
            const b8_t        isVolume     = output.size() > 4 && output.ends_with(".raw");
            const std::string actualPath   = actual + "/" + output;
            const std::string expectedPath = expected + "/" + output;

            if (isVolume) {
                Result<std::vector<f32_t>> expectedVolume = ReadRawVolume(expectedPath);
                Result<std::vector<f32_t>> actualVolume   = ReadRawVolume(actualPath);
                if (!expectedVolume || !actualVolume) {
                    Fail("%s/%s: %s", name.c_str(), output.c_str(),
                         (!expectedVolume ? expectedVolume : actualVolume).error().Format().data());
                    caseFailed = true;
                    continue;
                }
                if (expectedVolume->size() != actualVolume->size()) {
                    Fail("%s/%s: %zu samples, expected %zu", name.c_str(), output.c_str(),
                         actualVolume->size(), expectedVolume->size());
                    caseFailed = true;
                    continue;
                }
                u64_t differing = 0;
                f64_t worst     = 0.0;
                for (usize_t i = 0; i < expectedVolume->size(); ++i) {
                    const f64_t difference = std::fabs(static_cast<f64_t>((*actualVolume)[i])
                                                       - static_cast<f64_t>((*expectedVolume)[i]));
                    worst = std::max(worst, difference);
                    // A volume is raw f32, so the tolerance is in absolute units rather than in
                    // 16-bit steps; a tolerance of zero means bit-exact.
                    if (difference > static_cast<f64_t>(testCase->tolerance)) {
                        ++differing;
                    }
                }
                if (differing != 0) {
                    Fail("%s/%s: %llu of %zu samples differ, worst %.6g", name.c_str(),
                         output.c_str(), static_cast<unsigned long long>(differing),
                         expectedVolume->size(), worst);
                    caseFailed = true;
                } else {
                    std::printf("     %s matches (%zu samples, worst %.6g)\n", output.c_str(),
                                expectedVolume->size(), worst);
                }
                continue;
            }

            Result<Image> expectedImage = DecodePng(expectedPath);
            if (!expectedImage) {
                Fail("%s/%s: %s", name.c_str(), output.c_str(),
                     expectedImage.error().Format().data());
                caseFailed = true;
                continue;
            }
            Result<Image> actualImage = DecodePng(actualPath);
            if (!actualImage) {
                Fail("%s/%s: %s", name.c_str(), output.c_str(),
                     actualImage.error().Format().data());
                caseFailed = true;
                continue;
            }
            if (!actualImage->Matches(*expectedImage)) {
                Fail("%s/%s: geometry is %ux%ux%u, expected %ux%ux%u", name.c_str(),
                     output.c_str(), actualImage->width, actualImage->height,
                     actualImage->components, expectedImage->width, expectedImage->height,
                     expectedImage->components);
                caseFailed = true;
                continue;
            }

            const Comparison comparison =
                Compare(*actualImage, *expectedImage, static_cast<u32_t>(testCase->tolerance),
                        actual + "/" + output + "_diff.png");
            if (comparison.differingSamples != 0) {
                Fail("%s/%s: %llu sample(s) differ by more than %u, worst difference %u",
                     name.c_str(), output.c_str(),
                     static_cast<unsigned long long>(comparison.differingSamples),
                     static_cast<u32_t>(testCase->tolerance), comparison.maximumDifference);
                caseFailed = true;
            } else {
                std::printf("     %s matches (worst difference %u, tolerance %u)\n",
                            output.c_str(), comparison.maximumDifference,
                            static_cast<u32_t>(testCase->tolerance));
            }
        }

        if (caseFailed) {
            continue;
        }

        // Performance regression, Release only: the numbers are meaningless with /Od.
#ifdef ENGINE_DEBUG
        std::printf("     performance comparison skipped in a Debug build\n");
        (void)first;
#else
        std::vector<f64_t> totals;
        std::vector<f64_t> gpus;
        totals.push_back(first->totalMs);
        gpus.push_back(first->gpuMs);
        for (u32_t pass = 1; pass < testCase->passes; ++pass) {
            const Result<PassResult> measured = RunOnce(**application, *testCase, actual);
            if (!measured) {
                Fail("%s: %s", name.c_str(), measured.error().Format().data());
                break;
            }
            totals.push_back(measured->totalMs);
            gpus.push_back(measured->gpuMs);
        }
        // The first pass is the warm-up: drop it.
        if (totals.size() > 1) {
            totals.erase(totals.begin());
            gpus.erase(gpus.begin());
        }
        const f64_t medianTotal = Median(totals);
        const f64_t medianGpu   = Median(gpus);
        std::printf("     median total %.2f ms, median GPU %.3f ms over %zu pass(es)\n",
                    medianTotal, medianGpu, totals.size());

        const std::string baselinePath =
            root + kBaselinesRelative + "/" + machine + ".json";
        Result<test::Json> baseline = test::Json::Load(baselinePath);
        // A baseline is re-recorded only on request, for the same reason golden outputs are: a
        // number that moves on its own measures nothing.
        if (!baseline || updateBaseline) {
            if (Status created =
                    platform::MakeDirectory(root + kBaselinesRelative);
                !created) {
                Warn("%s", created.error().Format().data());
            }
            std::FILE* file = std::fopen(baselinePath.c_str(), "wb");
            if (file == nullptr) {
                Warn("could not record a baseline at %s", baselinePath.c_str());
            } else {
                std::fprintf(file,
                             "{\n  \"%s\": { \"total_ms\": %.4f, \"gpu_ms\": %.4f }\n}\n",
                             name.c_str(), medianTotal, medianGpu);
                std::fclose(file);
                std::printf("NOTE %s baseline for this machine: %s\n",
                            updateBaseline ? "re-recorded the" : "recorded a first",
                            baselinePath.c_str());
            }
        } else {
            CheckTiming(*testCase, "total time", medianTotal,
                        baseline->Number(name + ".total_ms", 0.0));
            CheckTiming(*testCase, "GPU time", medianGpu,
                        baseline->Number(name + ".gpu_ms", 0.0));
        }

        if (Status created = platform::MakeDirectory(root + kHistoryRelative); !created) {
            Warn("%s", created.error().Format().data());
        }
        const std::string historyPath = root + kHistoryRelative + "/" + machine + ".jsonl";
        if (std::FILE* file = std::fopen(historyPath.c_str(), "ab"); file != nullptr) {
            std::fprintf(file,
                         "{\"case\":\"%s\",\"commit\":\"%s\",\"total_ms\":%.4f,"
                         "\"gpu_ms\":%.4f}\n",
                         name.c_str(), CommitHash(), medianTotal, medianGpu);
            std::fclose(file);
        }
#endif
    }

    if (const Status closed = engine::shutdown(*application); !closed) {
        std::fprintf(stderr, "test_datasets: %s\n", closed.error().Format().data());
        return 1;
    }

    if (const u64_t errors = vulkan::ValidationErrorCount(); errors != 0) {
        Fail("the Vulkan debug messenger reported %llu error(s); see the log above",
             static_cast<unsigned long long>(errors));
    }

    std::printf("test_datasets: %u failure(s), %u warning(s)\n", g_failures, g_warnings);
    return g_failures == 0 ? 0 : 1;
}
