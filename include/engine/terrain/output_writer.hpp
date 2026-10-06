#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/terrain/mapping.hpp>
#    include <engine/vulkan/allocator.hpp>

#    include <array>
#    include <string_view>

namespace engine::terrain {

/// File format an output is written in, decided by its mapping (spec section 12).
enum class OutputFormat : u32_t {
    /// `R2 -> R1`: 16-bit grayscale PNG, normalized from the declared range.
    Grayscale16 = 0,
    /// `R2 -> R2` and `R2 -> R3`: 16-bit RGB PNG remapped from [-1, 1]. `R2 -> R2` writes blue as 0.
    Rgb16,
    /// `R3 -> Rm`: raw little-endian f32, dimensions recorded in the sidecar.
    RawF32,
};

[[nodiscard]] const char* ToString(OutputFormat format) noexcept;

/// The format a mapping is written in. `R2` with more than 3 components has no PNG form.
[[nodiscard]] Result<OutputFormat> FormatFor(Mapping mapping);

/// File extension for a format, without the dot.
[[nodiscard]] const char* ExtensionFor(OutputFormat format) noexcept;

/// Streams a 16-bit PNG one row at a time, so a full large map never sits in CPU RAM.
///
/// Rows arrive as `f32` samples and are normalized, clamped and byte-swapped to PNG's big-endian
/// order here. The clamped-sample count is reported, because clamping almost always means the
/// declared output range is wrong.
class PngWriter {
public:
    PngWriter() = default;
    ~PngWriter();

    PngWriter(PngWriter&& other) noexcept;
    PngWriter& operator=(PngWriter&& other) noexcept;
    ENGINE_NO_COPY(PngWriter);

    /// The format and the number of source components both come from `mapping`.
    /// `rangeMin`/`rangeMax` are used by `Grayscale16`; `Rgb16` always remaps from [-1, 1].
    [[nodiscard]] static Result<PngWriter> Create(std::string_view path, u32_t width, u32_t height,
                                                 Mapping mapping, f32_t rangeMin, f32_t rangeMax);

    /// Writes one row of `width * components` samples, where components comes from the format.
    [[nodiscard]] Status WriteRow(const f32_t* samples);

    /// Flushes the remaining PNG chunks and closes the file.
    [[nodiscard]] Status Finish();

    [[nodiscard]] u64_t ClampedSamples() const noexcept { return m_clamped; }
    [[nodiscard]] u32_t RowsWritten() const noexcept { return m_rowsWritten; }

private:
    void Release() noexcept;

    /// Opaque `spng_ctx*` and `FILE*`, so this header pulls in neither libspng nor <cstdio>.
    void* m_context = nullptr;
    void* m_file    = nullptr;
    /// One scratch row of `width * components` big-endian samples, from `CPU/General`.
    u16_t* m_row = nullptr;
    u32_t  m_width  = 0;
    u32_t  m_height = 0;
    /// Components per pixel in the PNG: 3 for `Rgb16`, 1 otherwise.
    u32_t m_components = 1;
    /// Components per sample in the incoming rows, which is the mapping's component count.
    u32_t m_sourceComponents = 1;
    u32_t m_rowsWritten      = 0;
    OutputFormat          m_format   = OutputFormat::Grayscale16;
    f32_t                 m_rangeMin = 0.0f;
    f32_t                 m_rangeMax = 1.0f;
    u64_t                 m_clamped  = 0;
};

/// Writes raw little-endian `f32` samples, for `R3` volumes.
[[nodiscard]] Status WriteRawVolume(std::string_view path, const f32_t* samples, u64_t count);

/// Everything the metadata sidecar records (spec section 12).
///
/// Fixed-size inline storage: the sidecar is filled across the engine boundary and written in one
/// place, and nothing here should allocate.
struct ExportMetadata {
    static constexpr usize_t kMaxOutputs = 8;
    static constexpr usize_t kMaxText    = 128;
    static constexpr usize_t kMaxPath    = 256;

    struct Output {
        std::array<char, 32>      name{};
        std::array<char, kMaxPath> file{};
        Mapping                   mapping;
        OutputFormat              format         = OutputFormat::Grayscale16;
        f32_t                     rangeMin       = 0.0f;
        f32_t                     rangeMax       = 1.0f;
        u64_t                     clampedSamples = 0;
    };

    struct Timings {
        f64_t totalMs    = 0.0;
        f64_t compileMs  = 0.0;
        f64_t pipelineMs = 0.0;
        f64_t gpuMs      = 0.0;
        f64_t readbackMs = 0.0;
        f64_t encodeMs   = 0.0;
    };

    std::array<char, kMaxPath> scriptPath{};
    /// 64-bit FNV-1a of the script contents, as 16 hex characters.
    std::array<char, 17> scriptHash{};

    u64_t seed       = 0;
    f64_t minX       = 0.0;
    f64_t minZ       = 0.0;
    f64_t maxX       = 0.0;
    f64_t maxZ       = 0.0;
    f64_t resolution = 1.0;
    u32_t sectionSize = 0;
    u32_t width       = 0;
    u32_t height      = 0;

    std::array<Output, kMaxOutputs> outputs{};
    usize_t                         outputCount = 0;

    Timings timings;

    u64_t                                                              peakCpuBytes = 0;
    std::array<u64_t, static_cast<usize_t>(vulkan::VramCategory::Count)> peakVram{};

    std::array<char, kMaxText> gpuName{};
    std::array<char, kMaxText> cpuName{};
    std::array<char, kMaxText> osName{};
    std::array<char, 32>       buildType{};
    u32_t                      driverVersion = 0;
    u32_t                      apiVersion    = 0;

    /// Adds an output entry, returning false when `kMaxOutputs` is already reached.
    [[nodiscard]] b8_t AddOutput(std::string_view name, std::string_view file, Mapping mapping,
                                 OutputFormat format, f32_t rangeMin, f32_t rangeMax,
                                 u64_t clampedSamples);
};

/// Writes the `.json` sidecar next to the outputs. The timing and environment fields are what the
/// performance regression tests compare.
[[nodiscard]] Status WriteMetadata(std::string_view path, const ExportMetadata& metadata);

/// 64-bit FNV-1a of a file's contents, as 16 hex characters. Empty on a read failure.
[[nodiscard]] std::array<char, 17> HashFile(std::string_view path);

} // namespace engine::terrain

#endif // IS_ENGINE
