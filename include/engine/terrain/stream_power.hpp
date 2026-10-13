#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>

#include <memory_resource>
#include <span>
#include <vector>

/// Stream-power erosion on the CPU, over a whole grid at once.
///
/// What the GPU ops cannot do: drainage is global. Every point's erosion depends on how much ground
/// drains through it, which can be the whole massif, so no section with a halo can compute it. This
/// runs on a coarse grid of the whole terrain, before any section, and its result reaches the sections
/// through `Terrain.Grid`.
///
/// The model is the detachment-limited stream-power law with tectonic uplift, solved implicitly in the
/// way Braun and Willett (2013) showed: each step the ground rises by `uplift`, depressions are filled
/// (priority flood, so every cell drains to the edge), each cell drains to its steepest neighbour,
/// drainage area is accumulated down the network, and the height of every cell is lowered towards its
/// receiver by `k A^m S`, solved from the outlets upwards so the step is unconditionally stable. What
/// comes out is a dendritic network of V valleys between sharp ridges, which is what eroded mountains
/// look like at the kilometre scale and what noise does not produce.
namespace engine::terrain {

struct StreamPowerParams {
    u32_t width = 0;
    u32_t depth = 0;
    /// Metres between grid values.
    f64_t cell = 50.0;
    u32_t iterations = 100;
    /// Erodibility, per time step. The steady-state slope is `uplift / (k A^m)`.
    f64_t k = 1e-4;
    /// Area exponent, about 0.4 to 0.6 in nature; the slope exponent is 1.
    f64_t m = 0.5;
    /// Hillslope talus: after each step no slope is left steeper than this (rise over run). Zero
    /// disables it.
    f64_t talus = 0.0;
    /// Hillslope diffusion, in square metres per step: soil creep rounds the ground between channels,
    /// which is what spaces valleys apart; without it they form one per grid cell. Kept below the
    /// explicit stability limit (a quarter of a cell squared) whatever is asked.
    f64_t diffusion = 0.0;
    /// Steps between recomputing the drainage network. It changes slowly once formed; the first steps
    /// always recompute it.
    u32_t refresh = 1;
};

/// Erodes `height` in place. `uplift` is metres per step at every cell, the same size as `height`.
/// The cells on the edge of the grid are the base level and never move.
[[nodiscard]] Status StreamPowerErode(std::span<f64_t> height, std::span<const f64_t> uplift,
                                      const StreamPowerParams& params);

} // namespace engine::terrain
