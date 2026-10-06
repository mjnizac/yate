#pragma once

#include <engine/application.hpp>
#include <engine/common.hpp>
#include <engine/error.hpp>

#include <array>
#include <string_view>

namespace engine::terrain {

/// Key/value parameters a job carries. Until the Lua runtime exists these also drive the noise
/// kernel; from milestone 6 on they are the user parameters `main(params)` receives, so the CLI
/// surface and the plumbing stay the same.
class ENGINE_API Params {
public:
    static constexpr usize_t kMaxEntries = 16;
    static constexpr usize_t kMaxKey     = 32;
    static constexpr usize_t kMaxValue   = 64;

    /// Parses `key=value`. Returns false when the form is wrong or the table is full.
    [[nodiscard]] b8_t Assign(std::string_view assignment);

    [[nodiscard]] b8_t Set(std::string_view key, std::string_view value);

    [[nodiscard]] std::string_view Text(std::string_view key) const;

    /// Numeric value of `key`, or `fallback` when it is absent or unparseable.
    [[nodiscard]] f64_t Number(std::string_view key, f64_t fallback) const;

    /// Reads a `a,b` pair. Returns false when `key` is absent or not a pair.
    [[nodiscard]] b8_t Pair(std::string_view key, f64_t& first, f64_t& second) const;

    [[nodiscard]] usize_t Count() const noexcept { return m_count; }
    [[nodiscard]] std::string_view KeyAt(usize_t index) const;
    [[nodiscard]] std::string_view ValueAt(usize_t index) const;

private:
    struct Entry {
        std::array<char, kMaxKey>   key{};
        std::array<char, kMaxValue> value{};
    };

    std::array<Entry, kMaxEntries> m_entries{};
    usize_t                        m_count = 0;
};

/// One batch export, as described by the `terrain_export` CLI (spec section 12).
struct ExportJob {
    std::string_view script;
    std::string_view outputDirectory = "out/";
    u64_t            seed            = 0;
    f64_t            minX            = 0.0;
    f64_t            minZ            = 0.0;
    f64_t            maxX            = 1024.0;
    f64_t            maxZ            = 1024.0;
    f64_t            resolution      = 1.0;
    u32_t            sectionSize     = 512;
    /// One file per tile instead of one stitched image.
    b8_t   tiles = false;
    Params params;
};

/// What an export produced, for the CLI log and for the dataset runner.
struct ExportSummary {
    static constexpr usize_t kMaxOutputs = 8;

    u32_t width        = 0;
    u32_t height       = 0;
    u32_t sectionsX    = 0;
    u32_t sectionsZ    = 0;
    u64_t sectionCount = 0;
    /// Samples the declared output range clamped, summed over every output.
    u64_t clampedSamples = 0;
    f64_t totalMs        = 0.0;
    f64_t gpuMs          = 0.0;

    std::array<std::array<char, 256>, kMaxOutputs> files{};
    usize_t                                        fileCount = 0;
    std::array<char, 256>                          metadataFile{};
};

/// Evaluates every section of `job` and writes its outputs plus the `.json` sidecar.
///
/// Until the graph compiler lands (milestone 5) the graph is a single fBm node, configured through
/// `job.params`: `frequency`, `octaves`, `lacunarity`, `persistence`, `amplitude`, `offset`,
/// `range`, `normalize` and `normals`.
[[nodiscard]] ENGINE_API Result<ExportSummary> RunExport(Application&     application,
                                                        const ExportJob& job);

} // namespace engine::terrain
