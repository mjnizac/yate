#include <engine/terrain/export.hpp>

#include <engine/lua.hpp>

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
/// The domain every output of a compiled script shares.
///
/// The script decides the domain by what it builds, so the grid cannot be laid out until the script
/// has run. Mixing domains in one job would mean two different grids and two different section
/// shapes, which is a job split rather than a graph.
[[nodiscard]] Result<Domain> DomainOf(const Graph& graph, std::string_view script) {
    const Domain domain = graph.Output(0).value.mapping.domain;
    for (usize_t i = 1; i < graph.OutputCount(); ++i) {
        const OutputRequest& output = graph.Output(i);
        if (output.value.mapping.domain != domain) {
            return std::unexpected(MakeScriptError(
                ErrorCode::ScriptError, ErrorStage::Script, script, 0,
                "output '{}' is {} but '{}' is {}; one job evaluates one domain", output.name.data(),
                ToString(output.value.mapping), graph.Output(0).name.data(),
                ToString(graph.Output(0).value.mapping)));
        }
    }
    return domain;
}

/// Everything one compiled output needs on the CPU side.
///
/// A 2D output is staged through a band and encoded on its own worker thread; a volume is written
/// brick by brick straight into the raw file, because a raw layout can be seeked into, needs no
/// staging, and costs a memcpy rather than a deflate.
struct OutputChannel {
    std::array<char, 256>                     path{};
    std::array<char, OutputRequest::kMaxName> name{};
    BandEncoder                               encoder;
    RawVolumeWriter                           raw;
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

    // The script runs before anything is sized, because what it builds is what decides the domain,
    // and the domain decides the grid, the section shape and the output formats.
    Graph                     graph;
    const lua::ScriptEnvironment environment{.seed       = job.seed,
                                             .minX       = job.minX,
                                             .minY       = job.minY,
                                             .minZ       = job.minZ,
                                             .maxX       = job.maxX,
                                             .maxY       = job.maxY,
                                             .maxZ       = job.maxZ,
                                             .resolution = job.resolution,
                                             .params     = &job.params};
    const Clock::time_point      scriptStart = Clock::now();
    if (Status built = lua::RunScript(graph, job.script, environment); !built) {
        return std::unexpected(built.error());
    }
    const f64_t scriptMs = MillisecondsSince(scriptStart);

    Result<Domain> resolvedDomain = DomainOf(graph, job.script);
    if (!resolvedDomain) {
        return std::unexpected(resolvedDomain.error());
    }
    const Domain domain = *resolvedDomain;

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

    Result<CompiledGraph> compiled =
        Compile(graph, CompileOptions{.extent     = extent,
                                      .resolution = static_cast<f32_t>(job.resolution)});
    if (!compiled) {
        return std::unexpected(compiled.error());
    }

    // How many sections the device could hold at once, checked before anything is allocated.
    //
    // The evaluator keeps one set of section buffers, so one section is what is actually in flight on
    // the GPU; the reason that is enough is measured rather than assumed. On a 4096x4096 export with
    // normals the GPU accounts for 21 ms of 2417 ms and the readback for 76 ms, so overlapping
    // dispatches would chase under 4% of the wall time. The pipeline that pays is on the CPU, where
    // encoding is 96% of the work and each output has its own worker with two bands. What this check
    // is for is the other direction: a section that does not fit should say so, with the numbers, and
    // not fail somewhere inside VMA.
    const vulkan::Allocator::HeapBudget vram = context.Memory().DeviceLocalBudget();
    const u64_t                         plannedBytes = compiled->stats.peakSectionBytes;
    const u64_t sectionsThatFit =
        plannedBytes == 0 ? 0 : vram.Available() / plannedBytes;
    if (vram.budget != 0 && sectionsThatFit == 0) {
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Evaluate,
                    "a {} section of this graph needs {} MiB of VRAM but only {} MiB of {} MiB are "
                    "available; use a smaller --section",
                    section, plannedBytes / (1024 * 1024), vram.Available() / (1024 * 1024),
                    vram.budget / (1024 * 1024));
    }
    LOG_INFO("VRAM budget: {} MiB available of {} MiB, {} KiB planned per section ({} would fit), "
             "1 section in flight on the GPU and 2 bands per output on the CPU",
             vram.Available() / (1024 * 1024), vram.budget / (1024 * 1024), plannedBytes / 1024,
             sectionsThatFit);

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
            if (Status started = out.encoder.Start(std::move(*writer), grid->width, section,
                                                   out.components);
                !started) {
                return std::unexpected(started.error());
            }
        }
        detail::CopyBounded(summary.files[summary.fileCount++], out.path.data());
    }

    std::pmr::vector<f64_t> gpuPerDispatch(compiled->stats.dispatchCount, 0.0,
                                           &memory::General().Resource());
    f64_t readbackMs = 0.0;
    // Time the main loop spent writing: `encodeMs` is a volume's own fwrite work, and `stallMs` is how
    // long handing a band over had to wait for a free one, which is the only part of PNG encoding that
    // is still on the critical path.
    f64_t encodeMs = 0.0;
    f64_t stallMs  = 0.0;

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
                        outputs[i].encoder.Filling().Absorb(samples, section, tileX0, bandRows,
                                                            tileColumns);
                    }
                    context.Readback().ReleaseOldest();
                }

                context.UpdatePlots();
            }

            // The bands are complete across the full width. Handing one over returns as soon as the
            // worker has a free band, so the next band is filled while this one is being deflated and
            // the outputs no longer wait on each other.
            const Clock::time_point handoffStart = Clock::now();
            for (usize_t i = 0; i < outputCount; ++i) {
                if (outputs[i].isVolume) {
                    continue;
                }
                if (Status submitted = outputs[i].encoder.Submit(bandRows); !submitted) {
                    return std::unexpected(submitted.error());
                }
            }
            stallMs += MillisecondsSince(handoffStart);
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
            // Drains the queue and joins, so the clamped count is final by the time it is read.
            if (Status finished = outputs[i].encoder.Finish(); !finished) {
                return std::unexpected(finished.error());
            }
            clamped = outputs[i].encoder.ClampedSamples();
            encodeMs += outputs[i].encoder.EncodeMilliseconds();
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
    metadata.timings.scriptMs   = scriptMs;
    metadata.timings.compileMs  = compiled->stats.milliseconds;
    metadata.timings.pipelineMs = kernels.CreationMilliseconds();
    metadata.timings.gpuMs      = gpuMs;
    metadata.timings.readbackMs = readbackMs;
    metadata.timings.encodeMs      = encodeMs;
    metadata.timings.encodeStallMs = stallMs;
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
