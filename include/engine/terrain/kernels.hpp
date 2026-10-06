#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/terrain/graph.hpp>
#    include <engine/vulkan/pipeline.hpp>

#    include <array>

namespace engine::vulkan {
class Context;
}

namespace engine::terrain {

/// First specialization constant id available to an op. Ids 0 to 2 are the workgroup size, so one
/// SPIR-V module serves both domains (see `vulkan::WorkgroupSpecialization`).
///
/// A node's `variant` is an opaque bitfield owned by its op, split into byte-wide fields that map
/// to consecutive constant ids from here on. That lets an op have several independent variant axes
/// without the pipeline map needing more than one key.
inline constexpr u32_t kFirstVariantConstantId = vulkan::SpecializationValues::kFirstVariantId;
inline constexpr u32_t kVariantFieldBits       = 8;
inline constexpr u32_t kVariantFieldMask       = 0xFFu;

/// Packs byte-wide variant fields, least significant first.
[[nodiscard]] constexpr u32_t PackVariant(u32_t field0, u32_t field1 = 0) noexcept {
    return (field0 & kVariantFieldMask) | ((field1 & kVariantFieldMask) << kVariantFieldBits);
}

/// Static description of one op: its kernel, its class and how the compiler may treat it.
///
/// This registry is the single source of truth. The graph builders, the compiler's validation and
/// the pipeline map all read it, so adding an op means adding one entry and one shader.
struct OpInfo {
    OpKind      kind      = OpKind::Count;
    const char* name      = "";
    /// Path of the compiled shader, relative to `<executable dir>/shaders/`.
    const char* shader    = "";
    NodeClass   nodeClass = NodeClass::Pointwise;
    u8_t        maxInputs   = 0;
    u8_t        maxChannels = 1;
    /// Parameter word the sample spacing is written into, or `kMaxNodeParams` when the op does not
    /// need world positions.
    usize_t resolutionWord = kMaxNodeParams;
    /// True when a CPU reference implementation exists, which is also what makes the op foldable.
    b8_t hasReference = false;
    /// Number of byte-wide fields packed into a node's `variant`, each becoming one
    /// specialization constant from id 3 upwards. Zero means the op has no variants.
    u8_t specFieldCount = 0;
};

[[nodiscard]] const OpInfo& OpInfoOf(OpKind kind) noexcept;

[[nodiscard]] b8_t CanFold(OpKind kind) noexcept;

[[nodiscard]] usize_t ResolutionParamWord(OpKind kind) noexcept;

/// CPU reference evaluation of one sample of a pointwise op.
///
/// Returns false when the op has no reference, which is the case for anything that depends on world
/// position. Two jobs rest on this: constant folding in the compiler, and `test_kernels`, which
/// checks every kernel against it (spec section 15, milestone 5).
[[nodiscard]] b8_t EvalPointwise(
    const Graph::Node&                                                        node,
    const std::array<std::array<f32_t, kMaxComponents>, kMaxNodeInputs>&      inputs,
    const std::array<u8_t, kMaxNodeInputs>&                                   inputComponents,
    std::array<f32_t, kMaxComponents>&                                        out);

/// Value and exact gradient of a noise sample, in the same layout the kernel writes.
struct NoiseSample {
    f32_t                value = 0.0f;
    std::array<f32_t, 3> gradient{};
};

/// CPU reference for the noise ops. `position` is the world position in metres, which the kernel
/// derives from the integer sample and the sample spacing.
[[nodiscard]] NoiseSample EvalNoise(const Graph::NoiseParams& params, u32_t seed,
                                    std::array<f32_t, 3> position);

/// Pipeline map keyed by `(op, variant, domain)`.
///
/// Pipelines are created on first use and kept for the process. Changing a numeric parameter never
/// reaches here: it only changes push constants. A variant that changes code shape becomes a new
/// pipeline from the same SPIR-V through a specialization constant, with no shader compilation
/// (spec section 9, stage 5).
class KernelLibrary {
public:
    KernelLibrary() = default;
    ~KernelLibrary() = default;

    KernelLibrary(KernelLibrary&&) noexcept = default;
    KernelLibrary& operator=(KernelLibrary&&) noexcept = default;
    ENGINE_NO_COPY(KernelLibrary);

    [[nodiscard]] static Result<KernelLibrary> Create(vulkan::Context& context);

    /// Returns the pipeline for this op, creating it on first use.
    [[nodiscard]] Result<const vulkan::ComputePipeline*> Get(OpKind kind, u32_t variant,
                                                            Domain domain);

    /// Cumulative time spent creating pipelines, for the metadata sidecar.
    [[nodiscard]] f64_t CreationMilliseconds() const noexcept { return m_creationMs; }

    [[nodiscard]] usize_t PipelineCount() const noexcept { return m_count; }

private:
    static constexpr usize_t kMaxPipelines = 64;

    struct Entry {
        OpKind                  kind    = OpKind::Count;
        u32_t                   variant = 0;
        Domain                  domain  = Domain::R2;
        vulkan::ComputePipeline pipeline;
    };

    vulkan::Context*               m_context = nullptr;
    std::array<Entry, kMaxPipelines> m_entries{};
    usize_t                          m_count      = 0;
    f64_t                            m_creationMs = 0.0;
};

} // namespace engine::terrain

#endif // IS_ENGINE
