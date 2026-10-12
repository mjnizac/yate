#include <engine/terrain/output_writer.hpp>
#include <engine/terrain/sha256.hpp>

#include <engine/assert.hpp>
#include <engine/engine.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/platform.hpp>

#include <spng.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <new>
#include <thread>
#include <cstring>
#include <utility>
#include <vector>

namespace engine::terrain {

namespace {

constexpr u16_t kMaxSample = 65535;

// PNG stores 16-bit samples big-endian and libspng does that conversion itself, so a row is handed
// over in host order. Swapping here as well put every 16-bit file the engine ever wrote out
// byte-reversed: the low byte landed in the high one, which turns a smooth height field into a
// sawtooth that reads as noise. Measured rather than assumed — a value of 0 over a declared range of
// +-100000 must appear in the file as `80 00`, and with the swap it appeared as `00 80`.

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

const char* ToString(PngFilter filter) noexcept {
    switch (filter) {
        case PngFilter::None: return "none";
        case PngFilter::Up: return "up";
        case PngFilter::All: return "all";
    }
    return "unknown";
}

// --- Parallel deflate ----------------------------------------------------------------------------
//
// The pixels of a PNG are one zlib stream, and zlib compresses a stream on one thread. On a 4096 square
// export with erosion that thread is 79% of the wall time (GPU 5%, readback 7%) while most of the cores
// sit idle, so this is where the export time goes.
//
// The stream is built the way pigz builds one. The filtered scanlines are cut into chunks of a fixed
// number of rows and each chunk is deflated on its own, as raw deflate, ending in a sync flush so that it
// stops on a byte boundary. Concatenated they are one valid deflate stream; the last chunk is finished
// instead of flushed, and the zlib header and the Adler-32 of the whole are written around it. Adler-32
// combines, so the checksum of the whole is folded together from the checksums of the chunks.
//
// What this must not change, and why it does not:
//
//   * **The bytes are a function of the rows alone.** A chunk is a fixed count of rows counted from the
//     top of the image, not from the top of a band, so a different section size cuts the same chunks and
//     the same file comes out. Neither the thread count nor which thread got which chunk is visible.
//   * **The pixels.** Filtering does not depend on chunking (the row above is the previous *raw* row,
//     whichever chunk it was in), and a decoder sees one ordinary zlib stream.
//   * **Memory.** At most `slots` chunks are held per output, so it is a bounded handful of megabytes.
//
// It costs a little size: each chunk starts with an empty dictionary, which at 32 rows of a few
// kilobytes each is a fraction of a percent.

constexpr u32_t   kChunkRows         = 32;
constexpr usize_t kMaxDeflateThreads = 4;

class ParallelDeflate {
public:
    enum class State : int { Free = 0, Queued, Compressing, Done };

    struct Slot {
        u8_t*   input          = nullptr;
        u8_t*   output         = nullptr;
        usize_t inputBytes     = 0;
        usize_t outputCapacity = 0;
        usize_t outputBytes    = 0;
        u32_t   adler          = 1;
        b8_t    last           = false;
        int     error          = 0;
        f64_t   compressMs     = 0.0;
        State   state          = State::Free;
    };

    /// Where the time of this output went, for the log. `compressMs` is summed over the workers, so it
    /// is CPU time; `blockedMs` is the time the producer waited for a free slot or for the last chunks,
    /// which is the part of the compression that was on the critical path; `writeMs` is time in `fwrite`.
    struct Timing {
        f64_t compressMs = 0.0;
        f64_t blockedMs  = 0.0;
        f64_t writeMs    = 0.0;
        usize_t threads  = 0;
    };
    [[nodiscard]] Timing Timings() const { return {m_compressMs, m_blockedMs, m_writeMs, m_threads}; }

    ParallelDeflate() = default;
    ~ParallelDeflate() { Destroy(); }
    ENGINE_NO_COPY(ParallelDeflate);
    ENGINE_NO_MOVE(ParallelDeflate);

    /// `file` stays owned by the caller. Writes the signature and IHDR.
    [[nodiscard]] Status Start(std::FILE* file, u32_t width, u32_t height, u32_t components,
                               PngCompression compression) {
        m_file     = file;
        m_height   = height;
        m_filter   = compression.filter;
        m_rowBytes = static_cast<usize_t>(width) * components * sizeof(u16_t);
        m_scanline = m_rowBytes + 1;
        m_level    = static_cast<int>(compression.level);
        m_strategy = compression.filter == PngFilter::None ? Z_DEFAULT_STRATEGY : Z_FILTERED;

        // The two bytes in front of the deflate data. FLEVEL is informational, and FCHECK makes the
        // pair a multiple of 31.
        m_zlibHeader[0] = 0x78;
        m_zlibHeader[1] = compression.level < 2 ? 0x01 : (compression.level < 6 ? 0x5E : 0x9C);

        const u32_t cores = std::thread::hardware_concurrency();
        m_threads = std::clamp<usize_t>(cores / 2, 1, kMaxDeflateThreads);
        m_slotCount = m_threads * 2;

        // The bound of what one chunk can deflate to, taken from a stream set up the same way.
        z_stream probe{};
        if (deflateInit2(&probe, m_level, Z_DEFLATED, -15, 8, m_strategy) != Z_OK) {
            ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export, "could not set up zlib");
        }
        const usize_t chunkBytes = static_cast<usize_t>(kChunkRows) * m_scanline;
        const usize_t bound      = deflateBound(&probe, static_cast<uLong>(chunkBytes)) + 128;
        deflateEnd(&probe);

        for (usize_t i = 0; i < m_slotCount; ++i) {
            m_slots[i].input = static_cast<u8_t*>(memory::General().Allocate(chunkBytes, 16));
            m_slots[i].output = static_cast<u8_t*>(memory::General().Allocate(bound, 16));
            m_slots[i].outputCapacity = bound;
            if (m_slots[i].input == nullptr || m_slots[i].output == nullptr) {
                ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export,
                            "could not allocate the deflate buffers");
            }
        }
        m_previous = static_cast<u8_t*>(memory::General().Allocate(m_rowBytes, 16));
        m_current  = static_cast<u8_t*>(memory::General().Allocate(m_rowBytes, 16));
        if (m_previous == nullptr || m_current == nullptr) {
            ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export, "could not allocate a row");
        }
        std::memset(m_previous, 0, m_rowBytes);

        // Signature and IHDR.
        static constexpr u8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (!Write(kSignature, sizeof(kSignature))) {
            ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not write the PNG header");
        }
        u8_t ihdr[13];
        StoreBigEndian32(ihdr + 0, width);
        StoreBigEndian32(ihdr + 4, height);
        ihdr[8]  = 16;
        ihdr[9]  = components == 3 ? 2 : 0; // Truecolour, or grayscale.
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        if (!WriteChunk("IHDR", ihdr, sizeof(ihdr))) {
            ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not write the PNG header");
        }

        m_adler = static_cast<u32_t>(adler32(0L, Z_NULL, 0));
        m_workers.reserve(m_threads);
        for (usize_t i = 0; i < m_threads; ++i) {
            m_workers.emplace_back(&ParallelDeflate::RunWorker, this);
        }
        m_started = true;
        return {};
    }

    /// One row of `rowBytes / 2` samples in host order.
    [[nodiscard]] Status AddRow(const u16_t* samples) {
        if (m_rowsAdded >= m_height) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                        "row {} is past the declared height of {}", m_rowsAdded, m_height);
        }
        Slot&         slot = m_slots[m_chunkNumber % m_slotCount];
        const u32_t   rowInChunk = m_rowsAdded % kChunkRows;
        if (rowInChunk == 0) {
            // The slot is taken back from the writer before it is filled.
            const auto blockedFrom = std::chrono::steady_clock::now();
            while (!SlotIsFree(slot)) {
                if (Status written = WriteOne(true); !written) {
                    return written;
                }
            }
            m_blockedMs += std::chrono::duration<f64_t, std::milli>(
                               std::chrono::steady_clock::now() - blockedFrom)
                               .count();
        }

        // Big-endian bytes, then the filter against the previous *raw* row.
        const usize_t count = m_rowBytes / sizeof(u16_t);
        for (usize_t i = 0; i < count; ++i) {
            m_current[2 * i]     = static_cast<u8_t>(samples[i] >> 8);
            m_current[2 * i + 1] = static_cast<u8_t>(samples[i] & 0xFF);
        }
        u8_t* destination = slot.input + static_cast<usize_t>(rowInChunk) * m_scanline;
        if (m_filter == PngFilter::Up) {
            destination[0] = 2;
            for (usize_t i = 0; i < m_rowBytes; ++i) {
                destination[1 + i] = static_cast<u8_t>(m_current[i] - m_previous[i]);
            }
        } else {
            destination[0] = 0;
            std::memcpy(destination + 1, m_current, m_rowBytes);
        }
        std::swap(m_current, m_previous);
        ++m_rowsAdded;

        const b8_t full = rowInChunk + 1 == kChunkRows;
        const b8_t last = m_rowsAdded == m_height;
        if (full || last) {
            slot.inputBytes = static_cast<usize_t>(rowInChunk + 1) * m_scanline;
            slot.last       = last;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                slot.state = State::Queued;
                ++m_submitted;
            }
            m_workAvailable.notify_one();
            ++m_chunkNumber;
            // Whatever has finished is written now, so the files grows as the image does.
            return WriteReady();
        }
        return {};
    }

    /// Writes the remaining chunks and the end of the file. All rows must have been added.
    [[nodiscard]] Status Finish() {
        if (m_rowsAdded != m_height) {
            ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Export,
                        "{} of {} rows were written before finishing", m_rowsAdded, m_height);
        }
        const auto blockedFrom = std::chrono::steady_clock::now();
        while (m_written < m_submitted) {
            if (Status written = WriteOne(true); !written) {
                return written;
            }
        }
        m_blockedMs += std::chrono::duration<f64_t, std::milli>(
                           std::chrono::steady_clock::now() - blockedFrom)
                           .count();
        Stop();
        if (!WriteChunk("IEND", nullptr, 0)) {
            ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not write the PNG end");
        }
        return {};
    }

private:
    static void StoreBigEndian32(u8_t* out, u32_t value) {
        out[0] = static_cast<u8_t>(value >> 24);
        out[1] = static_cast<u8_t>(value >> 16);
        out[2] = static_cast<u8_t>(value >> 8);
        out[3] = static_cast<u8_t>(value);
    }

    /// `fwrite` in pieces. One large `fwrite` is pathologically slow with this CRT: see
    /// `RawVolumeWriter::AppendRows`.
    [[nodiscard]] b8_t Write(const void* data, usize_t size) {
        const auto* bytes = static_cast<const u8_t*>(data);
        constexpr usize_t kPiece = 1u << 20;
        while (size > 0) {
            const usize_t take = size < kPiece ? size : kPiece;
            if (std::fwrite(bytes, 1, take, m_file) != take) {
                return false;
            }
            bytes += take;
            size -= take;
        }
        return true;
    }

    [[nodiscard]] b8_t WriteChunk(const char* type, const u8_t* data, usize_t size) {
        u8_t length[4];
        StoreBigEndian32(length, static_cast<u32_t>(size));
        u32_t crc = static_cast<u32_t>(crc32(0L, Z_NULL, 0));
        crc       = static_cast<u32_t>(crc32(crc, reinterpret_cast<const Bytef*>(type), 4));
        if (size != 0) {
            crc = static_cast<u32_t>(crc32(crc, data, static_cast<uInt>(size)));
        }
        u8_t tail[4];
        StoreBigEndian32(tail, crc);
        return Write(length, 4) && Write(type, 4) && (size == 0 || Write(data, size))
               && Write(tail, 4);
    }

    [[nodiscard]] b8_t SlotIsFree(const Slot& slot) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return slot.state == State::Free;
    }

    /// Writes the head chunk if it has finished, waiting for it when `block`. Returns once one chunk
    /// has been written, or at once when none can be.
    [[nodiscard]] Status WriteOne(b8_t block) {
        if (m_written >= m_submitted) {
            return {};
        }
        Slot& slot = m_slots[m_written % m_slotCount];
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (slot.state != State::Done) {
                if (!block) {
                    return {};
                }
                m_changed.wait(lock, [&slot] { return slot.state == State::Done; });
            }
        }
        if (slot.error != 0) {
            ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "zlib failed with code {}", slot.error);
        }

        m_compressMs += slot.compressMs;
        const auto writeFrom = std::chrono::steady_clock::now();

        // The checksum of everything so far, folded from the chunks'.
        m_adler = m_written == 0 ? slot.adler
                                 : static_cast<u32_t>(adler32_combine(
                                       m_adler, slot.adler, static_cast<z_off_t>(slot.inputBytes)));

        // One IDAT per chunk: the zlib header goes in front of the first and the checksum behind the
        // last, and the CRC runs over the pieces.
        const b8_t    first  = m_written == 0;
        const usize_t size   = (first ? 2 : 0) + slot.outputBytes + (slot.last ? 4 : 0);
        u8_t          length[4];
        StoreBigEndian32(length, static_cast<u32_t>(size));
        u32_t crc = static_cast<u32_t>(crc32(0L, Z_NULL, 0));
        crc       = static_cast<u32_t>(crc32(crc, reinterpret_cast<const Bytef*>("IDAT"), 4));
        b8_t ok   = Write(length, 4) && Write("IDAT", 4);
        if (first) {
            crc = static_cast<u32_t>(crc32(crc, m_zlibHeader, 2));
            ok  = ok && Write(m_zlibHeader, 2);
        }
        crc = static_cast<u32_t>(crc32(crc, slot.output, static_cast<uInt>(slot.outputBytes)));
        ok  = ok && Write(slot.output, slot.outputBytes);
        if (slot.last) {
            u8_t adler[4];
            StoreBigEndian32(adler, m_adler);
            crc = static_cast<u32_t>(crc32(crc, adler, 4));
            ok  = ok && Write(adler, 4);
        }
        u8_t tail[4];
        StoreBigEndian32(tail, crc);
        ok = ok && Write(tail, 4);
        m_writeMs += std::chrono::duration<f64_t, std::milli>(
                         std::chrono::steady_clock::now() - writeFrom)
                         .count();
        if (!ok) {
            ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not write the image data");
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            slot.state = State::Free;
        }
        ++m_written;
        return {};
    }

    [[nodiscard]] Status WriteReady() {
        for (;;) {
            const u64_t before = m_written;
            if (Status written = WriteOne(false); !written) {
                return written;
            }
            if (m_written == before) {
                return {};
            }
        }
    }

    void RunWorker() {
        platform::SetThreadName("engine-deflate");
        z_stream stream{};
        const b8_t ready = deflateInit2(&stream, m_level, Z_DEFLATED, -15, 8, m_strategy) == Z_OK;
        for (;;) {
            u64_t chunk = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_workAvailable.wait(
                    lock, [this] { return m_stopping || m_nextToCompress < m_submitted; });
                if (m_nextToCompress >= m_submitted) {
                    break; // Stopping, and nothing is left.
                }
                chunk = m_nextToCompress++;
                m_slots[chunk % m_slotCount].state = State::Compressing;
            }
            Slot& slot = m_slots[chunk % m_slotCount];

            const auto compressFrom = std::chrono::steady_clock::now();
            if (!ready) {
                slot.error       = Z_MEM_ERROR;
                slot.outputBytes = 0;
                slot.adler       = 1;
                slot.compressMs  = 0.0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    slot.state = State::Done;
                }
                m_changed.notify_all();
                continue;
            }
            deflateReset(&stream);
            stream.next_in   = slot.input;
            stream.avail_in  = static_cast<uInt>(slot.inputBytes);
            stream.next_out  = slot.output;
            stream.avail_out = static_cast<uInt>(slot.outputCapacity);
            const int code   = deflate(&stream, slot.last ? Z_FINISH : Z_SYNC_FLUSH);
            const b8_t good  = slot.last ? code == Z_STREAM_END : code == Z_OK;
            slot.error       = good && stream.avail_in == 0 ? 0 : (code != 0 ? code : Z_BUF_ERROR);
            slot.outputBytes = slot.outputCapacity - stream.avail_out;
            slot.adler       = static_cast<u32_t>(
                adler32(adler32(0L, Z_NULL, 0), slot.input, static_cast<uInt>(slot.inputBytes)));
            slot.compressMs = std::chrono::duration<f64_t, std::milli>(
                                  std::chrono::steady_clock::now() - compressFrom)
                                  .count();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                slot.state = State::Done;
            }
            m_changed.notify_all();
        }
        if (ready) {
            deflateEnd(&stream);
        }
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_workAvailable.notify_all();
        for (std::thread& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        m_workers.clear();
    }

    void Destroy() noexcept {
        Stop();
        for (usize_t i = 0; i < m_slotCount; ++i) {
            if (m_slots[i].input != nullptr) {
                memory::General().Free(m_slots[i].input);
                m_slots[i].input = nullptr;
            }
            if (m_slots[i].output != nullptr) {
                memory::General().Free(m_slots[i].output);
                m_slots[i].output = nullptr;
            }
        }
        if (m_previous != nullptr) {
            memory::General().Free(m_previous);
            m_previous = nullptr;
        }
        if (m_current != nullptr) {
            memory::General().Free(m_current);
            m_current = nullptr;
        }
    }

    std::FILE* m_file = nullptr;
    u32_t      m_height = 0;
    usize_t    m_rowBytes = 0;
    usize_t    m_scanline = 0;
    int        m_level = 2;
    int        m_strategy = Z_DEFAULT_STRATEGY;
    PngFilter  m_filter = PngFilter::Up;
    u8_t       m_zlibHeader[2] = {0x78, 0x01};
    b8_t       m_started = false;

    usize_t m_threads   = 1;
    usize_t m_slotCount = 2;
    std::array<Slot, kMaxDeflateThreads * 2> m_slots{};
    std::vector<std::thread> m_workers;

    u8_t* m_previous = nullptr;
    u8_t* m_current  = nullptr;
    u32_t m_rowsAdded   = 0;
    u64_t m_chunkNumber = 0;
    u64_t m_written     = 0;
    u32_t m_adler       = 1;
    f64_t m_compressMs  = 0.0;
    f64_t m_blockedMs   = 0.0;
    f64_t m_writeMs     = 0.0;

    std::mutex              m_mutex;
    std::condition_variable m_workAvailable;
    std::condition_variable m_changed;
    u64_t                   m_submitted      = 0;
    u64_t                   m_nextToCompress = 0;
    b8_t                    m_stopping       = false;
};

Result<PngFilter> ParsePngFilter(std::string_view name) {
    if (name == "none") {
        return PngFilter::None;
    }
    if (name == "up") {
        return PngFilter::Up;
    }
    if (name == "all") {
        return PngFilter::All;
    }
    ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                "unknown PNG filter '{}'; expected none, up or all", name);
}

Result<PngWriter> PngWriter::Create(std::string_view path, u32_t width, u32_t height,
                                   Mapping mapping, f32_t rangeMin, f32_t rangeMax,
                                   PngCompression compression) {
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
    if (!detail::CopyChecked(nullTerminated, path)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "the path {} is {} characters; the limit is 511", path, path.size());
    }

    std::FILE* file = std::fopen(nullTerminated.data(), "wb");
    if (file == nullptr) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not open {} for writing", path);
    }

    const u32_t components = ComponentsOf(format);

    // `None` and `Up` are built by `ParallelDeflate`. `All` tries every filter on every row and keeps the
    // smallest, which is libspng's own heuristic and not worth reimplementing for a setting that costs
    // twice the time for 1% of the size, so it stays on libspng, one thread.
    ParallelDeflate* deflate = nullptr;
    if (compression.filter != PngFilter::All) {
        void* storage = memory::General().Allocate(sizeof(ParallelDeflate), alignof(ParallelDeflate));
        if (storage == nullptr) {
            std::fclose(file);
            ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export,
                        "could not allocate the PNG encoder");
        }
        deflate = new (storage) ParallelDeflate{};
        if (Status started =
                deflate->Start(file, width, height, components, compression);
            !started) {
            deflate->~ParallelDeflate();
            memory::General().Free(storage);
            std::fclose(file);
            return std::unexpected(started.error());
        }

        PngWriter writer;
        writer.m_file             = file;
        writer.m_deflate          = deflate;
        writer.m_width            = width;
        writer.m_height           = height;
        writer.m_components       = components;
        writer.m_sourceComponents = mapping.components;
        writer.m_format           = format;
        writer.m_rangeMin         = rangeMin;
        writer.m_rangeMax         = rangeMax;
        writer.m_row              = static_cast<u16_t*>(memory::General().Allocate(
            static_cast<usize_t>(width) * components * sizeof(u16_t), alignof(u16_t)));
        if (writer.m_row == nullptr) {
            ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export,
                        "could not allocate a {}-sample scratch row", width * components);
        }
        LOG_DEBUG("writing {} ({}x{}, {}, parallel deflate)", path, width, height,
                  ToString(format));
        return writer;
    }

    spng_ctx* context = spng_ctx_new(SPNG_CTX_ENCODER);
    if (context == nullptr) {
        std::fclose(file);
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export, "could not create a libspng context");
    }

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

    const int filterChoice = compression.filter == PngFilter::All ? SPNG_FILTER_CHOICE_ALL
                             : compression.filter == PngFilter::Up ? SPNG_FILTER_CHOICE_UP
                                                                   : SPNG_FILTER_CHOICE_NONE;
    if (const int code = spng_set_option(context, SPNG_FILTER_CHOICE, filterChoice); code != 0) {
        return fail(code, "spng_set_option(SPNG_FILTER_CHOICE)");
    }
    if (const int code = spng_set_option(context, SPNG_IMG_COMPRESSION_LEVEL,
                                         static_cast<int>(compression.level));
        code != 0) {
        return fail(code, "spng_set_option(SPNG_IMG_COMPRESSION_LEVEL)");
    }
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
    if (m_deflate != nullptr) {
        auto* deflate = static_cast<ParallelDeflate*>(m_deflate);
        deflate->~ParallelDeflate();
        memory::General().Free(deflate);
        m_deflate = nullptr;
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
    m_deflate     = other.m_deflate;
    m_row         = other.m_row;
    m_width       = other.m_width;
    m_height      = other.m_height;
    m_components  = other.m_components;
    // Dropping this one wrote every vector output as its first component in red with green and blue
    // forced to zero, because the default is 1 and every writer is moved into its band encoder before
    // it writes a row. Normals, gradients and erosion flow were all affected for the project's whole
    // life, and no test could see it: the dataset goldens are files this same path produced.
    m_sourceComponents = other.m_sourceComponents;
    m_rowsWritten = other.m_rowsWritten;
    m_format      = other.m_format;
    m_rangeMin    = other.m_rangeMin;
    m_rangeMax    = other.m_rangeMax;
    m_clamped     = other.m_clamped;
    other.m_context = nullptr;
    other.m_file    = nullptr;
    other.m_deflate = nullptr;
    other.m_row     = nullptr;
    return *this;
}

Status PngWriter::WriteRow(const f32_t* samples) {
    if (m_context == nullptr && m_deflate == nullptr) {
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
                m_row[x * 3 + c] = quantized;
            }
        }
    } else {
        for (u32_t x = 0; x < m_width; ++x) {
            m_row[x] = Quantize(samples[x], m_rangeMin, m_rangeMax, m_clamped);
        }
    }

    if (m_deflate != nullptr) {
        ++m_rowsWritten;
        return static_cast<ParallelDeflate*>(m_deflate)->AddRow(m_row);
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
    if (m_context == nullptr && m_deflate == nullptr) {
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
    if (m_deflate != nullptr) {
        // Every row is in; the chunks still in flight are written and the stream is closed.
        auto*        deflate  = static_cast<ParallelDeflate*>(m_deflate);
        const Status finished = deflate->Finish();
        const ParallelDeflate::Timing time = deflate->Timings();
        LOG_INFO("png {}x{} ({}): deflate {:.0f} ms of cpu on {} thread(s), producer blocked {:.0f} ms, "
                 "{:.0f} ms in fwrite",
                 m_width, m_height, ToString(m_format), time.compressMs, time.threads, time.blockedMs,
                 time.writeMs);
        Release();
        return finished;
    }
    Release();
    return {};
}

// --- Raw volumes --------------------------------------------------------------------------------

Result<RawVolumeWriter> RawVolumeWriter::Create(std::string_view path, u32_t width, u32_t height,
                                              u32_t depth, u32_t components) {
    if (width == 0 || height == 0 || depth == 0 || components == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "volume is {}x{}x{} with {} component(s)", width, height, depth, components);
    }

    std::array<char, 512> nullTerminated{};
    if (!detail::CopyChecked(nullTerminated, path)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "the path {} is {} characters; the limit is 511", path, path.size());
    }
    std::FILE* file = std::fopen(nullTerminated.data(), "wb");
    if (file == nullptr) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not open {} for writing", path);
    }

    // A large stream buffer. The default is 4 KiB, and the CRT then turns a multi-megabyte band into
    // a stream of 4 KiB writes: measured, an 8192 square map took 38 s to write 930 MB that a plain
    // sequential write puts down in under a second.
    (void)std::setvbuf(file, nullptr, _IOFBF, 1u << 20);

    RawVolumeWriter writer;
    writer.m_file       = file;
    writer.m_width      = width;
    writer.m_height     = height;
    writer.m_depth      = depth;
    writer.m_components = components;

    LOG_DEBUG("writing {} ({}x{}x{}, {} component(s), raw f32)", path, width, height, depth,
              components);
    return writer;
}

RawVolumeWriter::~RawVolumeWriter() { Release(); }

void RawVolumeWriter::Release() noexcept {
    if (m_file != nullptr) {
        std::fclose(static_cast<std::FILE*>(m_file));
        m_file = nullptr;
    }
}

RawVolumeWriter::RawVolumeWriter(RawVolumeWriter&& other) noexcept { *this = std::move(other); }

RawVolumeWriter& RawVolumeWriter::operator=(RawVolumeWriter&& other) noexcept {
    if (this != &other) {
        Release();
        m_file           = other.m_file;
        m_width          = other.m_width;
        m_height         = other.m_height;
        m_depth          = other.m_depth;
        m_components     = other.m_components;
        m_samplesWritten = other.m_samplesWritten;
        other.m_file     = nullptr;
    }
    return *this;
}

Status RawVolumeWriter::AppendRows(const f32_t* rows, u32_t rowCount) {
    if (m_file == nullptr) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Export, "writing to a closed volume");
    }
    const usize_t samples = static_cast<usize_t>(rowCount) * m_width * m_components;
    // Written in pieces. One multi-megabyte `fwrite` is pathologically slow with this CRT where a
    // stream of 4 MiB ones is not: measured per call on an 8192 square map, 8 MiB appends ran at
    // 2 GiB/s and 24 MiB ones at 25 to 80 MiB/s, and split into 4 MiB pieces the same 24 MiB went at
    // 1.7 GiB/s. It cost 38 s of a 40 s export.
    constexpr usize_t kPiece = (4u << 20) / sizeof(f32_t);
    usize_t written = 0;
    while (written < samples) {
        const usize_t take = samples - written < kPiece ? samples - written : kPiece;
        const usize_t done = std::fwrite(rows + written, sizeof(f32_t), take,
                                         static_cast<std::FILE*>(m_file));
        written += done;
        if (done != take) {
            break;
        }
    }
    if (written != samples) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "wrote {} of {} samples of a band",
                    written, samples);
    }
    m_samplesWritten += static_cast<u64_t>(rowCount) * m_width;
    return {};
}

Status RawVolumeWriter::WriteBrick(std::array<u32_t, 3> origin, std::array<u32_t, 3> extent,
                                  std::array<u32_t, 3> sourceStride, const f32_t* brick) {
    if (m_file == nullptr) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Export, "writing to a closed volume");
    }
    auto* file = static_cast<std::FILE*>(m_file);

    // One fwrite per run of contiguous samples along x, which is the longest run the layout allows.
    for (u32_t y = 0; y < extent[1]; ++y) {
        const u64_t volumeY = origin[1] + y;
        if (volumeY >= m_height) {
            break;
        }
        for (u32_t z = 0; z < extent[2]; ++z) {
            const u64_t volumeZ = origin[2] + z;
            if (volumeZ >= m_depth) {
                break;
            }
            const u32_t columns =
                origin[0] + extent[0] <= m_width ? extent[0] : m_width - origin[0];
            if (columns == 0) {
                continue;
            }

            const u64_t destSample =
                ((volumeY * m_depth) + volumeZ) * m_width + origin[0];
            const u64_t destOffset = destSample * m_components * sizeof(f32_t);
            // 64-bit seek: `long` is 32 bits on Windows, so the cast that was here wrapped at 2 GiB
            // and a volume past that wrote its later rows over its earlier ones.
#ifdef _WIN32
            const int seeked = _fseeki64(file, static_cast<__int64>(destOffset), SEEK_SET);
#else
            const int seeked = fseeko(file, static_cast<off_t>(destOffset), SEEK_SET);
#endif
            if (seeked != 0) {
                ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export,
                            "could not seek to offset {} in the volume", destOffset);
            }

            const u64_t sourceSample =
                (static_cast<u64_t>(y) * sourceStride[2] + z) * sourceStride[0];
            const usize_t written =
                std::fwrite(brick + sourceSample * m_components, sizeof(f32_t),
                            static_cast<usize_t>(columns) * m_components, file);
            if (written != static_cast<usize_t>(columns) * m_components) {
                ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export,
                            "wrote {} of {} samples of a volume row", written,
                            columns * m_components);
            }
            m_samplesWritten += columns;
        }
    }
    return {};
}

Status RawVolumeWriter::Finish() {
    if (m_file == nullptr) {
        return {};
    }
    const u64_t expected = static_cast<u64_t>(m_width) * m_height * m_depth;
    if (m_samplesWritten != expected) {
        ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Export,
                    "{} of {} volume samples were written", m_samplesWritten, expected);
    }
    const b8_t ok = std::ferror(static_cast<std::FILE*>(m_file)) == 0;
    Release();
    if (!ok) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "writing the volume failed");
    }
    return {};
}

// --- BandBuffer ---------------------------------------------------------------------------------

BandBuffer::~BandBuffer() {
    if (m_data != nullptr) {
        memory::General().Free(m_data);
    }
}

Status BandBuffer::Create(u32_t width, u32_t rows, u32_t components) {
    m_width      = width;
    m_components = components;
    m_samples    = static_cast<u64_t>(width) * rows * components;
    m_data       = static_cast<f32_t*>(
        memory::General().Allocate(static_cast<usize_t>(m_samples) * sizeof(f32_t), alignof(f32_t)));
    if (m_data == nullptr) {
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export,
                    "could not allocate a {}x{} band of {} component(s)", width, rows, components);
    }
    return {};
}

f32_t* BandBuffer::Row(u32_t row) noexcept {
    return m_data + static_cast<u64_t>(row) * m_width * m_components;
}

void BandBuffer::Absorb(const f32_t* section, u32_t sectionStride, u32_t column, u32_t rows,
                        u32_t columns) {
    for (u32_t row = 0; row < rows; ++row) {
        std::memcpy(Row(row) + static_cast<u64_t>(column) * m_components,
                    section + static_cast<u64_t>(row) * sectionStride * m_components,
                    static_cast<usize_t>(columns) * m_components * sizeof(f32_t));
    }
}

// --- BandEncoder --------------------------------------------------------------------------------

/// Everything the worker and the producer share.
///
/// Held behind a pointer so the header needs no <thread> or <condition_variable>, which would reach
/// every translation unit that writes an output.
struct BandEncoder::State {
    PngWriter   writer;
    BandBuffer  bands[2];
    std::thread worker;

    std::mutex              mutex;
    std::condition_variable readyToEncode;
    std::condition_variable readyToFill;

    /// Index of the band the worker should encode, or -1 when there is nothing queued.
    int   queued      = -1;
    u32_t queuedRows  = 0;
    int   filling     = 0;
    b8_t  finishing   = false;
    /// The first error the worker hit. Reported to the producer on its next call, because a failed
    /// write must stop the export rather than let it finish and look successful.
    Error failure;
    b8_t  failed = false;
    f64_t encodeMilliseconds = 0.0;
};

/// Drains the queue, writing each band's rows in order.
void BandEncoder::RunWorker(State& state) {
    platform::SetThreadName("engine-encode");
    for (;;) {
        int   band = -1;
        u32_t rows = 0;
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.readyToEncode.wait(lock,
                                     [&state] { return state.queued >= 0 || state.finishing; });
            if (state.queued < 0) {
                return; // Finishing with nothing left.
            }
            band = state.queued;
            rows = state.queuedRows;
        }

        const auto             start = std::chrono::steady_clock::now();
        Status                 written;
        for (u32_t row = 0; row < rows && written; ++row) {
            written = state.writer.WriteRow(state.bands[band].Row(row));
        }
        const f64_t milliseconds =
            std::chrono::duration<f64_t, std::milli>(std::chrono::steady_clock::now() - start)
                .count();

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.encodeMilliseconds += milliseconds;
            if (!written && !state.failed) {
                state.failed  = true;
                state.failure = written.error();
            }
            state.queued = -1;
        }
        state.readyToFill.notify_one();
    }
}

BandEncoder::~BandEncoder() { Release(); }

void BandEncoder::Release() noexcept {
    if (m_state == nullptr) {
        return;
    }
    if (m_state->worker.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->finishing = true;
        }
        m_state->readyToEncode.notify_one();
        m_state->worker.join();
    }
    m_state->~State();
    memory::General().Free(m_state);
    m_state = nullptr;
}

Status BandEncoder::Start(PngWriter&& writer, u32_t width, u32_t rows, u32_t components) {
    Release();
    void* storage = memory::General().Allocate(sizeof(State), alignof(State));
    if (storage == nullptr) {
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export,
                    "could not allocate the band encoder state");
    }
    m_state = new (storage) State{};
    m_state->writer = std::move(writer);
    for (BandBuffer& band : m_state->bands) {
        if (Status created = band.Create(width, rows, components); !created) {
            return created;
        }
    }
    m_state->worker = std::thread(&BandEncoder::RunWorker, std::ref(*m_state));
    return {};
}

BandBuffer& BandEncoder::Filling() noexcept {
    ENGINE_ASSERT(m_state != nullptr, "the band encoder was never started");
    return m_state->bands[m_state->filling];
}

Status BandEncoder::Submit(u32_t rows) {
    ENGINE_ASSERT_RETURN(Status{}, m_state != nullptr, "the band encoder was never started");
    {
        std::unique_lock<std::mutex> lock(m_state->mutex);
        // Waiting here is what bounds the pipeline to two bands per output: the producer cannot run
        // further ahead than one band, however fast the GPU is.
        m_state->readyToFill.wait(lock, [this] { return m_state->queued < 0; });
        if (m_state->failed) {
            return std::unexpected(m_state->failure);
        }
        m_state->queued     = m_state->filling;
        m_state->queuedRows = rows;
        m_state->filling    = 1 - m_state->filling;
    }
    m_state->readyToEncode.notify_one();
    return {};
}

Status BandEncoder::Finish() {
    ENGINE_ASSERT_RETURN(Status{}, m_state != nullptr, "the band encoder was never started");
    {
        std::unique_lock<std::mutex> lock(m_state->mutex);
        m_state->readyToFill.wait(lock, [this] { return m_state->queued < 0; });
        m_state->finishing = true;
    }
    m_state->readyToEncode.notify_one();
    m_state->worker.join();

    if (m_state->failed) {
        return std::unexpected(m_state->failure);
    }
    return m_state->writer.Finish();
}

u64_t BandEncoder::ClampedSamples() const noexcept {
    return m_state != nullptr ? m_state->writer.ClampedSamples() : 0;
}

f64_t BandEncoder::EncodeMilliseconds() const noexcept {
    return m_state != nullptr ? m_state->encodeMilliseconds : 0.0;
}

// --- Metadata -----------------------------------------------------------------------------------

b8_t ExportMetadata::AddOutput(std::string_view name, std::string_view file, Mapping mapping,
                               OutputFormat format, f32_t rangeMin, f32_t rangeMax,
                               u64_t clampedSamples, const OutputStatistics& statistics) {
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
    entry.statistics     = statistics;
    return true;
}

Result<std::array<char, 65>> HashFile(std::string_view path) {
    std::array<char, 512> nullTerminated{};
    if (!detail::CopyChecked(nullTerminated, path)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "the path of a file to hash is {} characters; the limit is 511", path.size());
    }

    std::FILE* file = std::fopen(nullTerminated.data(), "rb");
    if (file == nullptr) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "could not open {} to hash it", path);
    }
    Sha256                  hash;
    std::array<u8_t, 65536> chunk{};
    usize_t                 read = 0;
    while ((read = std::fread(chunk.data(), 1, chunk.size(), file)) > 0) {
        hash.Update(chunk.data(), read);
    }
    const b8_t failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        ENGINE_FAIL(ErrorCode::IoError, ErrorStage::Export, "reading {} to hash it failed", path);
    }
    return hash.Final();
}

Status WriteMetadata(std::string_view path, const ExportMetadata& metadata) {
    std::array<char, 512> nullTerminated{};
    if (!detail::CopyChecked(nullTerminated, path)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "the path {} is {} characters; the limit is 511", path, path.size());
    }
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
    json.Key("shader_hash", std::string_view{metadata.shaderHash.data()});
    json.Key("params_hash", std::string_view{metadata.paramsHash.data()});
    json.Key("prelude_hash", std::string_view{metadata.preludeHash.data()});
    json.Key("seed", metadata.seed);
    json.Key("resolution", metadata.resolution);
    json.Key("section_size", metadata.sectionSize);
    json.Key("width", metadata.width);
    json.Key("height", metadata.height);
    json.Key("depth", metadata.depth);

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
        json.BeginObject("statistics");
        json.Key("min", output.statistics.minimum);
        json.Key("max", output.statistics.maximum);
        json.Key("mean", output.statistics.mean);
        json.Key("samples", output.statistics.samples);
        json.Key("non_finite", output.statistics.nonFinite);
        json.End();
        json.End();
    }
    json.EndArray();

    json.BeginObject("timings_ms");
    json.Key("total", metadata.timings.totalMs);
    json.Key("script", metadata.timings.scriptMs);
    json.Key("graph_compilation", metadata.timings.compileMs);
    json.Key("pipeline_creation", metadata.timings.pipelineMs);
    json.Key("gpu", metadata.timings.gpuMs);
    json.Key("readback", metadata.timings.readbackMs);
    json.Key("encode_and_write", metadata.timings.encodeMs);
    json.Key("encode_stall", metadata.timings.encodeStallMs);
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
