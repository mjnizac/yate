#pragma once

// Measurements of a land/sea heightfield: the criteria the Lands macros are accepted against.
//
// Everything here is global (connected components, distances between masses, box counting) and so
// cannot be a per-sample op; it runs on an exported raw field. Header-only and standard library only,
// so the same code serves `test_lands` and the `terrain_metrics` tool.
//
// Conventions, fixed once: a cell is land when its height is above `level` (the sea level, 0 by
// default) and water otherwise; components are 4-connected; areas are in square metres and lengths in
// metres; a field is `width` x `depth`, x fastest, `resolution` metres per sample.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace metrics {

struct Field {
    std::size_t        width = 0;
    std::size_t        depth = 0;
    double             resolution = 1.0;
    std::vector<float> values;

    [[nodiscard]] float At(std::size_t x, std::size_t z) const { return values[z * width + x]; }
};

[[nodiscard]] inline bool LoadRaw(const std::string& path, std::size_t width, std::size_t depth,
                                  double resolution, Field& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    out.width      = width;
    out.depth      = depth;
    out.resolution = resolution;
    out.values.resize(width * depth);
    stream.read(reinterpret_cast<char*>(out.values.data()),
                static_cast<std::streamsize>(out.values.size() * sizeof(float)));
    return static_cast<std::size_t>(stream.gcount()) == out.values.size() * sizeof(float);
}

/// Land components, labelled 1.. in order of discovery; 0 is water.
struct Components {
    std::vector<std::uint32_t> label;
    /// Cells per component, index 0 unused.
    std::vector<std::size_t> cells;
    [[nodiscard]] std::size_t Count() const { return cells.empty() ? 0 : cells.size() - 1; }
};

/// Labels the connected regions where `inside(x, z)` holds. 4-connectivity.
template <typename Predicate>
[[nodiscard]] Components Label(std::size_t width, std::size_t depth, Predicate inside) {
    Components result;
    result.label.assign(width * depth, 0);
    result.cells.assign(1, 0);
    std::vector<std::size_t> stack;
    for (std::size_t start = 0; start < width * depth; ++start) {
        if (result.label[start] != 0 || !inside(start % width, start / width)) {
            continue;
        }
        const auto id = static_cast<std::uint32_t>(result.cells.size());
        result.cells.push_back(0);
        stack.push_back(start);
        result.label[start] = id;
        while (!stack.empty()) {
            const std::size_t at = stack.back();
            stack.pop_back();
            ++result.cells[id];
            const std::size_t x = at % width;
            const std::size_t z = at / width;
            const std::array<std::array<long long, 2>, 4> neighbours = {
                {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
            for (const auto& n : neighbours) {
                const long long nx = static_cast<long long>(x) + n[0];
                const long long nz = static_cast<long long>(z) + n[1];
                if (nx < 0 || nz < 0 || nx >= static_cast<long long>(width)
                    || nz >= static_cast<long long>(depth)) {
                    continue;
                }
                const std::size_t index = static_cast<std::size_t>(nz) * width + static_cast<std::size_t>(nx);
                if (result.label[index] == 0 && inside(static_cast<std::size_t>(nx), static_cast<std::size_t>(nz))) {
                    result.label[index] = id;
                    stack.push_back(index);
                }
            }
        }
    }
    return result;
}

struct Report {
    double landAreaFraction  = 0.0;
    std::size_t islands      = 0;
    /// Areas in square metres, largest first.
    std::vector<double> islandAreas;
    double dominantShare     = 0.0;
    /// Water bodies that do not touch the edge of the field: lagoons and lakes.
    std::size_t lagoons      = 0;
    /// Coast length in metres (boundary edges between a land cell and a water cell) and per square km.
    double coastLength       = 0.0;
    double coastPerKm2       = 0.0;
    /// Box-counting dimension of the coast over scales [minBox, maxBox] cells, and the fit's R^2.
    double boxDimension      = 0.0;
    double boxFit            = 0.0;
    /// Smallest gap between two different land masses, metres; 0 when there is only one.
    double minChannel        = 0.0;
    /// Land height percentiles above `level`: 10, 50, 90, 99.
    std::array<double, 4> hypsometry{};
};

[[nodiscard]] inline bool IsLand(const Field& f, std::size_t x, std::size_t z, double level) {
    return static_cast<double>(f.At(x, z)) > level;
}

/// Box-counting dimension of the coast: boundary cells counted in boxes of 2^k cells, k in
/// [kMin, kMax], then the slope of log N against log(1/size). Reported with its R^2, since the number
/// means little where the fit is poor. The result depends on the field's resolution, window and
/// level, so it is a diagnostic per coast type and range of scales, never a global target.
inline void BoxDimension(const Field& f, double level, int kMin, int kMax, double& dimension,
                         double& fit) {
    std::vector<char> coast(f.width * f.depth, 0);
    for (std::size_t z = 0; z < f.depth; ++z) {
        for (std::size_t x = 0; x < f.width; ++x) {
            const bool land = IsLand(f, x, z, level);
            const bool edge = (x + 1 < f.width && IsLand(f, x + 1, z, level) != land)
                              || (z + 1 < f.depth && IsLand(f, x, z + 1, level) != land);
            coast[z * f.width + x] = edge ? 1 : 0;
        }
    }
    std::vector<double> xs;
    std::vector<double> ys;
    for (int k = kMin; k <= kMax; ++k) {
        const std::size_t size = std::size_t{1} << k;
        std::size_t       boxes = 0;
        for (std::size_t bz = 0; bz < f.depth; bz += size) {
            for (std::size_t bx = 0; bx < f.width; bx += size) {
                bool hit = false;
                for (std::size_t z = bz; z < std::min(bz + size, f.depth) && !hit; ++z) {
                    for (std::size_t x = bx; x < std::min(bx + size, f.width); ++x) {
                        if (coast[z * f.width + x] != 0) {
                            hit = true;
                            break;
                        }
                    }
                }
                boxes += hit ? 1 : 0;
            }
        }
        if (boxes > 0) {
            xs.push_back(std::log(1.0 / static_cast<double>(size)));
            ys.push_back(std::log(static_cast<double>(boxes)));
        }
    }
    dimension = 0.0;
    fit       = 0.0;
    if (xs.size() < 3) {
        return;
    }
    const double n  = static_cast<double>(xs.size());
    double       sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        sx += xs[i];
        sy += ys[i];
        sxx += xs[i] * xs[i];
        sxy += xs[i] * ys[i];
        syy += ys[i] * ys[i];
    }
    const double denominator = n * sxx - sx * sx;
    if (denominator == 0.0) {
        return;
    }
    dimension = (n * sxy - sx * sy) / denominator;
    const double r = (n * sxy - sx * sy) / std::sqrt(denominator * (n * syy - sy * sy) + 1e-300);
    fit            = r * r;
}

/// Manhattan distance, in cells, between the two nearest cells of different land components: a
/// multi-source flood from every land cell, remembering which component reached a cell first; where
/// two floods meet, the gap is the sum of their distances. Returns +inf with fewer than two masses.
[[nodiscard]] inline double MinGapCells(const Field& f, const Components& land) {
    if (land.Count() < 2) {
        return std::numeric_limits<double>::infinity();
    }
    const std::size_t        count = f.width * f.depth;
    std::vector<std::int32_t> distance(count, -1);
    std::vector<std::uint32_t> owner(count, 0);
    std::vector<std::size_t>   queue;
    queue.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (land.label[i] != 0) {
            distance[i] = 0;
            owner[i]    = land.label[i];
            queue.push_back(i);
        }
    }
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t at = queue[head];
        const std::size_t x  = at % f.width;
        const std::size_t z  = at / f.width;
        const std::array<std::array<long long, 2>, 4> neighbours = {{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
        for (const auto& n : neighbours) {
            const long long nx = static_cast<long long>(x) + n[0];
            const long long nz = static_cast<long long>(z) + n[1];
            if (nx < 0 || nz < 0 || nx >= static_cast<long long>(f.width) || nz >= static_cast<long long>(f.depth)) {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(nz) * f.width + static_cast<std::size_t>(nx);
            if (distance[index] < 0) {
                distance[index] = distance[at] + 1;
                owner[index]    = owner[at];
                queue.push_back(index);
            } else if (owner[index] != owner[at]) {
                best = std::min(best, static_cast<double>(distance[index] + distance[at] + 1));
            }
        }
    }
    return best;
}

[[nodiscard]] inline Report Measure(const Field& f, double level = 0.0, int boxMin = 2, int boxMax = 7) {
    Report report;
    const std::size_t total = f.width * f.depth;

    Components land = Label(f.width, f.depth, [&](std::size_t x, std::size_t z) { return IsLand(f, x, z, level); });
    const double cellArea = f.resolution * f.resolution;
    std::size_t  landCells = 0;
    for (std::size_t id = 1; id < land.cells.size(); ++id) {
        landCells += land.cells[id];
        report.islandAreas.push_back(static_cast<double>(land.cells[id]) * cellArea);
    }
    std::sort(report.islandAreas.begin(), report.islandAreas.end(), std::greater<>());
    report.islands          = land.Count();
    report.landAreaFraction = total == 0 ? 0.0 : static_cast<double>(landCells) / static_cast<double>(total);
    report.dominantShare    = landCells == 0 ? 0.0
                              : report.islandAreas.front() / (static_cast<double>(landCells) * cellArea);

    // Water components that never reach the border are lagoons or lakes.
    Components water = Label(f.width, f.depth, [&](std::size_t x, std::size_t z) { return !IsLand(f, x, z, level); });
    std::vector<char> reachesBorder(water.cells.size(), 0);
    for (std::size_t x = 0; x < f.width; ++x) {
        reachesBorder[water.label[x]]                           = 1;
        reachesBorder[water.label[(f.depth - 1) * f.width + x]] = 1;
    }
    for (std::size_t z = 0; z < f.depth; ++z) {
        reachesBorder[water.label[z * f.width]]               = 1;
        reachesBorder[water.label[z * f.width + f.width - 1]] = 1;
    }
    for (std::size_t id = 1; id < water.cells.size(); ++id) {
        report.lagoons += reachesBorder[id] == 0 ? 1 : 0;
    }

    std::size_t edges = 0;
    for (std::size_t z = 0; z < f.depth; ++z) {
        for (std::size_t x = 0; x < f.width; ++x) {
            const bool here = IsLand(f, x, z, level);
            edges += (x + 1 < f.width && IsLand(f, x + 1, z, level) != here) ? 1 : 0;
            edges += (z + 1 < f.depth && IsLand(f, x, z + 1, level) != here) ? 1 : 0;
        }
    }
    // Boundary edges of a pixelated curve overstate its length by up to sqrt(2) on diagonals; the
    // number is only comparable between fields of the same resolution.
    report.coastLength = static_cast<double>(edges) * f.resolution;
    const double areaKm2 = static_cast<double>(total) * cellArea / 1e6;
    report.coastPerKm2   = areaKm2 > 0.0 ? (report.coastLength / 1000.0) / areaKm2 : 0.0;

    BoxDimension(f, level, boxMin, boxMax, report.boxDimension, report.boxFit);

    const double gap = MinGapCells(f, land);
    report.minChannel = std::isfinite(gap) ? gap * f.resolution : 0.0;

    std::vector<float> heights;
    heights.reserve(landCells);
    for (std::size_t i = 0; i < total; ++i) {
        if (static_cast<double>(f.values[i]) > level) {
            heights.push_back(f.values[i]);
        }
    }
    if (!heights.empty()) {
        std::sort(heights.begin(), heights.end());
        const std::array<double, 4> p = {0.10, 0.50, 0.90, 0.99};
        for (std::size_t k = 0; k < 4; ++k) {
            report.hypsometry[k] = static_cast<double>(
                heights[std::min(heights.size() - 1, static_cast<std::size_t>(p[k] * static_cast<double>(heights.size())))]);
        }
    }
    return report;
}

/// Mean slope, in degrees, of the cells whose depth below `level` lies in [from, to) metres: the
/// shelf (shallow, gentle), the break and the slope (steeper), the abyssal plain (flat again).
[[nodiscard]] inline double MeanSlopeInDepth(const Field& f, double level, double from, double to) {
    double sum   = 0.0;
    double count = 0.0;
    for (std::size_t z = 1; z + 1 < f.depth; ++z) {
        for (std::size_t x = 1; x + 1 < f.width; ++x) {
            const double depth = level - static_cast<double>(f.At(x, z));
            if (depth < from || depth >= to) {
                continue;
            }
            const double gx = (static_cast<double>(f.At(x + 1, z)) - static_cast<double>(f.At(x - 1, z))) / (2.0 * f.resolution);
            const double gz = (static_cast<double>(f.At(x, z + 1)) - static_cast<double>(f.At(x, z - 1))) / (2.0 * f.resolution);
            sum += std::atan(std::hypot(gx, gz)) * 57.29577951308232;
            count += 1.0;
        }
    }
    return count > 0.0 ? sum / count : 0.0;
}

} // namespace metrics
