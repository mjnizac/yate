#include <engine/terrain/export.hpp>

#include <engine/assert.hpp>
#include <engine/engine.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/platform.hpp>
#include <engine/terrain/compiler.hpp>
#include <engine/terrain/evaluator.hpp>
#include <engine/terrain/graph.hpp>
#include <engine/terrain/kernels.hpp>
#include <engine/terrain/mapping.hpp>
#include <engine/terrain/output_writer.hpp>
#include <engine/vulkan/buffer_pool.hpp>
#include <engine/vulkan/context.hpp>
#include <engine/vulkan/pipeline.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory_resource>
#include <utility>
#include <vector>

namespace engine::terrain {

// --- Params -------------------------------------------------------------------------------------

b8_t Params::Set(std::string_view key, std::string_view value) {
    for (usize_t i = 0; i < m_count; ++i) {
        if (std::string_view{m_entries[i].key.data()} == key) {
            detail::CopyBounded(m_entries[i].value, value);
            return true;
        }
    }
    if (m_count == kMaxEntries) {
        return false;
    }
    detail::CopyBounded(m_entries[m_count].key, key);
    detail::CopyBounded(m_entries[m_count].value, value);
    ++m_count;
    return true;
}

b8_t Params::Assign(std::string_view assignment) {
    const usize_t equals = assignment.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return false;
    }
    return Set(assignment.substr(0, equals), assignment.substr(equals + 1));
}

std::string_view Params::Text(std::string_view key) const {
    for (usize_t i = 0; i < m_count; ++i) {
        if (std::string_view{m_entries[i].key.data()} == key) {
            return std::string_view{m_entries[i].value.data()};
        }
    }
    return {};
}

f64_t Params::Number(std::string_view key, f64_t fallback) const {
    const std::string_view text = Text(key);
    if (text.empty()) {
        return fallback;
    }
    char*       end   = nullptr;
    const f64_t value = std::strtod(text.data(), &end);
    return end == text.data() ? fallback : value;
}

b8_t Params::Pair(std::string_view key, f64_t& first, f64_t& second) const {
    const std::string_view text  = Text(key);
    const usize_t          comma = text.find(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    std::array<char, kMaxValue> buffer{};
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

std::string_view Params::KeyAt(usize_t index) const {
    return index < m_count ? std::string_view{m_entries[index].key.data()} : std::string_view{};
}

std::string_view Params::ValueAt(usize_t index) const {
    return index < m_count ? std::string_view{m_entries[index].value.data()} : std::string_view{};
}

// --- Export -------------------------------------------------------------------------------------

namespace {

using Clock        = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<f64_t, std::milli>;

constexpr u64_t kGpuTimeoutNanoseconds = 30ull * 1000 * 1000 * 1000;

f64_t MillisecondsSince(Clock::time_point start) {
    return Milliseconds(Clock::now() - start).count();
}

/// Sampling grid the job describes, in integer samples.
///
/// The origin is an integer sample index, never a float: every kernel derives its world position
/// from it, which is what keeps neighbouring sections bit-identical at their borders.
struct Grid {
    i64_t originX     = 0;
    i64_t originZ     = 0;
    u32_t width       = 0;
    u32_t height      = 0;
    u32_t sectionSize = 0;
    u32_t sectionsX   = 0;
    u32_t sectionsZ   = 0;
};

[[nodiscard]] Result<Grid> MakeGrid(const ExportJob& job) {
    if (job.resolution <= 0.0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "resolution must be positive, got {}", job.resolution);
    }
    const i64_t minX = static_cast<i64_t>(std::llround(job.minX / job.resolution));
    const i64_t minZ = static_cast<i64_t>(std::llround(job.minZ / job.resolution));
    const i64_t maxX = static_cast<i64_t>(std::llround(job.maxX / job.resolution));
    const i64_t maxZ = static_cast<i64_t>(std::llround(job.maxZ / job.resolution));
    if (maxX <= minX || maxZ <= minZ) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "bounds span {}x{} samples at resolution {}", maxX - minX, maxZ - minZ,
                    job.resolution);
    }
    if (job.sectionSize == 0 || !IsPowerOfTwo(job.sectionSize)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "section size must be a power of two, got {}", job.sectionSize);
    }

    Grid grid;
    grid.originX     = minX;
    grid.originZ     = minZ;
    grid.width       = static_cast<u32_t>(maxX - minX);
    grid.height      = static_cast<u32_t>(maxZ - minZ);
    grid.sectionSize = job.sectionSize;
    grid.sectionsX   = (grid.width + job.sectionSize - 1) / job.sectionSize;
    grid.sectionsZ   = (grid.height + job.sectionSize - 1) / job.sectionSize;
    return grid;
}

/// Builds the graph the job describes.
///
/// This is the graph-built-from-C++ of milestone 5. The Lua runtime replaces this one function in
/// milestone 6, and nothing downstream of it changes when it does.
[[nodiscard]] Status BuildJobGraph(Graph& graph, const Params& params) {
    Graph::NoiseParams noise;
    noise.frequency   = static_cast<f32_t>(params.Number("frequency", noise.frequency));
    noise.octaves     = static_cast<u32_t>(params.Number("octaves", noise.octaves));
    noise.lacunarity  = static_cast<f32_t>(params.Number("lacunarity", noise.lacunarity));
    noise.persistence = static_cast<f32_t>(params.Number("persistence", noise.persistence));
    noise.amplitude   = static_cast<f32_t>(params.Number("amplitude", noise.amplitude));
    noise.offset      = static_cast<f32_t>(params.Number("offset", noise.offset));
    noise.normalize   = params.Number("normalize", 1.0) != 0.0;

    const std::string_view kind = params.Text("kind");
    if (kind == "ridged") {
        noise.kind = NoiseKind::Ridged;
    } else if (kind == "billow") {
        noise.kind = NoiseKind::Billow;
    } else if (!kind.empty() && kind != "simplex") {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Script,
                    "unknown noise kind {}; expected simplex, ridged or billow", kind);
    }

    // No script yet, so the location names the parameter block that drove the node.
    const SourceLocation location{.file = "<params>", .line = 0};

    Result<Value> value = graph.AddNoise(noise, location);
    if (!value) {
        return std::unexpected(value.error());
    }

    // The range defaults to the band the kernel actually produces, so a forgotten range parameter
    // does not silently clamp the whole map.
    f64_t rangeMin = noise.offset - noise.amplitude;
    f64_t rangeMax = noise.offset + noise.amplitude;
    (void)params.Pair("range", rangeMin, rangeMax);
    if (Status requested = graph.RequestOutput("height", *value, static_cast<f32_t>(rangeMin),
                                               static_cast<f32_t>(rangeMax));
        !requested) {
        return requested;
    }

    if (params.Number("normals", 0.0) == 0.0) {
        return {};
    }

    // Channel 1 is the analytic gradient the same dispatch already produced.
    Result<Value> gradient = graph.Channel(*value, 1);
    if (!gradient) {
        return std::unexpected(gradient.error());
    }
    Result<Value> normals = graph.AddNormals(
        *gradient, static_cast<f32_t>(params.Number("vertical_scale", 1.0)), location);
    if (!normals) {
        return std::unexpected(normals.error());
    }
    return graph.RequestOutput("normals", *normals, -1.0f, 1.0f);
}

/// A band of full-width rows held in CPU RAM while its sections are evaluated, then streamed into
/// the PNG. Bounds CPU memory to one section row rather than the whole image.
class Band {
public:
    Band() = default;

    [[nodiscard]] Status Create(u32_t width, u32_t rows, u32_t components) {
        m_width      = width;
        m_components = components;
        m_samples    = static_cast<u64_t>(width) * rows * components;
        m_data       = static_cast<f32_t*>(memory::General().Allocate(
            static_cast<usize_t>(m_samples) * sizeof(f32_t), alignof(f32_t)));
        if (m_data == nullptr) {
            ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Export,
                        "could not allocate a {}x{} band of {} component(s)", width, rows,
                        components);
        }
        return {};
    }

    ~Band() {
        if (m_data != nullptr) {
            memory::General().Free(m_data);
        }
    }

    ENGINE_NO_COPY(Band);
    ENGINE_NO_MOVE(Band);

    [[nodiscard]] f32_t* Row(u32_t row) noexcept {
        return m_data + static_cast<u64_t>(row) * m_width * m_components;
    }

    /// Copies the interior of one section into the band at column `column`.
    void Absorb(const f32_t* section, u32_t sectionStride, u32_t column, u32_t rows,
                u32_t columns) {
        for (u32_t row = 0; row < rows; ++row) {
            std::memcpy(Row(row) + static_cast<u64_t>(column) * m_components,
                        section + static_cast<u64_t>(row) * sectionStride * m_components,
                        static_cast<usize_t>(columns) * m_components * sizeof(f32_t));
        }
    }

private:
    f32_t* m_data       = nullptr;
    u32_t  m_width      = 0;
    u32_t  m_components = 1;
    u64_t  m_samples    = 0;
};

/// Everything one compiled output needs on the CPU side.
struct OutputChannel {
    std::array<char, 256>                     path{};
    std::array<char, OutputRequest::kMaxName> name{};
    PngWriter                                 writer;
    Band                                      band;
    u32_t                                     buffer     = kInvalidBuffer;
    u32_t                                     components = 1;
    u64_t                                     bytes      = 0;
    Mapping                                   mapping;
    f32_t                                     rangeMin = 0.0f;
    f32_t                                     rangeMax = 1.0f;
};

/// Builds `<directory><name>.<extension>` into `out`.
void MakePath(std::array<char, 256>& out, std::string_view directory, std::string_view name,
              const char* extension) {
    const b8_t needsSeparator =
        !directory.empty() && directory.back() != '/' && directory.back() != '\\';
    std::snprintf(out.data(), out.size(), "%.*s%s%.*s.%s", static_cast<int>(directory.size()),
                  directory.data(), needsSeparator ? "/" : "", static_cast<int>(name.size()),
                  name.data(), extension);
}

/// Fills the environment and peak-memory parts of the sidecar.
void FillEnvironment(ExportMetadata& metadata, vulkan::Context& context) {
    detail::CopyBounded(metadata.gpuName, context.Info().Name());
    detail::CopyBounded(metadata.cpuName, platform::CpuName());
    detail::CopyBounded(metadata.osName, platform::OsName());
#ifdef ENGINE_DEBUG
    detail::CopyBounded(metadata.buildType, "Debug");
#else
    detail::CopyBounded(metadata.buildType, "Release");
#endif
    metadata.driverVersion = context.Info().driverVersion;
    metadata.apiVersion    = context.Info().apiVersion;

    metadata.peakCpuBytes = memory::PeakCpuBytes();
    for (usize_t i = 0; i < metadata.peakVram.size(); ++i) {
        metadata.peakVram[i] = context.Memory().CategoryPeak(static_cast<vulkan::VramCategory>(i));
    }
}

} // namespace

Result<ExportSummary> RunExport(Application& application, const ExportJob& job) {
#ifdef TRACY_ENABLE
    ZoneScopedN("terrain::RunExport");
#endif
    const Clock::time_point jobStart = Clock::now();

    if (job.tiles) {
        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Export,
                    "--tiles is not implemented yet; see docs/status.md (milestone 7)");
    }

    Result<Grid> grid = MakeGrid(job);
    if (!grid) {
        return std::unexpected(grid.error());
    }

    vulkan::Context&    context = VulkanContext(application);
    const u32_t         section = grid->sectionSize;
    const SectionExtent extent{.x = section, .y = 1, .z = section};

    LOG_INFO("export: {}x{} samples, {}x{} section(s) of {}, resolution {} m/px", grid->width,
             grid->height, grid->sectionsX, grid->sectionsZ, section, job.resolution);

    Graph graph;
    if (Status built = BuildJobGraph(graph, job.params); !built) {
        return std::unexpected(built.error());
    }

    Result<CompiledGraph> compiled =
        Compile(graph, CompileOptions{.extent     = extent,
                                      .resolution = static_cast<f32_t>(job.resolution)});
    if (!compiled) {
        return std::unexpected(compiled.error());
    }

    KernelLibrary& kernels = KernelsOf(application);
    Result<SectionResources> resources = SectionResources::Create(context, *compiled);
    if (!resources) {
        return std::unexpected(resources.error());
    }
    Result<DispatchTimers> timers = DispatchTimers::Create(context, compiled->stats.dispatchCount);
    if (!timers) {
        return std::unexpected(timers.error());
    }

    if (Status created = platform::MakeDirectory(job.outputDirectory); !created) {
        return std::unexpected(created.error());
    }

    ExportSummary summary;
    summary.width        = grid->width;
    summary.height       = grid->height;
    summary.sectionsX    = grid->sectionsX;
    summary.sectionsZ    = grid->sectionsZ;
    summary.sectionCount = static_cast<u64_t>(grid->sectionsX) * grid->sectionsZ;

    // One writer and one band per compiled output. Fixed storage, because a writer and a band are
    // neither copyable nor movable and the maximum is small and known.
    std::array<OutputChannel, ExportSummary::kMaxOutputs> outputs{};
    const usize_t outputCount =
        compiled->outputs.size() < outputs.size() ? compiled->outputs.size() : outputs.size();
    for (usize_t i = 0; i < outputCount; ++i) {
        const CompiledOutput& info = compiled->outputs[i];
        OutputChannel&        out  = outputs[i];
        out.name       = info.name;
        out.buffer     = info.buffer;
        out.mapping    = info.mapping;
        out.components = info.mapping.components;
        out.rangeMin   = info.rangeMin;
        out.rangeMax   = info.rangeMax;
        out.bytes      = ValueSize(info.mapping, extent, 0);

        Result<OutputFormat> format = FormatFor(info.mapping);
        if (!format) {
            return std::unexpected(format.error());
        }
        MakePath(out.path, job.outputDirectory, info.name.data(), ExtensionFor(*format));

        Result<PngWriter> writer = PngWriter::Create(out.path.data(), grid->width, grid->height,
                                                    info.mapping, info.rangeMin, info.rangeMax);
        if (!writer) {
            return std::unexpected(writer.error());
        }
        out.writer = std::move(*writer);

        if (Status created = out.band.Create(grid->width, section, out.components); !created) {
            return std::unexpected(created.error());
        }
        detail::CopyBounded(summary.files[summary.fileCount++], out.path.data());
    }

    std::pmr::vector<f64_t> gpuPerDispatch(compiled->stats.dispatchCount, 0.0,
                                           &memory::General().Resource());
    f64_t readbackMs = 0.0;
    f64_t encodeMs   = 0.0;

    vulkan::Queue& queue = context.ComputeQueue();

    for (u32_t bandIndex = 0; bandIndex < grid->sectionsZ; ++bandIndex) {
        const u32_t bandZ0   = bandIndex * section;
        const u32_t bandRows = grid->height - bandZ0 < section ? grid->height - bandZ0 : section;

        for (u32_t tileIndex = 0; tileIndex < grid->sectionsX; ++tileIndex) {
#ifdef TRACY_ENABLE
            FrameMarkNamed("Section");
#endif
            const u32_t tileX0 = tileIndex * section;
            const u32_t tileColumns =
                grid->width - tileX0 < section ? grid->width - tileX0 : section;

            Result<VkCommandBuffer> commands = queue.BeginOneShot();
            if (!commands) {
                return std::unexpected(commands.error());
            }

            // Every section is dispatched over the full section extent and only its useful interior
            // is kept, so an edge section computes exactly what an interior one does and the result
            // cannot depend on where the edges fall.
            const SectionJob sectionJob{
                .origin = {static_cast<i32_t>(grid->originX + tileX0), 0,
                           static_cast<i32_t>(grid->originZ + bandZ0)},
                .extent = extent,
                .seed   = static_cast<u32_t>(job.seed)};
            if (Status recorded =
                    RecordSection(kernels, *compiled, *resources, *timers, *commands, sectionJob);
                !recorded) {
                return std::unexpected(recorded.error());
            }

            // Every requested output is copied into the readback ring in the same submission.
            std::array<VkDeviceSize, ExportSummary::kMaxOutputs> readbackOffsets{};
            for (usize_t i = 0; i < outputCount; ++i) {
                const vulkan::SectionSlot& slot = resources->Slot(outputs[i].buffer);
                vulkan::ComputeToTransferBarrier(*commands, slot.buffer, slot.offset, slot.size);
                Result<VkDeviceSize> offset = context.Readback().Reserve(outputs[i].bytes, 16);
                if (!offset) {
                    return std::unexpected(offset.error());
                }
                readbackOffsets[i] = *offset;
                const VkBufferCopy copy{
                    .srcOffset = slot.offset, .dstOffset = *offset, .size = outputs[i].bytes};
                vkCmdCopyBuffer(*commands, slot.buffer, context.Readback().GetBuffer().handle, 1,
                                &copy);
            }

            const Clock::time_point waitStart = Clock::now();
            Result<u64_t>           submitted = queue.EndAndSubmit(*commands);
            if (!submitted) {
                return std::unexpected(submitted.error());
            }
            if (Status waited = queue.WaitTimeline(*submitted, kGpuTimeoutNanoseconds); !waited) {
                return std::unexpected(waited.error());
            }
            readbackMs += MillisecondsSince(waitStart);
            timers->Accumulate(gpuPerDispatch);

            // Released in reverse, because the ring is FIFO and these were reserved in order.
            for (usize_t i = outputCount; i-- > 0;) {
                if (Status invalidated = context.Memory().InvalidateBuffer(
                        context.Readback().GetBuffer(), readbackOffsets[i], outputs[i].bytes);
                    !invalidated) {
                    return std::unexpected(invalidated.error());
                }
                outputs[i].band.Absorb(
                    static_cast<const f32_t*>(context.Readback().MappedAt(readbackOffsets[i])),
                    section, tileX0, bandRows, tileColumns);
                context.Readback().ReleaseOldest();
            }

            context.EndTick();
        }

        // The bands are complete across the full width: stream their rows out and reuse them.
        const Clock::time_point encodeStart = Clock::now();
        for (u32_t row = 0; row < bandRows; ++row) {
            for (usize_t i = 0; i < outputCount; ++i) {
                if (Status written = outputs[i].writer.WriteRow(outputs[i].band.Row(row));
                    !written) {
                    return std::unexpected(written.error());
                }
            }
        }
        encodeMs += MillisecondsSince(encodeStart);
        LOG_DEBUG("band {} of {} written ({} rows)", bandIndex + 1, grid->sectionsZ, bandRows);
    }

    ExportMetadata metadata;
    f64_t          gpuMs = 0.0;
    for (const f64_t milliseconds : gpuPerDispatch) {
        gpuMs += milliseconds;
    }

    for (usize_t i = 0; i < outputCount; ++i) {
        const u64_t clamped = outputs[i].writer.ClampedSamples();
        if (Status finished = outputs[i].writer.Finish(); !finished) {
            return std::unexpected(finished.error());
        }
        summary.clampedSamples += clamped;

        Result<OutputFormat> format = FormatFor(outputs[i].mapping);
        (void)metadata.AddOutput(outputs[i].name.data(), outputs[i].path.data(),
                                 outputs[i].mapping, format ? *format : OutputFormat::Grayscale16,
                                 outputs[i].rangeMin, outputs[i].rangeMax, clamped);
    }

    detail::CopyBounded(metadata.scriptPath, job.script);
    metadata.scriptHash         = HashFile(job.script);
    metadata.seed               = job.seed;
    metadata.minX               = job.minX;
    metadata.minZ               = job.minZ;
    metadata.maxX               = job.maxX;
    metadata.maxZ               = job.maxZ;
    metadata.resolution         = job.resolution;
    metadata.sectionSize        = section;
    metadata.width              = grid->width;
    metadata.height             = grid->height;
    metadata.timings.compileMs  = compiled->stats.milliseconds;
    metadata.timings.pipelineMs = kernels.CreationMilliseconds();
    metadata.timings.gpuMs      = gpuMs;
    metadata.timings.readbackMs = readbackMs;
    metadata.timings.encodeMs   = encodeMs;
    FillEnvironment(metadata, context);

    summary.gpuMs            = gpuMs;
    summary.totalMs          = MillisecondsSince(jobStart);
    metadata.timings.totalMs = summary.totalMs;

    // GPU time per op, summed over every section. This is the measurement that decides whether
    // kernel fusion is worth building (spec section 9).
    for (usize_t i = 0; i < compiled->dispatches.size() && i < gpuPerDispatch.size(); ++i) {
        LOG_INFO("  gpu {} {:.3f} ms over {} section(s)",
                 ToString(compiled->dispatches[i].kind), gpuPerDispatch[i], summary.sectionCount);
    }

    MakePath(summary.metadataFile, job.outputDirectory, "metadata", "json");
    if (Status written = WriteMetadata(summary.metadataFile.data(), metadata); !written) {
        return std::unexpected(written.error());
    }

    LOG_INFO("export finished: {} section(s) in {:.1f} ms ({:.1f} ms GPU), {} clamped sample(s)",
             summary.sectionCount, summary.totalMs, gpuMs, summary.clampedSamples);
    return summary;
}

} // namespace engine::terrain
