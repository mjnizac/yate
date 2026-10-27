// Rivers and lakes (Hydrology.Drain) against grids whose answers are known.
//
// A bowl too large to fill is a lake at its rim; a shallow pit is breached by a cut no deeper than the
// limit; a deep, small pit is filled instead; a V valley drains along its floor into one river whose area
// is the whole valley's and whose width follows it; ground under the sea level is an outlet when it
// reaches the edge. Afterwards nothing but lakes holds water: the drained grid has no closed depression of
// its own.

#include "test_support.hpp"
#include "relief_metrics.hpp"

#include <engine/memory/general.hpp>
#include <engine/memory/allocator.hpp>
#include <engine/terrain/hydrology.hpp>

#include <cmath>
#include <functional>

using namespace engine;
using terrain::HydrologyParams;
using terrain::HydrologyResult;

namespace {

constexpr u32_t kSide = 64;

[[nodiscard]] std::pmr::vector<f64_t> Make(const std::function<f64_t(f64_t, f64_t)>& height) {
    std::pmr::vector<f64_t> values(&memory::General().Resource());
    for (u32_t z = 0; z < kSide; ++z) {
        for (u32_t x = 0; x < kSide; ++x) {
            values.push_back(height(static_cast<f64_t>(x), static_cast<f64_t>(z)));
        }
    }
    return values;
}

[[nodiscard]] HydrologyResult Empty() {
    std::pmr::memory_resource* r = &memory::General().Resource();
    return {.height = std::pmr::vector<f64_t>(r), .area = std::pmr::vector<f64_t>(r),
            .lake = std::pmr::vector<f64_t>(r), .riverDistance = std::pmr::vector<f64_t>(r),
            .riverWidth = std::pmr::vector<f64_t>(r), .riverDepth = std::pmr::vector<f64_t>(r),
            .riverLevel = std::pmr::vector<f64_t>(r), .receiver = std::pmr::vector<i32_t>(r),
            .sea = std::pmr::vector<u8_t>(r)};
}

[[nodiscard]] HydrologyParams Params() {
    HydrologyParams p;
    p.width = p.depth = kSide;
    p.cell            = 10.0;
    // The bowls here are a few hectares, under the default 20.
    p.lakeArea = 1e4;
    return p;
}

/// The share of the grid in closed depressions, treating lakes as water at their surface.
[[nodiscard]] double PitShare(const HydrologyResult& r) {
    metrics::Field f;
    f.width = f.depth = kSide;
    f.resolution      = 10.0;
    for (usize_t i = 0; i < r.height.size(); ++i) {
        f.values.push_back(static_cast<float>(std::max(r.height[i], r.lake[i])));
    }
    return metrics::MeasureRelief(f, {.level = -1e30, .pitDepth = 0.05}).pitShare;
}

void TestLake() {
    test::Section("a large bowl is a lake at its rim");
    // A 30 m deep bowl in a plane at 100 m; its rim, the lowest point around it, is the plane.
    const auto heights = Make([](f64_t x, f64_t z) {
        const f64_t r = std::hypot(x - 32.0, z - 32.0);
        return 100.0 - (r < 15.0 ? 30.0 * (1.0 - r * r / 225.0) : 0.0) + 0.01 * x;
    });
    HydrologyResult r = Empty();
    CHECK(terrain::Drain(heights, Params(), r).has_value());
    CHECK_EQ(r.lakes, 1u);
    const usize_t centre = 32 * kSide + 32;
    // The spill is the lowest rim cell, on the low (x small) side: about 100 + 0.01 * 17.
    std::printf("     lake surface %.3f, pits %.5f\n", r.lake[centre], PitShare(r));
    CHECK(std::fabs(r.lake[centre] - (100.0 + 0.01 * 17.0)) < 0.1);
    CHECK(std::fabs(r.height[centre] - heights[centre]) < 1e-9); // the bowl is kept
    CHECK(PitShare(r) < 1e-9);
}

void TestBreach() {
    test::Section("a shallow pit is breached, a deep small one is filled");
    // A 2 m pit and a 12 m pit, both small, on a plane sloping to x = 0.
    const auto heights = Make([](f64_t x, f64_t z) {
        f64_t h = 0.5 * x + 50.0;
        if (std::hypot(x - 20.0, z - 20.0) < 2.0) {
            h -= 2.0;
        }
        if (std::hypot(x - 44.0, z - 44.0) < 2.0) {
            h -= 12.0;
        }
        return h;
    });
    HydrologyParams p = Params();
    p.breachDepth     = 3.0;
    HydrologyResult r = Empty();
    CHECK(terrain::Drain(heights, p, r).has_value());
    CHECK_EQ(r.lakes, 0u);
    f64_t deepestCut = 0.0;
    f64_t deepPit    = 0.0;
    for (usize_t i = 0; i < heights.size(); ++i) {
        deepestCut = std::max(deepestCut, heights[i] - r.height[i]);
    }
    deepPit = r.height[44 * kSide + 44] - heights[44 * kSide + 44];
    CHECK(deepestCut <= 3.0 + 1e-6); // no cut deeper than the limit
    CHECK(deepestCut > 0.5);         // and the shallow pit was cut, not filled
    std::printf("     deepest cut %.3f, deep pit raised %.3f, pits %.5f\n", deepestCut, deepPit, PitShare(r));
    CHECK(deepPit > 10.5);           // the deep one was filled, to its spill on the low side
    CHECK(PitShare(r) < 1e-9);
}

void TestValley() {
    test::Section("a V valley is one river along its floor");
    // Sides at 0.3 towards z = 32, the floor at 0.05 towards x = 0, 10 m cells: the floor gathers the
    // whole valley, about 63 x 64 cells at its mouth.
    const auto heights = Make([](f64_t x, f64_t z) { return std::fabs(z - 32.0) * 3.0 + x * 0.5 + 10.0; });
    HydrologyParams p = Params();
    p.channelArea     = 2e4;
    HydrologyResult r = Empty();
    CHECK(terrain::Drain(heights, p, r).has_value());
    const usize_t mouth = 32 * kSide + 1;
    CHECK(r.area[mouth] > 0.9 * 62.0 * 62.0 * 100.0);
    CHECK(r.riverDistance[32 * kSide + 20] < 1.0);
    CHECK(r.riverDistance[40 * kSide + 20] > 40.0); // capped at the band, a few widths out
    CHECK(std::fabs(r.riverWidth[mouth] - 10.0 * std::sqrt(r.area[mouth] / 1e6)) < 2.0);
    CHECK(r.rivers > 30u);
}

void TestSea() {
    test::Section("ground under the sea level is an outlet when it reaches the edge");
    // A plane at 5 m with a basin down to -10 m in the middle. Closed, it is a lake even under a sea at 0,
    // as the Dead Sea is; with a channel under the sea level out to the edge it is the sea.
    const auto basin = [](f64_t x, f64_t z) {
        const f64_t r = std::hypot(x - 32.0, z - 32.0);
        return r < 20.0 ? -10.0 + 15.0 * r / 20.0 : 5.0;
    };
    HydrologyParams p = Params();
    p.seaLevel        = 0.0;
    HydrologyResult closed = Empty();
    CHECK(terrain::Drain(Make(basin), p, closed).has_value());
    CHECK_EQ(closed.lakes, 1u);
    const auto open = Make([&](f64_t x, f64_t z) { return std::fabs(z - 32.0) < 1.5 && x < 32.0 ? -5.0 : basin(x, z); });
    HydrologyResult inlet = Empty();
    CHECK(terrain::Drain(open, p, inlet).has_value());
    CHECK_EQ(inlet.lakes, 0u);
}

} // namespace

int main() {
    if (Status started = memory::Init(); !started) {
        std::printf("FAIL memory::Init\n");
        return 1;
    }
    TestLake();
    TestBreach();
    TestValley();
    TestSea();
    memory::Shutdown();
    return test::Summary("test_hydrology");
}
