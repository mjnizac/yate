#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>
#include <engine/terrain/mapping.hpp>

#include <array>
#include <string_view>

namespace engine::terrain {

inline constexpr u32_t kInvalidNode = ~0u;

/// Maximum values a node can read, and channels it can expose. Both match the kernel interface.
inline constexpr usize_t kMaxNodeInputs   = 4;
inline constexpr usize_t kMaxNodeChannels = 2;
/// Words of op parameters, matching `vulkan::KernelPushConstants::params`.
inline constexpr usize_t kMaxNodeParams = 10;

/// Largest blur radius a node may ask for. Capped because a halo is carried in one byte of the
/// kernel interface, and because the padded section grows as `(s + 2h)^n`.
inline constexpr u32_t kMaxBlurRadius = 32;

/// Every operation the graph can hold. Each one maps to one precompiled compute kernel, with
/// variants selected by specialization constants (spec section 9, stage 5).
enum class OpKind : u16_t {
    /// Writes a constant over the section.
    Const = 0,
    /// Writes the integer world-space sample coordinate.
    Coords,
    /// Fractal noise with its analytic gradient. Channel 0 is the value, channel 1 the gradient.
    Noise,
    /// Unit surface normal from a gradient.
    Normals,
    /// Pointwise arithmetic, with broadcasting of an `Rn -> R1` operand.
    Arith,
    /// Pointwise curve: clamp, linear remap, power, smoothstep.
    Curve,
    /// Linear interpolation between two values, driven by a mask.
    Blend,
    /// Mask from the magnitude of a gradient, with a smooth ramp between two slopes.
    SlopeMask,
    /// Component extraction and assembly.
    Vector,
    /// Box blur over a declared radius. The only neighbourhood op so far.
    Blur,
    Count,
};

[[nodiscard]] ENGINE_API const char* ToString(OpKind kind) noexcept;

/// How much of its neighbourhood an op reads, which is what halo propagation works from
/// (spec section 9, stage 3).
enum class NodeClass : u8_t {
    /// Each output sample depends only on the same sample of its inputs, plus world coordinates.
    Pointwise = 0,
    /// Reads a fixed radius around the sample.
    Neighborhood,
    /// Many iterations with data movement; the op declares its effective influence radius.
    Iterative,
};

[[nodiscard]] ENGINE_API const char* ToString(NodeClass nodeClass) noexcept;

/// Operation an `Arith` node performs. The value is the kernel's specialization constant.
enum class ArithOp : u32_t { Add = 0, Subtract, Multiply, Divide, Minimum, Maximum };

/// Curve a `Curve` node applies.
enum class CurveOp : u32_t { Clamp = 0, Remap, Power, Smoothstep };

/// Noise kind a `Noise` node evaluates.
enum class NoiseKind : u32_t { Simplex = 0, Ridged, Billow };

/// What a `Vector` node does.
enum class VectorOp : u32_t { Extract = 0, Combine };

/// Where in a script a node was created. Reported by every validation and compilation error.
struct SourceLocation {
    std::string_view file;
    u32_t            line = 0;
};

/// A value in the graph: one channel of one node, with the mapping that channel produces.
///
/// Lightweight by design: script functions perform no math, they only append nodes and hand these
/// back (spec section 8).
struct Value {
    u32_t   node    = kInvalidNode;
    u8_t    channel = 0;
    Mapping mapping{};

    [[nodiscard]] b8_t IsValid() const noexcept { return node != kInvalidNode; }
    [[nodiscard]] b8_t operator==(const Value&) const noexcept = default;
};

/// An output the graph was asked to produce.
struct OutputRequest {
    static constexpr usize_t kMaxName = 32;

    std::array<char, kMaxName> name{};
    Value                      value;
    /// Normalization range for scalar outputs. Vector outputs always remap [-1, 1].
    f32_t rangeMin = 0.0f;
    f32_t rangeMax = 1.0f;
};

} // namespace engine::terrain

#ifdef IS_ENGINE

namespace engine::terrain {

/// Lazy graph of operations.
///
/// Building a graph evaluates nothing: each builder validates its inputs' mappings, appends a node
/// and returns a handle. A mapping mismatch is reported at the script line that caused it, which is
/// the whole point of carrying `SourceLocation` on every node (spec section 8).
///
/// Node storage has a fixed capacity, so constructing a graph never allocates.
class Graph {
public:
    static constexpr usize_t kMaxNodes   = 1024;
    static constexpr usize_t kMaxOutputs = 8;

    struct Node {
        OpKind    kind      = OpKind::Count;
        NodeClass nodeClass = NodeClass::Pointwise;
        /// Specialization value selecting the kernel variant, for example the arithmetic op.
        u32_t variant = 0;
        /// Radius this node reads around a sample. Zero for pointwise ops.
        u32_t radius       = 0;
        u8_t  inputCount   = 0;
        u8_t  channelCount = 1;

        std::array<Value, kMaxNodeInputs>     inputs{};
        std::array<Mapping, kMaxNodeChannels> channels{};
        /// Raw parameter bits, copied straight into the kernel push constants.
        std::array<u32_t, kMaxNodeParams> params{};

        SourceLocation location;
    };

    Graph() = default;

    ENGINE_NO_COPY(Graph);
    ENGINE_NO_MOVE(Graph);

    void Clear() noexcept;

    [[nodiscard]] usize_t     NodeCount() const noexcept { return m_nodeCount; }
    [[nodiscard]] usize_t     OutputCount() const noexcept { return m_outputCount; }
    [[nodiscard]] const Node& NodeAt(u32_t index) const;

    // --- Node builders ---------------------------------------------------------------------------

    [[nodiscard]] Result<Value> AddConst(Mapping mapping, std::array<f32_t, 4> components,
                                         SourceLocation location);

    [[nodiscard]] Result<Value> AddCoords(Domain domain, SourceLocation location);

    struct NoiseParams {
        NoiseKind kind       = NoiseKind::Simplex;
        Domain    domain     = Domain::R2;
        f32_t     frequency  = 0.002f;
        u32_t     octaves    = 6;
        f32_t     lacunarity = 2.0f;
        /// Amplitude multiplier per octave. Strictly positive; usually at most 1, but not bounded.
        f32_t persistence = 0.5f;
        f32_t     amplitude  = 1.0f;
        f32_t     offset     = 0.0f;
        /// Decorrelates nodes that share the job seed.
        u32_t     seedSalt   = 0;
        /// Divides the octave sum by the sum of the amplitudes, so `amplitude` is the output
        /// half-range and the declared output range stays predictable. Cleared gives the classic
        /// unnormalized fBm, where `amplitude` is the first octave's amplitude.
        b8_t normalize = true;
    };

    /// Returns the value channel. `Channel(value, 1)` selects the analytic gradient.
    [[nodiscard]] Result<Value> AddNoise(const NoiseParams& params, SourceLocation location);

    /// `gradient` must be `R2 -> R2`. Produces `R2 -> R3`.
    [[nodiscard]] Result<Value> AddNormals(Value gradient, f32_t verticalScale,
                                           SourceLocation location);

    /// Both operands must share a domain. An `Rn -> R1` operand broadcasts over `Rn -> Rm`.
    [[nodiscard]] Result<Value> AddArith(ArithOp op, Value a, Value b, SourceLocation location);

    /// Pointwise curve over any mapping. `a` and `b` are the curve's parameters: the bounds for
    /// `Clamp` and `Smoothstep`, the output range for `Remap`, and the exponent in `a` for `Power`.
    [[nodiscard]] Result<Value> AddCurve(CurveOp op, Value input, f32_t a, f32_t b,
                                         SourceLocation location);

    /// `mask` must be `Rn -> R1` over the same domain. `a` and `b` must share a mapping.
    [[nodiscard]] Result<Value> AddBlend(Value a, Value b, Value mask, SourceLocation location);

    /// `gradient` must be `R2 -> R2`. Produces `R2 -> R1` ramping from 0 below `minSlope` to 1
    /// above `maxSlope`.
    [[nodiscard]] Result<Value> AddSlopeMask(Value gradient, f32_t minSlope, f32_t maxSlope,
                                             SourceLocation location);

    /// Extracts one component of `input`, producing `Rn -> R1`.
    [[nodiscard]] Result<Value> AddExtract(Value input, u8_t component, SourceLocation location);

    /// Box blur of `radius` taps on each side. Neighbourhood: this is what gives the producing node
    /// a halo, and the compiler propagates it backwards from here.
    [[nodiscard]] Result<Value> AddBlur(Value input, u32_t radius, SourceLocation location);

    /// Assembles 2 to 4 scalar values of the same domain into one `Rn -> Rm`.
    [[nodiscard]] Result<Value> AddCombine(const Value* components, u8_t count,
                                           SourceLocation location);

    /// Selects another channel of the node `value` belongs to.
    [[nodiscard]] Result<Value> Channel(Value value, u8_t channel) const;

    [[nodiscard]] Status RequestOutput(std::string_view name, Value value, f32_t rangeMin,
                                       f32_t rangeMax);

    [[nodiscard]] const OutputRequest& Output(usize_t index) const;

private:
    [[nodiscard]] Result<Value> Append(Node node, SourceLocation location);

    /// Checks that `value` refers to a live node and that its mapping is valid.
    [[nodiscard]] Status CheckValue(Value value, const char* role, SourceLocation location) const;

    std::array<Node, kMaxNodes>            m_nodes{};
    usize_t                                m_nodeCount = 0;
    std::array<OutputRequest, kMaxOutputs> m_outputs{};
    usize_t                                m_outputCount = 0;
};

} // namespace engine::terrain

#endif // IS_ENGINE
