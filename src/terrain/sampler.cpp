#include <engine/terrain/sampler.hpp>

#include <engine/memory/general.hpp>
#include <engine/terrain/kernels.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace engine::terrain {

namespace {

/// Both channels of a node at one sample.
using Sample = std::array<std::array<f32_t, kMaxComponents>, kMaxNodeChannels>;

/// Points evaluated together: every needed node holds one `Sample` per point of a chunk.
constexpr usize_t kChunk = 1024;

/// The same seed the noise kernel builds from the job seed and the node's salt (`fbm.comp.glsl`).
constexpr u32_t kHashPhi = 0x9E3779B9u;

[[nodiscard]] f32_t Word(const Graph::Node& node, usize_t word) { return std::bit_cast<f32_t>(node.params[word]); }

[[nodiscard]] f32_t CatmullRom(f32_t a, f32_t b, f32_t c, f32_t d, f32_t t) {
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}

/// `grid.comp.glsl` on the CPU: Catmull-Rom over the grid, the edge held or wrapped.
[[nodiscard]] f32_t SampleGrid(const Graph& graph, const Graph::Node& node, f32_t px, f32_t pz) {
    const std::span<const f32_t> data   = graph.GridData();
    const u32_t                  offset = node.params[0];
    const i32_t                  width  = static_cast<i32_t>(node.params[1]);
    const i32_t                  depth  = static_cast<i32_t>(node.params[2]);
    const b8_t                   wrap   = node.params[6] != 0;
    f32_t gx = (px - Word(node, 3)) / Word(node, 5);
    f32_t gz = (pz - Word(node, 4)) / Word(node, 5);
    if (!wrap) {
        gx = std::clamp(gx, 0.0f, static_cast<f32_t>(width - 1));
        gz = std::clamp(gz, 0.0f, static_cast<f32_t>(depth - 1));
    }
    const i32_t ix = static_cast<i32_t>(std::floor(gx));
    const i32_t iz = static_cast<i32_t>(std::floor(gz));
    const auto  at = [&](i32_t x, i32_t z) {
        if (wrap) {
            x = ((x % width) + width) % width;
            z = ((z % depth) + depth) % depth;
        }
        x = std::clamp(x, 0, width - 1);
        z = std::clamp(z, 0, depth - 1);
        return data[offset + static_cast<usize_t>(z) * static_cast<usize_t>(width) + static_cast<usize_t>(x)];
    };
    std::array<f32_t, 4> rows{};
    for (i32_t j = -1; j <= 2; ++j) {
        rows[static_cast<usize_t>(j + 1)] =
            CatmullRom(at(ix - 1, iz + j), at(ix, iz + j), at(ix + 1, iz + j), at(ix + 2, iz + j), gx - static_cast<f32_t>(ix));
    }
    return CatmullRom(rows[0], rows[1], rows[2], rows[3], gz - static_cast<f32_t>(iz));
}

class Sampler {
public:
    Sampler(const Graph& graph, u32_t seed, f32_t spacing) : m_graph(graph), m_seed(seed), m_spacing(spacing) {}

    /// Channel `value.channel` of `value`, component 0, at each of `positions` (at most `kChunk`).
    Status Run(Value value, std::span<const std::array<f32_t, 2>> positions, std::span<f32_t> out) {
        std::pmr::memory_resource* resource = &memory::General().Resource();
        const usize_t              count    = positions.size();
        // Which nodes the value depends on: inputs always come before the node that reads them.
        std::pmr::vector<u8_t> needed(value.node + 1, 0, resource);
        needed[value.node] = 1;
        for (u32_t n = value.node + 1; n-- > 0;) {
            if (needed[n] == 0) {
                continue;
            }
            const Graph::Node& node = m_graph.NodeAt(n);
            if (node.kind == OpKind::Gradient) {
                continue; // evaluated on its own, at shifted positions
            }
            for (u8_t i = 0; i < node.inputCount; ++i) {
                needed[node.inputs[i].node] = 1;
            }
        }
        std::pmr::vector<std::pmr::vector<Sample>> values(value.node + 1, resource);
        for (u32_t n = 0; n <= value.node; ++n) {
            if (needed[n] == 0) {
                continue;
            }
            values[n].assign(count, Sample{});
            if (Status evaluated = Node(n, positions, values); !evaluated) {
                return evaluated;
            }
        }
        for (usize_t p = 0; p < count; ++p) {
            out[p] = values[value.node][p][value.channel][0];
        }
        return {};
    }

private:
    Status Node(u32_t n, std::span<const std::array<f32_t, 2>> positions,
                std::pmr::vector<std::pmr::vector<Sample>>& values) {
        const Graph::Node& node  = m_graph.NodeAt(n);
        std::pmr::vector<Sample>& own = values[n];
        const auto input = [&](u8_t i, usize_t p) -> const std::array<f32_t, kMaxComponents>& {
            return values[node.inputs[i].node][p][node.inputs[i].channel];
        };
        if (node.channels[0].domain != Domain::R2) {
            ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Script,
                        "only R2 values can be sampled on the CPU; {} is {}", ToString(node.kind),
                        ToString(node.channels[0]));
        }
        switch (node.kind) {
            case OpKind::Coords:
                for (usize_t p = 0; p < positions.size(); ++p) {
                    own[p][0][0] = positions[p][0];
                    own[p][0][1] = positions[p][1];
                }
                return {};
            case OpKind::Noise: {
                Graph::NoiseParams params;
                params.kind        = static_cast<NoiseKind>(node.variant & kVariantFieldMask);
                params.normalize   = ((node.variant >> kVariantFieldBits) & kVariantFieldMask) != 0;
                params.domain      = Domain::R2;
                params.frequency   = Word(node, 0);
                params.octaves     = node.params[1];
                params.lacunarity  = Word(node, 2);
                params.persistence = Word(node, 3);
                params.amplitude   = Word(node, 4);
                params.offset      = Word(node, 5);
                const u32_t seed   = m_seed ^ (node.params[7] * kHashPhi);
                for (usize_t p = 0; p < positions.size(); ++p) {
                    const b8_t  warped = node.inputCount == 1;
                    const f32_t x      = warped ? input(0, p)[0] : positions[p][0];
                    const f32_t z      = warped ? input(0, p)[1] : positions[p][1];
                    const NoiseSample s = EvalNoise(params, seed, {x, 0.0f, z});
                    own[p][0][0]        = s.value;
                    own[p][1][0]        = s.gradient[0];
                    own[p][1][1]        = s.gradient[1];
                }
                return {};
            }
            case OpKind::Grid:
                for (usize_t p = 0; p < positions.size(); ++p) {
                    own[p][0][0] = SampleGrid(m_graph, node, input(0, p)[0], input(0, p)[1]);
                }
                return {};
            case OpKind::Gradient: {
                // Central differences of the input, which is evaluated again at four shifted positions.
                std::pmr::vector<std::array<f32_t, 2>> shifted(positions.size(), &memory::General().Resource());
                std::pmr::vector<f32_t>                ahead(positions.size(), &memory::General().Resource());
                std::pmr::vector<f32_t>                behind(positions.size(), &memory::General().Resource());
                for (usize_t axis = 0; axis < 2; ++axis) {
                    for (i32_t side = 0; side < 2; ++side) {
                        for (usize_t p = 0; p < positions.size(); ++p) {
                            shifted[p] = positions[p];
                            shifted[p][axis] += side == 0 ? m_spacing : -m_spacing;
                        }
                        if (Status ran = Run(node.inputs[0], shifted, side == 0 ? std::span<f32_t>(ahead) : std::span<f32_t>(behind)); !ran) {
                            return ran;
                        }
                    }
                    for (usize_t p = 0; p < positions.size(); ++p) {
                        own[p][0][axis] = (ahead[p] - behind[p]) / (2.0f * m_spacing);
                    }
                }
                return {};
            }
            case OpKind::Blur:
            case OpKind::ThermalErosion:
            case OpKind::HydraulicErosion:
            case OpKind::Gullies:
                for (usize_t p = 0; p < positions.size(); ++p) {
                    own[p][0] = input(0, p);
                }
                return {};
            default: {
                std::array<std::array<f32_t, kMaxComponents>, kMaxNodeInputs> inputs{};
                std::array<u8_t, kMaxNodeInputs>                               components{};
                for (u8_t i = 0; i < node.inputCount; ++i) {
                    components[i] = node.inputs[i].mapping.components;
                }
                for (usize_t p = 0; p < positions.size(); ++p) {
                    for (u8_t i = 0; i < node.inputCount; ++i) {
                        inputs[i] = input(i, p);
                    }
                    if (!EvalPointwise(node, inputs, components, own[p][0])) {
                        ENGINE_FAIL(ErrorCode::Unsupported, ErrorStage::Script,
                                    "{} cannot be sampled on the CPU", ToString(node.kind));
                    }
                }
                return {};
            }
        }
    }

    const Graph& m_graph;
    u32_t        m_seed;
    f32_t        m_spacing;
};

} // namespace

Status SampleGraph(const Graph& graph, Value value, u32_t seed, f32_t spacing,
                   std::span<const std::array<f32_t, 2>> positions, std::span<f32_t> out) {
    if (!value.IsValid() || value.node >= graph.NodeCount() || out.size() != positions.size()
        || !(spacing > 0.0f)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Script,
                    "sampling needs a value of this graph, one output per position and a spacing above zero");
    }
    Sampler sampler(graph, seed, spacing);
    for (usize_t at = 0; at < positions.size(); at += kChunk) {
        const usize_t n = std::min(kChunk, positions.size() - at);
        if (Status ran = sampler.Run(value, positions.subspan(at, n), out.subspan(at, n)); !ran) {
            return ran;
        }
    }
    return {};
}

} // namespace engine::terrain
