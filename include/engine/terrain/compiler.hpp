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
    /// Bit per input the compiler folded into the kernel as an immediate. Those slots are
    /// deliberately left unbound, and the evaluator must not treat that as a planning failure.
    u8_t immediateInputs = 0;
    /// Halo each input buffer was allocated with, which is this node's halo plus its own radius.
    std::array<u8_t, kMaxNodeInputs> inputHalos{};
    std::array<u32_t, kMaxNodeChannels> outputBuffers{};
    /// Times to run this kernel, each run reading what the previous one wrote. One for everything that
    /// is not iterative.
    u32_t iterations = 1;
    /// Buffers the iterations ping-pong between, before the last one writes `outputBuffers[0]`.
    ///
    /// Two for three or more iterations, one for exactly two, none otherwise: the last iteration always
    /// writes the node's own output, so the pair only has to hold what is still in flight.
    std::array<u32_t, 2> scratchBuffers{kInvalidBuffer, kInvalidBuffer};
    u8_t                 scratchCount = 0;
    /// Shape of the intermediate state, which is not always what the node produces: hydraulic erosion
    /// carries height, water and sediment between iterations and hands back a height.
    Mapping stateMapping{};
    /// Halo the intermediate state carries, which is this dispatch's halo plus its influence radius.
    ///
    /// Not the same as `halo`, and the difference is the whole correctness argument for an iterative
    /// op. A sample is wrong after one iteration if a neighbour it needed lay outside the computed
    /// region, and that error walks one cell inward per iteration. Computing the state over a halo of
    /// `halo + radius` means that after `radius` iterations the wrongness has reached exactly the
    /// boundary of `halo` and no further, so what the next dispatch reads is exact. One less and the
    /// outermost ring of the output would be subtly wrong, which is precisely the kind of error that
    /// only shows up as a seam between sections.
    u32_t stateHalo = 0;
    std::array<Mapping, kMaxNodeChannels> channels{};
    std::array<u32_t, kMaxNodeParams>   params{};

    SourceLocation location;
};

/// A physical buffer the evaluator acquires from the section pool before the first dispatch.
struct PlannedBuffer {
    /// `mapping`, `halo` and `bytes` describe the value the slot was *created* for. The planner
    /// reuses a slot for any later value that fits, so they do not describe its current occupant:
    /// only `sizeClass` is a property of the slot itself. To read a value out of a buffer, take its
    /// halo from the dispatch that wrote it, or from `CompiledOutput::halo`.
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
    /// Halo the producing dispatch wrote with, so the padded stride of the readback is
    /// `extent + 2 * halo`. A value read with a radius keeps its halo even as a requested output.
    u32_t                                     halo = 0;
    f32_t                                     rangeMin = 0.0f;
    f32_t                                     rangeMax = 1.0f;
};

/// What the compiler measured, logged and plotted after every compile (spec section 9).
struct CompileStats {
    u32_t nodesIn        = 0;
    /// Scalar operands folded into a kernel as an immediate, each one saving a dispatch and a buffer.
    u32_t operandsFolded = 0;
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
