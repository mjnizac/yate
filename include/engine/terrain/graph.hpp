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

/// Parameter word the evaluator overwrites with the total iteration count of an iterative op.
///
/// A kernel whose first and last iterations differ from the rest needs both the index and the count to
/// know where it is, and neither can come from a specialization constant: that would mean one pipeline
/// per iteration count.
inline constexpr usize_t kIterationCountWord = kMaxNodeParams - 2;

/// Parameter word the evaluator overwrites with the current iteration index of an iterative op.
///
/// Reserved rather than allocated per op: the evaluator writes it without knowing which op it is
/// running, so no op may use it for anything else. The last word, to stay out of the way of the
/// parameters a builder packs from the front.
inline constexpr usize_t kIterationParamWord = kMaxNodeParams - 1;

/// Largest blur radius a node may ask for. Capped because a halo is carried in one byte of the
/// kernel interface, and because the padded section grows as `(s + 2h)^n`.
inline constexpr u32_t kMaxBlurRadius = 32;

/// Largest halo any value may carry, set by the kernel interface: one byte per input slot.
///
/// An iterative op moves material one cell per iteration, so its influence radius *is* its iteration
/// count, and that radius is its halo. There is no way around it that keeps section borders
/// bit-identical: a section must compute every sample that can still affect its interior after the
/// last iteration. The cost is visible rather than hidden, and a job that asks for more than this is
/// refused with the arithmetic rather than quietly producing seams.
inline constexpr u32_t kMaxHalo = 255;

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
    /// Box blur over a declared radius.
    Blur,
    /// Gradient of a scalar field by central differences. The only op that is not analytic, for fields
    /// that have no closed form left to differentiate.
    Gradient,
    /// Thermal erosion: material slides downhill wherever the slope passes the talus angle.
    /// Iterative, one cell of influence per iteration.
    ThermalErosion,
    /// Hydraulic erosion: rain, flow, sediment transport and evaporation. Iterative, one cell of
    /// influence per iteration, carrying water and sediment between them.
    HydraulicErosion,
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

/// Which operand of an `Arith` node the compiler folded into the kernel as an immediate.
///
/// Set by the compiler, never by a script: a constant operand arrives as its own node, and folding it
/// in is what removes that node's dispatch and its section buffer.
enum class ArithImmediate : u32_t { None = 0, Left, Right };

/// Word of `Node::params` the folded immediate is stored in, as float bits.
inline constexpr usize_t kArithImmediateWord = 3;

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
        u32_t radius = 0;
        /// Times the kernel runs over a ping-ponged pair of buffers. One for everything that is not
        /// iterative.
        u32_t iterations = 1;
        u8_t  inputCount   = 0;
        u8_t  channelCount = 1;

        std::array<Value, kMaxNodeInputs>     inputs{};
        std::array<Mapping, kMaxNodeChannels> channels{};
        /// Raw parameter bits, copied straight into the kernel push constants.
        std::array<u32_t, kMaxNodeParams> params{};
        /// Shape of the state an iterative op carries between iterations, which is not always what the
        /// node produces: hydraulic erosion takes a height, carries height, water and sediment through
        /// the iterations, and hands back a height.
        ///
        /// Every iterative builder sets this, including the ones for which it equals channel 0. There
        /// is deliberately no "unset" value to test against: `Mapping{}` defaults to a perfectly valid
        /// `R2 -> R1`, so a sentinel check would silently accept it and give an `R3` node `R2` state
        /// buffers. That bug existed for exactly one commit and the seam test caught it.
        Mapping stateMapping{};

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

    struct ThermalParams {
        /// Iterations to run. Also the influence radius, and therefore the halo.
        u32_t iterations = 16;
        /// Slope, as a height difference per cell, below which nothing moves. The talus angle.
        f32_t talus = 0.02f;
        /// Fraction of the excess height moved per iteration, in (0, 0.5]. Above 0.5 the scheme
        /// oscillates instead of settling, because a cell can overshoot its neighbour.
        f32_t strength = 0.25f;
    };

    /// Thermal erosion of an `Rn -> R1` height field. Iterative: `iterations` dispatches over a
    /// ping-ponged pair of buffers, with a halo equal to the iteration count.
    [[nodiscard]] Result<Value> AddThermalErosion(Value input, const ThermalParams& params,
                                                 SourceLocation location);

    struct HydraulicParams {
        /// Iterations to run. Also the influence radius, and therefore the halo.
        u32_t iterations = 32;
        /// Water added to every cell per iteration.
        f32_t rain = 0.02f;
        /// Fraction of the water lost per iteration, in [0, 1).
        f32_t evaporation = 0.05f;
        /// Sediment a unit of flow can hold per unit of slope.
        f32_t capacity = 4.0f;
        /// How fast a shortfall in sediment is taken out of the bed, in (0, 1].
        f32_t erosionRate = 0.3f;
        /// How fast an excess is laid back down, in (0, 1].
        f32_t deposition = 0.3f;
        /// Fraction of a surface drop that moves per iteration, in (0, 0.25]. Above a quarter the four
        /// outflows of a cell can exceed the water it has, and the scheme stops being stable.
        f32_t flowRate = 0.15f;
    };

    /// Hydraulic erosion of an `Rn -> R1` height field, producing an `Rn -> R1` height.
    ///
    /// Water and sediment live only between iterations, in a three-component state the compiler
    /// allocates and nothing else can see. What comes out is terrain.
    [[nodiscard]] Result<Value> AddHydraulicErosion(Value input, const HydraulicParams& params,
                                                   SourceLocation location);

    /// Gradient of an `Rn -> R1` field by central differences, producing `Rn -> Rn`.
    ///
    /// For anything with an analytic derivative, use that instead: a noise node's `.gradient` channel is
    /// exact and comes out of the same evaluation as its value. This exists for a field that came out of
    /// blends, curves or erosion, where measuring is the only option left.
    [[nodiscard]] Result<Value> AddGradient(Value input, SourceLocation location);

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

/// A graph is large, because the spec asks for fixed-capacity storage so that building one never
/// allocates. The consequence is that it is a stack hazard: a few of them on one frame overflow the
/// default 1 MiB stack, which is how two tests crashed in Debug the moment they grew a sixth graph.
/// Keeping the number here means a future increase to `kMaxNodes` has to be a deliberate decision about
/// the stack as well as about memory.
static_assert(sizeof(Graph) <= 256 * 1024,
              "Graph is too large to keep several on one stack frame; either shrink a Node or stop "
              "putting more than one or two in a function");

} // namespace engine::terrain

#endif // IS_ENGINE
