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

[[nodiscard]] f32_t Smooth(f32_t e0, f32_t e1, f32_t x) {
    const f32_t t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// `water.comp.glsl` on the CPU, term for term: { the bound, the water surface }.
[[nodiscard]] std::array<f32_t, 2> EvalWater(const Graph& graph, const Graph::Node& node, f32_t px, f32_t pz,
                                             f32_t aux) {
    const std::span<const f32_t> data = graph.GridData();
    const u32_t                  block = node.params[0];
    const u32_t                  receiver = node.params[1];
    const u32_t                  mode = node.params[2];
    const auto at = [&](u32_t b, u32_t i) { return data[b + i]; };
    const auto field = [&](u32_t b, u32_t k) {
        const i32_t w = static_cast<i32_t>(at(b, 0));
        const i32_t d = static_cast<i32_t>(at(b, 1));
        const f32_t cell = at(b, 4);
        const f32_t gx = std::clamp((px - at(b, 2)) / cell, 0.0f, static_cast<f32_t>(w - 1));
        const f32_t gz = std::clamp((pz - at(b, 3)) / cell, 0.0f, static_cast<f32_t>(d - 1));
        const i32_t ix = static_cast<i32_t>(std::floor(gx));
        const i32_t iz = static_cast<i32_t>(std::floor(gz));
        std::array<f32_t, 4> rows{};
        for (i32_t j = -1; j <= 2; ++j) {
            const i32_t z = std::clamp(iz + j, 0, d - 1);
            std::array<f32_t, 4> v{};
            for (i32_t m = -1; m <= 2; ++m) {
                const i32_t x = std::clamp(ix + m, 0, w - 1);
                v[static_cast<usize_t>(m + 1)] = data[b + static_cast<u32_t>(at(b, 17)) + static_cast<u32_t>(z * w + x) * 4u + k];
            }
            rows[static_cast<usize_t>(j + 1)] = CatmullRom(v[0], v[1], v[2], v[3], gx - static_cast<f32_t>(ix));
        }
        return CatmullRom(rows[0], rows[1], rows[2], rows[3], gz - static_cast<f32_t>(iz));
    };
    // The exact distance to a river's course around the cell's nearest segment: { distance, level, half }.
    const auto course = [&](u32_t b) {
        const i32_t w    = static_cast<i32_t>(at(b, 0));
        const i32_t dd   = static_cast<i32_t>(at(b, 1));
        const f32_t cell = at(b, 4);
        const i32_t cx = std::clamp(static_cast<i32_t>(std::floor((px - at(b, 2)) / cell + 0.5f)), 0, w - 1);
        const i32_t cz = std::clamp(static_cast<i32_t>(std::floor((pz - at(b, 3)) / cell + 0.5f)), 0, dd - 1);
        const i32_t nearest = static_cast<i32_t>(data[b + static_cast<u32_t>(at(b, 17)) + static_cast<u32_t>(cz * w + cx) * 4u + 1u] + 0.5f);
        const i32_t window  = static_cast<i32_t>(at(b, 15));
        const i32_t count   = static_cast<i32_t>(at(b, 16));
        std::array<f32_t, 6> best{1e30f, 0.0f, 0.5f, 1.0f, 0.0f, 1.0f};
        for (i32_t s = std::max(0, nearest - window); s <= std::min(count - 2, nearest + window); ++s) {
            const u32_t a  = b + 20u + static_cast<u32_t>(s) * 6u;
            const f32_t ax = data[a], az = data[a + 1], bx = data[a + 6], bz = data[a + 7];
            const f32_t abx = bx - ax, abz = bz - az;
            const f32_t t   = std::clamp(((px - ax) * abx + (pz - az) * abz) / std::max(abx * abx + abz * abz, 1e-6f), 0.0f, 1.0f);
            const f32_t ex = px - ax - abx * t, ez = pz - az - abz * t;
            const f32_t d  = std::sqrt(ex * ex + ez * ez);
            if (d < best[0]) {
                const auto lerp = [&](u32_t k) { return data[a + k] + (data[a + 6 + k] - data[a + k]) * t; };
                best = {d, lerp(2), lerp(3), lerp(4), lerp(5), abx * ez - abz * ex >= 0.0f ? 1.0f : -1.0f};
            }
        }
        return best;
    };
    f32_t bound = 0.0f;
    f32_t water = -1e6f;
    if (mode <= 1) {
        const std::array<f32_t, 6> exact = course(block);
        const f32_t d      = std::min(exact[0], std::sqrt(std::max(field(block, 0), 0.0f)));
        const f32_t cellW  = at(block, 4);
        const f32_t level  = field(block, 2) + (exact[1] - field(block, 2)) * (1.0f - Smooth(cellW, 2.0f * cellW, exact[0]));
        const f32_t half   = std::max(exact[2], 0.5f);
        const f32_t depth  = exact[3];
        const f32_t axis   = field(block, 3);
        const u32_t kind   = static_cast<u32_t>(at(block, 5));
        const f32_t valley = at(block, 7);
        const f32_t steep  = at(block, 8);
        const f32_t plain  = at(block, 11);
        const f32_t e      = d - half;
        if (mode == 0) {
            const f32_t u    = d / half;
            const f32_t v    = exact[5] * u;
            const f32_t skew = exact[4];
            const f32_t across = v > skew ? (v - skew) / (1.0f - skew) : (v - skew) / (1.0f + skew);
            f32_t       cut    = std::clamp(1.0f - across * across, 0.0f, 1.0f);
            if (kind == 5) {
                cut *= 0.5f + 0.5f * std::cos(u * 9.4248f + 3.0f * std::sin(px * 0.0041f + pz * 0.0033f));
            }
            f32_t bed = level - cut * depth;
            bed += (axis - level) * Smooth(at(block, 14) * 1.5f, at(block, 14) * 6.0f, e);
            const f32_t open  = std::max(e, 0.0f);
            f32_t       flare = open;
            f32_t       rise  = 0.0f;
            if (kind == 1) {
                const f32_t step = at(block, 9);
                const f32_t t    = open * steep / step;
                const f32_t whole = std::floor(t);
                const f32_t s     = 1.0f - 0.92f;
                rise = step * (whole + Smooth(0.5f - 0.5f * s, 0.5f + 0.5f * s, t - whole));
            } else if (kind == 3) {
                const f32_t floor = valley * at(block, 10);
                const f32_t past  = std::max(e - floor, 0.0f);
                rise  = past * past * (steep / (2.0f * (valley - floor)));
                flare = past;
            } else if (kind == 4 || kind == 5) {
                const f32_t terrace = at(block, 12);
                rise = Smooth(0.0f, depth * 2.0f, e) + Smooth(plain, plain + 60.0f, e) * terrace
                       + Smooth(plain * 1.5f, plain * 1.5f + 60.0f, e) * terrace
                       + std::max(e - plain * 1.5f - 60.0f, 0.0f) * steep;
            } else {
                rise = open * steep;
            }
            rise += flare * flare * (steep / valley);
            bound = bed + rise * aux;
            water = level - Smooth(0.0f, 3.0f, e) * 20000.0f;
            if (receiver != Graph::kNoBlock) {
                const std::array<f32_t, 6> r = course(receiver);
                const f32_t                er = r[0] - std::max(r[2], 0.5f);
                water -= (1.0f - Smooth(-2.0f, 0.0f, er)) * 20000.0f;
            }
        } else {
            const f32_t levee  = (Smooth(0.0f, depth * 3.0f, e) - Smooth(depth * 3.0f, depth * 12.0f, e)) * at(block, 13);
            const f32_t within = Smooth(0.0f, depth, e) - Smooth(plain, plain + 30.0f, e);
            const f32_t fill   = std::min(level + 0.6f + levee, aux + 3.0f);
            bound = fill * within + (level - 20000.0f) * (1.0f - within);
        }
    } else {
        const f32_t s       = field(block, 0);
        const f32_t q       = -s;
        const f32_t L       = at(block, 6);
        const f32_t D       = at(block, 7);
        const f32_t size    = at(block, 8);
        const f32_t outside = Smooth(-at(block, 4), 0.0f, s);
        if (mode == 2) {
            const f32_t shelf = size * at(block, 10);
            f32_t       floor = 0.0f;
            if (shelf > 0.0f) {
                const f32_t sd = at(block, 11);
                floor = L - Smooth(0.0f, shelf * 0.3f, q) * (D * sd)
                        - Smooth(shelf, shelf + size * at(block, 9), q) * (D * (1.0f - sd));
            } else {
                floor = L - Smooth(0.0f, size * at(block, 9), q) * D;
            }
            const f32_t peak = aux + 0.5f;
            floor += peak * peak * Smooth(size * 0.05f, size * 0.3f, q) * (D * (0.3f + at(block, 14) * 2.2f));
            const f32_t around = L + 2.0f + std::max(s, 0.0f) * at(block, 12) + std::max(s - at(block, 13), 0.0f) * 20.0f;
            bound = floor * (1.0f - outside) + around * outside;
            water = L - Smooth(30.0f, 60.0f, s) * 20000.0f;
        } else {
            const f32_t rim = (L + 2.0f) - Smooth(0.0f, 120.0f, s) * 20000.0f;
            bound = rim * outside + (L - 20000.0f) * (1.0f - outside);
            if (static_cast<u32_t>(at(block, 5)) == 5) {
                const f32_t land = L + 1.5f - (1.0f - Smooth(0.0f, 150.0f, q)) * 5.0f;
                bound = land * (1.0f - outside) + (L - 20000.0f) * outside;
            }
            const f32_t crater = at(block, 15);
            if (crater > 0.0f) {
                const f32_t x     = std::fabs(s - size * 0.25f) / (size * 0.12f);
                const f32_t crest = std::exp(-0.5f * x * x);
                bound = std::max(bound, (L + crater) * crest + (L - 20000.0f) * (1.0f - crest));
            }
        }
    }
    return {bound, water};
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
            case OpKind::Water:
                for (usize_t p = 0; p < positions.size(); ++p) {
                    const std::array<f32_t, 2> w = EvalWater(m_graph, node, input(0, p)[0], input(0, p)[1], input(1, p)[0]);
                    own[p][0][0] = w[0];
                    own[p][1][0] = w[1];
                }
                return {};
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
