#pragma once

#include <engine/common.hpp>
#include <engine/error.hpp>
#include <engine/terrain/graph.hpp>

#include <array>
#include <span>

namespace engine::terrain {

/// Evaluates an `R2 -> R1` value of a graph on the CPU at world positions, for a script that has to
/// look at its own terrain before it decides something: where rivers rise, where lakes lie.
///
/// Pointwise ops use their CPU references, noise and grids are evaluated as their kernels do, and a
/// gradient is taken by central differences over `spacing`. Ops that read a neighbourhood or iterate
/// (blur, erosion, gullies) are passed through, their first input standing for their output: what a
/// script samples is the shape of the terrain, which those ops refine but do not move. `seed` is the job
/// seed, as the noise kernels receive it.
[[nodiscard]] ENGINE_API Status SampleGraph(const Graph& graph, Value value, u32_t seed, f32_t spacing,
                                            std::span<const std::array<f32_t, 2>> positions,
                                            std::span<f32_t> out);

} // namespace engine::terrain
