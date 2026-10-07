#include <engine/terrain/compiler.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>
#include <engine/terrain/kernels.hpp>
#include <engine/terrain/output_writer.hpp>

#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>
#endif

#include <chrono>
#include <cstring>

namespace engine::terrain {

namespace {

using Clock        = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<f64_t, std::milli>;

/// Per-node bookkeeping the stages fill in turn.
struct NodeWork {
    /// Node this one was replaced by, after folding and CSE. Equal to its own index when it stands.
    u32_t canonical = kInvalidNode;
    /// Constant value, when folding proved the node is one.
    std::array<f32_t, kMaxComponents> constant{};
    b8_t  isConstant = false;
    b8_t  reachable  = false;
    /// Accumulated halo: the sum of the radii of every path from this node to an output.
    u32_t halo = 0;
    /// Channels some consumer or output asks for.
    u8_t channelMask = 0;
    /// Dispatch index this node was emitted as, for the liveness pass.
    u32_t dispatch = kInvalidBuffer;
    /// Buffer each channel was assigned.
    std::array<u32_t, kMaxNodeChannels> buffers{kInvalidBuffer, kInvalidBuffer};
    /// Last dispatch index that reads each channel. Per channel, not per node: a multi-output op
    /// whose value feeds an output while its gradient is consumed and finished would otherwise pin
    /// the gradient buffer for the whole section.
    std::array<u32_t, kMaxNodeChannels> lastReader{kInvalidBuffer, kInvalidBuffer};
    /// Bit per channel a requested output reads, so that channel's buffer survives the section.
    u8_t outputChannels = 0;
    /// Operand this node folded into its kernel as an immediate, or `kMaxNodeInputs` for none. That
    /// operand reads no buffer, so every pass that walks inputs has to skip it.
    u8_t  immediateInput = static_cast<u8_t>(kMaxNodeInputs);
    f32_t immediateValue = 0.0f;
};

/// True when this input was folded into the kernel and needs neither a buffer nor a producer.
[[nodiscard]] b8_t IsImmediate(const std::pmr::vector<NodeWork>& work, u32_t node, u8_t input) {
    return work[node].immediateInput == input;
}

/// Follows the replacement chain to the node that survived canonicalization.
[[nodiscard]] u32_t Resolve(const std::pmr::vector<NodeWork>& work, u32_t node) {
    u32_t current = node;
    while (current != kInvalidNode && work[current].canonical != current) {
        current = work[current].canonical;
    }
    return current;
}

/// Structural hash of a node: op, variant, resolved inputs and parameters. Two nodes with the same
/// hash are compared field by field before being merged, so a collision costs a comparison, never
/// a wrong result.
[[nodiscard]] u64_t StructuralHash(const Graph::Node& node,
                                   const std::pmr::vector<NodeWork>& work) {
    u64_t hash = 0xCBF29CE484222325ull;
    const auto feed = [&hash](u64_t value) {
        hash ^= value;
        hash *= 0x100000001B3ull;
    };

    feed(static_cast<u64_t>(node.kind));
    feed(node.variant);
    feed(node.inputCount);
    feed(node.channelCount);
    for (u8_t i = 0; i < node.inputCount; ++i) {
        feed(Resolve(work, node.inputs[i].node));
        feed(node.inputs[i].channel);
    }
    for (u8_t i = 0; i < node.channelCount; ++i) {
        feed(static_cast<u64_t>(node.channels[i].domain));
        feed(node.channels[i].components);
    }
    for (const u32_t word : node.params) {
        feed(word);
    }
    return hash;
}

[[nodiscard]] b8_t SameNode(const Graph::Node& a, const Graph::Node& b,
                            const std::pmr::vector<NodeWork>& work) {
    if (a.kind != b.kind || a.variant != b.variant || a.inputCount != b.inputCount
        || a.channelCount != b.channelCount) {
        return false;
    }
    for (u8_t i = 0; i < a.inputCount; ++i) {
        if (Resolve(work, a.inputs[i].node) != Resolve(work, b.inputs[i].node)
            || a.inputs[i].channel != b.inputs[i].channel) {
            return false;
        }
    }
    for (u8_t i = 0; i < a.channelCount; ++i) {
        if (!(a.channels[i] == b.channels[i])) {
            return false;
        }
    }
    return std::memcmp(a.params.data(), b.params.data(), sizeof(a.params)) == 0;
}

/// Reads a node's constant components, when it is one.
[[nodiscard]] b8_t ConstantOf(const Graph& graph, const std::pmr::vector<NodeWork>& work,
                              Value input, std::array<f32_t, kMaxComponents>& out,
                              u8_t& components) {
    const u32_t node = Resolve(work, input.node);
    if (node == kInvalidNode || !work[node].isConstant || input.channel != 0) {
        return false;
    }
    out        = work[node].constant;
    components = graph.NodeAt(node).channels[0].components;
    return true;
}

// --- Stages -------------------------------------------------------------------------------------

[[nodiscard]] Status Validate(const Graph& graph) {
#ifdef TRACY_ENABLE
    ZoneScopedN("compiler: validation");
#endif
    if (graph.OutputCount() == 0) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Validation,
                    "the graph requests no outputs");
    }

    for (u32_t index = 0; index < graph.NodeCount(); ++index) {
        const Graph::Node& node = graph.NodeAt(index);
        if (node.kind == OpKind::Count) {
            return std::unexpected(MakeScriptError(ErrorCode::InternalError,
                                                  ErrorStage::Validation, node.location.file,
                                                  node.location.line, "node {} has no op", index));
        }
        const OpInfo& info = OpInfoOf(node.kind);
        if (node.inputCount > info.maxInputs) {
            return std::unexpected(MakeScriptError(
                ErrorCode::InvalidArgument, ErrorStage::Validation, node.location.file,
                node.location.line, "{} takes at most {} input(s), got {}", ToString(node.kind),
                info.maxInputs, node.inputCount));
        }
        if (node.nodeClass != info.nodeClass) {
            return std::unexpected(MakeScriptError(
                ErrorCode::InternalError, ErrorStage::Validation, node.location.file,
                node.location.line, "{} is classified {} but the registry says {}",
                ToString(node.kind), ToString(node.nodeClass), ToString(info.nodeClass)));
        }

        for (u8_t i = 0; i < node.inputCount; ++i) {
            const Value& input = node.inputs[i];
            // Builders only ever reference already-created nodes, so a back edge is impossible
            // through the API; the compiler still asserts there are none (spec section 9, stage 1).
            if (input.node >= index) {
                return std::unexpected(MakeScriptError(
                    ErrorCode::InternalError, ErrorStage::Validation, node.location.file,
                    node.location.line, "node {} reads node {}, which would form a cycle", index,
                    input.node));
            }
            const Graph::Node& producer = graph.NodeAt(input.node);
            if (input.channel >= producer.channelCount) {
                return std::unexpected(MakeScriptError(
                    ErrorCode::NotFound, ErrorStage::Validation, node.location.file,
                    node.location.line, "{} reads channel {} of {}, which exposes {}",
                    ToString(node.kind), input.channel, ToString(producer.kind),
                    producer.channelCount));
            }
            if (!(input.mapping == producer.channels[input.channel])) {
                return std::unexpected(MakeScriptError(
                    ErrorCode::InvalidArgument, ErrorStage::Validation, node.location.file,
                    node.location.line, "{} expects {} on input {}, but the producer yields {}",
                    ToString(node.kind), ToString(input.mapping), i,
                    ToString(producer.channels[input.channel])));
            }
        }
    }

    for (usize_t i = 0; i < graph.OutputCount(); ++i) {
        const OutputRequest& request = graph.Output(i);
        if (request.value.node >= graph.NodeCount()) {
            ENGINE_FAIL(ErrorCode::NotFound, ErrorStage::Validation,
                        "output {} refers to a node outside the graph", request.name.data());
        }
        // An output must have a file format, which is what rejects mappings with no PNG form.
        if (Result<OutputFormat> format = FormatFor(request.value.mapping); !format) {
            return std::unexpected(format.error());
        }
    }
    return {};
}

/// Constant folding and common-subexpression elimination.
void Canonicalize(const Graph& graph, std::pmr::vector<NodeWork>& work, CompileStats& stats) {
#ifdef TRACY_ENABLE
    ZoneScopedN("compiler: canonicalization");
#endif
    // Hash buckets for CSE. Linear probing over a fixed table keeps this allocation-free.
    constexpr usize_t kBuckets = 2 * Graph::kMaxNodes;
    std::array<u32_t, kBuckets> table{};
    table.fill(kInvalidNode);

    for (u32_t index = 0; index < graph.NodeCount(); ++index) {
        const Graph::Node& node = graph.NodeAt(index);
        work[index].canonical   = index;

        // Constant folding: a pointwise op whose inputs are all constants is one itself.
        std::array<std::array<f32_t, kMaxComponents>, kMaxNodeInputs> inputs{};
        std::array<u8_t, kMaxNodeInputs>                             counts{};
        b8_t allConstant = node.nodeClass == NodeClass::Pointwise && CanFold(node.kind);
        for (u8_t i = 0; i < node.inputCount && allConstant; ++i) {
            allConstant = ConstantOf(graph, work, node.inputs[i], inputs[i], counts[i]);
        }
        if (allConstant) {
            std::array<f32_t, kMaxComponents> folded{};
            if (EvalPointwise(node, inputs, counts, folded)) {
                work[index].isConstant = true;
                work[index].constant   = folded;
                ++stats.nodesFolded;
            }
        }

        // CSE: an identical node already in the table takes this one's place.
        const u64_t hash = StructuralHash(node, work);
        usize_t     slot = static_cast<usize_t>(hash % kBuckets);
        for (usize_t probe = 0; probe < kBuckets; ++probe) {
            const usize_t at = (slot + probe) % kBuckets;
            if (table[at] == kInvalidNode) {
                table[at] = index;
                break;
            }
            const u32_t candidate = table[at];
            if (SameNode(graph.NodeAt(candidate), node, work)) {
                work[index].canonical = candidate;
                ++stats.nodesEliminated;
                break;
            }
        }
    }
}

/// Folds a scalar constant operand of an `Arith` node into the kernel as an immediate.
///
/// Without this, `height * 650.0 + 200.0` costs two `Const` dispatches that each fill a whole section
/// buffer with one repeated number. On the `blended_r2` case that was 0.16 ms of 1.6 ms of GPU time for
/// two floats, and the graph drops from 10 dispatches to 8.
///
/// It does not reduce peak VRAM, which was the guess before measuring: the planner was already reusing
/// those buffers for later values, so the peak is set by how many values are live at once and not by
/// how many constants the graph mentions.
///
/// Only a one-component constant qualifies, because the immediate is a single float and the kernel
/// broadcasts it the way it broadcasts a one-component operand. A node whose operands are *both*
/// constant never reaches here: folding already turned it into a constant itself.
void FoldScalarOperands(const Graph& graph, std::pmr::vector<NodeWork>& work, CompileStats& stats) {
#ifdef TRACY_ENABLE
    ZoneScopedN("compiler: operand folding");
#endif
    for (u32_t index = 0; index < graph.NodeCount(); ++index) {
        const Graph::Node& node = graph.NodeAt(index);
        if (node.kind != OpKind::Arith || work[index].isConstant
            || work[index].canonical != index) {
            continue;
        }
        for (u8_t input = 0; input < node.inputCount; ++input) {
            std::array<f32_t, kMaxComponents> value{};
            u8_t                              components = 0;
            if (!ConstantOf(graph, work, node.inputs[input], value, components)
                || components != 1) {
                continue;
            }
            work[index].immediateInput = input;
            work[index].immediateValue = value[0];
            ++stats.operandsFolded;
            break;
        }
    }
}

/// Marks what the outputs reach, and which channels of it they need.
void MarkReachable(const Graph& graph, std::pmr::vector<NodeWork>& work) {
#ifdef TRACY_ENABLE
    ZoneScopedN("compiler: dead-node removal");
#endif
    for (usize_t i = 0; i < graph.OutputCount(); ++i) {
        const OutputRequest& request = graph.Output(i);
        const u32_t          node    = Resolve(work, request.value.node);
        const u8_t bit         = static_cast<u8_t>(1u << request.value.channel);
        work[node].reachable   = true;
        work[node].outputChannels = static_cast<u8_t>(work[node].outputChannels | bit);
        work[node].channelMask    = static_cast<u8_t>(work[node].channelMask | bit);
    }

    // Nodes only ever read lower indices, so one backwards sweep is enough.
    for (u32_t index = static_cast<u32_t>(graph.NodeCount()); index-- > 0;) {
        if (!work[index].reachable) {
            continue;
        }
        const Graph::Node& node = graph.NodeAt(index);
        for (u8_t i = 0; i < node.inputCount; ++i) {
            if (IsImmediate(work, index, i)) {
                continue; // Folded into the kernel, so its producer is not reached through here.
            }
            const u32_t producer = Resolve(work, node.inputs[i].node);
            work[producer].reachable = true;
            work[producer].channelMask =
                static_cast<u8_t>(work[producer].channelMask | (1u << node.inputs[i].channel));
        }
    }
}

/// Halo propagation: each node's output must cover the padding every downstream consumer reads
/// (spec section 9, stage 4). Because nothing is fused, a pointwise node feeding a neighbourhood
/// node is simply computed over the enlarged area.
void PropagateHalo(const Graph& graph, std::pmr::vector<NodeWork>& work, CompileStats& stats) {
#ifdef TRACY_ENABLE
    ZoneScopedN("compiler: halo propagation");
#endif
    for (u32_t index = static_cast<u32_t>(graph.NodeCount()); index-- > 0;) {
        if (!work[index].reachable) {
            continue;
        }
        const Graph::Node& node     = graph.NodeAt(index);
        const u32_t        required = work[index].halo + node.radius;
        for (u8_t i = 0; i < node.inputCount; ++i) {
            if (IsImmediate(work, index, i)) {
                continue;
            }
            const u32_t producer = Resolve(work, node.inputs[i].node);
            if (work[producer].halo < required) {
                work[producer].halo = required;
            }
        }
        if (work[index].halo > stats.maxHalo) {
            stats.maxHalo = work[index].halo;
        }
    }
}

/// Refuses a graph whose propagated halo cannot be represented or paid for.
///
/// The kernel interface carries a halo in one byte per slot, and a section with halo `h` computes
/// `(s + 2h)^n` samples, so a halo is a real cost and not a detail. Chaining iterative ops is what
/// reaches the limit: two 200-iteration erosions in series need 400 samples of padding on every side.
/// Saying so with the arithmetic beats either truncating the halo, which puts seams back, or failing
/// somewhere in the evaluator.
[[nodiscard]] Status CheckHalos(const Graph& graph, const std::pmr::vector<NodeWork>& work) {
    for (u32_t index = 0; index < graph.NodeCount(); ++index) {
        if (!work[index].reachable || work[index].halo <= kMaxHalo) {
            continue;
        }
        const Graph::Node& node = graph.NodeAt(index);
        return std::unexpected(MakeScriptError(
            ErrorCode::Unsupported, ErrorStage::Compile, node.location.file, node.location.line,
            "{} would need a halo of {}, over the limit of {}; every op between here and an output "
            "adds its radius, and an iterative op contributes one per iteration",
            ToString(node.kind), work[index].halo, kMaxHalo));
    }
    return {};
}

/// Buffer planning: assigns physical buffers, reusing one as soon as its last consumer has run.
/// A buffer is only reused for a value whose computed size fits in it and whose domain matches, so
/// `R2` and `R3` values never share a slot (spec sections 7.3 and 9).
class BufferPlanner {
public:
    BufferPlanner(CompiledGraph& compiled, SectionExtent extent)
        : m_compiled(compiled), m_extent(extent) {}

    [[nodiscard]] u32_t Acquire(Mapping mapping, u32_t halo) {
        const u64_t     bytes     = ValueSize(mapping, m_extent, halo);
        const SizeClass sizeClass = ClassOf(mapping, m_extent, halo);

        // Smallest free buffer that still fits, so a large slot is not wasted on a small value.
        u32_t best      = kInvalidBuffer;
        u64_t bestSlot  = 0;
        for (usize_t i = 0; i < m_free.size(); ++i) {
            const PlannedBuffer& candidate = m_compiled.buffers[m_free[i]];
            if (!candidate.sizeClass.Accepts(mapping.domain, bytes)) {
                continue;
            }
            if (best == kInvalidBuffer || candidate.sizeClass.slotSize < bestSlot) {
                best      = static_cast<u32_t>(i);
                bestSlot  = candidate.sizeClass.slotSize;
            }
        }
        if (best != kInvalidBuffer) {
            const u32_t buffer = m_free[best];
            m_free.erase(m_free.begin() + static_cast<isize_t>(best));
            m_live += m_compiled.buffers[buffer].sizeClass.slotSize;
            TrackPeak();
            return buffer;
        }

        m_compiled.buffers.push_back(PlannedBuffer{
            .mapping = mapping, .halo = halo, .bytes = bytes, .sizeClass = sizeClass});
        m_live += sizeClass.slotSize;
        TrackPeak();
        return static_cast<u32_t>(m_compiled.buffers.size() - 1);
    }

    void Release(u32_t buffer) {
        m_live -= m_compiled.buffers[buffer].sizeClass.slotSize;
        m_free.push_back(buffer);
    }

    [[nodiscard]] u64_t PeakBytes() const noexcept { return m_peak; }

private:
    void TrackPeak() {
        if (m_live > m_peak) {
            m_peak = m_live;
        }
    }

    CompiledGraph&          m_compiled;
    SectionExtent           m_extent;
    std::pmr::vector<u32_t> m_free{&memory::General().Resource()};
    u64_t                   m_live = 0;
    u64_t                   m_peak = 0;
};

} // namespace

Result<CompiledGraph> Compile(const Graph& graph, const CompileOptions& options) {
#ifdef TRACY_ENABLE
    ZoneScopedN("terrain::Compile");
#endif
    const Clock::time_point start = Clock::now();

    if (Status validated = Validate(graph); !validated) {
        return std::unexpected(validated.error());
    }

    CompiledGraph compiled(memory::General().Resource());
    compiled.stats.nodesIn = static_cast<u32_t>(graph.NodeCount());

    std::pmr::vector<NodeWork> work(graph.NodeCount(), &memory::General().Resource());
    if (options.canonicalize) {
        Canonicalize(graph, work, compiled.stats);
    } else {
        for (u32_t index = 0; index < graph.NodeCount(); ++index) {
            work[index].canonical = index;
        }
    }

    FoldScalarOperands(graph, work, compiled.stats);
    MarkReachable(graph, work);
    for (u32_t index = 0; index < graph.NodeCount(); ++index) {
        if (!work[index].reachable) {
            ++compiled.stats.nodesDead;
        }
    }

    PropagateHalo(graph, work, compiled.stats);
    if (Status halos = CheckHalos(graph, work); !halos) {
        return std::unexpected(halos.error());
    }

#ifdef TRACY_ENABLE
    ZoneNamedN(scheduleZone, "compiler: scheduling and buffer planning", true);
#endif

    // Node indices are already a topological order: a builder can only reference nodes that exist.
    // Emit one dispatch per live node in that order.
    for (u32_t index = 0; index < graph.NodeCount(); ++index) {
        if (!work[index].reachable || work[index].canonical != index) {
            continue;
        }
        work[index].dispatch = static_cast<u32_t>(compiled.dispatches.size());
        const Graph::Node& node = graph.NodeAt(index);

        Dispatch dispatch;
        dispatch.node        = index;
        dispatch.kind        = node.kind;
        dispatch.nodeClass   = node.nodeClass;
        dispatch.variant     = node.variant;
        dispatch.halo        = work[index].halo;
        dispatch.channelMask = work[index].channelMask;
        dispatch.inputCount  = node.inputCount;
        dispatch.iterations  = node.iterations;
        dispatch.params      = node.params;
        dispatch.location    = node.location;
        dispatch.inputBuffers.fill(kInvalidBuffer);
        dispatch.outputBuffers.fill(kInvalidBuffer);
        for (u8_t channel = 0; channel < node.channelCount; ++channel) {
            dispatch.channels[channel] = node.channels[channel];
        }

        // A folded operand becomes a specialization value and a parameter word, which is the whole
        // cost of the optimisation on this side.
        if (work[index].immediateInput != static_cast<u8_t>(kMaxNodeInputs)) {
            const ArithImmediate which = work[index].immediateInput == 0 ? ArithImmediate::Left
                                                                        : ArithImmediate::Right;
            dispatch.variant = PackVariant(node.variant & kVariantFieldMask,
                                           static_cast<u32_t>(which));
            dispatch.immediateInputs =
                static_cast<u8_t>(1u << work[index].immediateInput);
            std::memcpy(&dispatch.params[kArithImmediateWord], &work[index].immediateValue,
                        sizeof(f32_t));
        }

        // Ops that need world positions take the sample spacing as a parameter, baked here so the
        // evaluator does not have to know which ops care.
        if (const usize_t word = ResolutionParamWord(node.kind); word < kMaxNodeParams) {
            std::memcpy(&dispatch.params[word], &options.resolution, sizeof(f32_t));
        }

        compiled.dispatches.push_back(dispatch);
    }

    // Liveness: the last dispatch that reads each channel of each node.
    for (const Dispatch& dispatch : compiled.dispatches) {
        const Graph::Node& node = graph.NodeAt(dispatch.node);
        for (u8_t i = 0; i < node.inputCount; ++i) {
            if (IsImmediate(work, dispatch.node, i)) {
                continue;
            }
            const u32_t producer = Resolve(work, node.inputs[i].node);
            work[producer].lastReader[node.inputs[i].channel] = work[dispatch.node].dispatch;
        }
    }

    BufferPlanner planner(compiled, options.extent);

    for (usize_t i = 0; i < compiled.dispatches.size(); ++i) {
        Dispatch&          dispatch = compiled.dispatches[i];
        const Graph::Node& node     = graph.NodeAt(dispatch.node);

        // Inputs are already produced; bind the buffers their producers were given, along with the
        // halo each was allocated with. The kernel needs that halo because a wider border means a
        // different row stride, which only a neighbourhood op ever sees.
        for (u8_t input = 0; input < node.inputCount; ++input) {
            if (IsImmediate(work, dispatch.node, input)) {
                // Left unbound on purpose: the specialization says this slot is an immediate, and the
                // kernel never dereferences it.
                continue;
            }
            const u32_t producer = Resolve(work, node.inputs[input].node);
            dispatch.inputBuffers[input] = work[producer].buffers[node.inputs[input].channel];
            dispatch.inputHalos[input]   = static_cast<u8_t>(work[producer].halo);
        }

        for (u8_t channel = 0; channel < node.channelCount; ++channel) {
            if ((dispatch.channelMask & (1u << channel)) == 0) {
                continue;
            }
            const u32_t buffer = planner.Acquire(node.channels[channel], dispatch.halo);
            dispatch.outputBuffers[channel] = buffer;
            work[dispatch.node].buffers[channel] = buffer;
        }

        // An iterative dispatch exchanges buffers between iterations, so it needs one or two of its
        // own. They are acquired after the output and released before the next dispatch is planned,
        // because nothing outside this dispatch ever reads them: the planner can hand the same slots to
        // the next value, which is why an iterative node costs at most two extra buffers rather than
        // one per iteration.
        if (dispatch.iterations > 1) {
            dispatch.stateHalo    = dispatch.halo + node.radius;
            dispatch.scratchCount = dispatch.iterations == 2 ? 1 : 2;
            for (u8_t k = 0; k < dispatch.scratchCount; ++k) {
                dispatch.scratchBuffers[k] = planner.Acquire(node.channels[0], dispatch.stateHalo);
            }
            for (u8_t k = 0; k < dispatch.scratchCount; ++k) {
                compiled.buffers[dispatch.scratchBuffers[k]].lastUse = static_cast<u32_t>(i);
                planner.Release(dispatch.scratchBuffers[k]);
            }
        }

        // Release every channel whose last reader was this dispatch, so the next value can reuse
        // its buffer. A channel a requested output reads is never released: it has to survive the
        // whole section.
        for (u8_t input = 0; input < node.inputCount; ++input) {
            if (IsImmediate(work, dispatch.node, input)) {
                continue;
            }
            const u32_t producer = Resolve(work, node.inputs[input].node);
            const u8_t  channel  = node.inputs[input].channel;
            if ((work[producer].outputChannels & (1u << channel)) != 0) {
                continue;
            }
            if (work[producer].lastReader[channel] != i) {
                continue;
            }
            const u32_t buffer = work[producer].buffers[channel];
            if (buffer != kInvalidBuffer) {
                compiled.buffers[buffer].lastUse = static_cast<u32_t>(i);
                planner.Release(buffer);
                work[producer].buffers[channel] = kInvalidBuffer;
            }
        }
    }

    for (usize_t i = 0; i < graph.OutputCount(); ++i) {
        const OutputRequest& request = graph.Output(i);
        const u32_t          node    = Resolve(work, request.value.node);
        CompiledOutput       output;
        output.name     = request.name;
        output.buffer   = work[node].buffers[request.value.channel];
        output.mapping  = request.value.mapping;
        output.rangeMin = request.rangeMin;
        output.rangeMax = request.rangeMax;
        if (output.buffer == kInvalidBuffer) {
            ENGINE_FAIL(ErrorCode::InternalError, ErrorStage::Compile,
                        "output {} was not assigned a buffer", output.name.data());
        }
        compiled.outputs.push_back(output);
    }

    compiled.stats.dispatchCount    = static_cast<u32_t>(compiled.dispatches.size());
    compiled.stats.bufferCount      = static_cast<u32_t>(compiled.buffers.size());
    compiled.stats.peakSectionBytes = planner.PeakBytes();
    compiled.stats.milliseconds     = Milliseconds(Clock::now() - start).count();

    LOG_INFO("graph compiled in {:.2f} ms: {} node(s) -> {} dispatch(es) ({} folded, {} operand(s) "
             "inlined, {} eliminated, "
             "{} dead), {} buffer(s), max halo {}, peak {} KiB per section",
             compiled.stats.milliseconds, compiled.stats.nodesIn, compiled.stats.dispatchCount,
             compiled.stats.nodesFolded, compiled.stats.operandsFolded,
             compiled.stats.nodesEliminated, compiled.stats.nodesDead,
             compiled.stats.bufferCount, compiled.stats.maxHalo,
             compiled.stats.peakSectionBytes / 1024);
    return compiled;
}

} // namespace engine::terrain
