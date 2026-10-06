#pragma once

#include <engine/common.hpp>

#ifdef IS_ENGINE

#    include <engine/error.hpp>
#    include <engine/terrain/graph.hpp>
#    include <engine/terrain/mapping.hpp>

#    include <array>
#    include <memory_resource>
#    include <vector>

namespace engine::terrain {

inline constexpr u32_t kInvalidBuffer = ~0u;

/// One kernel launch in the compiled program. Each node becomes exactly one dispatch; there is no
/// kernel fusion in this version (spec section 9).
struct Dispatch {
    /// Index of the node in the source graph, so an error or a Tracy zone can name its script line.
    u32_t     node      = kInvalidNode;
    OpKind    kind      = OpKind::Count;
    NodeClass nodeClass = NodeClass::Pointwise;
    /// Specialization value selecting the kernel variant.
    u32_t variant = 0;
    /// Padding this dispatch must compute, from halo propagation.
    u32_t halo = 0;
    /// Bit per channel the dispatch actually writes. A channel nothing consumes is not written.
    u8_t channelMask = 0;
    u8_t inputCount  = 0;

    std::array<u32_t, kMaxNodeInputs>   inputBuffers{};
    std::array<u32_t, kMaxNodeChannels> outputBuffers{};
    std::array<Mapping, kMaxNodeChannels> channels{};
    std::array<u32_t, kMaxNodeParams>   params{};

    SourceLocation location;
};

/// A physical buffer the evaluator acquires from the section pool before the first dispatch.
struct PlannedBuffer {
    Mapping   mapping;
    u32_t     halo  = 0;
    u64_t     bytes = 0;
    SizeClass sizeClass;
    /// Dispatch index after which the buffer is free again, or `kInvalidBuffer` when it holds an
    /// output and must survive the whole section.
    u32_t lastUse = kInvalidBuffer;
};

struct CompiledOutput {
    std::array<char, OutputRequest::kMaxName> name{};
    u32_t                                     buffer = kInvalidBuffer;
    Mapping                                   mapping;
    f32_t                                     rangeMin = 0.0f;
    f32_t                                     rangeMax = 1.0f;
};

/// What the compiler measured, logged and plotted after every compile (spec section 9).
struct CompileStats {
    u32_t nodesIn        = 0;
    u32_t nodesFolded    = 0;
    u32_t nodesEliminated = 0;
    u32_t nodesDead      = 0;
    u32_t dispatchCount  = 0;
    u32_t bufferCount    = 0;
    u32_t maxHalo        = 0;
    /// Peak intermediate VRAM for one section, known before evaluation starts.
    u64_t peakSectionBytes = 0;
    f64_t milliseconds     = 0.0;
};

struct CompiledGraph {
    std::pmr::vector<Dispatch>       dispatches;
    std::pmr::vector<PlannedBuffer>  buffers;
    std::pmr::vector<CompiledOutput> outputs;
    CompileStats                     stats;

    explicit CompiledGraph(std::pmr::memory_resource& resource)
        : dispatches(&resource), buffers(&resource), outputs(&resource) {}
};

struct CompileOptions {
    /// Interior size of a section. Buffer sizes and size classes are derived from it.
    SectionExtent extent;
    /// Sample spacing in metres, baked into the ops that need world positions.
    f32_t resolution = 1.0f;
    /// Enables constant folding and common-subexpression elimination. Off makes a compiled graph
    /// map one-to-one onto the source nodes, which the compiler tests rely on.
    b8_t canonicalize = true;
};

/// Turns a lazy graph into an ordered list of dispatches plus a buffer plan.
///
/// Stages, each a Tracy zone: validation, canonicalization (constant folding, CSE, dead-node
/// removal), classification, halo propagation, scheduling and buffer planning.
[[nodiscard]] Result<CompiledGraph> Compile(const Graph& graph, const CompileOptions& options);

} // namespace engine::terrain

#endif // IS_ENGINE
