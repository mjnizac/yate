#include <engine/terrain/output_writer.hpp>

#include <engine/assert.hpp>
#include <engine/engine.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>

#include <spng.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace engine::terrain {

namespace {

constexpr u16_t kMaxSample = 65535;

/// PNG stores 16-bit samples big-endian, and libspng hands `SPNG_FMT_PNG` rows through untouched,
/// so the swap is ours to do.
constexpr u16_t ToBigEndian(u16_t value) noexcept {
    return static_cast<u16_t>((value >> 8) | (value << 8));
}

/// Maps `value` from [min, max] to [0, 65535], clamping. `clamped` counts the samples that fell
/// outside, which is the signal that a declared range is wrong.
constexpr u16_t Quantize(f32_t value, f32_t minimum, f32_t maximum, u64_t& clamped) noexcept {
    const f32_t span = maximum - minimum;
    if (span <= 0.0f) {
        return 0;
    }
    const f32_t normalized = (value - minimum) / span;
    if (normalized <= 0.0f) {
        if (normalized < 0.0f) {
            ++clamped;
        }
        return 0;
    }
    if (normalized >= 1.0f) {
        if (normalized > 1.0f) {
            ++clamped;
        }
        return kMaxSample;
    }
    return static_cast<u16_t>(normalized * static_cast<f32_t>(kMaxSample) + 0.5f);
}

u32_t ComponentsOf(OutputFormat format) noexcept {
    return format == OutputFormat::Rgb16 ? 3u : 1u;
}

/// Minimal JSON object writer: fixed output, no heap, correct commas and escaping.
class JsonWriter {
public:
    explicit JsonWriter(std::FILE* file) noexcept : m_file(file) {}

    void BeginObject(const char* key = nullptr) {
        Separator();
        if (key != nullptr) {
            std::fprintf(m_file, "\"%s\": ", key);
        }
        std::fputc('{', m_file);
        Push(false);
    }

    void BeginArray(const char* key) {
        Separator();
        std::fprintf(m_file, "\"%s\": [", key);
        Push(true);
    }

    void End() {
        const b8_t wasArray = m_isArray[m_depth - 1];
        Pop();
        std::fputc(wasArray ? ']' : '}', m_file);
    }

    void EndArray() { End(); }

    void Key(const char* key, std::string_view value) {
        Separator();
        std::fprintf(m_file, "\"%s\": \"", key);
        WriteEscaped(value);
        std::fputc('"', m_file);
    }

    void Key(const char* key, u64_t value) {
        Separator();
        std::fprintf(m_file, "\"%s\": %llu", key, static_cast<unsigned long long>(value));
    }

    void Key(const char* key, u32_t value) { Key(key, static_cast<u64_t>(value)); }

    void Key(const char* key, f64_t value) {
        Separator();
        std::fprintf(m_file, "\"%s\": %.9g", key, value);
    }

private:
    static constexpr usize_t kMaxDepth = 8;

    void Push(b8_t isArray) {
        ENGINE_ASSERT_RETURN(, m_depth < kMaxDepth, "JSON nesting deeper than {}", kMaxDepth);
        m_isArray[m_depth] = isArray;
        m_first[m_depth]   = true;
        ++m_depth;
    }

    void Pop() {
        ENGINE_ASSERT_RETURN(, m_depth > 0, "unbalanced JSON object");
        --m_depth;
    }

    void Separator() {
        if (m_depth == 0) {
            return;
        }
        if (m_first[m_depth - 1]) {
            m_first[m_depth - 1] = false;
        } else {
            std::fputc(',', m_file);
        }
    }

    void WriteEscaped(std::string_view text) {
        for (const char c : text) {
            switch (c) {
                case '"': std::fputs("\\\"", m_file); break;
                case '\\': std::fputs("\\\\", m_file); break;
                case '\n': std::fputs("\\n", m_file); break;
                case '\r': std::fputs("\\r", m_file); break;
                case '\t': std::fputs("\\t", m_file); break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        std::fprintf(m_file, "\\u%04x", static_cast<unsigned>(c));
                    } else {
                        std::fputc(c, m_file);
                    }
            }
        }
    }

    std::FILE*                     m_file;
    std::array<b8_t, kMaxDepth>    m_isArray{};
    std::array<b8_t, kMaxDepth>    m_first{};
    usize_t                        m_depth = 0;
};

} // namespace

const char* ToString(OutputFormat format) noexcept {
    switch (format) {
        case OutputFormat::Grayscale16: return "png16-gray";
        case OutputFormat::Rgb16: return "png16-rgb";
        case OutputFormat::RawF32: return "raw-f32";
    }
    return "unknown";
}

const char* ExtensionFor(OutputFormat format) noexcept {
    return format == OutputFormat::RawF32 ? "raw" : "png";
}

Result<OutputFormat> FormatFor(Mapping mapping) {
    if (!IsValid(mapping)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export, "invalid mapping");
    }
    if (mapping.domain == Domain::R3) {
        return OutputFormat::RawF32;
    }
    if (mapping.components == 1) {
        return OutputFormat::Grayscale16;
    }
    if (mapping.components <= 3) {
        return OutputFormat::Rgb16;
    }
    ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Export,
                "{} has no PNG form; only 1 to 3 components are supported", ToString(mapping));
}

// --- PngWriter ----------------------------------------------------------------------------------

Result<PngWriter> PngWriter::Create(std::string_view path, u32_t width, u32_t height,
                                   Mapping mapping, f32_t rangeMin, f32_t rangeMax) {
    Result<OutputFormat> resolved = FormatFor(mapping);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    const OutputFormat format = *resolved;
    if (format == OutputFormat::RawF32) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "a raw volume is not written through PngWriter");
    }
    if (width == 0 || height == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export, "image is {}x{}", width,
                    height);
    }

    std::array<char, 512> nullTerminated{};
    detail::CopyBounded(nullTerminated, path);

    std::FILE* file = std::fopen(nullTerminated.data(), "wb");
    if (file == nullptr) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not open {} for writing", path);
    }

    spng_ctx* context = spng_ctx_new(SPNG_CTX_ENCODER);
    if (context == nullptr) {
        std::fclose(file);
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export, "could not create a libspng context");
    }

    const u32_t components = ComponentsOf(format);

    spng_ihdr header{};
    header.width      = width;
    header.height     = height;
    header.bit_depth  = 16;
    header.color_type = format == OutputFormat::Rgb16
                            ? static_cast<u8_t>(SPNG_COLOR_TYPE_TRUECOLOR)
                            : static_cast<u8_t>(SPNG_COLOR_TYPE_GRAYSCALE);

    const auto fail = [&](int code, const char* call) {
        spng_ctx_free(context);
        std::fclose(file);
        return std::unexpected(MakeError(ErrorCode::IoError, ErrorStage::Export, "{} failed: {}",
                                        call, spng_strerror(code)));
    };

    if (const int code = spng_set_png_file(context, file); code != 0) {
        return fail(code, "spng_set_png_file");
    }
    if (const int code = spng_set_ihdr(context, &header); code != 0) {
        return fail(code, "spng_set_ihdr");
    }
    // Progressive encoding: rows are handed over one at a time, so the whole image never has to
    // exist in CPU RAM (spec section 12).
    if (const int code = spng_encode_image(context, nullptr, 0, SPNG_FMT_PNG,
                                           SPNG_ENCODE_PROGRESSIVE | SPNG_ENCODE_FINALIZE);
        code != 0) {
        return fail(code, "spng_encode_image");
    }

    PngWriter writer;
    writer.m_context    = context;
    writer.m_file       = file;
    writer.m_width      = width;
    writer.m_height     = height;
    writer.m_components       = components;
    writer.m_sourceComponents = mapping.components;
    writer.m_format           = format;
    writer.m_rangeMin   = rangeMin;
    writer.m_rangeMax   = rangeMax;
    writer.m_row        = static_cast<u16_t*>(
        memory::General().Allocate(static_cast<usize_t>(width) * components * sizeof(u16_t),
                                   alignof(u16_t)));
    if (writer.m_row == nullptr) {
        spng_ctx_free(context);
        std::fclose(file);
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export,
                    "could not allocate a {}-sample scratch row", width * components);
    }

    LOG_DEBUG("writing {} ({}x{}, {})", path, width, height, ToString(format));
    return writer;
}

PngWriter::~PngWriter() { Release(); }

void PngWriter::Release() noexcept {
    if (m_row != nullptr) {
        memory::General().Free(m_row);
        m_row = nullptr;
    }
    if (m_context != nullptr) {
        spng_ctx_free(static_cast<spng_ctx*>(m_context));
        m_context = nullptr;
    }
    if (m_file != nullptr) {
        std::fclose(static_cast<std::FILE*>(m_file));
        m_file = nullptr;
    }
}

PngWriter::PngWriter(PngWriter&& other) noexcept { *this = std::move(other); }

PngWriter& PngWriter::operator=(PngWriter&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Release();
    m_context     = other.m_context;
    m_file        = other.m_file;
    m_row         = other.m_row;
    m_width       = other.m_width;
    m_height      = other.m_height;
    m_components  = other.m_components;
    m_rowsWritten = other.m_rowsWritten;
    m_format      = other.m_format;
    m_rangeMin    = other.m_rangeMin;
    m_rangeMax    = other.m_rangeMax;
    m_clamped     = other.m_clamped;
    other.m_context = nullptr;
    other.m_file    = nullptr;
    other.m_row     = nullptr;
    return *this;
}

Status PngWriter::WriteRow(const f32_t* samples) {
    if (m_context == nullptr) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Export, "writing to a closed PNG");
    }
    if (m_rowsWritten >= m_height) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "row {} is past the declared height of {}", m_rowsWritten, m_height);
    }

    if (m_format == OutputFormat::Rgb16) {
        // Vectors are remapped from [-1, 1]; an R2 -> R2 value has no third component, so blue
        // is written as 0 (spec section 12).
        for (u32_t x = 0; x < m_width; ++x) {
            for (u32_t c = 0; c < 3; ++c) {
                const u16_t quantized =
                    c < m_sourceComponents
                        ? Quantize(samples[x * m_sourceComponents + c], -1.0f, 1.0f, m_clamped)
                        : 0;
                m_row[x * 3 + c] = ToBigEndian(quantized);
            }
        }
    } else {
        for (u32_t x = 0; x < m_width; ++x) {
            m_row[x] = ToBigEndian(Quantize(samples[x], m_rangeMin, m_rangeMax, m_clamped));
        }
    }

    const usize_t rowBytes = static_cast<usize_t>(m_width) * m_components * sizeof(u16_t);
    const int     code = spng_encode_row(static_cast<spng_ctx*>(m_context), m_row, rowBytes);
    ++m_rowsWritten;
    if (code == SPNG_EOI) {
        return {}; // Last row accepted; libspng has finalized the stream.
    }
    if (code != 0) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "spng_encode_row failed: {}",
                    spng_strerror(code));
    }
    return {};
}

Status PngWriter::Finish() {
    if (m_context == nullptr) {
        return {};
    }
    if (m_rowsWritten != m_height) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Export,
                    "{} of {} rows were written before finishing", m_rowsWritten, m_height);
    }
    if (m_clamped != 0) {
        LOG_WARN("{} sample(s) were clamped by the declared output range; the range is probably "
                 "wrong",
                 m_clamped);
    }
    Release();
    return {};
}

// --- Raw volumes --------------------------------------------------------------------------------

Status WriteRawVolume(std::string_view path, const f32_t* samples, u64_t count) {
    std::array<char, 512> nullTerminated{};
    detail::CopyBounded(nullTerminated, path);
    std::FILE* file = std::fopen(nullTerminated.data(), "wb");
    if (file == nullptr) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not open {} for writing", path);
    }
    const usize_t written = std::fwrite(samples, sizeof(f32_t), static_cast<usize_t>(count), file);
    std::fclose(file);
    if (written != count) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "wrote {} of {} samples to {}", written,
                    count, path);
    }
    return {};
}

// --- Metadata -----------------------------------------------------------------------------------

b8_t ExportMetadata::AddOutput(std::string_view name, std::string_view file, Mapping mapping,
                               OutputFormat format, f32_t rangeMin, f32_t rangeMax,
                               u64_t clampedSamples) {
    if (outputCount == kMaxOutputs) {
        return false;
    }
    Output& entry = outputs[outputCount++];
    detail::CopyBounded(entry.name, name);
    detail::CopyBounded(entry.file, file);
    entry.mapping        = mapping;
    entry.format         = format;
    entry.rangeMin       = rangeMin;
    entry.rangeMax       = rangeMax;
    entry.clampedSamples = clampedSamples;
    return true;
}

std::array<char, 17> HashFile(std::string_view path) {
    std::array<char, 17>  text{};
    std::array<char, 512> nullTerminated{};
    detail::CopyBounded(nullTerminated, path);

    std::FILE* file = std::fopen(nullTerminated.data(), "rb");
    if (file == nullptr) {
        return text;
    }
    u64_t                   hash = 0xCBF29CE484222325ull;
    std::array<u8_t, 65536> chunk{};
    usize_t                 read = 0;
    while ((read = std::fread(chunk.data(), 1, chunk.size(), file)) > 0) {
        for (usize_t i = 0; i < read; ++i) {
            hash ^= chunk[i];
            hash *= 0x100000001B3ull;
        }
    }
    std::fclose(file);
    std::snprintf(text.data(), text.size(), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

Status WriteMetadata(std::string_view path, const ExportMetadata& metadata) {
    std::array<char, 512> nullTerminated{};
    detail::CopyBounded(nullTerminated, path);
    std::FILE* file = std::fopen(nullTerminated.data(), "wb");
    if (file == nullptr) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not open {} for writing", path);
    }

    JsonWriter json(file);
    json.BeginObject();

    json.Key("engine_version", Version());
    json.Key("engine_commit", CommitHash());
    json.Key("script", std::string_view{metadata.scriptPath.data()});
    json.Key("script_hash", std::string_view{metadata.scriptHash.data()});
    json.Key("seed", metadata.seed);
    json.Key("resolution", metadata.resolution);
    json.Key("section_size", metadata.sectionSize);
    json.Key("width", metadata.width);
    json.Key("height", metadata.height);

    json.BeginObject("bounds");
    json.Key("min_x", metadata.minX);
    json.Key("min_z", metadata.minZ);
    json.Key("max_x", metadata.maxX);
    json.Key("max_z", metadata.maxZ);
    json.End();

    json.BeginArray("outputs");
    for (usize_t i = 0; i < metadata.outputCount; ++i) {
        const ExportMetadata::Output& output = metadata.outputs[i];
        json.BeginObject();
        json.Key("name", std::string_view{output.name.data()});
        json.Key("file", std::string_view{output.file.data()});
        json.Key("mapping", ToString(output.mapping));
        json.Key("format", ToString(output.format));
        json.Key("range_min", static_cast<f64_t>(output.rangeMin));
        json.Key("range_max", static_cast<f64_t>(output.rangeMax));
        json.Key("clamped_samples", output.clampedSamples);
        json.End();
    }
    json.EndArray();

    json.BeginObject("timings_ms");
    json.Key("total", metadata.timings.totalMs);
    json.Key("graph_compilation", metadata.timings.compileMs);
    json.Key("pipeline_creation", metadata.timings.pipelineMs);
    json.Key("gpu", metadata.timings.gpuMs);
    json.Key("readback", metadata.timings.readbackMs);
    json.Key("encode_and_write", metadata.timings.encodeMs);
    json.End();

    json.BeginObject("peak_memory_bytes");
    json.Key("cpu", metadata.peakCpuBytes);
    json.BeginObject("vram");
    for (usize_t i = 0; i < metadata.peakVram.size(); ++i) {
        json.Key(ToString(static_cast<vulkan::VramCategory>(i)), metadata.peakVram[i]);
    }
    json.End();
    json.End();

    json.BeginObject("environment");
    json.Key("gpu", std::string_view{metadata.gpuName.data()});
    json.Key("driver_version", metadata.driverVersion);
    json.Key("vulkan_api_version", metadata.apiVersion);
    json.Key("cpu", std::string_view{metadata.cpuName.data()});
    json.Key("os", std::string_view{metadata.osName.data()});
    json.Key("build_type", std::string_view{metadata.buildType.data()});
    json.End();

    json.End();
    std::fputc('\n', file);
    const b8_t ok = std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "writing {} failed", path);
    }
    LOG_INFO("metadata sidecar written to {}", path);
    return {};
}

} // namespace engine::terrain
