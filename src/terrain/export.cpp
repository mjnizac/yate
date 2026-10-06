#include <engine/terrain/export.hpp>

#include <engine/assert.hpp>
#include <engine/engine.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/platform.hpp>
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

/// fBm configuration, read from the job parameters. Replaced by the compiled graph in milestone 5.
struct NoiseConfig {
    f32_t frequency  = 0.002f;
    u32_t octaves    = 6;
    f32_t lacunarity = 2.0f;
    f32_t persistence = 0.5f;
    f32_t amplitude  = 1.0f;
    f32_t offset     = 0.0f;
    f32_t rangeMin   = -1.0f;
    f32_t rangeMax   = 1.0f;
    b8_t  normals    = false;
    b8_t  normalize  = true;
};

NoiseConfig ReadNoiseConfig(const Params& params) {
    NoiseConfig config;
    config.frequency  = static_cast<f32_t>(params.Number("frequency", config.frequency));
    config.octaves    = static_cast<u32_t>(params.Number("octaves", config.octaves));
    config.lacunarity = static_cast<f32_t>(params.Number("lacunarity", config.lacunarity));
    config.persistence = static_cast<f32_t>(params.Number("persistence", config.persistence));
    config.amplitude  = static_cast<f32_t>(params.Number("amplitude", config.amplitude));
    config.offset     = static_cast<f32_t>(params.Number("offset", config.offset));
    config.octaves    = config.octaves == 0 ? 1 : config.octaves;

    f64_t rangeMin = 0.0;
    f64_t rangeMax = 0.0;
    if (params.Pair("range", rangeMin, rangeMax)) {
        config.rangeMin = static_cast<f32_t>(rangeMin);
        config.rangeMax = static_cast<f32_t>(rangeMax);
    } else {
        // Default to the amplitude band the kernel actually produces.
        config.rangeMin = config.offset - config.amplitude;
        config.rangeMax = config.offset + config.amplitude;
    }
    config.normals   = params.Number("normals", 0.0) != 0.0;
    config.normalize = params.Number("normalize", 1.0) != 0.0;
    return config;
}

/// A timestamp pair written around each section's dispatch, so the metadata reports real GPU time
/// rather than the wall time spent waiting.
class SectionTimer {
public:
    SectionTimer() = default;
    ~SectionTimer() { Release(); }

    SectionTimer(SectionTimer&& other) noexcept { *this = std::move(other); }

    SectionTimer& operator=(SectionTimer&& other) noexcept {
        if (this != &other) {
            Release();
            m_device     = other.m_device;
            m_pool       = other.m_pool;
            m_period     = other.m_period;
            other.m_device = VK_NULL_HANDLE;
            other.m_pool   = VK_NULL_HANDLE;
        }
        return *this;
    }

    ENGINE_NO_COPY(SectionTimer);

    [[nodiscard]] static Result<SectionTimer> Create(VkDevice device, f32_t timestampPeriod) {
        SectionTimer timer;
        timer.m_device = device;
        timer.m_period = timestampPeriod;
        const VkQueryPoolCreateInfo info{.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType  = VK_QUERY_TYPE_TIMESTAMP,
                                        .queryCount = 2};
        VK_TRY(vkCreateQueryPool(device, &info, nullptr, &timer.m_pool));
        return timer;
    }

    void Begin(VkCommandBuffer commands) const {
        vkCmdResetQueryPool(commands, m_pool, 0, 2);
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_pool, 0);
    }

    void End(VkCommandBuffer commands) const {
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_pool, 1);
    }

    /// Milliseconds between the two timestamps, or 0 when the device did not record them.
    [[nodiscard]] f64_t ReadMilliseconds() const {
        std::array<u64_t, 2> ticks{};
        if (vkGetQueryPoolResults(m_device, m_pool, 0, 2, sizeof(ticks), ticks.data(),
                                  sizeof(u64_t), VK_QUERY_RESULT_64_BIT)
            != VK_SUCCESS) {
            return 0.0;
        }
        if (ticks[1] <= ticks[0]) {
            return 0.0;
        }
        return static_cast<f64_t>(ticks[1] - ticks[0]) * static_cast<f64_t>(m_period) * 1e-6;
    }

private:
    void Release() noexcept {
        if (m_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_pool, nullptr);
            m_pool = VK_NULL_HANDLE;
        }
        m_device = VK_NULL_HANDLE;
    }

    VkDevice    m_device = VK_NULL_HANDLE;
    VkQueryPool m_pool   = VK_NULL_HANDLE;
    f32_t       m_period = 0.0f;
};

/// Holds a section slot and releases it when the export unwinds, including on an error path.
class SlotGuard {
public:
    SlotGuard() = default;

    ~SlotGuard() {
        if (m_pool != nullptr) {
            m_pool->Release(m_slot);
        }
    }

    ENGINE_NO_COPY(SlotGuard);
    ENGINE_NO_MOVE(SlotGuard);

    [[nodiscard]] Status Acquire(vulkan::SectionPool& pool, Mapping mapping, SectionExtent extent) {
        const u64_t         bytes = ValueSize(mapping, extent, 0);
        Result<vulkan::SectionSlot> slot = pool.Acquire(ClassOf(mapping, extent, 0), bytes);
        if (!slot) {
            return std::unexpected(slot.error());
        }
        m_pool  = &pool;
        m_slot  = *slot;
        m_bytes = bytes;
        return {};
    }

    [[nodiscard]] const vulkan::SectionSlot& Slot() const noexcept { return m_slot; }
    [[nodiscard]] u64_t                      Bytes() const noexcept { return m_bytes; }

private:
    vulkan::SectionPool* m_pool = nullptr;
    vulkan::SectionSlot  m_slot;
    u64_t                m_bytes = 0;
};

/// A band of full-width rows held in CPU RAM while its sections are evaluated, then streamed into
/// the PNG. Bounds CPU memory to one section row rather than the whole image.
class Band {
public:
    Band() = default;

    [[nodiscard]] Status Create(u32_t width, u32_t rows, u32_t components) {
        m_width      = width;
        m_rows       = rows;
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

    [[nodiscard]] f32_t*       Row(u32_t row) noexcept {
        return m_data + static_cast<u64_t>(row) * m_width * m_components;
    }
    [[nodiscard]] const f32_t* Row(u32_t row) const noexcept {
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
    f32_t*  m_data       = nullptr;
    u32_t   m_width      = 0;
    u32_t   m_rows       = 0;
    u32_t   m_components = 1;
    u64_t   m_samples    = 0;
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
    const NoiseConfig noise = ReadNoiseConfig(job.params);

    vulkan::Context& context = VulkanContext(application);
    const u32_t      section = grid->sectionSize;
    const SectionExtent extent{.x = section, .y = 1, .z = section};

    LOG_INFO("export: {}x{} samples, {}x{} section(s) of {}, resolution {} m/px", grid->width,
             grid->height, grid->sectionsX, grid->sectionsZ, section, job.resolution);

    // Pipeline creation is timed separately, because it is what a persisted pipeline cache removes.
    const Clock::time_point pipelineStart = Clock::now();
    Result<vulkan::ComputePipeline> fbm = vulkan::ComputePipeline::Create(
        context.Device(), "ops/fbm.comp.spv", vulkan::WorkgroupSpecialization(2),
        context.Pipelines().Handle());
    if (!fbm) {
        return std::unexpected(fbm.error());
    }
    // Normals are a second node in the chain: it consumes the analytic gradient the noise kernel
    // already produces, so no neighbour reads and no halo are involved.
    vulkan::ComputePipeline normalsPipeline;
    if (noise.normals) {
        Result<vulkan::ComputePipeline> created = vulkan::ComputePipeline::Create(
            context.Device(), "ops/normals.comp.spv", vulkan::WorkgroupSpecialization(2),
            context.Pipelines().Handle());
        if (!created) {
            return std::unexpected(created.error());
        }
        normalsPipeline = std::move(*created);
    }
    const f64_t pipelineMs = MillisecondsSince(pipelineStart);

    const Mapping heightMapping{Domain::R2, 1};
    const Mapping gradientMapping{Domain::R2, 2};
    const Mapping normalMapping{Domain::R2, 3};

    SlotGuard heightSlot;
    if (Status status = heightSlot.Acquire(context.Sections(), heightMapping, extent); !status) {
        return std::unexpected(status.error());
    }
    SlotGuard gradientSlot;
    SlotGuard normalSlot;
    if (noise.normals) {
        if (Status status = gradientSlot.Acquire(context.Sections(), gradientMapping, extent);
            !status) {
            return std::unexpected(status.error());
        }
        if (Status status = normalSlot.Acquire(context.Sections(), normalMapping, extent);
            !status) {
            return std::unexpected(status.error());
        }
    }

    Result<SectionTimer> timer =
        SectionTimer::Create(context.Device(), context.Info().timestampPeriod);
    if (!timer) {
        return std::unexpected(timer.error());
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

    std::array<char, 256> heightPath{};
    std::array<char, 256> normalPath{};
    MakePath(heightPath, job.outputDirectory, "height", "png");
    MakePath(normalPath, job.outputDirectory, "normals", "png");

    Result<PngWriter> heightWriter = PngWriter::Create(
        heightPath.data(), grid->width, grid->height, heightMapping, noise.rangeMin,
        noise.rangeMax);
    if (!heightWriter) {
        return std::unexpected(heightWriter.error());
    }
    PngWriter normalWriter;
    if (noise.normals) {
        Result<PngWriter> created = PngWriter::Create(normalPath.data(), grid->width, grid->height,
                                                     normalMapping, -1.0f, 1.0f);
        if (!created) {
            return std::unexpected(created.error());
        }
        normalWriter = std::move(*created);
    }

    Band heightBand;
    if (Status status = heightBand.Create(grid->width, section, 1); !status) {
        return std::unexpected(status.error());
    }
    Band normalBand;
    if (noise.normals) {
        if (Status status = normalBand.Create(grid->width, section, 3); !status) {
            return std::unexpected(status.error());
        }
    }

    vulkan::KernelPushConstants constants{};
    constants.extent      = {section, 1, section};
    constants.halo        = 0;
    constants.domain      = 2;
    constants.seed        = static_cast<u32_t>(job.seed);
    constants.channelMask = noise.normals ? 0x3u : 0x1u;
    constants.outputs[0]  = heightSlot.Slot().address;
    constants.outputs[1]  = noise.normals ? gradientSlot.Slot().address : 0;
    std::memcpy(&constants.params[0], &noise.frequency, sizeof(f32_t));
    constants.params[1] = noise.octaves;
    std::memcpy(&constants.params[2], &noise.lacunarity, sizeof(f32_t));
    std::memcpy(&constants.params[3], &noise.persistence, sizeof(f32_t));
    std::memcpy(&constants.params[4], &noise.amplitude, sizeof(f32_t));
    std::memcpy(&constants.params[5], &noise.offset, sizeof(f32_t));
    const f32_t resolution = static_cast<f32_t>(job.resolution);
    std::memcpy(&constants.params[6], &resolution, sizeof(f32_t));

    f64_t gpuMs      = 0.0;
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

            // Every section is dispatched over the full section extent and only its useful
            // interior is kept, so an edge section computes exactly what an interior one does and
            // the result cannot depend on where the edges fall.
            constants.origin = {static_cast<i32_t>(grid->originX + tileX0), 0,
                                static_cast<i32_t>(grid->originZ + bandZ0)};

            Result<VkCommandBuffer> commands = queue.BeginOneShot();
            if (!commands) {
                return std::unexpected(commands.error());
            }

            timer->Begin(*commands);
            fbm->Bind(*commands, constants);
            const std::array<u32_t, 3> groups =
                vulkan::DispatchSize(constants.domain, constants.extent, constants.halo);
            vkCmdDispatch(*commands, groups[0], groups[1], groups[2]);
            timer->End(*commands);

            vulkan::ComputeToTransferBarrier(*commands, heightSlot.Slot().buffer,
                                             heightSlot.Slot().offset, heightSlot.Slot().size);
            Result<VkDeviceSize> heightOffset =
                context.Readback().Reserve(heightSlot.Bytes(), 16);
            if (!heightOffset) {
                return std::unexpected(heightOffset.error());
            }
            const VkBufferCopy heightCopy{.srcOffset = heightSlot.Slot().offset,
                                          .dstOffset = *heightOffset,
                                          .size      = heightSlot.Bytes()};
            vkCmdCopyBuffer(*commands, heightSlot.Slot().buffer,
                            context.Readback().GetBuffer().handle, 1, &heightCopy);

            Result<VkDeviceSize> normalOffset = 0;
            if (noise.normals) {
                // One node, one dispatch, with a buffer barrier between dependent dispatches.
                vulkan::ComputeToComputeBarrier(*commands, gradientSlot.Slot().buffer,
                                                gradientSlot.Slot().offset,
                                                gradientSlot.Slot().size);
                vulkan::KernelPushConstants normalConstants = constants;
                normalConstants.channelMask = 0x1u;
                normalConstants.inputs[0]   = gradientSlot.Slot().address;
                normalConstants.outputs[0]  = normalSlot.Slot().address;
                normalConstants.outputs[1]  = 0;
                std::memset(normalConstants.params.data(), 0, sizeof(normalConstants.params));
                const f32_t verticalScale = 1.0f;
                std::memcpy(&normalConstants.params[0], &verticalScale, sizeof(f32_t));
                normalsPipeline.Bind(*commands, normalConstants);
                vkCmdDispatch(*commands, groups[0], groups[1], groups[2]);

                vulkan::ComputeToTransferBarrier(*commands, normalSlot.Slot().buffer,
                                                 normalSlot.Slot().offset,
                                                 normalSlot.Slot().size);
                normalOffset = context.Readback().Reserve(normalSlot.Bytes(), 16);
                if (!normalOffset) {
                    return std::unexpected(normalOffset.error());
                }
                const VkBufferCopy normalCopy{.srcOffset = normalSlot.Slot().offset,
                                              .dstOffset = *normalOffset,
                                              .size      = normalSlot.Bytes()};
                vkCmdCopyBuffer(*commands, normalSlot.Slot().buffer,
                                context.Readback().GetBuffer().handle, 1, &normalCopy);
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
            gpuMs += timer->ReadMilliseconds();

            if (Status invalidated = context.Memory().InvalidateBuffer(
                    context.Readback().GetBuffer(), *heightOffset, heightSlot.Bytes());
                !invalidated) {
                return std::unexpected(invalidated.error());
            }
            heightBand.Absorb(
                static_cast<const f32_t*>(context.Readback().MappedAt(*heightOffset)), section,
                tileX0, bandRows, tileColumns);

            if (noise.normals) {
                if (Status invalidated = context.Memory().InvalidateBuffer(
                        context.Readback().GetBuffer(), *normalOffset, normalSlot.Bytes());
                    !invalidated) {
                    return std::unexpected(invalidated.error());
                }
                normalBand.Absorb(
                    static_cast<const f32_t*>(context.Readback().MappedAt(*normalOffset)), section,
                    tileX0, bandRows, tileColumns);
                context.Readback().ReleaseOldest();
            }
            context.Readback().ReleaseOldest();

            context.EndTick();
        }

        // The band is complete across the full width: stream its rows out and reuse the buffer.
        const Clock::time_point encodeStart = Clock::now();
        for (u32_t row = 0; row < bandRows; ++row) {
            if (Status written = heightWriter->WriteRow(heightBand.Row(row)); !written) {
                return std::unexpected(written.error());
            }
            if (noise.normals) {
                if (Status written = normalWriter.WriteRow(normalBand.Row(row)); !written) {
                    return std::unexpected(written.error());
                }
            }
        }
        encodeMs += MillisecondsSince(encodeStart);
        LOG_DEBUG("band {} of {} written ({} rows)", bandIndex + 1, grid->sectionsZ, bandRows);
    }

    const u64_t heightClamped = heightWriter->ClampedSamples();
    if (Status finished = heightWriter->Finish(); !finished) {
        return std::unexpected(finished.error());
    }
    u64_t normalClamped = 0;
    if (noise.normals) {
        normalClamped = normalWriter.ClampedSamples();
        if (Status finished = normalWriter.Finish(); !finished) {
            return std::unexpected(finished.error());
        }
    }

    summary.clampedSamples = heightClamped + normalClamped;
    summary.gpuMs          = gpuMs;
    detail::CopyBounded(summary.files[summary.fileCount++], heightPath.data());
    if (noise.normals) {
        detail::CopyBounded(summary.files[summary.fileCount++], normalPath.data());
    }

    ExportMetadata metadata;
    detail::CopyBounded(metadata.scriptPath, job.script);
    metadata.scriptHash  = HashFile(job.script);
    metadata.seed        = job.seed;
    metadata.minX        = job.minX;
    metadata.minZ        = job.minZ;
    metadata.maxX        = job.maxX;
    metadata.maxZ        = job.maxZ;
    metadata.resolution  = job.resolution;
    metadata.sectionSize = section;
    metadata.width       = grid->width;
    metadata.height      = grid->height;
    (void)metadata.AddOutput("height", heightPath.data(), heightMapping,
                             OutputFormat::Grayscale16, noise.rangeMin, noise.rangeMax,
                             heightClamped);
    if (noise.normals) {
        (void)metadata.AddOutput("normals", normalPath.data(), normalMapping, OutputFormat::Rgb16,
                                 -1.0f, 1.0f, normalClamped);
    }
    metadata.timings.pipelineMs = pipelineMs;
    metadata.timings.gpuMs      = gpuMs;
    metadata.timings.readbackMs = readbackMs;
    metadata.timings.encodeMs   = encodeMs;
    FillEnvironment(metadata, context);

    summary.totalMs            = MillisecondsSince(jobStart);
    metadata.timings.totalMs   = summary.totalMs;

    MakePath(summary.metadataFile, job.outputDirectory, "height", "json");
    if (Status written = WriteMetadata(summary.metadataFile.data(), metadata); !written) {
        return std::unexpected(written.error());
    }

    LOG_INFO("export finished: {} section(s) in {:.1f} ms ({:.1f} ms GPU), {} clamped sample(s)",
             summary.sectionCount, summary.totalMs, gpuMs, summary.clampedSamples);
    return summary;
}

} // namespace engine::terrain
