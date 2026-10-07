#pragma once

#include <engine/common.hpp>

#include <engine/error.hpp>

#include <string_view>

namespace engine::terrain {

/// Row filter the PNG encoder may use. `All` makes libspng try every filter per row and keep the
/// smallest, which is what it does by default.
enum class PngFilter : u8_t { None = 0, Up, All };

[[nodiscard]] ENGINE_API const char* ToString(PngFilter filter) noexcept;

[[nodiscard]] ENGINE_API Result<PngFilter> ParsePngFilter(std::string_view name);

/// How hard the PNG encoder works. The defaults were measured on the `basic_fbm` case, where
/// encoding is 96% of the wall time: libspng's own defaults (`All`, level 6) take 341 ms and produce
/// 1072 KiB, while `Up` at level 1 takes 82 ms and produces 1124 KiB. Terrain data is high-entropy
/// noise, so searching filters and running the slow zlib passes buys about 5% of size for 4.2x the
/// time. Overridable per job because an archival export may want the opposite trade.
struct PngCompression {
    PngFilter filter = PngFilter::Up;
    /// zlib level, 0 to 9.
    u32_t level = 1;
};

} // namespace engine::terrain

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
                                                 Mapping mapping, f32_t rangeMin, f32_t rangeMax,
                                                 PngCompression compression);

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

/// Writes a raw little-endian `f32` volume one brick at a time.
///
/// A raw volume has a trivial layout, so a brick can be written straight into its place with a seek
/// instead of being staged through a full-size buffer. That keeps CPU memory at one brick no matter
/// how large the volume is, which is the same property the progressive PNG path gives the 2D
/// outputs.
///
/// Sample order is x fastest, then z, then y, matching what the kernels write.
class RawVolumeWriter {
public:
    RawVolumeWriter() = default;
    ~RawVolumeWriter();

    RawVolumeWriter(RawVolumeWriter&& other) noexcept;
    RawVolumeWriter& operator=(RawVolumeWriter&& other) noexcept;
    ENGINE_NO_COPY(RawVolumeWriter);

    [[nodiscard]] static Result<RawVolumeWriter> Create(std::string_view path, u32_t width,
                                                      u32_t height, u32_t depth, u32_t components);

    /// Copies one brick into place. `brick` holds `extent` samples in the kernel layout, and
    /// `origin` is where its first sample belongs in the volume.
    [[nodiscard]] Status WriteBrick(std::array<u32_t, 3> origin, std::array<u32_t, 3> extent,
                                   std::array<u32_t, 3> sourceStride, const f32_t* brick);

    [[nodiscard]] Status Finish();

    [[nodiscard]] u64_t SamplesWritten() const noexcept { return m_samplesWritten; }

private:
    void Release() noexcept;

    /// Opaque `FILE*`, so this header does not pull <cstdio> in.
    void* m_file      = nullptr;
    u32_t m_width     = 0;
    u32_t m_height    = 0;
    u32_t m_depth     = 0;
    u32_t m_components = 1;
    u64_t m_samplesWritten = 0;
};

/// A full-width strip of f32 samples, filled section by section and encoded as a unit.
class BandBuffer {
public:
    BandBuffer() = default;
    ~BandBuffer();

    ENGINE_NO_COPY(BandBuffer);
    ENGINE_NO_MOVE(BandBuffer);

    [[nodiscard]] Status Create(u32_t width, u32_t rows, u32_t components);

    [[nodiscard]] f32_t* Row(u32_t row) noexcept;

    /// Copies the interior of one section into the band at column `column`.
    void Absorb(const f32_t* section, u32_t sectionStride, u32_t column, u32_t rows,
                u32_t columns);

private:
    f32_t* m_data       = nullptr;
    u32_t  m_width      = 0;
    u32_t  m_components = 1;
    u64_t  m_samples    = 0;
};

/// Encodes bands into a `PngWriter` on a worker thread, one thread per output.
///
/// Encoding is 96% of an export's wall time and the GPU is half a percent of it, so overlapping the
/// GPU with the CPU buys almost nothing; what pays is getting the two outputs of a typical job off
/// each other's critical path and off the main loop's. The depth is two bands: one being filled from
/// the readback ring while the other is being deflated.
///
/// Rows still reach libspng strictly in order, because one output has one worker and a band is handed
/// over whole. That is what keeps progressive encoding valid.
class BandEncoder {
public:
    BandEncoder() = default;
    ~BandEncoder();

    ENGINE_NO_COPY(BandEncoder);
    ENGINE_NO_MOVE(BandEncoder);

    /// Takes ownership of `writer` and starts the worker.
    [[nodiscard]] Status Start(PngWriter&& writer, u32_t width, u32_t rows, u32_t components);

    /// The band the caller fills next. Valid until `Submit`.
    [[nodiscard]] BandBuffer& Filling() noexcept;

    /// Hands the filled band to the worker and swaps to the other one.
    ///
    /// Blocks while the worker still holds the other band, which is what bounds memory to two bands
    /// per output no matter how far ahead the GPU gets.
    [[nodiscard]] Status Submit(u32_t rows);

    /// Waits for the queue to drain, finalizes the PNG and joins the worker.
    [[nodiscard]] Status Finish();

    /// Samples the encoder had to clamp, available after `Finish`.
    [[nodiscard]] u64_t ClampedSamples() const noexcept;

    /// Milliseconds the worker spent encoding, which is wall time on the worker rather than on the
    /// main loop. Reported separately for exactly that reason.
    [[nodiscard]] f64_t EncodeMilliseconds() const noexcept;

private:
    /// Everything the worker and the producer share. Defined in the source file, so this header needs
    /// no <thread> or <condition_variable>.
    struct State;

    static void RunWorker(State& state);

    void Release() noexcept;

    State* m_state = nullptr;
};

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
        /// Loading the script and running its `main`, which is where the graph comes from.
        f64_t scriptMs   = 0.0;
        f64_t compileMs  = 0.0;
        f64_t pipelineMs = 0.0;
        f64_t gpuMs      = 0.0;
        f64_t readbackMs = 0.0;
        /// Encoding, measured where it happens: on the band-encoder workers for a PNG, on the main
        /// loop for a volume. It overlaps the rest of the export, so it does not add up to `totalMs`.
        f64_t encodeMs = 0.0;
        /// How long the main loop had to wait for a free band. This is the only part of encoding that
        /// is still on the critical path, so it is the number that says whether the pipeline is deep
        /// enough.
        f64_t encodeStallMs = 0.0;
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
    /// Samples along y. One for a 2D export; the brick height for a volume.
    u32_t depth = 1;

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
