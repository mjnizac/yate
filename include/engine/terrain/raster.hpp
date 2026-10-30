#pragma once

#include <engine/common.hpp>

#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

/// Whole-raster passes on the CPU that a script would otherwise loop over in Lua: the land prelude
/// (`Lands.Template`) rasters a map of a few hundred pixels a side and runs dozens of them on it, which in
/// Lua took most of its time. Each is the same arithmetic as the Lua it replaced, so the results are the
/// same to the bit. Square rasters only, `n` pixels a side, x fastest.
namespace engine::terrain::raster {

/// What `Distance` gives a pixel with no source anywhere: the square root of the 1e20 the envelope uses
/// for "none".
inline constexpr f64_t kNoSource = 1e10;

/// The exact Euclidean distance, in pixels, from every pixel to the nearest one where `source` is set:
/// Felzenszwalb and Huttenlocher's lower envelope of parabolas along the rows, after a scan each way down
/// the columns (the sources are points on a column line, so no envelope is needed there).
void Distance(std::span<const u8_t> source, u32_t n, std::span<f64_t> out);

/// A square `radius` pixels each way, by running counts along the rows and then the columns: with `all`
/// a pixel stays set only if its whole square is (erosion), else if its square touches a set one
/// (dilation). At the edge the square is cut to the raster.
void Square(std::span<u8_t> mask, u32_t n, u32_t radius, b8_t all);

/// Three passes of a box `radius` pixels wide each way, rows then columns: about a Gaussian of that
/// radius. At the edge the box is cut to the raster and averages what it covers.
void Box(std::span<f64_t> values, u32_t n, u32_t radius);

/// The beaches' width offshore (the land prelude's): each pixel of sea within `reach` pixels of the coast
/// takes `atCoast` at the coast nearest it, stepped back along the distance's gradient (`sdf`, metres,
/// `cell` metres a pixel), and at most a third of the water's half-width there, the distance where it
/// stops growing stepped forward; past `reach`, nothing. Land keeps its own.
void Project(std::span<const f64_t> sdf, u32_t n, f64_t cell, u32_t reach, std::span<const f64_t> atCoast,
             std::span<f64_t> out);

/// How the land (`land` set) drains to its coast: a priority flood from the land pixels with a sea pixel
/// beside them (four neighbours, off the raster's rim) over `height`, each pixel reached from its lowest
/// reached neighbour of eight and raised to it where lower, so the pits are filled and every path reaches
/// the sea. `parent` is the pixel each land pixel drains into, its index plus one; 0 for an outlet on the
/// coast, -1 where nothing drains (the sea, the rim). `area` is the pixels draining through each, itself
/// included. Ties go to the lower index, so the result is the same every run.
void Drain(std::span<const u8_t> land, std::span<const f64_t> height, u32_t n, std::span<f64_t> parent,
           std::span<f64_t> area);

/// A template's jittered lattice (the land prelude's `g`): `nx` by `nz` points at `px`, `pz` (in `units`
/// across the map, `spacing` apart before the jitter), each quad split along the diagonal `diag` says (set:
/// a-b-d and a-d-c; else a-b-c and b-d-c).
struct Lattice {
    std::span<const f64_t> px, pz, h;
    std::span<const u8_t>  diag;
    u32_t                  nx = 0, nz = 0;
    f64_t                  spacing = 1.0, units = 1.0;
};

/// Each pixel of an `n` raster over the lattice in the triangle of its points it falls in: the triangle's
/// corners (`corners`, three a pixel, the points' indices plus one) and the pixel's barycentric weights
/// (`weights`, three a pixel), `height` the lattice's heights read linearly there. Off every triangle (the
/// map's rim) the nearest corner of the triangles in its slot, weight 1, and `found` clear.
void Triangles(const Lattice& g, u32_t n, std::span<f64_t> corners, std::span<f64_t> weights,
               std::span<f64_t> height, std::span<u8_t> found);

/// A connected body of land or water (four neighbours): its pixels, the sums of their x and z, and whether
/// it touches the raster's edge.
struct Body {
    f64_t cells = 0.0, x = 0.0, z = 0.0;
    b8_t  edge = false;
};

/// The land's connected masses and the water's bodies, each list largest first (ties in the order they
/// were met, scanning x fastest), and `region`: each land pixel its mass's rank from 1, each sea pixel the
/// rank of the nearest land, grown outwards a ring of four neighbours at a time (0 with no land at all).
void Label(std::span<const u8_t> land, u32_t n, std::span<f64_t> region, std::pmr::vector<Body>& masses,
           std::pmr::vector<Body>& waters);

/// A 3 x 3 weighted mean (4 at the centre, 2 on the sides, 1 at the corners), the edge repeated.
void Soften(std::span<const f64_t> values, u32_t n, std::span<f64_t> out);

/// Where a raster lies: `n` pixels of `cell` metres a side from its corner (`x0`, `z0`).
struct Placement {
    u32_t n    = 0;
    f64_t x0   = 0.0;
    f64_t z0   = 0.0;
    f64_t cell = 1.0;
};

/// `values`, a raster at `from`, read at the pixel centres of a raster at `to`: bilinear, or the nearest
/// pixel with `nearest` (for ids). Off `from` a pixel takes `outside`.
void Resample(std::span<const f64_t> values, const Placement& from, const Placement& to, b8_t nearest,
              f64_t outside, std::span<f64_t> out);

/// A script's numbers kept for the rest of the process under `key`, so the next run of it (the viewer
/// re-runs a script whenever the camera moves on in an unbounded world) gets them back instead of
/// computing them again. The last `kKept` are kept, oldest out first; the key says everything they were
/// computed from, so a hit is the result.
inline constexpr usize_t kKept = 48;
void Keep(std::string_view key, std::span<const f64_t> numbers);
/// The numbers kept under `key` into `out`, or false if none are.
[[nodiscard]] b8_t Recall(std::string_view key, std::pmr::vector<f64_t>& out);
/// Frees what `Keep` holds. Called at engine shutdown, before the memory system checks for leaks.
void ReleaseKept() noexcept;

} // namespace engine::terrain::raster
