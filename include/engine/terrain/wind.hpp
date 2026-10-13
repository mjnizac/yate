#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>

#include <span>

/// Aeolian sand transport on the CPU, over a whole periodic grid: Werner's (1995) slab model with the
/// shadow zone, the avalanche and the reptation (local creep) of Desertscape-style simulations.
///
/// Sand is a stack of slabs per cell. Repeatedly a cell is picked; unless it is bare or in the wind
/// shadow of the dune upwind of it, a slab is lifted and hops `hop` cells downwind, where it settles
/// with probability `pSand` on sand and `pBare` on bare ground, always in a shadow, and hops on
/// otherwise. Wherever a slab leaves or lands, slopes past the angle of repose avalanche. Out of those
/// rules dunes form, migrate, merge and coarsen: transverse ridges where sand is plentiful, crescent
/// barchans where it is scarce, slip faces at the angle of repose, gentle backs, flat bare interdunes.
/// The grid wraps on both axes, so the result tiles. The wind blows along +x.
namespace engine::terrain {

struct WindParams {
    u32_t width = 0;
    u32_t depth = 0;
    /// Metres between cells, and metres of sand in one slab.
    f64_t cell = 20.0;
    f64_t slab = 3.0;
    /// Picks per cell over the whole run.
    u32_t iterations = 200;
    /// Cells a slab travels per hop.
    u32_t hop = 5;
    f64_t pSand = 0.6;
    f64_t pBare = 0.4;
    /// Angles in degrees: the shadow behind a crest, and the angle of repose.
    f64_t shadow = 15.0;
    f64_t repose = 33.0;
    /// Probability that a lifted slab only creeps to a neighbour downwind (reptation), which rounds
    /// crests and softens every transition.
    f64_t creep = 0.2;
    /// Passes of a 3 x 3 average over the result: sand comes in slabs, and a slab is a step.
    u32_t smooth = 2;
    u32_t seed = 0;
};

/// Moves the sand in `height` (metres per cell, x fastest) in place.
[[nodiscard]] Status WindErode(std::span<f64_t> height, const WindParams& params);

/// Frees the cached results (see wind.cpp). Called at engine shutdown.
void ReleaseWindCache() noexcept;

} // namespace engine::terrain
