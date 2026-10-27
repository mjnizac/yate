#include <engine/terrain/hydrology.hpp>

#include <engine/memory/general.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <utility>

namespace engine::terrain {

namespace {

constexpr std::array<i32_t, 8> kDx = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr std::array<i32_t, 8> kDz = {-1, -1, -1, 0, 0, 1, 1, 1};

/// The slope a flat is given so every cell still drains: small against any real slope, large against
/// rounding at the heights terrain has.
constexpr f64_t kEpsilon = 1e-4;

/// A min-heap of (level, cell), allocated once.
class Heap {
public:
    explicit Heap(std::pmr::memory_resource* resource) : m_entries(resource) {}
    void Reserve(usize_t count) { m_entries.reserve(count); }
    void Push(f64_t level, u32_t cell) {
        m_entries.emplace_back(level, cell);
        std::push_heap(m_entries.begin(), m_entries.end(), std::greater<>{});
    }
    [[nodiscard]] std::pair<f64_t, u32_t> Pop() {
        std::pop_heap(m_entries.begin(), m_entries.end(), std::greater<>{});
        const auto top = m_entries.back();
        m_entries.pop_back();
        return top;
    }
    [[nodiscard]] b8_t Empty() const { return m_entries.empty(); }
    void Clear() { m_entries.clear(); }

private:
    std::pmr::vector<std::pair<f64_t, u32_t>> m_entries;
};

struct Grid {
    i32_t width = 0;
    i32_t depth = 0;
    [[nodiscard]] b8_t Inside(i32_t x, i32_t z) const { return x >= 0 && z >= 0 && x < width && z < depth; }
    [[nodiscard]] b8_t OnEdge(i32_t x, i32_t z) const {
        return x == 0 || z == 0 || x == width - 1 || z == depth - 1;
    }
};

} // namespace

Status Drain(std::span<const f64_t> input, const HydrologyParams& params, HydrologyResult& result) {
    const usize_t count = static_cast<usize_t>(params.width) * params.depth;
    if (params.width < 3 || params.depth < 3 || input.size() != count || !(params.cell > 0.0)
        || !(params.breachDepth >= 0.0) || !(params.lakeArea >= 0.0) || !(params.lakeDepth >= 0.0)
        || !(params.channelArea > 0.0) || !(params.widthScale >= 0.0) || !(params.depthScale >= 0.0)) {
        ENGINE_FAIL(ErrorCode::InvalidArgument, ErrorStage::Script,
                    "hydrology needs a grid of at least 3 x 3 with {} heights, a cell greater than zero "
                    "and breach, lake and channel sizes of zero or more",
                    count);
    }
    std::pmr::memory_resource* resource = &memory::General().Resource();
    const Grid  grid{static_cast<i32_t>(params.width), static_cast<i32_t>(params.depth)};
    const f64_t cell     = params.cell;
    const f64_t cellArea = cell * cell;
    const auto  isOutlet = [&](u32_t i, const std::pmr::vector<f64_t>& h) {
        const i32_t x = static_cast<i32_t>(i % params.width);
        const i32_t z = static_cast<i32_t>(i / params.width);
        return grid.OnEdge(x, z) || h[i] <= params.seaLevel;
    };

    std::pmr::vector<f64_t> h(input.begin(), input.end(), resource);
    std::pmr::vector<f64_t> level(count, 0.0, resource);
    std::pmr::vector<u8_t>  done(count, 0, resource);
    Heap                    open(resource);
    open.Reserve(count);

    // 1. Priority flood from the outlets: `level` is every cell filled to the lowest way out, plus a
    // small slope across flats so every cell drains.
    for (u32_t i = 0; i < count; ++i) {
        if (isOutlet(i, h)) {
            level[i] = h[i];
            done[i]  = 1;
            open.Push(level[i], i);
        }
    }
    while (!open.Empty()) {
        const u32_t c = open.Pop().second;
        const i32_t x = static_cast<i32_t>(c % params.width);
        const i32_t z = static_cast<i32_t>(c / params.width);
        for (usize_t d = 0; d < 8; ++d) {
            const i32_t nx = x + kDx[d];
            const i32_t nz = z + kDz[d];
            if (!grid.Inside(nx, nz)) {
                continue;
            }
            const u32_t q = static_cast<u32_t>(nz * grid.width + nx);
            if (done[q] == 0) {
                done[q]  = 1;
                level[q] = std::max(h[q], level[c] + kEpsilon);
                open.Push(level[q], q);
            }
        }
    }
    // Steepest descent on the filled surface: the way out of every depression.
    const auto steepest = [&](u32_t i, const std::pmr::vector<f64_t>& surface) {
        const i32_t x    = static_cast<i32_t>(i % params.width);
        const i32_t z    = static_cast<i32_t>(i / params.width);
        f64_t       best = 0.0;
        i32_t       to   = -1;
        for (usize_t d = 0; d < 8; ++d) {
            const i32_t nx = x + kDx[d];
            const i32_t nz = z + kDz[d];
            if (!grid.Inside(nx, nz)) {
                continue;
            }
            const u32_t q    = static_cast<u32_t>(nz * grid.width + nx);
            const f64_t drop = (surface[i] - surface[q]) / ((kDx[d] != 0 && kDz[d] != 0) ? 1.41421356237 : 1.0);
            if (drop > best) {
                best = drop;
                to   = static_cast<i32_t>(q);
            }
        }
        return to;
    };

    // 2. Each depression, decided whole (Lindsay's hybrid breaching and filling): one no deeper than
    // `breachDepth` is breached, a channel cut from its lowest cell along the way out until the ground
    // falls below it, so no cell is lowered by more than its depth; a larger, deeper one is a lake, the
    // terrain keeping its bowl and the water at the spill level; any other is filled. Deciding cell by
    // cell instead cut the rim of a lake partway and lowered its surface by the breach depth.
    std::pmr::vector<i32_t> label(count, -1, resource);
    std::pmr::vector<u32_t> stack(resource);
    std::pmr::vector<u32_t> members(resource);
    std::pmr::vector<u8_t>  inLake(count, 0, resource);
    result.lakes = 0;
    for (u32_t start = 0; start < count; ++start) {
        if (label[start] >= 0 || !(level[start] > h[start] + 1e-9)) {
            continue;
        }
        members.clear();
        stack.clear();
        stack.push_back(start);
        label[start]  = static_cast<i32_t>(start);
        f64_t deepest = 0.0;
        f64_t surface = -1e300;
        u32_t lowest  = start;
        while (!stack.empty()) {
            const u32_t i = stack.back();
            stack.pop_back();
            members.push_back(i);
            deepest = std::max(deepest, level[i] - h[i]);
            surface = std::max(surface, level[i]);
            lowest  = h[i] < h[lowest] ? i : lowest;
            const i32_t x = static_cast<i32_t>(i % params.width);
            const i32_t z = static_cast<i32_t>(i / params.width);
            for (usize_t d = 0; d < 8; ++d) {
                const i32_t nx = x + kDx[d];
                const i32_t nz = z + kDz[d];
                if (!grid.Inside(nx, nz)) {
                    continue;
                }
                const u32_t q = static_cast<u32_t>(nz * grid.width + nx);
                if (label[q] < 0 && level[q] > h[q] + 1e-9) {
                    label[q] = static_cast<i32_t>(start);
                    stack.push_back(q);
                }
            }
        }
        if (deepest <= params.breachDepth) {
            f64_t target = h[lowest];
            for (i32_t p = steepest(lowest, level); p >= 0; p = steepest(static_cast<u32_t>(p), level)) {
                target -= kEpsilon;
                if (h[static_cast<u32_t>(p)] < target) {
                    break;
                }
                h[static_cast<u32_t>(p)] = target;
            }
            continue;
        }
        const b8_t isLake = static_cast<f64_t>(members.size()) * cellArea >= params.lakeArea
                            && deepest >= params.lakeDepth;
        result.lakes += isLake ? 1u : 0u;
        for (const u32_t i : members) {
            if (isLake) {
                inLake[i] = 1;
                level[i]  = surface;
            } else {
                h[i] = level[i];
            }
        }
    }

    // 3. Routing on the terrain with the lakes at their surface, flats given a slope by a second flood.
    std::pmr::vector<f64_t> surfaceHeight(count, 0.0, resource);
    for (u32_t i = 0; i < count; ++i) {
        surfaceHeight[i] = inLake[i] != 0 ? level[i] : h[i];
    }
    std::pmr::vector<f64_t> s(count, 0.0, resource);
    std::pmr::vector<u32_t> order(resource);
    order.reserve(count);
    std::fill(done.begin(), done.end(), u8_t{0});
    open.Clear();
    for (u32_t i = 0; i < count; ++i) {
        if (isOutlet(i, surfaceHeight)) {
            s[i]    = surfaceHeight[i];
            done[i] = 1;
            open.Push(s[i], i);
        }
    }
    while (!open.Empty()) {
        const u32_t c = open.Pop().second;
        order.push_back(c);
        const i32_t x = static_cast<i32_t>(c % params.width);
        const i32_t z = static_cast<i32_t>(c / params.width);
        for (usize_t d = 0; d < 8; ++d) {
            const i32_t nx = x + kDx[d];
            const i32_t nz = z + kDz[d];
            if (!grid.Inside(nx, nz)) {
                continue;
            }
            const u32_t q = static_cast<u32_t>(nz * grid.width + nx);
            if (done[q] == 0) {
                done[q] = 1;
                s[q]    = std::max(surfaceHeight[q], s[c] + kEpsilon);
                open.Push(s[q], q);
            }
        }
    }
    std::pmr::vector<i32_t> receiver(count, -1, resource);
    for (u32_t i = 0; i < count; ++i) {
        receiver[i] = isOutlet(i, surfaceHeight) ? -1 : steepest(i, s);
    }
    // The lake surface, and 20 m under the ground elsewhere: a smooth field, because the GPU reads it with
    // Catmull-Rom, which overshoots a jump to a far sentinel by a large fraction of the jump.
    result.lake.assign(count, 0.0);
    for (u32_t i = 0; i < count; ++i) {
        result.lake[i] = inLake[i] != 0 ? level[i] : s[i] - 20.0;
    }
    result.area.assign(count, cellArea);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (receiver[*it] >= 0) {
            result.area[static_cast<u32_t>(receiver[*it])] += result.area[*it];
        }
    }

    // 4. River centre lines: each channel cell to its receiver, smoothed by pulling every point towards
    // the middle of its neighbours along the river, its receiver and its largest tributary.
    const auto channel = [&](u32_t i) { return result.area[i] >= params.channelArea; };
    std::pmr::vector<i32_t> mainInflow(count, -1, resource);
    for (u32_t i = 0; i < count; ++i) {
        const i32_t r = receiver[i];
        if (r >= 0 && channel(i)) {
            const i32_t best = mainInflow[static_cast<u32_t>(r)];
            if (best < 0 || result.area[i] > result.area[static_cast<u32_t>(best)]) {
                mainInflow[static_cast<u32_t>(r)] = static_cast<i32_t>(i);
            }
        }
    }
    std::pmr::vector<f64_t> px(count, 0.0, resource);
    std::pmr::vector<f64_t> pz(count, 0.0, resource);
    for (u32_t i = 0; i < count; ++i) {
        px[i] = static_cast<f64_t>(i % params.width);
        pz[i] = static_cast<f64_t>(i / params.width);
    }
    for (int pass = 0; pass < 3; ++pass) {
        std::pmr::vector<f64_t> nx(px, resource);
        std::pmr::vector<f64_t> nz(pz, resource);
        for (u32_t i = 0; i < count; ++i) {
            const i32_t r = receiver[i];
            const i32_t u = mainInflow[i];
            if (!channel(i) || r < 0 || u < 0) {
                continue;
            }
            nx[i] = 0.5 * px[i] + 0.25 * (px[static_cast<u32_t>(r)] + px[static_cast<u32_t>(u)]);
            nz[i] = 0.5 * pz[i] + 0.25 * (pz[static_cast<u32_t>(r)] + pz[static_cast<u32_t>(u)]);
        }
        px.swap(nx);
        pz.swap(nz);
    }

    // 5. Distance from every cell to the nearest centre line, in a band a few widths wide.
    const auto widthOf = [&](f64_t area) { return params.widthScale * std::sqrt(area / 1e6); };
    const auto depthOf = [&](f64_t area) { return params.depthScale * std::pow(area / 1e6, 0.3); };
    f64_t widest = 0.0;
    for (u32_t i = 0; i < count; ++i) {
        widest = channel(i) ? std::max(widest, widthOf(result.area[i])) : widest;
    }
    const i32_t band = static_cast<i32_t>(std::ceil((0.5 * widest) / cell)) + 4;
    result.riverDistance.assign(count, static_cast<f64_t>(band) * cell);
    result.riverWidth.assign(count, 0.0);
    result.riverDepth.assign(count, 0.0);
    // Above the ground where no river is near, so the field stays smooth and carves nothing there.
    result.riverLevel.assign(s.begin(), s.end());
    for (f64_t& v : result.riverLevel) {
        v += 50.0;
    }
    result.rivers = 0;
    for (u32_t i = 0; i < count; ++i) {
        const i32_t r = receiver[i];
        if (!channel(i) || r < 0) {
            continue;
        }
        const u32_t ru = static_cast<u32_t>(r);
        // Inside a lake the lake's surface is the water.
        if (inLake[i] != 0 && inLake[ru] != 0) {
            continue;
        }
        ++result.rivers;
        const f64_t ax = px[i], az = pz[i], bx = px[ru], bz = pz[ru];
        const f64_t w0 = widthOf(result.area[i]), w1 = widthOf(result.area[ru]);
        const f64_t d0 = depthOf(result.area[i]), d1 = depthOf(result.area[ru]);
        const f64_t l0 = s[i], l1 = s[ru];
        const i32_t x0 = static_cast<i32_t>(std::floor(std::min(ax, bx))) - band;
        const i32_t x1 = static_cast<i32_t>(std::ceil(std::max(ax, bx))) + band;
        const i32_t z0 = static_cast<i32_t>(std::floor(std::min(az, bz))) - band;
        const i32_t z1 = static_cast<i32_t>(std::ceil(std::max(az, bz))) + band;
        const f64_t ex = bx - ax, ez = bz - az;
        const f64_t length2 = ex * ex + ez * ez;
        for (i32_t z = std::max(z0, 0); z <= std::min(z1, grid.depth - 1); ++z) {
            for (i32_t x = std::max(x0, 0); x <= std::min(x1, grid.width - 1); ++x) {
                const f64_t t  = length2 > 0.0
                                     ? std::clamp(((x - ax) * ex + (z - az) * ez) / length2, 0.0, 1.0)
                                     : 0.0;
                const f64_t dx = x - (ax + t * ex);
                const f64_t dz = z - (az + t * ez);
                const f64_t distance = std::sqrt(dx * dx + dz * dz) * cell;
                const u32_t at       = static_cast<u32_t>(z * grid.width + x);
                if (distance < result.riverDistance[at]) {
                    result.riverDistance[at] = distance;
                    result.riverWidth[at]    = w0 + (w1 - w0) * t;
                    result.riverDepth[at]    = d0 + (d1 - d0) * t;
                    result.riverLevel[at]    = l0 + (l1 - l0) * t;
                }
            }
        }
    }
    result.height = std::move(h);
    return {};
}

} // namespace engine::terrain
