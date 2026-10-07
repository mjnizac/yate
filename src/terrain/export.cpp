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
///
/// A 2D grid has one sample along y and one section row there, so the brick loop below is the same
/// code for both domains.
struct Grid {
    i64_t originX     = 0;
    i64_t originY     = 0;
    i64_t originZ     = 0;
    u32_t width       = 0;
    u32_t depth       = 1;
    u32_t height      = 0;
    u32_t sectionSize = 0;
    u32_t sectionsX   = 0;
    u32_t sectionsY   = 1;
    u32_t sectionsZ   = 0;
};

[[nodiscard]] Result<Grid> MakeGrid(const ExportJob& job, Domain domain) {
    if (job.resolution <= 0.0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "resolution must be positive, got {}", job.resolution);
    }
    const auto samples = [&](f64_t world) {
        return static_cast<i64_t>(std::llround(world / job.resolution));
    };
    const i64_t minX = samples(job.minX);
    const i64_t minZ = samples(job.minZ);
    const i64_t maxX = samples(job.maxX);
    const i64_t maxZ = samples(job.maxZ);
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

    if (domain == Domain::R3) {
        const i64_t minY = samples(job.minY);
        const i64_t maxY = samples(job.maxY);
        if (maxY <= minY) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                        "an R3 export needs a y range; got {} to {} at resolution {}", job.minY,
                        job.maxY, job.resolution);
        }
        grid.originY   = minY;
        grid.depth     = static_cast<u32_t>(maxY - minY);
        grid.sectionsY = (grid.depth + job.sectionSize - 1) / job.sectionSize;
    }
    return grid;
}

/// Builds the graph the job describes.
///
/// This is the graph-built-from-C++ of milestone 5. The Lua runtime replaces this one function in
/// milestone 6, and nothing downstream of it changes when it does.
[[nodiscard]] Status BuildJobGraph(Graph& graph, const Params& params, Domain domain) {
    Graph::NoiseParams noise;
    noise.domain      = domain;
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

    // An optional blur, which is what puts a halo on the graph and makes the padded sections real.
    const u32_t blurRadius = static_cast<u32_t>(params.Number("blur", 0.0));
    if (blurRadius != 0) {
        Result<Value> blurred = graph.AddBlur(*value, blurRadius, location);
        if (!blurred) {
            return std::unexpected(blurred.error());
        }
        value = blurred;
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

    // The gradient channel, exported directly rather than turned into normals. This is the only way
    // to get an Rn -> Rn output out of the pipeline, which is a mapping the writer handles
    // differently from both a heightmap and a normal map.
    if (params.Number("gradient", 0.0) != 0.0) {
        Result<Value> gradient = graph.Channel(Value{.node = 0, .channel = 0, .mapping = {}}, 1);
        if (!gradient) {
            return std::unexpected(gradient.error());
        }
        if (Status requested = graph.RequestOutput("gradient", *gradient, -1.0f, 1.0f);
            !requested) {
            return requested;
        }
    }

    if (params.Number("normals", 0.0) == 0.0) {
        return {};
    }
    if (domain != Domain::R2) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Script,
                    "normals are a surface property and need an R2 graph");
    }

    // Channel 1 of the noise node is the analytic gradient that same dispatch already produced. A
    // blur would have consumed only the value, so the gradient is taken from the noise directly.
    Result<Value> noiseValue = graph.Channel(Value{.node = 0, .channel = 0, .mapping = {}}, 1);
    if (!noiseValue) {
        return std::unexpected(noiseValue.error());
    }
    Result<Value> normals = graph.AddNormals(
        *noiseValue, static_cast<f32_t>(params.Number("vertical_scale", 1.0)), location);
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
///
/// A 2D output is staged through a band and streamed into a PNG; a volume is written brick by brick
/// straight into the raw file, because a raw layout can be seeked into and needs no staging.
struct OutputChannel {
    std::array<char, 256>                     path{};
    std::array<char, OutputRequest::kMaxName> name{};
    PngWriter                                 png;
    RawVolumeWriter                           raw;
    Band                                      band;
    b8_t                                      isVolume   = false;
    u32_t                                     buffer     = kInvalidBuffer;
    u32_t                                     components = 1;
    u64_t                                     bytes      = 0;
    Mapping                                   mapping;
    OutputFormat                              format   = OutputFormat::Grayscale16;
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

    const f64_t  requestedDomain = job.params.Number("domain", 2.0);
    const Domain domain = requestedDomain == 3.0 ? Domain::R3 : Domain::R2;
    if (requestedDomain != 2.0 && requestedDomain != 3.0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Export,
                    "domain must be 2 or 3, got {}", requestedDomain);
    }

    Result<Grid> grid = MakeGrid(job, domain);
    if (!grid) {
        return std::unexpected(grid.error());
    }

    vulkan::Context& context = VulkanContext(application);
    const u32_t      section = grid->sectionSize;
    // An R2 section is a tile with one sample of height; an R3 section is a cubic brick.
    const SectionExtent extent{.x = section,
                               .y = domain == Domain::R3 ? section : 1,
                               .z = section};

    LOG_INFO("export: {}x{}x{} samples, {}x{}x{} section(s) of {}, resolution {} m/px, {}",
             grid->width, grid->depth, grid->height, grid->sectionsX, grid->sectionsY,
             grid->sectionsZ, section, job.resolution, domain == Domain::R3 ? "R3" : "R2");

    Graph graph;
    if (Status built = BuildJobGraph(graph, job.params, domain); !built) {
        return std::unexpected(built.error());
    }

    Result<CompiledGraph> compiled =
        Compile(graph, CompileOptions{.extent     = extent,
                                      .resolution = static_cast<f32_t>(job.resolution)});
    if (!compiled) {
        return std::unexpected(compiled.error());
    }

    KernelLibrary&           kernels   = KernelsOf(application);
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
    summary.depth        = grid->depth;
    summary.sectionsX    = grid->sectionsX;
    summary.sectionsY    = grid->sectionsY;
    summary.sectionsZ    = grid->sectionsZ;
    summary.sectionCount = static_cast<u64_t>(grid->sectionsX) * grid->sectionsY * grid->sectionsZ;

    // One writer per compiled output. Fixed storage, because a writer and a band are neither
    // copyable nor movable and the maximum is small and known.
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
        out.bytes      = ValueSize(info.mapping, extent, compiled->buffers[info.buffer].halo);

        Result<OutputFormat> format = FormatFor(info.mapping);
        if (!format) {
            return std::unexpected(format.error());
        }
        out.format   = *format;
        out.isVolume = *format == OutputFormat::RawF32;
        if (out.isVolume != (domain == Domain::R3)) {
            ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Export,
                        "output {} is {} but the job domain is {}", info.name.data(),
                        ToString(info.mapping), domain == Domain::R3 ? "R3" : "R2");
        }
        MakePath(out.path, job.outputDirectory, info.name.data(), ExtensionFor(*format));

        if (out.isVolume) {
            Result<RawVolumeWriter> writer = RawVolumeWriter::Create(
                out.path.data(), grid->width, grid->depth, grid->height, out.components);
            if (!writer) {
                return std::unexpected(writer.error());
            }
            out.raw = std::move(*writer);
        } else {
            Result<PngWriter> writer =
                PngWriter::Create(out.path.data(), grid->width, grid->height, info.mapping,
                                  info.rangeMin, info.rangeMax, job.png);
            if (!writer) {
                return std::unexpected(writer.error());
            }
            out.png = std::move(*writer);
            if (Status created = out.band.Create(grid->width, section, out.components); !created) {
                return std::unexpected(created.error());
            }
        }
        detail::CopyBounded(summary.files[summary.fileCount++], out.path.data());
    }

    std::pmr::vector<f64_t> gpuPerDispatch(compiled->stats.dispatchCount, 0.0,
                                           &memory::General().Resource());
    f64_t readbackMs = 0.0;
    f64_t encodeMs   = 0.0;

    vulkan::Queue& queue = context.ComputeQueue();

    for (u32_t brickY = 0; brickY < grid->sectionsY; ++brickY) {
        const u32_t originY = brickY * section;
        const u32_t layers =
            domain == Domain::R3
                ? (grid->depth - originY < section ? grid->depth - originY : section)
                : 1;

        for (u32_t bandIndex = 0; bandIndex < grid->sectionsZ; ++bandIndex) {
            const u32_t bandZ0 = bandIndex * section;
            const u32_t bandRows =
                grid->height - bandZ0 < section ? grid->height - bandZ0 : section;

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

                // Every section is dispatched over the full section extent and only its useful
                // interior is kept, so an edge section computes exactly what an interior one does and
                // the result cannot depend on where the edges fall.
                const SectionJob sectionJob{
                    .origin = {static_cast<i32_t>(grid->originX + tileX0),
                               static_cast<i32_t>(grid->originY + originY),
                               static_cast<i32_t>(grid->originZ + bandZ0)},
                    .extent = extent,
                    .seed   = static_cast<u32_t>(job.seed)};
                if (Status recorded = RecordSection(queue, kernels, *compiled, *resources, *timers,
                                                    *commands, sectionJob);
                    !recorded) {
                    return std::unexpected(recorded.error());
                }

                // Every requested output is copied into the readback ring in the same submission.
                std::array<VkDeviceSize, ExportSummary::kMaxOutputs> readbackOffsets{};
                for (usize_t i = 0; i < outputCount; ++i) {
                    const vulkan::SectionSlot& slot = resources->Slot(outputs[i].buffer);
                    vulkan::ComputeToTransferBarrier(*commands, slot.buffer, slot.offset,
                                                     slot.size);
                    Result<VkDeviceSize> offset = context.Readback().Reserve(outputs[i].bytes, 16);
                    if (!offset) {
                        return std::unexpected(offset.error());
                    }
                    readbackOffsets[i] = *offset;
                    const VkBufferCopy copy{
                        .srcOffset = slot.offset, .dstOffset = *offset, .size = outputs[i].bytes};
                    vkCmdCopyBuffer(*commands, slot.buffer, context.Readback().GetBuffer().handle,
                                    1, &copy);
                }

                const Clock::time_point waitStart = Clock::now();
                Result<u64_t>           submitted = queue.EndAndSubmit(*commands);
                if (!submitted) {
                    return std::unexpected(submitted.error());
                }
                if (Status waited = queue.WaitTimeline(*submitted, kGpuTimeoutNanoseconds);
                    !waited) {
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
                    const auto* samples = static_cast<const f32_t*>(
                        context.Readback().MappedAt(readbackOffsets[i]));

                    if (outputs[i].isVolume) {
                        const Clock::time_point writeStart = Clock::now();
                        if (Status written = outputs[i].raw.WriteBrick(
                                {tileX0, originY, bandZ0}, {tileColumns, layers, bandRows},
                                {section, section, section}, samples);
                            !written) {
                            return std::unexpected(written.error());
                        }
                        encodeMs += MillisecondsSince(writeStart);
                    } else {
                        outputs[i].band.Absorb(samples, section, tileX0, bandRows, tileColumns);
                    }
                    context.Readback().ReleaseOldest();
                }

                context.UpdatePlots();
            }

            // The bands are complete across the full width: stream their rows out and reuse them.
            const Clock::time_point encodeStart = Clock::now();
            for (u32_t row = 0; row < bandRows; ++row) {
                for (usize_t i = 0; i < outputCount; ++i) {
                    if (outputs[i].isVolume) {
                        continue;
                    }
                    if (Status written = outputs[i].png.WriteRow(outputs[i].band.Row(row));
                        !written) {
                        return std::unexpected(written.error());
                    }
                }
            }
            encodeMs += MillisecondsSince(encodeStart);
        }
    }

    ExportMetadata metadata;
    f64_t          gpuMs = 0.0;
    for (const f64_t milliseconds : gpuPerDispatch) {
        gpuMs += milliseconds;
    }

    for (usize_t i = 0; i < outputCount; ++i) {
        u64_t clamped = 0;
        if (outputs[i].isVolume) {
            if (Status finished = outputs[i].raw.Finish(); !finished) {
                return std::unexpected(finished.error());
            }
        } else {
            clamped = outputs[i].png.ClampedSamples();
            if (Status finished = outputs[i].png.Finish(); !finished) {
                return std::unexpected(finished.error());
            }
        }
        summary.clampedSamples += clamped;
        (void)metadata.AddOutput(outputs[i].name.data(), outputs[i].path.data(),
                                 outputs[i].mapping, outputs[i].format, outputs[i].rangeMin,
                                 outputs[i].rangeMax, clamped);
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
    metadata.depth              = grid->depth;
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
        LOG_INFO("  gpu {} {:.3f} ms over {} section(s)", ToString(compiled->dispatches[i].kind),
                 gpuPerDispatch[i], summary.sectionCount);
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
