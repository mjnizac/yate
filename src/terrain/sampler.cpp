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
            const f32_t around = L + 2.0f + std::max(s, 0.0f) * field(block, 1) + std::max(s - at(block, 13), 0.0f) * 20.0f;
            bound = floor * (1.0f - outside) + around * outside;
            water = L - Smooth(30.0f, 60.0f, s) * 20000.0f;
        } else {
            const f32_t rim = (L + 2.0f) - Smooth(0.0f, 120.0f, s) * 20000.0f;
            bound = rim * outside + (L - 20000.0f) * (1.0f - outside);
            if (static_cast<u32_t>(at(block, 5)) == 5) {
                const f32_t land = L + 1.5f - (1.0f - Smooth(0.0f, 150.0f, q)) * 5.0f;
                bound = land * (1.0f - outside) + (L - 20000.0f) * outside;
            }
            const f32_t moraine = field(block, 2);
            if (moraine > 0.5f) {
                const f32_t x     = (s - 80.0f) / 40.0f;
                const f32_t crest = std::exp(-0.5f * x * x);
                bound = std::max(bound, (L + moraine) * crest + (L - 20000.0f) * (1.0f - crest));
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

/// `lands.comp.glsl` on the CPU, term for term: { the signed distance to the coast, the land id }.
[[nodiscard]] std::array<f32_t, 2> EvalLands(const Graph& graph, const Graph::Node& node, f32_t px, f32_t pz) {
    constexpr u32_t                kElement = 48;
    const std::span<const f32_t>   data     = graph.GridData();
    const u32_t                    block    = node.params[0];
    const auto at = [&](u32_t i) { return data[i]; };
    const auto index = [&](u32_t i) { return static_cast<u32_t>(data[i]); };
    const auto smoothMin = [](f32_t a, f32_t b, f32_t k) {
        if (k <= 0.0f) {
            return std::min(a, b);
        }
        const f32_t h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
        return b + (a - b) * h - k * h * (1.0f - h);
    };
    const auto segment = [](f32_t x, f32_t z, f32_t ax, f32_t az, f32_t bx, f32_t bz, f32_t& h) {
        const f32_t abx = bx - ax, abz = bz - az;
        const f32_t l2  = abx * abx + abz * abz;
        h               = std::clamp(l2 > 0.0f ? ((x - ax) * abx + (z - az) * abz) / l2 : 0.0f, 0.0f, 1.0f);
        const f32_t dx = x - ax - abx * h, dz = z - az - abz * h;
        return std::sqrt(dx * dx + dz * dz);
    };
    const f32_t reach    = at(block + 6);
    const u32_t points   = block + index(block + 10);
    const auto  distance = [&](u32_t e, f32_t wx, f32_t wz) -> f32_t {
        const u32_t kind  = index(e);
        const f32_t dx    = wx - at(e + 3), dz = wz - at(e + 4);
        const f32_t c     = at(e + 5), s = at(e + 6), scale = at(e + 7);
        const f32_t x     = (c * dx + s * dz) / scale;
        const f32_t z     = (-s * dx + c * dz) / scale;
        const f32_t a     = at(e + 8), b = at(e + 9), n = at(e + 10);
        const f32_t rho   = std::sqrt(x * x + z * z);
        f32_t       dist  = 0.0f;
        f32_t       h     = 0.0f;
        switch (kind) {
            case 0: dist = rho - a; break;
            case 1: {
                const f32_t k0 = std::sqrt((x / a) * (x / a) + (z / b) * (z / b));
                const f32_t k1 = std::sqrt((x / (a * a)) * (x / (a * a)) + (z / (b * b)) * (z / (b * b)));
                dist = k1 > 0.0f ? k0 * (k0 - 1.0f) / k1 : -std::min(a, b);
                break;
            }
            case 2: {
                const f32_t qx = std::abs(x) - (a - n), qz = std::abs(z) - (b - n);
                const f32_t inside = std::max(qx, qz);
                const f32_t ox = std::max(qx, 0.0f), oz = std::max(qz, 0.0f);
                dist = std::sqrt(ox * ox + oz * oz) + (inside < 0.0f ? inside : 0.0f) - n;
                break;
            }
            case 3: {
                const f32_t u  = std::max(std::abs(x) / a, 1e-20f);
                const f32_t v  = std::max(std::abs(z) / b, 1e-20f);
                const f32_t sum = std::pow(u, n) + std::pow(v, n);
                if (sum < 1e-12f) {
                    dist = -std::min(a, b);
                    break;
                }
                const f32_t r      = std::pow(sum, 1.0f / n);
                const f32_t common = std::pow(sum, 1.0f / n - 1.0f);
                const f32_t gx = common * std::pow(u, n - 1.0f) / a, gz = common * std::pow(v, n - 1.0f) / b;
                dist = (r - 1.0f) / std::max(std::sqrt(gx * gx + gz * gz), 1e-9f);
                break;
            }
            case 4: {
                const u32_t count = index(e + 9);
                const f32_t theta = std::atan2(z, x);
                f32_t       r = 1.0f, dr = 0.0f;
                for (u32_t i = 0; i < count; ++i) {
                    const f32_t k = at(e + 10 + i * 3), amp = at(e + 11 + i * 3), phase = at(e + 12 + i * 3);
                    r += amp * std::sin(k * theta + phase);
                    dr += amp * k * std::cos(k * theta + phase);
                }
                r *= a;
                dr *= a;
                const f32_t g = dr / std::max(rho, at(e + 36));
                dist = (rho - r) / std::sqrt(1.0f + g * g);
                break;
            }
            case 5: dist = segment(x, z, -a, 0.0f, a, 0.0f, h) - b; break;
            case 6: dist = std::abs(rho - a) - 0.5f * b; break;
            case 7: {
                const f32_t qx = std::abs(z), qy = x;
                const f32_t sx = std::sin(b), sy = std::cos(b);
                const f32_t ex = qx - sx * a, ey = qy - sy * a;
                const f32_t d = sy * qx > sx * qy ? std::sqrt(ex * ex + ey * ey) : std::abs(rho - a);
                dist = d - 0.5f * n;
                break;
            }
            case 8: {
                const f32_t qx = std::abs(z), qy = x;
                const f32_t cx = std::sin(b), cy = std::cos(b);
                const f32_t l  = rho - a;
                const f32_t t  = std::clamp(qx * cx + qy * cy, 0.0f, a);
                const f32_t mx = qx - cx * t, my = qy - cy * t;
                const f32_t side = cy * qx - cx * qy;
                const f32_t sign = side > 0.0f ? 1.0f : (side < 0.0f ? -1.0f : 0.0f);
                dist = std::max(l, std::sqrt(mx * mx + my * my) * sign);
                break;
            }
            case 9:
            case 10: {
                const b8_t  polygon = kind == 9;
                const u32_t first = index(e + 22), count = index(e + 23);
                f32_t       best = 1e30f;
                b8_t        inside = false;
                for (u32_t i = 0; i + (polygon ? 0u : 1u) < count; ++i) {
                    const u32_t pa = points + (first + i) * 3, pb = points + (first + (i + 1) % count) * 3;
                    const f32_t ax = at(pa), az = at(pa + 1), bx = at(pb), bz = at(pb + 1);
                    const f32_t d = segment(x, z, ax, az, bx, bz, h);
                    if (polygon) {
                        best = std::min(best, d);
                        if ((az > z) != (bz > z) && x < ax + (z - az) * (bx - ax) / (bz - az)) {
                            inside = !inside;
                        }
                    } else {
                        best = std::min(best, d - 0.5f * (at(pa + 2) + (at(pb + 2) - at(pa + 2)) * h));
                    }
                }
                dist = polygon && inside ? -best : best;
                break;
            }
            default: return -reach;
        }
        if (kind == 1 || kind == 3 || kind == 4) {
            const f32_t inner = at(e + 36);
            dist              = (rho - inner) + (dist - (rho - inner)) * Smooth(0.25f * inner, inner, rho);
        }
        return dist * scale;
    };

    const f32_t bin   = at(block + 1);
    const i32_t binsX = static_cast<i32_t>(at(block + 4)), binsZ = static_cast<i32_t>(at(block + 5));
    const u32_t elements = block + index(block + 7), offsets = block + index(block + 8), lists = block + index(block + 9);
    u32_t       first = block + index(block + 12);
    u32_t       count = index(block + 11);
    const i32_t gx = static_cast<i32_t>(std::floor((px - at(block + 2)) / bin));
    const i32_t gz = static_cast<i32_t>(std::floor((pz - at(block + 3)) / bin));
    if (gx >= 0 && gz >= 0 && gx < binsX && gz < binsZ) {
        const u32_t b = static_cast<u32_t>(gz * binsX + gx);
        const u32_t s = index(offsets + b);
        first         = lists + s;
        count         = index(offsets + b + 1) - s;
    }
    f32_t d = reach, best = 1e30f, id = 0.0f;
    for (u32_t i = 0; i < count; ++i) {
        const u32_t e    = elements + index(first + i) * kElement;
        const f32_t op   = at(e + 1);
        if (op > 1.5f) {
            continue;
        }
        const f32_t dist = distance(e, px, pz);
        const f32_t k    = at(e + 20);
        if (op < 0.5f) {
            d = smoothMin(d, dist, k);
            if (dist < best) {
                best = dist;
                id   = at(e + 2);
            }
        } else {
            d = -smoothMin(-d, dist, k);
            if (dist < 0.0f) {
                best = 1e30f;
            }
        }
        d = std::clamp(d, -reach, reach);
    }
    if (node.params[1] == 0) {
        return {d, id};
    }
    const auto profile = [&](u32_t e, f32_t s) -> f32_t {
        const u32_t kind = index(e + 24);
        const f32_t a = at(e + 25), b = at(e + 26);
        switch (kind) {
            case 1: {
                const f32_t w = 1.0f - std::clamp(s / b, 0.0f, 1.0f);
                return a * std::sqrt(std::max(1.0f - w * w, 0.0f));
            }
            case 2: return a * std::clamp(s / b, 0.0f, 1.0f);
            case 3: return a * Smooth(0.0f, b, s);
            case 4: {
                const f32_t t = (s - b) / at(e + 27);
                return a * std::exp(-0.5f * t * t) * Smooth(0.0f, b, s)
                     + at(e + 28) * Smooth(at(e + 30), at(e + 30) + at(e + 29), s);
            }
            case 5: {
                const f32_t along = (px - at(e + 3)) * at(e + 28) + (pz - at(e + 4)) * at(e + 29);
                const f32_t floor = at(e + 30);
                return Smooth(0.0f, b, s) * -smoothMin(-(a + at(e + 27) * along), -floor, std::max(0.25f * std::abs(a - floor), 1.0f));
            }
            default: return 0.0f;
        }
    };
    const f32_t blend = std::max(at(block + 13), 1e-3f);
    const f32_t shore = std::max(-d, 0.0f);
    if (node.params[1] == 2) {
        f32_t raise = 0.0f, trench = 0.0f;
        b8_t  rose  = false;
        for (u32_t i = 0; i < count; ++i) {
            const u32_t e  = elements + index(first + i) * kElement;
            const f32_t op = at(e + 1);
            if (op > 1.5f) {
                const f32_t s = -distance(e, px, pz);
                if (s > 0.0f) {
                    const f32_t h = profile(e, s);
                    raise         = rose ? -smoothMin(-raise, -h, blend) : h;
                    rose          = true;
                }
            } else if (op < 0.5f && at(e + 37) > 0.0f) {
                const f32_t t = (distance(e, px, pz) - at(e + 38)) / at(e + 39);
                trench        = std::max(trench, at(e + 37) * std::exp(-0.5f * t * t));
            }
        }
        return {raise - trench, id};
    }
    f32_t       value = 0.0f;
    b8_t        any   = false;
    for (u32_t i = 0; i < count; ++i) {
        const u32_t e = elements + index(first + i) * kElement;
        if (at(e + 1) < 0.5f) {
            const f32_t s = std::min(-distance(e, px, pz), shore);
            if (s > 0.0f) {
                const f32_t h = profile(e, s);
                value         = any ? -smoothMin(-value, -h, blend) : h;
                any           = true;
            }
        }
    }
    return {any ? value : 0.0f, id};
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
            case OpKind::Lands:
                for (usize_t p = 0; p < positions.size(); ++p) {
                    const std::array<f32_t, 2> l = EvalLands(m_graph, node, input(0, p)[0], input(0, p)[1]);
                    own[p][0][0] = l[0];
                    own[p][1][0] = l[1];
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
