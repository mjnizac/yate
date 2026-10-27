#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>

#include <memory_resource>
#include <span>
#include <vector>

/// Rivers and lakes on the CPU, over a whole grid at once, as `StreamPower` is: drainage is global, so no
/// section with a halo can compute it.
///
/// One pass makes the grid drain and describes its water:
///
///   1. A priority flood from the outlets (the edge of the grid and every cell at or below the sea
///      level). A depression met on the way is breached when that costs little: the path the flood came
///      in by is cut down so the pit drains, lowering no cell by more than `breachDepth` (Lindsay's
///      hybrid breaching and filling, which changes the terrain least). Otherwise it is filled to its
///      spill level.
///   2. A filled depression large and deep enough (`lakeArea`, `lakeDepth`) is a lake: the terrain keeps
///      its bowl and the water surface is the spill level. A smaller one is filled in the terrain.
///   3. Every cell drains to its steepest neighbour on that surface, and drainage area is accumulated.
///   4. Cells draining at least `channelArea` are rivers. Their centre lines, cell to receiver, are
///      smoothed so they do not zigzag along the eight directions, and the distance from every cell to
///      the nearest one is written with that river's width, depth and water level. Interpolated on the
///      GPU, a distance draws a crisp channel at any resolution, where a mask on the grid would be as
///      coarse as the grid.
///
/// Width and depth follow hydraulic geometry: `width = widthScale * (A / 1 km2)^0.5` and
/// `depth = depthScale * (A / 1 km2)^0.3`, metres.
namespace engine::terrain {

struct HydrologyParams {
    u32_t width = 0;
    u32_t depth = 0;
    /// Metres between grid values.
    f64_t cell = 50.0;
    /// Cells at or below this height that reach the edge through others like them are sea: outlets, as
    /// the edge is. A closed basin below it is not the sea, and can be a lake.
    f64_t seaLevel = -1e30;
    /// Deepest cut, metres, that may breach a depression instead of filling it.
    f64_t breachDepth = 3.0;
    /// A filled depression at least this large (square metres) and this deep (metres) is a lake.
    f64_t lakeArea  = 2e5;
    f64_t lakeDepth = 2.0;
    /// Drainage area, square metres, from which a cell carries a river.
    f64_t channelArea = 1e6;
    f64_t widthScale  = 10.0;
    f64_t depthScale  = 1.5;
};

struct HydrologyResult {
    /// The terrain made to drain: breached and filled, lakes left as bowls.
    std::pmr::vector<f64_t> height;
    /// Drainage area through each cell, square metres.
    std::pmr::vector<f64_t> area;
    /// The water surface of the lake a cell is in; elsewhere 20 m under the drained ground. Every field
    /// stays smooth where there is no water, because `Terrain.Grid` reads them with Catmull-Rom, which
    /// overshoots a jump to a far sentinel by a large fraction of the jump.
    std::pmr::vector<f64_t> lake;
    /// Metres from the nearest river centre line, capped a few widths out.
    std::pmr::vector<f64_t> riverDistance;
    /// That river's width and depth, metres, and its water level; where no river is near, the level is
    /// 50 m above the drained ground.
    std::pmr::vector<f64_t> riverWidth;
    std::pmr::vector<f64_t> riverDepth;
    std::pmr::vector<f64_t> riverLevel;
    u32_t                   lakes  = 0;
    u32_t                   rivers = 0;
};

[[nodiscard]] Status Drain(std::span<const f64_t> height, const HydrologyParams& params,
                           HydrologyResult& result);

} // namespace engine::terrain
