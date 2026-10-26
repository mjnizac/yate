#pragma once

// Relief measurements of a height field: slopes, local relief, closed depressions, drainage, the power
// spectrum and grid artefacts, plus a hillshade with cast shadows. They are what a terrain example is
// accepted against, next to the land/sea numbers in terrain_metrics.hpp, and header-only and standard
// library only for the same reason: `test_relief`, `test_library` and the `terrain_metrics` tool share
// them.
//
// Cells at or below `level` are sea: outlets for the drainage, left out of every other measure. Pass
// -infinity for a field without sea. Derivatives are central differences, so the outer ring of samples
// is skipped.

#include "terrain_metrics.hpp"

#include <complex>
#include <functional>
#include <queue>
#include <utility>

namespace metrics {

struct ReliefOptions {
    double level = 0.0;
    /// Side, in metres, of the squares local relief is taken over.
    double block = 1000.0;
    /// Drainage area, in square metres, from which a cell counts as a channel.
    double channelArea = 1e5;
    /// Depth, in metres, a filled cell must exceed to count as part of a closed depression.
    double pitDepth = 0.1;
};

struct Relief {
    /// Slope in degrees at the 50th, 90th, 99th and 99.9th percentiles.
    std::array<double, 4> slope{};
    /// Share of land cells steeper than 35 and 45 degrees.
    double steep35 = 0.0;
    double steep45 = 0.0;
    /// Mean of max - min over the blocks, metres.
    double localRelief = 0.0;
    /// (mean - min) / (max - min) of the land heights: low for a plain with peaks, high for a plateau.
    double hypsometricIntegral = 0.0;
    /// 90th percentile of |laplacian|, 1/m.
    double curvatureP90 = 0.0;
    /// Closed depressions: their count, their share of the land and the deepest, metres.
    std::size_t pits = 0;
    double pitShare = 0.0;
    double pitDepth = 0.0;
    /// Channel length per land area, km per square km.
    double drainageDensity = 0.0;
    /// Exponent beta of P(k) ~ k^-beta over the radially averaged spectrum, and the fit's R^2.
    double spectralSlope = 0.0;
    double spectralFit = 0.0;
    /// Spectral power on the axes over the power on the diagonals, each relative to its ring, in the
    /// upper band (a quarter to a half of the sampling rate): 1 for an isotropic surface, above it for
    /// structure aligned with the grid. A spatial ratio of second
    /// differences was tried first and rejected: it reads 1.38 on an isotropic band-limited field,
    /// because near the Nyquist rate a second difference is not isotropic.
    double axisDiagonalRatio = 0.0;
};

[[nodiscard]] inline double Percentile(std::vector<double>& values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t at = std::min(values.size() - 1, static_cast<std::size_t>(p * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

/// In-place radix-2 FFT; `inverse` flips the sign of the exponent and does not normalise.
inline void Fft(std::vector<std::complex<double>>& a, bool inverse = false) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * 3.14159265358979323846 / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < n; start += length) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < length / 2; ++k) {
                const std::complex<double> u = a[start + k];
                const std::complex<double> v = a[start + k + length / 2] * w;
                a[start + k]              = u + v;
                a[start + k + length / 2] = u - v;
                w *= step;
            }
        }
    }
}

/// Rows then columns of an n x n grid, x fastest.
inline void Fft2(std::vector<std::complex<double>>& grid, std::size_t n, bool inverse = false) {
    std::vector<std::complex<double>> line(n);
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t r = 0; r < n; ++r) {
            for (std::size_t i = 0; i < n; ++i) {
                line[i] = pass == 0 ? grid[r * n + i] : grid[i * n + r];
            }
            Fft(line, inverse);
            for (std::size_t i = 0; i < n; ++i) {
                (pass == 0 ? grid[r * n + i] : grid[i * n + r]) = line[i];
            }
        }
    }
}

/// Slope of a least-squares line through (xs, ys) and its R^2.
inline void FitLine(const std::vector<double>& xs, const std::vector<double>& ys, double& slope, double& fit) {
    const double n = static_cast<double>(xs.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        sx += xs[i];
        sy += ys[i];
        sxx += xs[i] * xs[i];
        sxy += xs[i] * ys[i];
        syy += ys[i] * ys[i];
    }
    const double denominator = n * sxx - sx * sx;
    slope = denominator != 0.0 ? (n * sxy - sx * sy) / denominator : 0.0;
    const double r = (n * sxy - sx * sy) / std::sqrt(denominator * (n * syy - sy * sy) + 1e-300);
    fit = r * r;
}

/// Spectral exponent over the largest centred power-of-two square (at most 1024), detrended by a plane
/// and Hann-windowed, fitted between 4 cycles and a quarter of the square; and the axis-to-diagonal
/// power ratio of `Relief::axisDiagonalRatio` from the same spectrum.
inline void SpectralSlope(const Field& f, double& beta, double& fit, double& anisotropy) {
    beta = fit = anisotropy = 0.0;
    std::size_t n = 1;
    while (n * 2 <= std::min({f.width, f.depth, std::size_t{1024}})) {
        n *= 2;
    }
    if (n < 64) {
        return;
    }
    const std::size_t x0 = (f.width - n) / 2;
    const std::size_t z0 = (f.depth - n) / 2;
    // The plane through the square, by least squares on centred coordinates.
    double mean = 0, sxh = 0, szh = 0, sxx = 0;
    const double c = (static_cast<double>(n) - 1.0) / 2.0;
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            const double h = f.At(x0 + x, z0 + z);
            mean += h;
            sxh += (static_cast<double>(x) - c) * h;
            szh += (static_cast<double>(z) - c) * h;
        }
    }
    for (std::size_t x = 0; x < n; ++x) {
        sxx += (static_cast<double>(x) - c) * (static_cast<double>(x) - c);
    }
    mean /= static_cast<double>(n * n);
    const double ax = sxh / (sxx * static_cast<double>(n));
    const double az = szh / (sxx * static_cast<double>(n));
    std::vector<std::complex<double>> grid(n * n);
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            const double wx = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(x) / static_cast<double>(n - 1));
            const double wz = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(z) / static_cast<double>(n - 1));
            const double h  = f.At(x0 + x, z0 + z) - mean - ax * (static_cast<double>(x) - c) - az * (static_cast<double>(z) - c);
            grid[z * n + x] = h * wx * wz;
        }
    }
    Fft2(grid, n);
    const auto wave = [n](std::size_t i) { return static_cast<double>(i <= n / 2 ? i : n - i); };
    const auto ring = [&](std::size_t x, std::size_t z) {
        return static_cast<std::size_t>(std::lround(std::hypot(wave(x), wave(z))));
    };
    std::vector<double> power(n, 0.0);
    std::vector<double> count(n, 0.0);
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            const std::size_t k = ring(x, z);
            if (k < n) {
                power[k] += std::norm(grid[z * n + x]);
                count[k] += 1.0;
            }
        }
    }
    // The cells exactly on the axes against those exactly on the diagonals, each over the mean power of
    // its ring so the spectrum's slope cancels. A structure aligned with the grid puts its energy on
    // those lines; an isotropic surface spreads it evenly.
    std::array<double, 2> line{};
    std::array<double, 2> lineCount{};
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            const std::size_t k    = ring(x, z);
            const int         side = (x == 0) != (z == 0) ? 0 : wave(x) == wave(z) && x != 0 ? 1 : -1;
            if (side < 0 || k < n / 4 || k >= n / 2 || power[k] <= 0.0) {
                continue;
            }
            line[static_cast<std::size_t>(side)] += std::norm(grid[z * n + x]) / (power[k] / count[k]);
            lineCount[static_cast<std::size_t>(side)] += 1.0;
        }
    }
    if (line[1] > 0.0 && lineCount[0] > 0.0) {
        anisotropy = (line[0] / lineCount[0]) / (line[1] / lineCount[1]);
    }
    std::vector<double> xs;
    std::vector<double> ys;
    for (std::size_t k = 4; k <= n / 4; ++k) {
        if (count[k] > 0.0 && power[k] > 0.0) {
            xs.push_back(std::log(static_cast<double>(k)));
            ys.push_back(std::log(power[k] / count[k]));
        }
    }
    if (xs.size() >= 3) {
        FitLine(xs, ys, beta, fit);
        beta = -beta;
    }
}

constexpr std::array<std::array<int, 2>, 8> kD8 = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}}};

/// Fills the closed depressions by a priority flood from the edge and the sea, with a tiny gradient
/// across flats so every land cell drains (Barnes et al. 2014, "+epsilon"), then routes D8 on the
/// filled surface and accumulates drainage area from the top down.
inline void Drainage(const Field& f, const ReliefOptions& o, Relief& r) {
    const std::size_t count = f.width * f.depth;
    const auto land = [&](std::size_t i) { return static_cast<double>(f.values[i]) > o.level; };
    std::vector<double>      filled(count);
    std::vector<char>        done(count, 0);
    std::vector<std::size_t> order;
    order.reserve(count);
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t x = i % f.width;
        const std::size_t z = i / f.width;
        if (!land(i) || x == 0 || z == 0 || x + 1 == f.width || z + 1 == f.depth) {
            filled[i] = f.values[i];
            done[i]   = 1;
            open.emplace(filled[i], i);
        }
    }
    constexpr double kEpsilon = 1e-6;
    while (!open.empty()) {
        const std::size_t at = open.top().second;
        open.pop();
        order.push_back(at);
        const long long x = static_cast<long long>(at % f.width);
        const long long z = static_cast<long long>(at / f.width);
        for (const auto& d : kD8) {
            const long long nx = x + d[0];
            const long long nz = z + d[1];
            if (nx < 0 || nz < 0 || nx >= static_cast<long long>(f.width) || nz >= static_cast<long long>(f.depth)) {
                continue;
            }
            const std::size_t q = static_cast<std::size_t>(nz) * f.width + static_cast<std::size_t>(nx);
            if (done[q] == 0) {
                done[q]   = 1;
                filled[q] = std::max(static_cast<double>(f.values[q]), filled[at] + kEpsilon);
                open.emplace(filled[q], q);
            }
        }
    }

    std::size_t landCells = 0;
    std::vector<char> pit(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (!land(i)) {
            continue;
        }
        ++landCells;
        const double depth = filled[i] - static_cast<double>(f.values[i]);
        if (depth > o.pitDepth) {
            pit[i]     = 1;
            r.pitDepth = std::max(r.pitDepth, depth);
        }
    }
    const Components pits = Label(f.width, f.depth, [&](std::size_t x, std::size_t z) { return pit[z * f.width + x] != 0; });
    std::size_t pitCells = 0;
    for (std::size_t id = 1; id < pits.cells.size(); ++id) {
        pitCells += pits.cells[id];
    }
    r.pits     = pits.Count();
    r.pitShare = landCells == 0 ? 0.0 : static_cast<double>(pitCells) / static_cast<double>(landCells);

    // Steepest descent on the filled surface. Cells on the edge or in the sea are outlets.
    std::vector<std::int64_t> receiver(count, -1);
    std::vector<double>       step(count, 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t x = i % f.width;
        const std::size_t z = i / f.width;
        if (!land(i) || x == 0 || z == 0 || x + 1 == f.width || z + 1 == f.depth) {
            continue;
        }
        double best = 0.0;
        for (const auto& d : kD8) {
            const auto        q        = static_cast<std::size_t>(static_cast<long long>(i) + d[1] * static_cast<long long>(f.width) + d[0]);
            const double      distance = (d[0] != 0 && d[1] != 0 ? 1.41421356237 : 1.0) * f.resolution;
            const double      drop     = (filled[i] - filled[q]) / distance;
            if (drop > best) {
                best        = drop;
                receiver[i] = static_cast<std::int64_t>(q);
                step[i]     = distance;
            }
        }
    }
    const double cellArea = f.resolution * f.resolution;
    std::vector<double> area(count, 0.0);
    double channel = 0.0;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const std::size_t i = *it;
        if (!land(i)) {
            continue;
        }
        area[i] += cellArea;
        if (receiver[i] >= 0) {
            area[static_cast<std::size_t>(receiver[i])] += area[i];
            channel += area[i] >= o.channelArea ? step[i] : 0.0;
        }
    }
    const double landKm2 = static_cast<double>(landCells) * cellArea / 1e6;
    r.drainageDensity    = landKm2 > 0.0 ? (channel / 1000.0) / landKm2 : 0.0;
}

[[nodiscard]] inline Relief MeasureRelief(const Field& f, const ReliefOptions& o = {}) {
    Relief r;
    const auto land = [&](std::size_t x, std::size_t z) { return static_cast<double>(f.At(x, z)) > o.level; };
    const auto h    = [&](std::size_t x, std::size_t z) { return static_cast<double>(f.At(x, z)); };
    const double res = f.resolution;

    std::vector<double> slopes;
    std::vector<double> curvatures;
    double lowest = std::numeric_limits<double>::infinity();
    double highest = -lowest;
    double sum = 0.0;
    std::size_t cells = 0;
    for (std::size_t z = 0; z < f.depth; ++z) {
        for (std::size_t x = 0; x < f.width; ++x) {
            if (!land(x, z)) {
                continue;
            }
            lowest  = std::min(lowest, h(x, z));
            highest = std::max(highest, h(x, z));
            sum += h(x, z);
            ++cells;
            if (x == 0 || z == 0 || x + 1 == f.width || z + 1 == f.depth) {
                continue;
            }
            const double c  = h(x, z);
            const double gx = (h(x + 1, z) - h(x - 1, z)) / (2.0 * res);
            const double gz = (h(x, z + 1) - h(x, z - 1)) / (2.0 * res);
            slopes.push_back(std::atan(std::hypot(gx, gz)) * 57.29577951308232);
            const double dxx = h(x + 1, z) - 2.0 * c + h(x - 1, z);
            const double dzz = h(x, z + 1) - 2.0 * c + h(x, z - 1);
            curvatures.push_back(std::fabs(dxx + dzz) / (res * res));
        }
    }
    if (slopes.empty()) {
        return r;
    }
    double steep35 = 0, steep45 = 0;
    for (const double s : slopes) {
        steep35 += s > 35.0 ? 1.0 : 0.0;
        steep45 += s > 45.0 ? 1.0 : 0.0;
    }
    r.steep35 = steep35 / static_cast<double>(slopes.size());
    r.steep45 = steep45 / static_cast<double>(slopes.size());
    const std::array<double, 4> p = {0.5, 0.9, 0.99, 0.999};
    for (std::size_t k = 0; k < 4; ++k) {
        r.slope[k] = Percentile(slopes, p[k]);
    }
    r.curvatureP90 = Percentile(curvatures, 0.9);
    r.hypsometricIntegral = highest > lowest ? (sum / static_cast<double>(cells) - lowest) / (highest - lowest) : 0.0;

    // Local relief over whole blocks that are at least half land.
    const std::size_t b = std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(o.block / res)));
    double relief = 0.0;
    double blocks = 0.0;
    for (std::size_t bz = 0; bz + b <= f.depth; bz += b) {
        for (std::size_t bx = 0; bx + b <= f.width; bx += b) {
            double lo = std::numeric_limits<double>::infinity();
            double hi = -lo;
            std::size_t inside = 0;
            for (std::size_t z = bz; z < bz + b; ++z) {
                for (std::size_t x = bx; x < bx + b; ++x) {
                    if (land(x, z)) {
                        lo = std::min(lo, h(x, z));
                        hi = std::max(hi, h(x, z));
                        ++inside;
                    }
                }
            }
            if (inside * 2 >= b * b) {
                relief += hi - lo;
                blocks += 1.0;
            }
        }
    }
    r.localRelief = blocks > 0.0 ? relief / blocks : 0.0;

    Drainage(f, o, r);
    SpectralSlope(f, r.spectralSlope, r.spectralFit, r.axisDiagonalRatio);
    return r;
}

/// A grey hillshade, 8 bits per sample, z down the image: Lambert from a sun at `azimuth` degrees
/// (0 from -z, the top of the image; 90 from +x; 315 the top left) and `elevation` degrees, and a cast
/// shadow swept along the sun's direction. The sweep needs a whole step on the grid, so the azimuth is
/// rounded to a multiple of 45 degrees. Shadowed ground keeps the ambient term only.
[[nodiscard]] inline std::vector<std::uint8_t> Hillshade(const Field& f, double azimuth = 315.0, double elevation = 35.0) {
    const double rad = 3.14159265358979323846 / 180.0;
    const int    octant = (static_cast<int>(std::lround(azimuth / 45.0)) % 8 + 8) % 8;
    const double a      = octant * 45.0 * rad;
    const int    sx     = static_cast<int>(std::lround(std::sin(a)));
    const int    sz     = static_cast<int>(std::lround(-std::cos(a)));
    const double length = std::hypot(static_cast<double>(sx), static_cast<double>(sz));
    const double e      = elevation * rad;
    const double drop   = length * f.resolution * std::tan(e);
    const double lx = sx / length * std::cos(e), ly = std::sin(e), lz = sz / length * std::cos(e);

    std::vector<double>       shadow(f.width * f.depth, -std::numeric_limits<double>::infinity());
    std::vector<std::uint8_t> image(f.width * f.depth, 0);
    // Visit every cell after the one towards the sun, so its shadow line is known.
    for (std::size_t zi = 0; zi < f.depth; ++zi) {
        const std::size_t z = sz < 0 ? zi : f.depth - 1 - zi;
        for (std::size_t xi = 0; xi < f.width; ++xi) {
            const std::size_t x  = sx > 0 ? f.width - 1 - xi : xi;
            const long long   px = static_cast<long long>(x) + sx;
            const long long   pz = static_cast<long long>(z) + sz;
            double line = -std::numeric_limits<double>::infinity();
            if (px >= 0 && pz >= 0 && px < static_cast<long long>(f.width) && pz < static_cast<long long>(f.depth)) {
                const std::size_t prev = static_cast<std::size_t>(pz) * f.width + static_cast<std::size_t>(px);
                line = std::max(shadow[prev], static_cast<double>(f.values[prev])) - drop;
            }
            const std::size_t i = z * f.width + x;
            shadow[i] = line;
            const std::size_t x0 = x == 0 ? 0 : x - 1, x1 = std::min(x + 1, f.width - 1);
            const std::size_t z0 = z == 0 ? 0 : z - 1, z1 = std::min(z + 1, f.depth - 1);
            const double gx = (f.At(x1, z) - f.At(x0, z)) / (static_cast<double>(x1 - x0) * f.resolution);
            const double gz = (f.At(x, z1) - f.At(x, z0)) / (static_cast<double>(z1 - z0) * f.resolution);
            const double lambert = std::max(0.0, (-gx * lx + ly - gz * lz) / std::sqrt(gx * gx + 1.0 + gz * gz));
            const double lit     = static_cast<double>(f.values[i]) < line ? 0.0 : lambert;
            image[i] = static_cast<std::uint8_t>(std::lround(255.0 * (0.2 + 0.8 * lit)));
        }
    }
    return image;
}

/// An 8-bit greyscale PNG with stored (uncompressed) deflate blocks: no zlib needed, larger files.
[[nodiscard]] inline bool WriteGreyPng(const std::string& path, const std::vector<std::uint8_t>& pixels, std::size_t width,
                                       std::size_t height) {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const auto be32  = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int s = 24; s >= 0; s -= 8) {
            v.push_back(static_cast<std::uint8_t>(x >> s));
        }
    };
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        be32(out, static_cast<std::uint32_t>(data.size()));
        std::uint32_t crc = 0xFFFFFFFFu;
        const auto    add = [&](std::uint8_t b) {
            out.push_back(b);
            crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
        };
        for (int i = 0; i < 4; ++i) {
            add(static_cast<std::uint8_t>(type[i]));
        }
        for (const std::uint8_t b : data) {
            add(b);
        }
        be32(out, crc ^ 0xFFFFFFFFu);
    };
    std::vector<std::uint8_t> header;
    be32(header, static_cast<std::uint32_t>(width));
    be32(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8, 0, 0, 0, 0});
    chunk("IHDR", header);

    std::vector<std::uint8_t> raw;
    raw.reserve((width + 1) * height);
    for (std::size_t z = 0; z < height; ++z) {
        raw.push_back(0);
        raw.insert(raw.end(), pixels.begin() + static_cast<std::ptrdiff_t>(z * width),
                   pixels.begin() + static_cast<std::ptrdiff_t>((z + 1) * width));
    }
    std::vector<std::uint8_t> zlib = {0x78, 0x01};
    for (std::size_t at = 0; at < raw.size(); at += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        zlib.push_back(at + n == raw.size() ? 1 : 0);
        zlib.insert(zlib.end(), {static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8),
                                 static_cast<std::uint8_t>(~n), static_cast<std::uint8_t>(~n >> 8)});
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
    }
    std::uint32_t s1 = 1, s2 = 0;
    for (const std::uint8_t b : raw) {
        s1 = (s1 + b) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    be32(zlib, (s2 << 16) | s1);
    chunk("IDAT", zlib);
    chunk("IEND", {});

    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(stream);
}

} // namespace metrics
