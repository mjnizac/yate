// The relief metrics and the hillshade against fields whose answers are known.
//
// Terrain examples are accepted against these numbers, so what is under test is the ruler: a plane has
// one slope everywhere and no pits, a V valley drains along its floor and nowhere else, a bowl is one
// closed depression as deep as its lowest rim, a checkerboard is all axis ripple, a field synthesised
// with a power-law spectrum gives its exponent back, and a cliff casts a shadow as long as its height
// over the tangent of the sun. Tolerances are what the discretisation costs, not numbers picked to pass.

#include "test_support.hpp"
#include "relief_metrics.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr double kNoSea = -std::numeric_limits<double>::infinity();
constexpr double kDegrees = 57.29577951308232;

template <typename Height>
[[nodiscard]] metrics::Field Make(std::size_t side, double resolution, Height height) {
    metrics::Field f;
    f.width      = side;
    f.depth      = side;
    f.resolution = resolution;
    f.values.resize(side * side);
    for (std::size_t z = 0; z < side; ++z) {
        for (std::size_t x = 0; x < side; ++x) {
            f.values[z * side + x] = static_cast<float>(height(static_cast<double>(x), static_cast<double>(z)));
        }
    }
    return f;
}

/// A random field with radially averaged power k^-beta, real part of an inverse FFT of random phases.
[[nodiscard]] metrics::Field PowerLaw(double beta, std::size_t n = 512, std::uint64_t seed = 0x9E3779B97F4A7C15ull) {
    std::vector<std::complex<double>> grid(n * n);
    std::uint64_t state = seed;
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const double phase = static_cast<double>(state >> 11) / 9007199254740992.0 * 6.283185307179586;
            const double kx = static_cast<double>(x <= n / 2 ? x : n - x);
            const double kz = static_cast<double>(z <= n / 2 ? z : n - z);
            const double k  = std::hypot(kx, kz);
            // Only inside the inscribed disc: the corners of the square spectrum are a checkerboard,
            // which is the grid artefact the axis-to-diagonal ratio is there to catch.
            const bool inside = k > 0.0 && k <= static_cast<double>(n / 2);
            grid[z * n + x] = inside ? std::polar(std::pow(k, -beta / 2.0), phase) : 0.0;
        }
    }
    metrics::Fft2(grid, n, true);
    metrics::Field f;
    f.width = f.depth = n;
    f.resolution      = 10.0;
    f.values.resize(n * n);
    for (std::size_t i = 0; i < n * n; ++i) {
        f.values[i] = static_cast<float>(grid[i].real() * 1e3);
    }
    return f;
}

void TestPlane() {
    test::Section("a plane has one slope, no pits and a hypsometric integral of one half");
    const metrics::Field f = Make(256, 10.0, [](double x, double) { return 100.0 + 0.5 * x * 10.0; });
    const metrics::Relief r = metrics::MeasureRelief(f, {.level = kNoSea});
    const double expected = std::atan(0.5) * kDegrees;
    for (const double s : r.slope) {
        CHECK(std::fabs(s - expected) < 1e-3);
    }
    CHECK_EQ(r.steep35, 0.0);
    CHECK_EQ(r.pits, std::size_t{0});
    CHECK(std::fabs(r.hypsometricIntegral - 0.5) < 0.01);
    CHECK(r.curvatureP90 < 1e-6);
    // 1000 m blocks are 100 samples: the height changes over 99 steps of 5 m.
    CHECK(std::fabs(r.localRelief - 495.0) < 0.5);

    const metrics::Field steep = Make(128, 10.0, [](double x, double) { return x * 10.0 * std::tan(40.0 / kDegrees); });
    const metrics::Relief s = metrics::MeasureRelief(steep, {.level = kNoSea});
    CHECK_EQ(s.steep35, 1.0);
    CHECK_EQ(s.steep45, 0.0);
}

void TestValley() {
    test::Section("a V valley drains along its floor only");
    // Sides at 0.3 towards the floor (z = 128), the floor down at 0.05 towards x = 0. A side cell's
    // steepest drop is straight to the floor (0.3 against 0.247 on the diagonal), so the only cells over
    // 1000 samples of area are on the floor, from x = 1 to x = 251: 2.51 km of channel on 6.55 km2.
    const metrics::Field f = Make(256, 10.0, [](double x, double z) { return std::fabs(z - 128.0) * 3.0 + x * 0.5; });
    const metrics::Relief r = metrics::MeasureRelief(f, {.level = kNoSea, .channelArea = 1e5});
    CHECK(std::fabs(r.drainageDensity - 2.51 / 6.5536) < 0.003);
    CHECK_EQ(r.pits, std::size_t{0});
}

void TestBowl() {
    test::Section("a bowl is one closed depression as deep as its lowest rim");
    const metrics::Field f = Make(256, 10.0, [](double x, double z) {
        const double r = std::hypot(x - 128.0, z - 128.0) * 10.0;
        return 200.0 * (1.0 - std::exp(-(r * r) / (400.0 * 400.0)));
    });
    double rim = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < 256; ++i) {
        rim = std::min({rim, static_cast<double>(f.At(i, 0)), static_cast<double>(f.At(0, i)),
                        static_cast<double>(f.At(i, 255)), static_cast<double>(f.At(255, i))});
    }
    // Every cell more than the pit depth under the rim, except the edge, which is an outlet.
    double below = 0.0;
    for (const float v : f.values) {
        below += static_cast<double>(v) < rim - 0.1 ? 1.0 : 0.0;
    }
    const metrics::Relief r = metrics::MeasureRelief(f, {.level = kNoSea});
    CHECK_EQ(r.pits, std::size_t{1});
    CHECK(std::fabs(r.pitDepth - (rim - static_cast<double>(f.At(128, 128)))) < 0.01);
    CHECK(std::fabs(r.pitShare - below / (256.0 * 256.0)) < 1e-3);
}

void TestRipple() {
    test::Section("a ripple along the grid is anisotropic, a power-law field is not");
    const metrics::Field smoothField = PowerLaw(3.0);
    const metrics::Relief smooth = metrics::MeasureRelief(smoothField, {.level = kNoSea});
    // The same field with an offset per column, as a seam or a row-wise residue would leave, as large
    // as the field's own mean step between neighbours: all of its energy lies on one axis.
    double step = 0.0;
    for (std::size_t i = 1; i < smoothField.values.size(); ++i) {
        step += std::fabs(static_cast<double>(smoothField.values[i]) - smoothField.values[i - 1]);
    }
    step /= static_cast<double>(smoothField.values.size());
    metrics::Field rippled = smoothField;
    std::uint64_t  state   = 12345;
    std::vector<double> column(rippled.width);
    for (double& c : column) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        c     = (static_cast<double>(state >> 11) / 9007199254740992.0 - 0.5) * 2.0 * step;
    }
    for (std::size_t i = 0; i < rippled.values.size(); ++i) {
        rippled.values[i] += static_cast<float>(column[i % rippled.width]);
    }
    const metrics::Relief ripple = metrics::MeasureRelief(rippled, {.level = kNoSea});
    std::printf("     axis/diagonal: isotropic %.3f, rippled %.3f\n", smooth.axisDiagonalRatio, ripple.axisDiagonalRatio);
    // Over six seeds an isotropic 512 square reads 0.96 to 1.15: the spread of a few hundred cells.
    CHECK(smooth.axisDiagonalRatio > 0.7 && smooth.axisDiagonalRatio < 1.3);
    CHECK(ripple.axisDiagonalRatio > 2.0);
}

void TestSpectrum() {
    test::Section("the spectral slope gives a synthesised exponent back");
    for (const double beta : {2.0, 3.0, 4.0}) {
        double measured = 0.0;
        double fit      = 0.0;
        double ratio    = 0.0;
        metrics::SpectralSlope(PowerLaw(beta), measured, fit, ratio);
        std::printf("     beta %.1f measured %.3f (R2 %.3f)\n", beta, measured, fit);
        CHECK(std::fabs(measured - beta) < 0.3);
        CHECK(fit > 0.9);
    }
}

void TestHillshade() {
    test::Section("a cliff casts a shadow as long as its height over tan(elevation)");
    // 100 m high to the left of x = 128, sun from the left at 45 degrees, 10 m samples: the shadow line
    // falls 10 m per sample and meets the ground at x = 137, so x = 128..136 are in shadow.
    const metrics::Field f = Make(256, 10.0, [](double x, double) { return x < 128.0 ? 100.0 : 0.0; });
    const std::vector<std::uint8_t> image = metrics::Hillshade(f, 270.0, 45.0);
    const auto at = [&](std::size_t x) { return image[128 * 256 + x]; };
    const auto lit = static_cast<std::uint8_t>(std::lround(255.0 * (0.2 + 0.8 * std::sin(45.0 / kDegrees))));
    const auto dark = static_cast<std::uint8_t>(std::lround(255.0 * 0.2));
    CHECK_EQ(at(100), lit);
    for (std::size_t x = 129; x <= 136; ++x) {
        CHECK_EQ(at(x), dark);
    }
    CHECK_EQ(at(138), lit);
    // The same cliff lit from the right casts nothing on the low ground.
    CHECK_EQ(metrics::Hillshade(f, 90.0, 45.0)[128 * 256 + 132], lit);

    const char* path = "out/test_relief_hillshade.png";
    CHECK(metrics::WriteGreyPng(path, image, 256, 256));
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    // Signature, IHDR, IDAT (zlib header, two stored blocks of headers, the rows, adler), IEND.
    const std::size_t rows = 257 * 256;
    CHECK_EQ(static_cast<std::size_t>(stream.tellg()), std::size_t{8 + 25 + 12 + 2 + 2 * 5 + rows + 4 + 12});
}

} // namespace

int main() {
    TestPlane();
    TestValley();
    TestBowl();
    TestRipple();
    TestSpectrum();
    TestHillshade();
    return test::Summary("test_relief");
}
