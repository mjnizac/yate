#include <engine/terrain/graph.hpp>

#include <engine/assert.hpp>
#include <engine/log.hpp>

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
            "Noise frequency must be positive, got {}", params.frequency));
    }

    Node node;
    node.kind         = OpKind::Noise;
    node.nodeClass    = NodeClass::Pointwise;
    node.variant      = static_cast<u32_t>(params.kind);
    node.channelCount = 2;
    node.channels[0]  = Mapping{params.domain, 1};
    // The gradient has one component per axis: R2 -> R2, R3 -> R3.
    node.channels[1] = Mapping{params.domain, static_cast<u8_t>(params.domain == Domain::R3 ? 3 : 2)};

    PackFloat(node.params, 0, params.frequency);
    node.params[1] = params.octaves;
    PackFloat(node.params, 2, params.lacunarity);
    PackFloat(node.params, 3, params.gain);
    PackFloat(node.params, 4, params.amplitude);
    PackFloat(node.params, 5, params.offset);
    // params[6] is the sample spacing, filled by the evaluator from the job resolution.
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

Result<Value> Graph::AddArith(ArithOp op, Value a, Value b, SourceLocation location) {
    if (Status checked = CheckValue(a, "the first operand", location); !checked) {
        return std::unexpected(checked.error());
    }
    if (Status checked = CheckValue(b, "the second operand", location); !checked) {
        return std::unexpected(checked.error());
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
    return Append(node, location);
}

Result<Value> Graph::AddCurve(CurveOp op, Value input, f32_t a, f32_t b,
                              SourceLocation location) {
    if (Status checked = CheckValue(input, "Curve input", location); !checked) {
        return std::unexpected(checked.error());
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
