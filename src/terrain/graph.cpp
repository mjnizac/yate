#include <engine/terrain/graph.hpp>

#include <engine/assert.hpp>
#include <engine/terrain/kernels.hpp>
#include <engine/log.hpp>
#include <engine/memory/general.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::terrain {

const char* ToString(OpKind kind) noexcept {
    switch (kind) {
        case OpKind::Const: return "Const";
        case OpKind::Coords: return "Coords";
        case OpKind::Noise: return "Noise";
        case OpKind::Normals: return "Normals";
        case OpKind::Arith: return "Arith";
        case OpKind::Curve: return "Curve";
        case OpKind::Blend: return "Blend";
        case OpKind::SlopeMask: return "SlopeMask";
        case OpKind::Vector: return "Vector";
        case OpKind::Blur: return "Blur";
        case OpKind::Gradient: return "Gradient";
        case OpKind::ThermalErosion: return "ThermalErosion";
        case OpKind::HydraulicErosion: return "HydraulicErosion";
        case OpKind::Transform: return "Transform";
        case OpKind::Shape: return "Shape";
        case OpKind::Gullies: return "Gullies";
        case OpKind::Grid: return "Grid";
        case OpKind::Count: break;
    }
    return "<unknown op>";
}

const char* ToString(NodeClass nodeClass) noexcept {
    switch (nodeClass) {
        case NodeClass::Pointwise: return "pointwise";
        case NodeClass::Neighborhood: return "neighborhood";
        case NodeClass::Iterative: return "iterative";
    }
    return "<unknown class>";
}

namespace {

/// Packs a float into a parameter word. The kernels read them back with `uintBitsToFloat`.
void PackFloat(std::array<u32_t, kMaxNodeParams>& params, usize_t word, f32_t value) {
    std::memcpy(&params[word], &value, sizeof(f32_t));
}

/// Broadcast rule for pointwise binary ops: equal mappings pass through, and an `Rn -> R1`
/// operand spreads over an `Rn -> Rm` one (spec section 8).
[[nodiscard]] b8_t BroadcastMapping(Mapping a, Mapping b, Mapping& out) {
    if (a.domain != b.domain) {
        return false;
    }
    if (a.components == b.components) {
        out = a;
        return true;
    }
    if (a.components == 1) {
        out = b;
        return true;
    }
    if (b.components == 1) {
        out = a;
        return true;
    }
    return false;
}

} // namespace

void Graph::Clear() noexcept {
    m_nodeCount   = 0;
    m_outputCount = 0;
    m_gridData.clear();
    m_look = Look{};
}

const Graph::Node& Graph::NodeAt(u32_t index) const {
    ENGINE_ASSERT(index < m_nodeCount, "node index {} is out of range ({} nodes)", index,
                  m_nodeCount);
    return m_nodes[index < m_nodeCount ? index : 0];
}

Result<Value> Graph::Append(Node node, SourceLocation location) {
    if (m_nodeCount == kMaxNodes) {
        return std::unexpected(MakeScriptError(ErrorCode::OutOfMemory, ErrorStage::Script,
                                              location.file, location.line,
                                              "the graph already holds {} nodes", kMaxNodes));
    }
    node.location          = location;
    const u32_t index      = static_cast<u32_t>(m_nodeCount);
    m_nodes[m_nodeCount++] = node;
    return Value{.node = index, .channel = 0, .mapping = node.channels[0]};
}

Status Graph::CheckValue(Value value, const char* role, SourceLocation location) const {
    if (!value.IsValid() || value.node >= m_nodeCount) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "{} is not a value produced by this graph", role));
    }
    if (!IsValid(value.mapping)) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "{} has an invalid mapping", role));
    }
    return {};
}

Result<Value> Graph::Channel(Value value, u8_t channel) const {
    if (!value.IsValid() || value.node >= m_nodeCount) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Validation,
                    "channel {} requested from a value that is not in this graph", channel);
    }
    const Node& node = m_nodes[value.node];
    if (channel >= node.channelCount) {
        return std::unexpected(MakeScriptError(
            ErrorCode::NotFound, ErrorStage::Validation, node.location.file, node.location.line,
            "{} exposes {} channel(s), channel {} was requested", ToString(node.kind),
            node.channelCount, channel));
    }
    return Value{.node = value.node, .channel = channel, .mapping = node.channels[channel]};
}

// --- Builders -----------------------------------------------------------------------------------

Result<Value> Graph::AddConst(Mapping mapping, std::array<f32_t, 4> components,
                              SourceLocation location) {
    if (!IsValid(mapping)) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "Const was given an invalid mapping"));
    }
    Node node;
    node.kind        = OpKind::Const;
    node.nodeClass   = NodeClass::Pointwise;
    node.channelCount = 1;
    node.channels[0] = mapping;
    for (usize_t i = 0; i < 4; ++i) {
        PackFloat(node.params, i, components[i]);
    }
    node.params[4] = mapping.components;
    return Append(node, location);
}

Result<Value> Graph::AddCoords(Domain domain, SourceLocation location) {
    Node node;
    node.kind         = OpKind::Coords;
    node.nodeClass    = NodeClass::Pointwise;
    node.channelCount = 1;
    // Coords produces one component per axis of its domain.
    node.channels[0] = Mapping{domain, static_cast<u8_t>(domain == Domain::R3 ? 3 : 2)};
    return Append(node, location);
}

Result<Value> Graph::AddNoise(const NoiseParams& params, SourceLocation location) {
    if (params.octaves == 0) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "Noise needs at least one octave"));
    }
    if (params.frequency <= 0.0f) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Noise frequency must be strictly greater than zero, got {}", params.frequency));
    }
    if (params.persistence <= 0.0f) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Noise persistence must be strictly greater than zero, got {}", params.persistence));
    }
    if (params.lacunarity <= 0.0f) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Noise lacunarity must be strictly greater than zero, got {}", params.lacunarity));
    }

    if (params.kind == NoiseKind::Cellular && params.domain == Domain::R3) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "cellular noise is 2D only; drop 'domain = 3'"));
    }
    const b8_t warped = params.position.IsValid();
    if (warped) {
        if (Status checked = CheckValue(params.position, "Noise position", location); !checked) {
            return std::unexpected(checked.error());
        }
        const Mapping wanted{params.domain, static_cast<u8_t>(params.domain == Domain::R3 ? 3 : 2)};
        if (!(params.position.mapping == wanted)) {
            return std::unexpected(MakeScriptError(
                ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
                "a {} noise expects an {} position field, got {}",
                params.domain == Domain::R3 ? "3D" : "2D", ToString(wanted),
                ToString(params.position.mapping)));
        }
    }

    Node node;
    node.kind      = OpKind::Noise;
    node.nodeClass = NodeClass::Pointwise;
    // Three variant axes: the base function, whether the octave sum is normalized, and whether a
    // position field is read.
    node.variant      = PackVariant(static_cast<u32_t>(params.kind), params.normalize ? 1u : 0u,
                                    warped ? 1u : 0u);
    if (warped) {
        node.inputCount = 1;
        node.inputs[0]  = params.position;
    }
    node.channelCount = 2;
    node.channels[0]  = Mapping{params.domain, 1};
    // The gradient has one component per axis: R2 -> R2, R3 -> R3.
    node.channels[1] = Mapping{params.domain, static_cast<u8_t>(params.domain == Domain::R3 ? 3 : 2)};

    PackFloat(node.params, 0, params.frequency);
    node.params[1] = params.octaves;
    PackFloat(node.params, 2, params.lacunarity);
    PackFloat(node.params, 3, params.persistence);
    PackFloat(node.params, 4, params.amplitude);
    PackFloat(node.params, 5, params.offset);
    // params[6] is the sample spacing, filled by the compiler from the job resolution.
    node.params[7] = params.seedSalt;
    return Append(node, location);
}

Result<Value> Graph::AddNormals(Value gradient, f32_t verticalScale, SourceLocation location) {
    if (Status checked = CheckValue(gradient, "Normals input", location); !checked) {
        return std::unexpected(checked.error());
    }
    const Mapping wanted{Domain::R2, 2};
    if (!(gradient.mapping == wanted)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Normals expects {} input, got {}", ToString(wanted), ToString(gradient.mapping)));
    }

    Node node;
    node.kind         = OpKind::Normals;
    node.nodeClass    = NodeClass::Pointwise;
    node.inputCount   = 1;
    node.inputs[0]    = gradient;
    node.channelCount = 1;
    node.channels[0]  = Mapping{Domain::R2, 3};
    PackFloat(node.params, 0, verticalScale == 0.0f ? 1.0f : verticalScale);
    return Append(node, location);
}

Result<Value> Graph::AddArith(ArithOp op, Value a, Value b, SourceLocation location,
                              f32_t smooth) {
    if (Status checked = CheckValue(a, "the first operand", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (Status checked = CheckValue(b, "the second operand", location); !checked) {
        return std::unexpected(checked.error());
    }
    const b8_t isSmooth = op == ArithOp::SmoothMinimum || op == ArithOp::SmoothMaximum;
    if (isSmooth && !(smooth > 0.0f)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "a smooth minimum or maximum needs a blend width greater than zero, got {}", smooth));
    }

    Mapping result{};
    if (!BroadcastMapping(a.mapping, b.mapping, result)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "{} cannot combine {} with {}; operands must share a domain and either match in "
            "component count or have one scalar side",
            ToString(OpKind::Arith), ToString(a.mapping), ToString(b.mapping)));
    }

    Node node;
    node.kind         = OpKind::Arith;
    node.nodeClass    = NodeClass::Pointwise;
    node.variant      = static_cast<u32_t>(op);
    node.inputCount   = 2;
    node.inputs[0]    = a;
    node.inputs[1]    = b;
    node.channelCount = 1;
    node.channels[0]  = result;
    node.params[0]    = a.mapping.components;
    node.params[1]    = b.mapping.components;
    node.params[2]    = result.components;
    if (isSmooth) {
        PackFloat(node.params, kArithSmoothWord, smooth);
    }
    return Append(node, location);
}

Result<Value> Graph::AddCurve(CurveOp op, Value input, f32_t a, f32_t b,
                              SourceLocation location) {
    if (Status checked = CheckValue(input, "Curve input", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (const b8_t profile = op == CurveOp::ProfileLinear || op == CurveOp::ProfileSmooth
                             || op == CurveOp::ProfileDome || op == CurveOp::ProfileGaussian;
        profile && !(a > 0.0f)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "a profile needs a width greater than zero, got {}", a));
    }
    if (op == CurveOp::ProfilePlateau && !(a >= 0.0f && b >= 0.0f && a + b > 0.0f)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "a plateau needs a non-negative edge and falloff that are not both zero, got {} and {}",
            a, b));
    }
    if (op == CurveOp::Terrace && !(a > 0.0f && b >= 0.0f && b <= 1.0f)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "a terrace needs a step greater than zero and a sharpness in [0, 1], got {} and {}", a,
            b));
    }
    if (op == CurveOp::Remap && a == b) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "Remap needs a non-empty output range"));
    }
    if (op == CurveOp::Smoothstep && a == b) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "Smoothstep needs a non-empty edge range"));
    }

    Node node;
    node.kind         = OpKind::Curve;
    node.nodeClass    = NodeClass::Pointwise;
    node.variant      = static_cast<u32_t>(op);
    node.inputCount   = 1;
    node.inputs[0]    = input;
    node.channelCount = 1;
    node.channels[0]  = input.mapping;
    PackFloat(node.params, 0, a);
    PackFloat(node.params, 1, b);
    node.params[2] = input.mapping.components;
    return Append(node, location);
}

Result<Value> Graph::AddBlend(Value a, Value b, Value mask, SourceLocation location) {
    if (Status checked = CheckValue(a, "Blend a", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (Status checked = CheckValue(b, "Blend b", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (Status checked = CheckValue(mask, "Blend mask", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (!(a.mapping == b.mapping)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Blend expects a and b to share a mapping, got {} and {}", ToString(a.mapping),
            ToString(b.mapping)));
    }
    const Mapping wantedMask{a.mapping.domain, 1};
    if (!(mask.mapping == wantedMask)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Blend expects a {} mask, got {}", ToString(wantedMask), ToString(mask.mapping)));
    }

    Node node;
    node.kind         = OpKind::Blend;
    node.nodeClass    = NodeClass::Pointwise;
    node.inputCount   = 3;
    node.inputs[0]    = a;
    node.inputs[1]    = b;
    node.inputs[2]    = mask;
    node.channelCount = 1;
    node.channels[0]  = a.mapping;
    node.params[0]    = a.mapping.components;
    return Append(node, location);
}

Result<Value> Graph::AddSlopeMask(Value gradient, f32_t minSlope, f32_t maxSlope,
                                  SourceLocation location) {
    if (Status checked = CheckValue(gradient, "SlopeMask input", location); !checked) {
        return std::unexpected(checked.error());
    }
    const Mapping wanted{Domain::R2, 2};
    if (!(gradient.mapping == wanted)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "SlopeMask expects {} input, got {}", ToString(wanted), ToString(gradient.mapping)));
    }
    if (maxSlope <= minSlope) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "SlopeMask needs max greater than min, got {} and {}", minSlope, maxSlope));
    }

    Node node;
    node.kind         = OpKind::SlopeMask;
    node.nodeClass    = NodeClass::Pointwise;
    node.inputCount   = 1;
    node.inputs[0]    = gradient;
    node.channelCount = 1;
    node.channels[0]  = Mapping{Domain::R2, 1};
    PackFloat(node.params, 0, minSlope);
    PackFloat(node.params, 1, maxSlope);
    return Append(node, location);
}

Result<Value> Graph::AddGrid(Value position, std::span<const f32_t> values, const GridParams& params,
                            SourceLocation location) {
    // 16 M values, 64 MiB: far beyond any drainage grid, and a typo that asks for more is caught here.
    constexpr u64_t kMaxGridValues = 16ull << 20;
    if (Status checked = CheckValue(position, "Grid position", location); !checked) {
        return std::unexpected(checked.error());
    }
    const Mapping planar{Domain::R2, 2};
    if (!(position.mapping == planar)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Grid position must be {}, got {}", ToString(planar), ToString(position.mapping)));
    }
    const u64_t count = static_cast<u64_t>(params.width) * params.depth;
    if (params.width < 2 || params.depth < 2 || count != values.size()
        || m_gridData.size() + count > kMaxGridValues) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Grid needs at least 2 x 2 values and exactly width x depth of them ({} x {} = {}), got "
            "{}; all grids together hold at most {}",
            params.width, params.depth, count, values.size(), kMaxGridValues));
    }
    if (!(params.cell > 0.0f) || !std::isfinite(params.originX) || !std::isfinite(params.originZ)
        || !std::all_of(values.begin(), values.end(), [](f32_t v) { return std::isfinite(v); })) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "Grid needs a cell greater than zero and finite values"));
    }

    if (m_gridData.empty()) {
        m_gridData = std::pmr::vector<f32_t>(&memory::General().Resource());
    }
    const u32_t offset = static_cast<u32_t>(m_gridData.size());
    m_gridData.insert(m_gridData.end(), values.begin(), values.end());

    Node node;
    node.kind         = OpKind::Grid;
    node.nodeClass    = NodeClass::Pointwise;
    node.inputCount   = 1;
    node.inputs[0]    = position;
    node.channelCount = 1;
    node.channels[0]  = Mapping{Domain::R2, 1};
    node.params[0]    = offset;
    node.params[1]    = params.width;
    node.params[2]    = params.depth;
    PackFloat(node.params, 3, params.originX);
    PackFloat(node.params, 4, params.originZ);
    PackFloat(node.params, 5, params.cell);
    node.params[6] = params.wrap ? 1u : 0u;
    return Append(node, location);
}

Result<Value> Graph::AddGullies(Value height, Value gradient, Value position,
                               const GulliesParams& params, SourceLocation location) {
    const Mapping scalar{Domain::R2, 1};
    const Mapping planar{Domain::R2, 2};
    const struct {
        Value       value;
        Mapping     wanted;
        const char* role;
    } inputs[] = {{height, scalar, "Gullies input"},
                  {gradient, planar, "Gullies gradient"},
                  {position, planar, "Gullies position"}};
    for (const auto& input : inputs) {
        if (Status checked = CheckValue(input.value, input.role, location); !checked) {
            return std::unexpected(checked.error());
        }
        if (!(input.value.mapping == input.wanted)) {
            return std::unexpected(MakeScriptError(
                ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
                "{} must be {}, got {}", input.role, ToString(input.wanted),
                ToString(input.value.mapping)));
        }
    }
    if (!(params.cell > 0.0f) || !(params.full > 0.0f) || !(params.gain > 0.0f)
        || !(params.lacunarity > 0.0f) || !std::isfinite(params.strength) || params.octaves == 0) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Gullies needs cell, full, gain and lacunarity greater than zero, a finite strength and "
            "at least one octave"));
    }

    Node node;
    node.kind         = OpKind::Gullies;
    node.nodeClass    = NodeClass::Pointwise;
    node.inputCount   = 3;
    node.inputs[0]    = height;
    node.inputs[1]    = gradient;
    node.inputs[2]    = position;
    node.channelCount = 1;
    node.channels[0]  = scalar;
    PackFloat(node.params, 0, params.cell);
    node.params[1] = params.octaves;
    PackFloat(node.params, 2, params.strength);
    PackFloat(node.params, 3, params.full);
    PackFloat(node.params, 4, params.gain);
    PackFloat(node.params, 5, params.lacunarity);
    node.params[6] = params.seedSalt;
    return Append(node, location);
}

Result<Value> Graph::AddTransform(Value position, const std::array<f32_t, 6>& matrix,
                                  SourceLocation location) {
    if (Status checked = CheckValue(position, "Transform input", location); !checked) {
        return std::unexpected(checked.error());
    }
    const Mapping wanted{Domain::R2, 2};
    if (!(position.mapping == wanted)) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Transform expects an {} position field, got {}; shapes live in the (x, z) plane",
            ToString(wanted), ToString(position.mapping)));
    }
    for (const f32_t entry : matrix) {
        if (!std::isfinite(entry)) {
            return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument,
                                                  ErrorStage::Validation, location.file,
                                                  location.line,
                                                  "Transform has a non-finite entry"));
        }
    }

    Node node;
    node.kind         = OpKind::Transform;
    node.nodeClass    = NodeClass::Pointwise;
    node.inputCount   = 1;
    node.inputs[0]    = position;
    node.channelCount = 1;
    node.channels[0]  = wanted;
    for (usize_t i = 0; i < matrix.size(); ++i) {
        PackFloat(node.params, i, matrix[i]);
    }
    return Append(node, location);
}

Result<Value> Graph::AddShape(ShapeKind kind, Value position, Value accumulator,
                              const ShapeParams& params, SourceLocation location) {
    const b8_t resolves = kind == ShapeKind::ResolvePolygon || kind == ShapeKind::ResolvePolyline;
    const b8_t chained  = kind == ShapeKind::SegmentsNext;
    const b8_t segments = kind == ShapeKind::SegmentsFirst || chained;
    const Mapping wanted{Domain::R2, 2};

    if (!resolves) {
        if (Status checked = CheckValue(position, "Shape position", location); !checked) {
            return std::unexpected(checked.error());
        }
        if (!(position.mapping == wanted)) {
            return std::unexpected(MakeScriptError(
                ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
                "a shape expects an {} position field, got {}; shapes live in the (x, z) plane",
                ToString(wanted), ToString(position.mapping)));
        }
    }
    if (resolves || chained) {
        if (Status checked = CheckValue(accumulator, "the segment chain", location); !checked) {
            return std::unexpected(checked.error());
        }
        if (!(accumulator.mapping == wanted)) {
            return std::unexpected(MakeScriptError(
                ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
                "the segment chain must be {}, got {}", ToString(wanted),
                ToString(accumulator.mapping)));
        }
    }
    for (const f32_t entry : params.values) {
        if (!std::isfinite(entry)) {
            return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument,
                                                  ErrorStage::Validation, location.file,
                                                  location.line,
                                                  "a shape has a non-finite parameter"));
        }
    }

    const auto reject = [&](const char* what) -> Result<Value> {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line, "{}", what));
    };
    switch (kind) {
        case ShapeKind::Circle:
            if (!(params.values[0] >= 0.0f)) {
                return reject("a circle needs a radius of at least zero");
            }
            break;
        case ShapeKind::Ellipse:
            if (!(params.values[0] > 0.0f && params.values[1] > 0.0f)) {
                return reject("an ellipse needs both radii greater than zero");
            }
            break;
        case ShapeKind::Box:
            if (!(params.values[0] > 0.0f && params.values[1] > 0.0f)) {
                return reject("a box needs a size greater than zero on both axes");
            }
            if (!(params.values[2] >= 0.0f
                  && params.values[2] <= std::min(params.values[0], params.values[1]))) {
                return reject("a box's rounding must be between zero and its smaller half-size");
            }
            break;
        case ShapeKind::BoxChebyshev:
            if (!(params.values[0] > 0.0f && params.values[1] > 0.0f)) {
                return reject("a box needs a size greater than zero on both axes");
            }
            break;
        case ShapeKind::SegmentsFirst:
        case ShapeKind::SegmentsNext:
            if (params.segments < 1 || params.segments > 2) {
                return reject("a segment node holds one or two segments");
            }
            break;
        case ShapeKind::ResolvePolygon:
        case ShapeKind::ResolvePolyline:
            break;
    }

    Node node;
    node.kind       = OpKind::Shape;
    node.nodeClass  = NodeClass::Pointwise;
    // Two variant fields: which shape, and how many segments this node folds in. The count is not
    // a parameter word because the compiler writes words 8 and 9 of every node with the iteration
    // count and index, and a pointwise op's tenth word would be zeroed.
    node.variant    = PackVariant(static_cast<u32_t>(kind), params.segments);
    node.channelCount = 1;
    node.channels[0]  = segments ? wanted : Mapping{Domain::R2, 1};
    if (resolves) {
        node.inputCount = 1;
        node.inputs[0]  = accumulator;
    } else {
        node.inputCount = chained ? 2 : 1;
        node.inputs[0]  = position;
        if (chained) {
            node.inputs[1] = accumulator;
        }
    }
    for (usize_t i = 0; i < params.values.size(); ++i) {
        PackFloat(node.params, i, params.values[i]);
    }
    return Append(node, location);
}

Result<Value> Graph::AddExtract(Value input, u8_t component, SourceLocation location) {
    if (Status checked = CheckValue(input, "the extracted value", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (component >= input.mapping.components) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "{} has {} component(s), component {} was requested", ToString(input.mapping),
            input.mapping.components, component));
    }

    Node node;
    node.kind         = OpKind::Vector;
    node.nodeClass    = NodeClass::Pointwise;
    node.variant      = static_cast<u32_t>(VectorOp::Extract);
    node.inputCount   = 1;
    node.inputs[0]    = input;
    node.channelCount = 1;
    node.channels[0]  = Mapping{input.mapping.domain, 1};
    node.params[0]    = component;
    node.params[1]    = input.mapping.components;
    return Append(node, location);
}

Result<Value> Graph::AddBlur(Value input, u32_t radius, SourceLocation location) {
    if (Status checked = CheckValue(input, "Blur input", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (radius == 0 || radius > kMaxBlurRadius) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Blur radius must be between 1 and {}, got {}", kMaxBlurRadius, radius));
    }

    Node node;
    node.kind      = OpKind::Blur;
    node.nodeClass = NodeClass::Neighborhood;
    // The radius is what halo propagation adds to every producer upstream of this node.
    node.radius       = radius;
    node.inputCount   = 1;
    node.inputs[0]    = input;
    node.channelCount = 1;
    node.channels[0]  = input.mapping;
    node.params[0]    = radius;
    node.params[1]    = input.mapping.components;
    return Append(node, location);
}

/// Shared by both erosion builders: an iteration count that is also a halo, and a height field.
[[nodiscard]] Status CheckErosionInput(OpKind kind, Value input, u32_t iterations,
                                       SourceLocation location) {
    if (input.mapping.components != 1) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "{} expects an Rn->R1 height field, got {}", ToString(kind),
            ToString(input.mapping)));
    }
    if (iterations == 0 || iterations > kMaxHalo) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "{} expects iterations in [1, {}], got {}; the iteration count is the influence radius "
            "and therefore the halo, so a section would have to compute {} extra samples on each side",
            ToString(kind), kMaxHalo, iterations, iterations));
    }
    return {};
}

Result<Value> Graph::AddGradient(Value input, SourceLocation location) {
    if (Status checked = CheckValue(input, "Gradient input", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (input.mapping.components != 1) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "{} expects an Rn->R1 field, got {}", ToString(OpKind::Gradient),
            ToString(input.mapping)));
    }

    Node node;
    node.kind         = OpKind::Gradient;
    node.nodeClass    = NodeClass::Neighborhood;
    node.radius       = 1;
    node.inputCount   = 1;
    node.inputs[0]    = input;
    node.channelCount = 1;
    // One component per axis of the domain, which is what makes the result usable by Normals and
    // SlopeMask without a conversion.
    node.channels[0] = Mapping{input.mapping.domain,
                               static_cast<u8_t>(input.mapping.domain == Domain::R3 ? 3 : 2)};
    return Append(node, location);
}

Result<Value> Graph::AddThermalErosion(Value input, const ThermalParams& params,
                                       SourceLocation location) {
    if (Status checked = CheckValue(input, "ThermalErosion input", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (Status checked =
            CheckErosionInput(OpKind::ThermalErosion, input, params.iterations, location);
        !checked) {
        return std::unexpected(checked.error());
    }
    if (!(params.talus >= 0.0f)) {
        return std::unexpected(MakeScriptError(ErrorCode::InvalidArgument, ErrorStage::Validation,
                                              location.file, location.line,
                                              "{} expects a talus of at least 0, got {}",
                                              ToString(OpKind::ThermalErosion), params.talus));
    }
    // Above one half a cell can give away more than the difference to its neighbour, which makes the
    // scheme oscillate instead of settle. That is a mistake worth naming, not a style of erosion.
    if (!(params.strength > 0.0f) || params.strength > 0.5f) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "{} expects a strength in (0, 0.5], got {}; above one half the scheme oscillates instead "
            "of settling",
            ToString(OpKind::ThermalErosion), params.strength));
    }

    Node node;
    node.kind         = OpKind::ThermalErosion;
    node.nodeClass    = NodeClass::Iterative;
    node.iterations   = params.iterations;
    node.radius       = params.iterations;
    node.inputCount   = 1;
    node.inputs[0]    = input;
    node.channelCount = 1;
    node.channels[0]  = input.mapping;
    // The state is just the height, but it is stated rather than inferred: see Node::stateMapping.
    node.stateMapping = input.mapping;
    std::memcpy(&node.params[0], &params.talus, sizeof(f32_t));
    std::memcpy(&node.params[1], &params.strength, sizeof(f32_t));
    return Append(node, location);
}

Result<Value> Graph::AddHydraulicErosion(Value input, const HydraulicParams& params,
                                         SourceLocation location) {
    if (Status checked = CheckValue(input, "HydraulicErosion input", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (Status checked =
            CheckErosionInput(OpKind::HydraulicErosion, input, params.iterations, location);
        !checked) {
        return std::unexpected(checked.error());
    }

    // Each bound is a stability condition rather than a matter of taste, so each one says why.
    struct Range {
        const char* name;
        f32_t       value;
        f32_t       minimum;
        f32_t       maximum;
        const char* why;
    };
    const Range ranges[] = {
        {"rain", params.rain, 0.0f, 1e6f, "water cannot be negative"},
        {"evaporation", params.evaporation, 0.0f, 0.999f,
         "evaporating everything would leave nothing to carry sediment"},
        {"capacity", params.capacity, 0.0f, 1e6f, "a negative capacity would deposit without limit"},
        {"erosion_rate", params.erosionRate, 0.0f, 1.0f,
         "above one an iteration would cut deeper than the sediment deficit"},
        {"deposition", params.deposition, 0.0f, 1.0f,
         "above one an iteration would lay down more than is suspended"},
        {"flow_rate", params.flowRate, 1e-6f, 0.25f,
         "above a quarter the four outflows of a cell can exceed the water it has"},
    };
    for (const Range& range : ranges) {
        if (!(range.value >= range.minimum) || !(range.value <= range.maximum)) {
            return std::unexpected(MakeScriptError(
                ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
                "{} expects {} in [{}, {}], got {}: {}", ToString(OpKind::HydraulicErosion),
                range.name, range.minimum, range.maximum, range.value, range.why));
        }
    }

    Node node;
    node.kind         = OpKind::HydraulicErosion;
    node.nodeClass    = NodeClass::Iterative;
    node.iterations   = params.iterations;
    node.radius       = params.iterations;
    node.inputCount   = 1;
    node.inputs[0]    = input;
    node.channelCount = 2;
    node.channels[0]  = input.mapping;
    // Water and sediment as they stood after the last iteration. Two components rather than three,
    // because the height is channel 0 and repeating it would be a second copy to keep in step.
    node.channels[1] = Mapping{input.mapping.domain, 2};
    // Height, water and sediment, carried between iterations and invisible to the rest of the graph.
    node.stateMapping = Mapping{input.mapping.domain, 3};
    std::memcpy(&node.params[0], &params.rain, sizeof(f32_t));
    std::memcpy(&node.params[1], &params.evaporation, sizeof(f32_t));
    std::memcpy(&node.params[2], &params.capacity, sizeof(f32_t));
    std::memcpy(&node.params[3], &params.erosionRate, sizeof(f32_t));
    std::memcpy(&node.params[4], &params.deposition, sizeof(f32_t));
    std::memcpy(&node.params[5], &params.flowRate, sizeof(f32_t));
    return Append(node, location);
}

Result<Value> Graph::AddCombine(const Value* components, u8_t count, SourceLocation location) {
    if (components == nullptr || count < 2 || count > kMaxComponents) {
        return std::unexpected(MakeScriptError(
            ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
            "Combine takes 2 to {} components, got {}", kMaxComponents, count));
    }
    if (count > kMaxNodeInputs) {
        return std::unexpected(MakeScriptError(
            ErrorCode::Unsupported, ErrorStage::Validation, location.file, location.line,
            "Combine can bind at most {} inputs", kMaxNodeInputs));
    }

    Node node;
    node.kind       = OpKind::Vector;
    node.nodeClass  = NodeClass::Pointwise;
    node.variant    = static_cast<u32_t>(VectorOp::Combine);
    node.inputCount = count;
    for (u8_t i = 0; i < count; ++i) {
        if (Status checked = CheckValue(components[i], "a Combine component", location);
            !checked) {
            return std::unexpected(checked.error());
        }
        const Mapping wanted{components[0].mapping.domain, 1};
        if (!(components[i].mapping == wanted)) {
            return std::unexpected(MakeScriptError(
                ErrorCode::InvalidArgument, ErrorStage::Validation, location.file, location.line,
                "Combine expects {} components, component {} is {}", ToString(wanted), i,
                ToString(components[i].mapping)));
        }
        node.inputs[i] = components[i];
    }
    node.channelCount = 1;
    node.channels[0]  = Mapping{components[0].mapping.domain, count};
    node.params[0]    = count;
    return Append(node, location);
}

Status Graph::RequestOutput(std::string_view name, Value value, f32_t rangeMin, f32_t rangeMax) {
    if (m_outputCount == kMaxOutputs) {
        ENGINE_FAIL(ErrorCode::OutOfMemory, ErrorStage::Validation,
                    "a graph can request at most {} outputs", kMaxOutputs);
    }
    if (!value.IsValid() || value.node >= m_nodeCount) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Validation,
                    "output {} is not a value produced by this graph", name);
    }
    if (name.empty()) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Validation, "an output needs a name");
    }
    for (usize_t i = 0; i < m_outputCount; ++i) {
        if (std::string_view{m_outputs[i].name.data()} == name) {
            ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Validation,
                        "output {} was requested twice", name);
        }
    }
    if (value.mapping.components == 1 && rangeMax <= rangeMin) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Validation,
                    "output {} declares an empty range [{}, {}]", name, rangeMin, rangeMax);
    }

    // The name becomes a file stem, so a name cut short would collide with another output's or write
    // a file nobody asked for.
    if (name.size() >= OutputRequest::kMaxName) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Validation,
                    "output name '{}' is {} characters; the limit is {}", name, name.size(),
                    OutputRequest::kMaxName - 1);
    }

    OutputRequest& request = m_outputs[m_outputCount++];
    detail::CopyBounded(request.name, name);
    request.value    = value;
    request.rangeMin = rangeMin;
    request.rangeMax = rangeMax;
    return {};
}

const OutputRequest& Graph::Output(usize_t index) const {
    ENGINE_ASSERT(index < m_outputCount, "output index {} is out of range ({} outputs)", index,
                  m_outputCount);
    return m_outputs[index < m_outputCount ? index : 0];
}

} // namespace engine::terrain
