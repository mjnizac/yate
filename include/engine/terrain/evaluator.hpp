#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/terrain/compiler.hpp>
#    include <engine/terrain/kernels.hpp>
#    include <engine/vulkan/buffer_pool.hpp>

#    include <array>
#    include <memory_resource>
#    include <vector>

namespace engine::vulkan {
class Context;
}

namespace engine::terrain {

/// The section-pool slots a compiled graph needs, acquired once and reused for every section.
///
/// The buffer plan already decided how many there are and how large, so the peak VRAM a section
/// costs is known before the first dispatch (spec section 9, stage 6).
class SectionResources {
public:
    SectionResources() = default;
    ~SectionResources();

    SectionResources(SectionResources&& other) noexcept;
    SectionResources& operator=(SectionResources&& other) noexcept;
    ENGINE_NO_COPY(SectionResources);

    [[nodiscard]] static Result<SectionResources> Create(vulkan::Context&     context,
                                                        const CompiledGraph& compiled);

    [[nodiscard]] const vulkan::SectionSlot& Slot(u32_t buffer) const;

    /// Bytes actually held, which is the sum of the slot sizes the pool handed over.
    [[nodiscard]] u64_t TotalBytes() const noexcept { return m_totalBytes; }

private:
    void Release() noexcept;

    vulkan::SectionPool*                 m_pool = nullptr;
    std::pmr::vector<vulkan::SectionSlot> m_slots;
    u64_t                                 m_totalBytes = 0;
};

/// Timestamp pairs around every dispatch, so the metadata sidecar can report GPU time per op.
class DispatchTimers {
public:
    DispatchTimers() = default;
    ~DispatchTimers();

    DispatchTimers(DispatchTimers&& other) noexcept;
    DispatchTimers& operator=(DispatchTimers&& other) noexcept;
    ENGINE_NO_COPY(DispatchTimers);

    [[nodiscard]] static Result<DispatchTimers> Create(vulkan::Context& context,
                                                      u32_t            dispatchCount);

    void Reset(VkCommandBuffer commands) const;
    void Begin(VkCommandBuffer commands, u32_t dispatch) const;
    void End(VkCommandBuffer commands, u32_t dispatch) const;

    /// Adds the milliseconds each dispatch took into `totals`, which is indexed by dispatch.
    void Accumulate(std::pmr::vector<f64_t>& totals) const;

    [[nodiscard]] b8_t IsValid() const noexcept { return m_pool != VK_NULL_HANDLE; }

private:
    void Release() noexcept;

    VkDevice    m_device = VK_NULL_HANDLE;
    VkQueryPool m_pool   = VK_NULL_HANDLE;
    u32_t       m_count  = 0;
    f32_t       m_period = 0.0f;
};

struct SectionJob {
    /// Integer sample origin of the section's interior.
    std::array<i32_t, 3> origin{};
    SectionExtent        extent;
    u32_t                seed = 0;
};

/// Records every dispatch of one section, in topological order, with a buffer memory barrier
/// wherever two dispatches actually depend on each other (spec section 9, stage 6).
[[nodiscard]] Status RecordSection(KernelLibrary& kernels, const CompiledGraph& compiled,
                                   const SectionResources& resources, const DispatchTimers& timers,
                                   VkCommandBuffer commands, const SectionJob& job);

} // namespace engine::terrain

#endif // IS_ENGINE
