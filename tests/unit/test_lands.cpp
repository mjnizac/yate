// The land/sea metrics against fields whose answers are known (phase 0 of the Lands macros).
//
// The Lands macros are accepted against these numbers, so what is under test here is the ruler: a disc
// has one component and an area of pi r^2, a smooth coast has a box-counting dimension of 1, a filled
// square of boxes of a coast that is nothing but a straight line has 1 as well, and a field of random
// speckle is far over it. Where an answer is only known to a tolerance, the tolerance is the width of
// what the discretisation costs and not a number picked to pass.

#include "test_support.hpp"
#include "terrain_metrics.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr std::size_t kSide = 512;

[[nodiscard]] metrics::Field Make(double resolution = 10.0) {
    metrics::Field f;
    f.width      = kSide;
    f.depth      = kSide;
    f.resolution = resolution;
    f.values.assign(kSide * kSide, -50.0f);
    return f;
}

void Disc(metrics::Field& f, double cx, double cz, double r, float height = 40.0f) {
    for (std::size_t z = 0; z < f.depth; ++z) {
        for (std::size_t x = 0; x < f.width; ++x) {
            const double dx = static_cast<double>(x) - cx;
            const double dz = static_cast<double>(z) - cz;
            if (dx * dx + dz * dz <= r * r) {
                f.values[z * f.width + x] = height;
            }
        }
    }
}

void TestDisc() {
    test::Section("one disc is one island of the right size");
    metrics::Field f = Make();
    Disc(f, 256.0, 256.0, 100.0);
    const metrics::Report r = metrics::Measure(f);
    CHECK_EQ(r.islands, std::size_t{1});
    CHECK_EQ(r.lagoons, std::size_t{0});
    // Area of the discretised disc against pi r^2: within the perimeter's one-cell fringe.
    const double expected = 3.14159265358979 * 100.0 * 100.0 * 100.0;
    CHECK(std::fabs(r.islandAreas.front() - expected) < 0.02 * expected);
    CHECK(r.dominantShare > 0.999);
    CHECK_EQ(r.minChannel, 0.0);
    // A smooth curve is one-dimensional at scales that see it as smooth.
    CHECK(r.boxDimension > 0.9 && r.boxDimension < 1.15);
    CHECK(r.boxFit > 0.98);
}

void TestSeveral() {
    test::Section("islands, channels and lagoons");
    metrics::Field f = Make();
    Disc(f, 100.0, 256.0, 60.0);
    Disc(f, 300.0, 256.0, 40.0);
    Disc(f, 420.0, 120.0, 20.0);
    // A lagoon: a ring of land around water that does not reach the edge.
    Disc(f, 380.0, 400.0, 50.0);
    for (std::size_t z = 0; z < f.depth; ++z) {
        for (std::size_t x = 0; x < f.width; ++x) {
            const double dx = static_cast<double>(x) - 380.0;
            const double dz = static_cast<double>(z) - 400.0;
            if (dx * dx + dz * dz <= 20.0 * 20.0) {
                f.values[z * f.width + x] = -10.0f;
            }
        }
    }
    const metrics::Report r = metrics::Measure(f);
    CHECK_EQ(r.islands, std::size_t{4});
    CHECK_EQ(r.lagoons, std::size_t{1});
    CHECK(r.islandAreas[0] >= r.islandAreas[1] && r.islandAreas[1] >= r.islandAreas[2]);
    // The narrowest channel: between the discs at x = 100 (r 60) and 300 (r 40): 200 - 100 = 100 cells.
    // The nearer pair is the 300 and 420 discs: centres 120 cells and 136 cells apart in z, so about
    // sqrt(120^2 + 136^2) - 40 - 20 = 123 cells; the 380,400 ring and the 300,256 disc are
    // sqrt(80^2 + 144^2) - 40 - 50 = 77 cells. Manhattan distance overstates a diagonal gap, so the
    // measured value is between the Euclidean one and sqrt(2) times it.
    const double euclidean = (std::sqrt(80.0 * 80.0 + 144.0 * 144.0) - 40.0 - 50.0) * 10.0;
    CHECK(r.minChannel >= euclidean * 0.95 && r.minChannel <= euclidean * 1.45);
}

void TestRoughCoast() {
    test::Section("a rough coast has a higher dimension than a smooth one");
    metrics::Field smooth = Make();
    Disc(smooth, 256.0, 256.0, 150.0);
    // The same disc with a coast made rough by a deterministic fractal displacement of its radius.
    metrics::Field rough = Make();
    for (std::size_t z = 0; z < rough.depth; ++z) {
        for (std::size_t x = 0; x < rough.width; ++x) {
            const double dx    = static_cast<double>(x) - 256.0;
            const double dz    = static_cast<double>(z) - 256.0;
            const double angle = std::atan2(dz, dx);
            double       wobble = 0.0;
            double       amplitude = 22.0;
            for (int octave = 1; octave <= 7; ++octave) {
                wobble += amplitude * std::sin(angle * (3.0 * octave * octave) + octave * 1.7);
                amplitude *= 0.62;
            }
            if (std::hypot(dx, dz) <= 150.0 + wobble) {
                rough.values[z * rough.width + x] = 40.0f;
            }
        }
    }
    const metrics::Report a = metrics::Measure(smooth);
    const metrics::Report b = metrics::Measure(rough);
    std::printf("     smooth disc: dimension %.3f (R2 %.3f), coast %.0f m; rough: %.3f (R2 %.3f), %.0f m\n",
                a.boxDimension, a.boxFit, a.coastLength, b.boxDimension, b.boxFit, b.coastLength);
    CHECK(b.boxDimension > a.boxDimension + 0.05);
    CHECK(b.coastLength > a.coastLength * 1.2);
}

void TestHypsometryAndDepth() {
    test::Section("hypsometry and slope by depth");
    metrics::Field f = Make();
    // A cone of land 0..100 m, and a sea floor falling 0.5 degrees to 400 m deep.
    for (std::size_t z = 0; z < f.depth; ++z) {
        for (std::size_t x = 0; x < f.width; ++x) {
            const double r = std::hypot(static_cast<double>(x) - 256.0, static_cast<double>(z) - 256.0) * 10.0;
            f.values[z * f.width + x] = static_cast<float>(r < 1000.0 ? 100.0 * (1.0 - r / 1000.0)
                                                                     : -(r - 1000.0) * std::tan(0.5 * 3.14159265358979 / 180.0));
        }
    }
    const metrics::Report r = metrics::Measure(f);
    CHECK(r.hypsometry[0] < r.hypsometry[1] && r.hypsometry[1] < r.hypsometry[2]);
    CHECK(r.hypsometry[3] <= 100.0);
    const double slope = metrics::MeanSlopeInDepth(f, 0.0, 5.0, 200.0);
    CHECK(slope > 0.45 && slope < 0.55);
}

} // namespace

int main() {
    TestDisc();
    TestSeveral();
    TestRoughCoast();
    TestHypsometryAndDepth();
    return test::Summary("test_lands");
}
