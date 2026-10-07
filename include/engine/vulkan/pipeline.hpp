#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/vulkan/vk.hpp>

#    include <array>
#    include <string_view>

namespace engine::vulkan {

/// Fixed project-wide workgroup sizes (spec section 9, stage 5). Every op kernel declares the
/// matching `local_size_*`, so dispatch sizes are computed the same way for every op.
inline constexpr u32_t kWorkgroupSizeR2 = 8;
inline constexpr u32_t kWorkgroupSizeR3 = 4;

/// Number of input and output values a single kernel dispatch can bind.
inline constexpr usize_t kMaxKernelInputs  = 4;
inline constexpr usize_t kMaxKernelOutputs = 2;
/// Words of op-specific parameters. Numeric parameters never create a pipeline.
inline constexpr usize_t kKernelParamWords = 10;

/// The uniform interface of every op kernel: inputs and outputs are buffer device addresses, so
/// no descriptor set has to be managed per dispatch (spec section 9, stage 5).
///
/// Sized to exactly 128 bytes, the minimum `maxPushConstantsSize` Vulkan guarantees.
struct KernelPushConstants {
    /// Integer section origin in samples. World positions are derived from it plus the local
    /// offset, which keeps neighbouring sections bit-identical at their borders.
    std::array<i32_t, 3> origin{};
    /// Interior size of the section per axis, in samples. `y` is 1 for `R2`.
    std::array<u32_t, 3> extent{};
    /// Padding computed by halo propagation, applied on every axis of the domain.
    u32_t halo = 0;
    /// Domain in bits 0-7 (2 for `R2`, 3 for `R3`) and the channel mask in bits 8-15.
    ///
    /// Packed together because the struct must stay inside the guaranteed 128-byte push-constant
    /// range and `inputs` needs 8-byte alignment: two separate words would cost four bytes of
    /// padding and a parameter word with it. Both sides already go through accessors, here and in
    /// lib/kernel.lib.glsl, so nothing reads the raw field.
    u32_t flags = 2 | (1 << 8);
    u32_t seed  = 0;
    /// Halo of each bound input, one byte per slot, least significant first.
    ///
    /// An input is not guaranteed to have the same halo as the output: halo propagation gives a
    /// producer `output halo + the consumer's radius`, so a neighbourhood op reads a buffer with a
    /// wider border and therefore a different row stride. Pointwise ops have radius zero and can
    /// keep using `halo` for both sides.
    u32_t inputHalos = 0;

    void SetDomain(u32_t domain) noexcept { flags = (flags & ~0xFFu) | (domain & 0xFFu); }
    void SetChannelMask(u32_t mask) noexcept {
        flags = (flags & ~0xFF00u) | ((mask & 0xFFu) << 8);
    }
    [[nodiscard]] u32_t Domain() const noexcept { return flags & 0xFFu; }
    [[nodiscard]] u32_t ChannelMask() const noexcept { return (flags >> 8) & 0xFFu; }

    std::array<VkDeviceAddress, kMaxKernelInputs>  inputs{};
    std::array<VkDeviceAddress, kMaxKernelOutputs> outputs{};
    /// Op-specific parameter block, reinterpreted by each kernel.
    std::array<u32_t, kKernelParamWords> params{};
};

static_assert(sizeof(KernelPushConstants) == 128,
              "the kernel interface must fit the guaranteed 128-byte push-constant range");

/// Number of workgroups needed to cover a padded section.
///
/// `R2` dispatches are 2D and map `(gl_GlobalInvocationID.x, .y)` to `(x, z)`; `R3` dispatches
/// are 3D and map `.xyz` to `(x, y, z)`. `lib/kernel.glsl` implements the matching side.
[[nodiscard]] constexpr std::array<u32_t, 3> DispatchSize(u32_t domain, std::array<u32_t, 3> extent,
                                                          u32_t halo) noexcept {
    const u32_t padding = 2 * halo;
    if (domain == 3) {
        const u32_t local = kWorkgroupSizeR3;
        return {(extent[0] + padding + local - 1) / local,
                (extent[1] + padding + local - 1) / local,
                (extent[2] + padding + local - 1) / local};
    }
    const u32_t local = kWorkgroupSizeR2;
    return {(extent[0] + padding + local - 1) / local, (extent[2] + padding + local - 1) / local,
            1};
}

/// Specialization constants, which create a new pipeline from the same SPIR-V without any
/// shader compilation. Used for variants that change code shape, such as the noise kind.
///
/// Ids 0, 1 and 2 are reserved for the workgroup size, so one module serves both domains.
/// Op variants start at id 3.
struct SpecializationValues {
    static constexpr usize_t kMaxValues     = 8;
    static constexpr u32_t   kFirstVariantId = 3;

    std::array<u32_t, kMaxValues> values{};
    u32_t                         count = 0;

    void Add(u32_t value) noexcept {
        if (count < kMaxValues) {
            values[count++] = value;
        }
    }

    [[nodiscard]] b8_t operator==(const SpecializationValues& other) const noexcept {
        if (count != other.count) {
            return false;
        }
        for (u32_t i = 0; i < count; ++i) {
            if (values[i] != other.values[i]) {
                return false;
            }
        }
        return true;
    }
};

/// Fills ids 0..2 with the workgroup size of `domain`, matching `lib/kernel.glsl`.
[[nodiscard]] constexpr SpecializationValues WorkgroupSpecialization(u32_t domain) noexcept {
    SpecializationValues values;
    if (domain == 3) {
        values.Add(kWorkgroupSizeR3);
        values.Add(kWorkgroupSizeR3);
        values.Add(kWorkgroupSizeR3);
    } else {
        values.Add(kWorkgroupSizeR2);
        values.Add(kWorkgroupSizeR2);
        values.Add(1);
    }
    return values;
}

/// `VkPipelineCache` persisted next to the executable, so repeated runs skip driver compilation.
class PipelineCache {
public:
    PipelineCache() = default;
    ~PipelineCache();

    PipelineCache(PipelineCache&& other) noexcept;
    PipelineCache& operator=(PipelineCache&& other) noexcept;
    ENGINE_NO_COPY(PipelineCache);

    /// Loads `fileName` from the executable directory, or starts empty when it is absent.
    [[nodiscard]] static Result<PipelineCache> Create(VkDevice device, std::string_view fileName);

    /// Writes the cache back to disk. Failures are logged, not fatal.
    void Save() const noexcept;

    [[nodiscard]] VkPipelineCache Handle() const noexcept { return m_cache; }

private:
    VkDevice             m_device = VK_NULL_HANDLE;
    VkPipelineCache      m_cache  = VK_NULL_HANDLE;
    std::array<char, 64> m_fileName{};
};

/// One compute pipeline built from a `.spv` file compiled at build time.
class ComputePipeline {
public:
    ComputePipeline() = default;
    ~ComputePipeline();

    ComputePipeline(ComputePipeline&& other) noexcept;
    ComputePipeline& operator=(ComputePipeline&& other) noexcept;
    ENGINE_NO_COPY(ComputePipeline);

    /// `spirvRelativePath` is relative to `<executable dir>/shaders/`, for example
    /// `ops/constant.comp.spv`.
    [[nodiscard]] static Result<ComputePipeline> Create(VkDevice device,
                                                       std::string_view spirvRelativePath,
                                                       const SpecializationValues& specialization,
                                                       VkPipelineCache             cache);

    [[nodiscard]] VkPipeline       Handle() const noexcept { return m_pipeline; }
    [[nodiscard]] VkPipelineLayout Layout() const noexcept { return m_layout; }
    [[nodiscard]] b8_t             IsValid() const noexcept { return m_pipeline != VK_NULL_HANDLE; }

    /// Binds the pipeline and uploads `constants` on `commands`.
    void Bind(VkCommandBuffer commands, const KernelPushConstants& constants) const;

private:
    VkDevice         m_device   = VK_NULL_HANDLE;
    VkPipelineLayout m_layout   = VK_NULL_HANDLE;
    VkPipeline       m_pipeline = VK_NULL_HANDLE;
};

/// Inserts the buffer memory barrier placed between two dependent dispatches
/// (spec section 9, stage 6).
void ComputeToComputeBarrier(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset,
                             VkDeviceSize size);

/// Barrier from a compute write to a transfer read, used before a readback copy.
void ComputeToTransferBarrier(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset,
                              VkDeviceSize size);

} // namespace engine::vulkan

#endif // IS_ENGINE
