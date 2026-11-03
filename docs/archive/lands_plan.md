> Archive. Original plan for `Lands.*` (phases 1 to 3 done); phases 4 to 8 now live in `docs/roadmap.md`. Kept whole for its algorithms and the analysis of the references; do not read it end to end (`tools/dev.sh headings docs/archive/lands_plan.md`).

# Plan: islands, archipelagos and continents from geometric shapes

Plan to rebuild YATE's land generation (`Lands.*`) and, afterwards, the components that depend on it. It
covers the analysis of four reference generators, the agreed criteria, what the engine already has and
reuses, the chosen algorithms (the references' own or a more efficient one for this architecture), the API,
the phases with their acceptance criteria, the example catalogue and the validation.

Versioned document. Whatever still holds when each phase closes moves to `docs/status.md`, `docs/todo.md`
and `docs/lua_reference.md`, as always.

Revision 2: inventory of what can be reused, detailed algorithms, fused `Lands` op, tools for Claude,
example catalogue with infinite plane and fixed sizes, and their tests.

## Status

**Phase 1 done** (2026-10-28). Decisions taken while implementing it, which change what is written below:

* **Relief:** profiles are not blended with softmin weights (the deepest element, e.g. a `fill`, cancelled
  the others). Each land element applies its profile to its own inward distance, capped by that of the
  common coast, and the profiles are joined with a smooth maximum. Only elements that contain the sample
  count, which keeps the bin culling exact.
* **Centre of oval and radial shapes:** inside the inscribed circle the distance blends into that circle's;
  without it there was a star and a false pit at the centre.
* **Seabed:** a third element type (`seabed = true`) and a `seabed` mode of the op; the active margin's
  trench goes at the foot of the slope (shelf + 0.9 of the slope) and at 0.8 of the abyssal depth.
* **Id after a water cut:** the land before a water element that covers the sample stops being its owner
  (an islet in a bay has its own id).
* **Lagoons:** explicit `lagoon` role on a water element.
* **`edit.scale`** on an element defined by `area` multiplies the area by the scale squared.
* **`Lands.Island` requires `preset`;** the old macro's keys fail explaining what to use.
  `Lands.Archipelago` and `Lands.Continent` stay on the grid macro until phases 2 and 3.
* Steps 1.7 to 1.11 went into a single commit because they touched the same files at once.

**Phase 2 done** (2026-10-28). Decisions:

* **`scatter` is an element key**, not a type: the remaining keys are the template. Ids `"<id>/<slot>"`.
* **Bounded:** slots are placed in order (not largest first) so that asking for one more does not move the
  previous ones; if one does not fit, it shrinks up to 3 times (8 on a curve), otherwise it is skipped and
  reported.
* **Infinite:** Wei's rule in one pass (a candidate survives if no conflicting candidate has higher
  priority), with no greedy chains, so that it depends only on the neighbouring cells.
* **`land_fraction`:** the areas of the scatters are rescaled by closed formula and the closest pass is
  kept; there is no bisection over sampling.
* **Margin without creases:** a fourth op mode (`margin`) joins the land with a smooth minimum of half
  shelf+slope; only the seabed uses it, the coast is still the hard union. The trench of several islands
  is joined as a 4-norm, and the island arc uses its own trench (a seabed `arc` with a negative cone).
* **Scattered water** (channels, lakes) does not avoid declared land; later scatters avoid the land of
  earlier ones.

**Phase 3 done** (2026-10-28). Decisions:

* **`ridge` is an element key**, like `scatter`: one relief element per crest segment (`saddles + 1` bands
  that narrow to 0.4 of their width at the ends, where the saddles come from) and spurs (`capsule`) every
  `spacing` (1.2 widths by default, rounded to the whole number of spurs that fits), on alternating sides
  and turned 20 to 40°. Ids `"<id>/crest/<n>"` and `"<id>/spur/<n>"`. The highest segment measures exactly
  `height` above the land; the others, 0.8 to 1.
* **Relief elements** (`relief = true`, a fourth operation type of the op): they do not change the coast;
  those that rise are joined by a smooth maximum and those that sink (a basin) by a smooth minimum, and they
  fade in from the coast over `blend` metres.
* **Land profiles are joined by their maximum**, not by a smooth maximum: the smooth one raised two equal
  overlapping plateaus (the atoll rose 15 m above its `flat` height; the `all_land` plateau did not reach
  280 m). In the fillet of a smooth union each profile is measured with how much the smoothing has grown
  the land at that sample, `max(h - d, 0)` (hard union of the land minus the coast): there its own distance
  is positive even though the land continues, and measured from it the profile left a closed groove under
  the coastal cliff (0.23 % of the continent, up to 87 m). Adding `smooth` (enlarged every element by
  `smooth` metres) and `smooth/4` (left the groove) were tried first. Outside the fillets nothing changes.
  The affected island goldens were regenerated.
* **Goldens in Debug:** Debug shaders are compiled unoptimised (`-g`), so a sample on a discontinuity can
  come out different from Release; `lands_mediterranean` is sampled on an offset grid (±49152 m at 384 m)
  for that reason.
* **Bands** join their segments with a smooth minimum of a quarter of their width: without it there was a
  crease where the nearest segment changes.
* **Land presets** in `Lands.Preset`/`Lands.Continent`: compositions from the table below with a `turn`
  drawn from the seed (taklamakan does not turn: its ranges follow the edges). Lowland is a plateau of
  0.08 of `height` that rises over `size * 0.01` (how long the coastal cliff takes to vanish) with
  `cliff_height = 0.05 height`: with the plateau slower or lower than the cliff a closed groove was left
  behind the coast (0.4 % of the peninsula in pits). `all_land` has no lakes (`land_mask = 1`); `lakeland`
  has them.
* **Water classified in the report** (7.9): connected components of the water on the report grid; the
  `role` of the first water element whose centre falls inside wins, otherwise `ocean` if it touches the
  frame, `sea` above `classes.sea` (1/100 of the map, 1000 km² when infinite) and `lake`.
  `land.info.waters` and `script_report.lands.waters`.
* **`frame.edge_margin`** and **percentages** of the bounded frame in any position (`at = { "30%", "75%" }`).
* **`Lands.Continent` is a preset** on `Compose`; the grid macro was removed entirely (about 600 lines) and
  its keys fail saying what to use. `lands_continent` was regenerated at 192 km.
* **Land fraction of the continents**: the composition's (0.2 to 0.7 depending on the preset), not the
  old macro's 70 %; `land_fraction` only rescales the scatters.
* **Reference bands** (step 3.5): measured on the heightmaps in `docs/references/` (not versioned) with
  `terrain_metrics`; the archipelago ones are painted maps and are not used. They are checked in
  `test_compose` (not in `test_lands`, which has no engine): land fraction and share of the largest mass.
  Coast dimension and hypsometric integral stay out of band until phase 4 (detail), as expected for shapes
  without detail, and are printed. Islands: land 0.17 to 0.45, largest mass 0.85 to 1, dimension 1.02 to
  1.39, integral 0.26 to 0.45; continents: 0.20 to 0.89, 0.42 to 1, 1.12 to 1.38 and 0.15 to 0.62.
  `fractious` is not measured by the largest mass (it is fragmented land on purpose; its reference type
  would be the archipelagos).
* The fillet fix was a separate `fix:` commit, after 3.4, because it turned up while reviewing the 3.6
  examples with their goldens' seed.

**Outlines before phase 4** (2026-10-29). The images in `docs/references/` are the requested result and the
reviewed generators the minimum. Decisions:

* **The 5.2 rejection is reverted for bounded maps:** `Lands.Template` ports Azgaar's 14 templates as they
  are (their steps and numbers, a jittered grid of 10 000 cells, integer heights); its land fraction falls
  within the spread of its own generator (6 previews per template on azgaar.github.io).
* **On top, what brings it closer to the references:** wide folding (0.08 of the map; more folds the land),
  fjords walked inland (8 per map), fractal fraying in distance (2.5 cells) by its roughness field and a
  sub-cell warp on the GPU; exact distance (Felzenszwalb). Noise in height units (mapgen2's threshold)
  barely moved the coast and was dropped; so were valleys by `StreamPower`, which only crenellated the
  coast.
* **Presets with a template use it by default;** `outline = "shapes"` gives the composition with ids.
  `land_fraction` sets the sea level by quantile.

---

## Contents

1. [Goal and scope](#1-goal-and-scope)
2. [Fundamental criterion and principles](#2-fundamental-criterion-and-principles)
3. [Starting point: what exists today and what is reused](#3-starting-point-what-exists-today-and-what-is-reused)
4. [Analysis of the reference repositories](#4-analysis-of-the-reference-repositories)
5. [What is taken, how it is translated and what is dropped](#5-what-is-taken-how-it-is-translated-and-what-is-dropped)
6. [Architecture: the fused `Lands` op](#6-architecture-the-fused-lands-op)
7. [Detailed algorithms](#7-detailed-algorithms)
8. [Proposed API](#8-proposed-api)
9. [Infinite plane and bounded maps](#9-infinite-plane-and-bounded-maps)
10. [Tools for Claude to work precisely](#10-tools-for-claude-to-work-precisely)
11. [Priority phases (1 to 3)](#11-priority-phases-1-to-3)
12. [Later phases (4 to 8)](#12-later-phases-4-to-8)
13. [Example catalogue](#13-example-catalogue)
14. [Validation and tests](#14-validation-and-tests)
15. [Risks and open decisions](#15-risks-and-open-decisions)
16. [Workflow](#16-workflow)
17. [References](#17-references)

---

## 1. Goal and scope

**Goal.** For the engine to generate islands, archipelagos, continents and any other distribution of land
and water (all land, land with inland seas or lakes, all sea with underwater relief) in a way that is:

* **controllable**: "5 islands of 2 to 4 km²", "make island 3 20 % bigger", "a peninsula to the south" or
  "cliffs higher than the interior" translate into parameters, not into rewriting a script;
* **measurable**: every element has an id, a real area and dimensions that are checked;
* **valid on the infinite plane and on fixed-size maps** with the same model;
* **efficient**: analytic shapes evaluated on the GPU in a single op; the CPU only places and measures;
* **with as little new code as possible**: what the engine already has is reused (section 3.4) and only
  what is missing is created, plus the tools Claude needs to work precisely (section 10).

Generation covers **the surface and the oceanic part**: a single surface, continuous across sea level. The
ocean starts from a geometric base and is improved in phase 6.

**Priority.** Islands, archipelagos and continents (phases 1 to 3). Detail, hydrology, full ocean,
climate, biomes and infrastructure come later (phases 4 to 8).

**Out of scope.** Politics, cultures, names and routes.

---

## 2. Fundamental criterion and principles

### 2.1 Fundamental criterion: simple geometric shapes first, detail after

Every element, land or ocean, is built **first** from analytic primitives (signed distances combined by
union, intersection and subtraction, and profiles over those distances). Noise, warp and erosion enter
**only** in the detail stage, on a shape that is already correct and measurable, with their amplitude
declared in metres.

Consequences:

* Primitives are **local**: they work on the infinite plane with no global grid.
* They are **resolution-independent** and evaluated on the GPU.
* **Distance to the coast** comes out of the composition; no distance transform is needed unless erosion
  moves the coast (phase 4).
* They are **editable**: "make island 3 bigger" changes a radius.
* **Time**: today the `lands_*` goldens spend 6 to 7 s (Release) and about 27 s (Debug) in Lua, building a
  CPU grid cell by cell. With analytic shapes, the script only places elements.

### 2.2 Principles

1. **Planform separate from profile.** Where there is land is one layer; how the height rises and falls is
   another. **Nothing assumes the centre is highest.** The profile is a curve over the distance to the
   coast and relief elements are placed on their own.
2. **Land and water compose freely.** Elements signed as land or water; any land fraction from 0 to 1.
   "Sea" is defined by label or by area, not by touching the edge.
3. **One model for the infinite plane and for fixed sizes** (section 9).
4. **Editable by Claude**: stable ids, a resolved list in the metadata, editing by id.
5. **The references' parameterisation, not their assumptions.** Operations and parameters are taken; not
   the rule "elevation = distance to the coast" as a default nor constants tied to the number of cells.
6. **Determinism**: all randomness comes from a seeded hash; nothing depends on query order or the window.
7. **World units**: metres and km².
8. **Reuse before writing**: every step starts by checking which part of the engine already does it.

---

## 3. Starting point: what exists today and what is reused

### 3.1 The current macro (`assets/lua/prelude/50_lands.lua`, 839 lines)

CPU grid (256 cells by default, up to 512) read on the GPU with `Terrain.Grid`:

1. Rotated elliptic **bodies** with a spine of high ground, seen through a domain warp. Layouts `single`,
   `scattered`, `hotspot`, `arc`, `atoll`, `continent`.
2. **Rivers**: `Erosion.StreamPower`.
3. **Sea level by quantile** (`land_fraction`, 0.7).
4. **Signed distance** (chamfer in Lua) and mass labels (`region_id`).
5. **Coast**: three-band warp (macro, meso, micro) with amplitude proportional to wavelength, skipping
   bands under 6 samples; coast type by hardness, sediment, exposure and curvature.
6. **Margin**: presets `compressed` (widths proportional to the map, for the viewer, which shows about
   28 km) and `geologic` (shelf 40 km at 130 m, slope 60 km down to 3500 m); atoll lagoon.
7. Optional drawn outlines (`area`, `areas`).

Outputs: `land_mask`, `land_height`, `bathymetry`, `surface_height`, `shore_mask`, `coast_type`,
`region_id` (7 of the 8 of `kMaxOutputs`).

### 3.2 Open limitations (`docs/todo.md`, "Lands macro")

* Chain, arc and atoll cover 6 to 12 % of the map, not 70 %.
* The 70 % island touches the map edge.
* Coast exposure has no fetch.
* The macros do not drain: no lakes or rivers as water.
* 256-cell grid: drainage as fine as 94 m at 24 km.
* No real reference measured with `terrain_metrics`.

### 3.3 Design problems for the new goals

* **Window quantile**: sea level depends on the exported area; incompatible with the infinite plane.
* **Profile assumption**: high spine and height rising towards the interior.
* **No stable ids or resolved list**.
* **CPU grid in Lua**: slow and bounded to `size`.
* **Graph limits**: a `Shapes.*` is a chain of nodes (a polygon is one node per two segments, and
  `Shapes.Spline` is limited to 128 segments); the graph allows 1024 nodes (`Graph::kMaxNodes`). A hundred
  islands as `Shapes` exhaust the graph. It is the same problem `Water` solved with a fused op.

### 3.4 Inventory of what can be reused

| Existing piece | Where | What it brings to the plan | Use |
| --- | --- | --- | --- |
| Fused `Water` op + block in the grid buffer | `water.comp.glsl`, `kernels.cpp`, `block` helper in `lua.cpp` | the exact pattern of "a list of elements in one block, one node" | **template for the `Lands` op** (section 6) |
| CPU sampler, same arithmetic as the GPU | `src/terrain/sampler.cpp` | evaluate the graph on the CPU | CPU reference of the op and measurement |
| `Terrain.Sample` | `lua.cpp` | sample any R2→R1 value on a grid or at points | **measure areas, land fraction, classify** with no new measuring code |
| Distances from `shape.comp.glsl` (circle, ellipse, rounded box, segments with parity) | GLSL | SDFs already tested | moved to `lib/shape.lib.glsl` and shared by `Shape` and `Lands` |
| `Combine.SmoothMin` / `SmoothMax` (polynomial smin) | `arith.comp.glsl` | smooth union with radius `k` | same formula inside `Lands` |
| `Shapes.*` (CSG, `Warp`, `Mask`, Catmull-Rom `Spline`) | `10_shapes.lua` | few, declared outlines; detail warp | used as is; `sample_spline` is reused for bands |
| `Landforms.*` (`Cone`, `Dome`, `Bowl`, `Plateau`, `Ridge`, `Trench`...) | `20_landforms.lua` | declared relief (few elements) | as is |
| `Curves.Profile` (`linear`, `smooth`, `dome`, `gaussian`), `Terrace`, `Smoothstep`, `Remap` | ops | profiles over distance | global profile and margin |
| `Terrain.Transform` (position, rotation, scale, pivot) | op | place shapes | declared elements |
| Three-band coast warp, margin (`MARGINS`), coast type (`COASTS`, exposure) | `50_lands.lua` | coast and margin detail | extracted into functions and reused |
| Mass labelling (chamfer + regions) | `50_lands.lua` | connected components in Lua | classification over `Terrain.Sample` |
| `hasher`, `make_noise`, `smooth` | `30/40/50_*.lua` | deterministic hash | unified into one (phase 1, step 1) |
| `generate.world_cell` | `46_network.lua` | fixed world cells + ring | **same rule for the infinite plane** |
| `Erosion.StreamPower`, `Erosion.Gullies`, `Ranges.Carve` | C++ / prelude | erosion and detail | phase 4 |
| `Water.Drain`, `Water.Network`, `Hydrology.*` | `45_water`, `46_network`, `hydrology.cpp` | lakes and rivers on any terrain | phase 5, no new algorithm |
| `metrics::Measure` (land fraction, islands, areas, dominant share, lagoons, coast, fractal dimension, minimum channel, hypsometry) | `tests/unit/terrain_metrics.hpp` | acceptance criteria | tests and the `terrain_metrics` tool |
| `tools/dev.sh review` (export + metrics + hillshade) | `tools/dev.sh` | visual review | as is, plus a subcommand (section 10) |
| Auto-discovered goldens (`case.json` + `script.lua`) | `tests/datasets/` | regression tests for the examples | one golden per selected example |
| `test_geometry` (closed values), `test_lands`, `test_cli`, `test_viewer`, `test_seams` | `tests/unit/` | established kinds of test | extended |
| Viewer: `--param`, `--resolution`, `--view`, `--target`, `--ring`, `--water`, `--screenshot` | `viewer_main.cpp` | see each example at different sizes | commands in each example's header |

**Essential new code** (what does not exist and cannot be expressed with the above):

1. The fused `Lands` op (GLSL + entry in `kOps` + block binding + reference in `sampler.cpp`).
2. The Lua layer `Lands.Compose` (placement, ids, editing, measurement with `Terrain.Sample`).
3. Writing the element list to the metadata (small binding, section 10).

---

## 4. Analysis of the reference repositories

### 4.1 mapgen2 (Red Blob Games, JavaScript)

Voronoi/Delaunay mesh (Delaunator + dual-mesh, "ghost" regions on the border), struct-of-arrays.

**Water and land** (`water.js`, `assign_water_r`):

* Coordinates normalised to [-1, 1]; **Chebyshev** distance `d = max(|nx|, |ny|)`.
* `n = fbm(amplitudes, nx, ny)`; `n = lerp(n, 0.5, round)`.
* Water if `n − (1 − inflate)·d² < 0`. `round` rounds; `inflate` reduces the edge penalty.
* **Ocean** (`assign_ocean_r`): flood fill from the ghost region; water not reached is lake.

**Elevation** (`elevation.js`):

* Multi-source BFS from the coast triangles; cost 0 on lake edges (they enter at the head of the queue), 1
  on land; relaxation if it improves; neighbour order with a random offset. Stores the parent side as a
  **downslope pointer**.
* `e = −d/minDistance` in ocean, `d/maxDistance` on land; region = mean of triangles; ocean ≤ −0.01.
* **Redistribution** by rank: `y = 1 − (1 − x)²` ⇒ `x = √S − √(S·(1 − y))`, `S = 1.1`, clipped to 1.

**Rivers** (`rivers.js`): springs with `0.3 ≤ e ≤ 0.9` outside the water; +1 flow per side travelled.

**Moisture** (`moisture.js`): seeds on river and lake shores; BFS over land; `m = 1 − (d/max)^0.5`; water
= 1; uniform redistribution by rank.

**Biomes** (`biomes.js`): `t = 1 − e + lerp(bias_north, bias_south, latitude)`. `OCEAN`; water `MARSH`
(t > 0.9), `ICE` (t < 0.2), `LAKE`; coast `BEACH`; land:

| Temperature | Moisture → biome |
| --- | --- |
| < 0.2 | > 0.50 `SNOW`, > 0.33 `TUNDRA`, > 0.16 `BARE`, else `SCORCHED` |
| 0.2 to 0.4 | > 0.66 `TAIGA`, > 0.33 `SHRUBLAND`, else `TEMPERATE_DESERT` |
| 0.4 to 0.7 | > 0.83 `TEMPERATE_RAIN_FOREST`, > 0.50 `TEMPERATE_DECIDUOUS_FOREST`, > 0.16 `GRASSLAND`, else `TEMPERATE_DESERT` |
| ≥ 0.7 | > 0.66 `TROPICAL_RAIN_FOREST`, > 0.33 `TROPICAL_SEASONAL_FOREST`, > 0.16 `GRASSLAND`, else `SUBTROPICAL_DESERT` |

### 4.2 voronoi-map-js (rjanicek, port of mapgen2)

**Shapes** (`island-shape.js`, `inside(q)`, q in [-1, 1]):

* **Radial** (`islandFactor = 1.07`): `bumps ∈ [1, 5]`, `startAngle`, `dipAngle`, `dipWidth ∈ [0.2, 0.7)`;
  mixed radius `0.5·(max(|x|,|y|) + |q|)`;
  `r1 = 0.5 + 0.40·sin(start + bumps·θ + cos((bumps+3)θ))`,
  `r2 = 0.7 − 0.20·sin(start + bumps·θ − sin((bumps+2)θ))`; **bay**: within `dipWidth` of `dipAngle`,
  `r1 = r2 = 0.2`; land if `length < r1` or `r1·islandFactor < length < r2` (ring of satellites).
* **Perlin** (`oceanRatio` rescaled to [0.1, 0.5]): land if `c > oceanRatio·(1 + d²)`.
* Square, Blob, Noise (broken: does not depend on position), Bitmap (no bounds).

**Pipeline** (`map.js`): `improveCorners` (corners to the mean of their centres); corners by BFS from the
border with +0.01 water / +1.01 land (+optional random), no local minima; redistribution with `S = 1.1`;
polygon is water if ≥ `lakeThreshold = 0.3` of its corners are; ocean by flood fill; coast; downslopes to
the lowest neighbour; watersheds (up to 100 passes); rivers `(width + height)/4` attempts with a source at
`0.3 ≤ e ≤ 0.9`; river moisture `min(3, 0.2·flow)`, lake 1, decay **0.9** per step; biomes by elevation
(> 0.8, > 0.6, > 0.3, rest).

**Noisy edges**: recursive subdivision within the quadrilateral (v0, d0, v1, d1), cut in [0.2, 0.8],
factors [0.6, 1.4], `noisyLineTradeoff = 0.5`, minimum length 10 by default, 3 between biomes, 100 between
oceans and **1 on coast, rivers and lava**: detail goes where it is seen.

**Lava**: e > 0.8 and moisture < 0.3, probability 0.2; broken (never creates lava).

### 4.3 heman (Philip Rideout, C)

**Generators** (`SEALEVEL = 0.5`, `DEFAULT_STRENGTH = 0.6`):

* **Island**: 3-channel noise, frequencies `{4, 16, 32, 64, 128}`, amplitudes
  `{0.2, 0.1, 0.05, 0.025, 0.0125}`; mask `0.707 − √(u² + v²) + n0` with `u, v` offset; threshold 0.5;
  SDF; final pass with warp `influence = z` and `z += 6·influence·n0`: **noise growing towards the interior,
  clean coast**.
* **Archipelago**: points `(x, y[, strength])`, cones `strength·(1 − d/r)` combined by **maximum**,
  `strength = 0.56` by default, `noiseamt ≈ 0.3`.
* **Rock**: `√(r² − d²)` with `r = 0.3 + n`. **Planet**: 3D lat-long OpenSimplex. **fBm**: lacunarity 2,
  gain 0.5.

**Distance**: Felzenszwalb–Huttenlocher, lower envelope of parabolas, O(n) per line, separable; signed
(positive inside), unsigned and **closest point coordinate field** (CPCF).

**Ops**: `warp` (gain 0.6, lacunarity 2, amplitude 0.05, frequency 8, **damped on peaks** `(1 − h)^4`);
`emboss` (fine on land `a = 0.0005, f = 256`; ocean **∝ depth** `a = 0.5, f = 1`); `percentiles`
(equal-area terraces); `stairstep`; Bridson and density-based Poisson; occlusion by horizon sweeps.

Bugs: `sobel` reads row 0; `merge_political` uses `MIN` for `MAX`; inconsistent distance normalisation.

### 4.4 Fantasy-Map-Generator (Azgaar, TypeScript)

Grid of ~10 000 cells, heights 0 to 100, sea at 20.

**Template language**: `Tool count height rangeX% rangeY%`.

| Operation | Algorithm |
| --- | --- |
| `Hill` | BFS: `h_neighbour = h^blobPower·U(0.9, 1.1)` until h ≤ 1; retries (50) if the peak would exceed 90 |
| `Pit` | same, subtracting; starts on land |
| `Range` | greedy path to a target at `width/8`–`width/3` (15 % of steps "shortcut"); rings `h = h^linePower − 1` until h < 2; **every 6 cells of the axis it walks downhill** with `(2·h + h_min)/3` (foothills) |
| `Trough` | same, subtracting; 20 %; up to `width/2` |
| `Strait` | edge-to-edge path through the central 30–70 %; `h^exp`, `exp = 0.9 − (0.1/width)·remaining` |
| `Mask` | `(1 − nx²)(1 − ny²)`; negative inverts (inland sea); blend `(h·(fr − 1) + h·d)/fr` |
| `Invert` | mirror with probability |
| `Add`, `Multiply` | on `all`, `land` or a range, respecting level 20 |
| `Smooth` | `(h·(fr − 1) + mean)/fr` |

`blobPower` (0.93 to 0.9973) and `linePower` (0.75 to 0.93) depend on the number of cells.

Templates (weight): `volcano` 3, `highIsland` 19, `lowIsland` 9, `continents` 16, `archipelago` 18,
`atoll` 1, `mediterranean` 5, `peninsula` 3, `pangea` 5, `isthmus` 2, `shattered` 7, `taklamakan` 1,
`oldWorld` 8, `fractious` 3. Structure of the most useful ones:

* `volcano`: central Hill 90–100, `Multiply 0.8` on 50–100, central Range, Smooth, side hills, `Mask 3`.
* `highIsland`: large Hill, `Add 7`, hills, Range, `Multiply 0.8` on land, `Mask 3`, Troughs, Pits.
* `lowIsland`: same with `Multiply 0.4` on 20–100: flat island.
* `continents`: two large separate Hills, hills, Ranges on the edges, two vertical `Strait`, Troughs.
* `archipelago`: `Add 11`, Ranges, Hills, `Trough 10`, vertical and horizontal `Strait`.
* `mediterranean`: relief stuck to the north and south edges and `Mask −2` (sea in the centre).
* `atoll`: central Hill, `Multiply 0.2` on 25–100 (sinks the centre).
* `peninsula`: Range from one edge, `Add`, Hill on the edge, `Invert`.
* `pangea`: Hills in the four corners and a central band, Troughs, Ranges.
* `isthmus`: diagonal chain of alternating Hill and Trough, `Invert x`.
* `shattered`: Hills, `Trough 10–20`, Ranges, `Pit 12–20`.
* `taklamakan`: high Hills on the four edges: closed basin.
* `fractious`: 12–15 Hills, `Mask −1.5`, `Mask 3`, `Add −20`, Ranges.

**Features**: flood fill + distance rings (+3 … −10). Subtypes: land `continent` > N/10 cells, `island` >
N/1000, else `isle`, `lake_island`; water `ocean` > N/25, `sea` > N/1000, else `gulf`; lake `frozen`
(t < −3 °C), `dry` (evaporation > 4·inflow with no inlets or outlets), `salt` (evaporation > inflow with no
outlet), `sinkhole`, `lava`, `freshwater`.

**Lakes**: level = minimum shore − 0.1; closed if there is no outlet below `lakeElevationLimit`;
evaporation `((700·(t + 0.006·h))/50 + 75)/(80 − t)` per cell.

**Temperature**: tropics between 16° and −20° with 0.15 °C/°, linear segments to the poles, −6.5 °C/km.

**Precipitation**: winds by 30° bands; moisture `maxPrec − h`; over water `+5·mod` and rain on the
windward coast; on land `max(moisture/(10·mod), 1) + Δh·(h_next/70)²`; above 85, moisture drops to 0
(**rain shadow**); modifier by 5° bands
`[4, 2, 2, 2, 1, 1, 2, 2, 2, 2, 3, 3, 2, 2, 1, 1, 1, 0.5]`.

**Rivers**: flow accumulated by height; river ≥ 30, navigable ≥ 100; depressions +0.1 iterative; channel
erosion with h ≥ 35, `min(flow/upstream_flow, 5)`; width `min(flow^0.7/500, 1)` + length; meanders.

**Coast**: fractalisation (`maxDepth 4`, `baseAmplitude 1.5·√length`, `amplitudeDecay 0.9`, `minEdge 1`)
with a **roughness field** (`roughnessScale 60`, `smoothThreshold 0.25`, `roughnessContrast 1.5`, lakes ×2
calmer).

**Ocean**: bathymetric rings at distances −1 … −9. **Biomes**: moisture (5) × temperature (26).

---

## 5. What is taken, how it is translated and what is dropped

### 5.1 Translation

| Concept | Translation in YATE | Section |
| --- | --- | --- |
| Azgaar `Hill` | outline element + profile (`dome`, `cone`) | 7.1, 7.4 |
| Azgaar `Pit` | water element or `bowl` relief | 7.1, 7.5 |
| Azgaar `Range` with foothills | `ridge` on a spline + geometric `spurs` | 7.5 |
| Azgaar `Trough` | `trench` on a spline | 7.5 |
| Azgaar `Strait` | water `band`/`capsule` | 7.1 |
| Azgaar `Mask` / negative | land or water `frame` element | 7.1 |
| Azgaar `Invert` | element `mirror` | 8.3 |
| Azgaar `Add`/`Multiply`/`Smooth` | profile parameters, union `smooth` | 7.2, 7.4 |
| Azgaar templates | `Lands.Compose` presets | 11 |
| Azgaar subtypes | classification by area (km²) | 7.9 |
| mapgen2 radial (`bumps`, `dip`, `islandFactor`) | `radial` + water `sector` + ring of satellites | 7.1 |
| mapgen2 `inflate`, `round` | `field` element (phase 4) | 12 |
| mapgen2 hypsometry | fixed curve over height | 7.4 |
| heman archipelago `(x, y, strength)` | `centers` | 7.6 |
| heman Poisson | placement by `scatter` | 7.6 |
| heman noise ∝ distance to the interior | detail (phase 4) | 12 |
| heman ocean `emboss` | seabed detail (phase 6) | 12 |
| heman Felzenszwalb | only if erosion moves the coast (phase 4) | 12 |
| Azgaar roughness field | coast detail (phase 4) | 7.10 |
| Azgaar climate, lakes, rivers | phases 5 and 7 | 12 |

### 5.2 Rejections

* **Voronoi/Delaunay mesh and polyline noisy edges**: YATE is raster on the GPU; primitives and warp
  cover the same independently of resolution.
* **Elevation = distance to the coast by default**: kept as the `dome` preset.
* **Redistribution by rank**: depends on the window; a fixed curve is used.
* **Ocean = water that touches the edge**: there is no edge on the infinite plane.
* **Constants per number of cells** (`blobPower`, `linePower`, N/10, N/1000): become metres and km².
* **Cell-by-cell BFS growth** (Azgaar's Hill, Range): depends on the grid; replaced by analytic
  primitives with the same effect.
* **Politics, cultures, routes, names; voronoi-map's lava.**
* **Bugs**: `sobel` and `merge_political` (heman); `makeNoise`, `makeBitmap`, lava (voronoi-map).

---

## 6. Architecture: the fused `Lands` op

### 6.1 Why a new op

Composing each island from `Shapes.*` costs several nodes (position, distance, union) and the graph has
1024; a polygon costs one node per two segments. The `Water` op already solved the same thing: **a list of
elements in one block of the grid buffer and one node**. `Lands` copies that pattern; it does not invent a
new mechanism.

`Shapes.*` and `Landforms.*` remain the way for a few hand-declared elements and for detail.

### 6.2 Op contract (like `water.comp.glsl`)

* Class: pointwise (halo 0); two neighbouring sections agree by construction.
* Inputs: `position` (R2→R2, default `Terrain.Coords`; a `Shapes.Warp` gives coast detail with no new
  code) and, optionally, an auxiliary field.
* Slot 3: the block, bound by the evaluator, same as `Water` and `Grid`.
* Two channels, like `Water` (`maxChannels = 2`).
* Modes (parameter 2):

  | Mode | Channel 0 | Channel 1 |
  | --- | --- | --- |
  | 0 `coast` | signed distance to the coast (land < 0), in metres, clipped to `D` | id of the dominant land element |
  | 1 `relief` | height above sea level of the per-element blended land profile | dominant id |
  | 2 `seabed` | depth of the margin and of the seabed elements | dominant id |

* CPU reference in `sampler.cpp`, **term by term** equal to the GLSL (like `Water`). That is what makes
  `Terrain.Sample` over the op and the GPU/CPU comparisons in the tests work.

### 6.3 Block layout

```
header (24 values)
  0 block mode   1 number of elements   2 bin size (m)   3..4 bin origin (x, z)
  5..6 bins in x, z  7 D: maximum measured distance (m)   8 start of elements   9 start of index
  10 start of points (bands and polygons)   11 number of points   12..23 global mode parameters
elements (32 values each)
  0 type  1 operation (add, subtract)  2 numeric id  3 x  4 z  5 rotation  6 scale
  7..14 shape parameters  15 smoothing k (m)  16 bounding radius (m)
  17..25 profile or margin (preset and parameters)  26 start of its points  27 number of points  28..31 free
bin index (CSR)
  offsets per bin, then element indices in global order
points (x, z, width) for bands, sampled arcs and polygons
```

The block is built by a new binding `Lands.Block{...}` that reuses the `block` helper in `lua.cpp` (the
same one as `RiverBlock`/`LakeBlock`), and the node is added the way `Hydrology.Carve` adds the `Water` one.

### 6.4 Per-sample evaluation

1. The sample's bin: `floor((p − origin)/bin)`. Outside the bins: distance `D` (open sea).
2. For the elements of the bin, in global order: distance of its shape (7.1) in its local space
   (translation, rotation, scale).
3. CSG in that order: `add` with polynomial smin, `subtract` with smax (7.2).
4. Dominant id and blend weights by softmin (7.3).
5. Depending on the mode: distance, blended land profile (7.4) or margin and seabed (7.7).
6. Clip to `±D`.

Cost per sample: O(elements of the bin), independent of the total. The bin size is chosen so that a bin
holds few elements (section 7.11).

---

## 7. Detailed algorithms

### 7.1 Primitive distances

All in local space, in metres, negative inside. The first four already exist in `shape.comp.glsl` and are
moved to `lib/shape.lib.glsl`.

| Primitive | Distance | Exact | Closed-form area |
| --- | --- | --- | --- |
| `circle` (r) | `|p| − r` | yes | `πr²` |
| `ellipse` (a, b) | the one in `shape.comp.glsl` | bounded approx. | `πab` |
| `box` (hx, hz, rounding) | the one in `shape.comp.glsl` | yes | `4hx·hz − (4 − π)ρ²` |
| `polygon` (points) | segments + parity, like `Shape`, but with no node limit | yes | Gauss's area formula (shoelace) |
| `superellipse` (a, b, n) | `f = (|x/a|^n + |z/b|^n)^(1/n) − 1`, divided by `|∇f|`: `d ≈ f/|∇f|` | 1st order | `4ab·Γ(1+1/n)²/Γ(1+2/n)` |
| `radial` (R, harmonics `a_k`, `φ_k`) | `r(θ) = R·(1 + Σ a_k·sin(kθ + φ_k))`; `d ≈ (|p| − r(θ))/√(1 + (r′(θ)/r(θ))²)` | 1st order | `πR²·(1 + ½·Σ a_k²)` (distinct harmonics are orthogonal) |
| `capsule` (a, b, r) | `sdSegment − r` | yes | `2r·|ab| + πr²` |
| `band` (points with width) | union of rounded cones per segment (Íñigo Quílez's `sdUnevenCapsule`) | yes per segment | numerical integration in Lua at build time |
| `ring` (R, w) | `||p| − R| − w/2` | yes | `2πR·w` |
| `arc` (R, angles, w) | Íñigo Quílez's `sdArc` − w/2 | yes | `R·Δθ·w + πw²/4` |
| `sector` (r, angle, reach) | Íñigo Quílez's `sdPie` | yes | `α·r²` (α = half-angle) |
| `frame` (map frame, margin, exponent) | large superellipse (high n = rounded rectangle) | 1st order | same as superellipse |

Notes:

* The **radial** shape generalises mapgen2's without noise: with `bumps = k` and `a_k` you get the lobes;
  two harmonics (k and k+3) reproduce the original `sin(kθ + cos((k+3)θ))` with a small error and a closed
  area. Validity constraint: `Σ|a_k| < 1` (positive radius) and `Σ k·|a_k| < 1.5` so that the gradient
  correction does not degrade.
* The **bay** (mapgen2's `dip`) is not baked into the shape: it is a subtracted water `sector`. It is edited
  separately.
* The **satellites** (`islandFactor`) are a `scatter` on a `ring` around the island, not a ring of land.
* The 1st-order approximations are exact on the coast (d = 0), which is what defines the area and the
  outline; far from it they drift somewhat, and the profile absorbs it.

### 7.2 Composition (CSG)

* Union: `smin(a, b, k) = mix(b, a, h) − k·h·(1 − h)`, `h = clamp(0.5 + 0.5·(b − a)/k, 0, 1)` (the one in
  `arith.comp.glsl`). Maximum error `k/4` relative to `min`; with `k = 0`, exact union.
* Subtraction: `smax(a, −b, k)`.
* `k` per element (metres): with `k` on the order of a strait's width, two nearby islands join in a natural
  isthmus; with `k = 0`, they stay apart.
* Order: that of the list. First land, then the water that cuts it, then land inside the water (an island
  in a lake), etc. The bin index keeps the global order, so the result does not depend on the bin.

### 7.3 Dominant id and blend weights

* Id: the land element with the smallest own distance at the sample.
* Weights to blend profiles and margins of different elements without a step:
  `w_i = exp(−(d_i − d_min)/τ)` normalised, with `τ = max(k_i, 1 sample)`. If all elements share a profile,
  the blend is the identity.

### 7.4 Land profile (no assumptions)

Height = `Σ w_i · P_i(s_i)` with `s = −d` (inward distance, ≥ 0) and `P_i` the element's profile.

Presets (parameters in metres; all editable and **none implicit**: if no profile is given, the script fails
with a message listing the presets):

| Preset | Definition | Parameters |
| --- | --- | --- |
| `dome` | `H·profile(s/W)` with the `dome` curve of `Curves.Profile` | H, W |
| `cone` | `H·min(s/W, 1)` | H, W |
| `plateau` | `H·smoothstep(0, E, s)` | H, E (edge width) |
| `rim` | `H_r·g(s; c, σ) + H_i·smoothstep(0, W, s)` with `g` a Gaussian centred `c` from the edge: **cliff higher than the interior** when `H_r > H_i` | H_r, c, σ, H_i, W |
| `caldera` | `rim` with `H_i < 0` relative to the edge: rises and drops again into a basin | H_r, c, σ, depth, basin radius |
| `tilt` | `smoothstep(0, E, s)·(H_0 + G·dot(p − c, u))`: tilted plane (cuesta), high on one side | H_0, gradient G, direction u, E |
| `flat` | `H·smoothstep(0, E, s)` with small H | H, E |
| `curve` | `Curves.Profile`/table of `(s, h)` points, interpolated | points |

Coast: the current macro's `beach_width`, `beach_height`, `cliff_height`, `cliff_width` parameters become
the initial stretch of the profile (s from 0 to the beach or cliff width), selected by coast type.

Optional hypsometry: `h' = H·C(h/H)` with `C` a fixed curve (`low`: `C(x) = x²`, `balanced`: identity,
`high`: `1 − (1 − x)²`, mapgen2's curve; or a table). It is monotonic, so it keeps the order of heights and
the drainage, and does not depend on the window.

### 7.5 Relief elements

* **Few and declared**: `Landforms.*` as is (no new code), added to the op's height.
* **Many or generated** (one volcano per island, ranges on every continent): the op's `relief` mode, with
  the same profiles as `Landforms` (`linear`, `smooth`, `dome`, `gaussian`).

**Range with foothills** (geometric translation of Azgaar's `Range`):

1. Axis: Catmull-Rom spline (`sample_spline` from `10_shapes.lua`) between given or generated points.
2. Cross-section: `Ridge` of width W and height H.
3. Height along it: `H(s) = H·(1 − Σ_j a_j·exp(−((s − s_j)/σ_j)²))`, saddles at positions `s_j` by hash
   (`saddles` parameter, from 0 to N).
4. Spurs: every `spacing` metres of arc (2.5·W by default), a segment from the axis in the direction of the
   axis normal turned `±(20° to 40°)` (alternating side), length `W·U(0.8, 1.6)`, starting height
   `0.7·H(s)` and linear fall to 0. Replaces Azgaar's "every 6 cells, walk downhill", which depends on the
   grid.
5. All as bands of the block (rounded cones), with no nodes per spur.

**Volcano**: `Cone` + crater `Bowl` (crater radius and depth). **Depression**: `Bowl`.
**Escarpment**: `band` with a stepped profile (one-step `Curves.Terrace`). **Valley**: `Trench`.

### 7.6 Placement (`scatter`)

**Sizes first.** Areas are drawn in the requested range (log-uniform by default: in nature there are many
more small islands than large ones; `distribution = "uniform"` available), converted to a radius with the
closed formulas of 7.1 and sorted largest first (placing the large ones first packs better, as the current
`scattered` layout already does).

**Bounded map: dart throwing with an acceleration grid** (Bridson adapted to different radii):

* Conflict between i and j if `|c_i − c_j| < R_i + R_j + spacing` (R = bounding radius).
* Acceleration grid with cell `2·R_max + spacing`; up to `k = 30` attempts per element (Bridson).
* Inside `region` (any element used as a mask: its distance is evaluated with the CPU reference) and at
  `edge_margin` from the frame.
* If an element does not fit after `k` attempts, it shrinks by 10 % and retries; if it still does not fit,
  it is skipped and a warning gives the number placed against the number requested.

**Infinite plane: per-cell Poisson with hash priority** (order-independent, local):

1. Each world cell (`world_cell`) is split into `m × m` subcells; each subcell proposes a candidate with a
   position, area and priority drawn from the hash of `(cell, subcell, seed)`.
2. A candidate survives if no conflicting candidate in the neighbouring cells (3 × 3) has higher priority.
   It is the one-pass variant of parallel Poisson sampling (Wei, 2008): it needs no global order, so two
   regions that share cells get exactly the same elements.
3. Requirement: `world_cell ≥ 2·R_max + spacing`, checked with a clear error.
4. `count` is read per world cell (density), as in `Water.Network`.

**Patterns** (the position of the centres; sizes follow the rules above):

| Pattern | Algorithm | Reference |
| --- | --- | --- |
| `uniform` | the above | Bridson, Wei |
| `cluster` | Neyman–Scott/Thomas process: Poisson parents with intensity λ_p; children `~N(parent, σ²)` numbering `Poisson(μ)`; conflict as above | natural clustered archipelagos |
| `line` | centres along a spline with gap `gap`; radius decreasing with arc length `R(s) = R_0·(1 − a·s/L)` (ageing hotspot chain) | current `hotspot` |
| `arc` | same on an `arc` | current `arc` |
| `ring` | centres on a `ring`, elements elongated tangentially (ratio 0.38 of the current `atoll` layout) | atoll |
| `centers` | given positions `(x, z, radius, strength)`; strength scales the profile | heman |
| `grid` | jittered grid (heman's `points_from_grid`) | utility |

**Stable ids**: `"<scatter id>/<slot>"`. When bounded, the slot is the index in the list of sizes drawn
with the slot's hash (not with the position in the loop), so `count = N + 1` leaves the first N untouched;
when infinite, the slot is `(cell, subcell)`.

### 7.7 Ocean margin and seabed (geometric base)

Over the seaward distance `d > 0`:

```
shelf     0 ≤ d < Ws:          z = −Zs·(d/Ws)^1.5            (gentle slope, < 1°)
slope     Ws ≤ d < Ws + Wt:    z = −Zs − (Za − Zs)·smoothstep(0, 1, (d − Ws)/Wt)^0.8
rise      up to + Wr:          smoothed towards the plain
abyssal   after:               z = −Za
```

* Existing presets reused: `compressed` (widths proportional to the frame) and `geologic`.
* Per element: `margin = "passive"` (wide shelf) or `"active"` (narrow shelf and trench: a Gaussian of
  depth `Zf` at distance `Df` from the coast). Blending between elements with the weights of 7.3.
* Lagoon: inside a land `ring`, depth `lagoon_depth` (as now).
* Seabed elements in phase 1: `seamount` (cone) and `guyot` (truncated cone), since they are the same
  primitives. The rest, in phase 6.
* `D` (maximum measured distance) = `Ws + Wt + Wr`; beyond it everything is abyssal plain, and the bin
  index only needs the elements closer than `D`.

### 7.8 Land fraction

* **Bounded**: bisection (8 to 10 iterations) over a scale factor common to the **generated** elements
  (not the declared or fixed ones). Area measured with `Terrain.Sample` of the `coast` mode on a 256² grid
  (counting cells with d < 0). Cost: a few CPU evaluations of a pointwise op.
* **Infinite**: statistical target. Without overlaps (hard-core), coverage ≈ `λ·E[A]` (λ = elements per
  m²); with overlaps allowed (Boolean model), `1 − exp(−λ·E[A])`. λ is calibrated with that formula and a
  warning is given if the requested spacing prevents it (in practice, discs that do not touch cannot cover
  more than ~55 %).
* "All land" does not use a fraction: it is a land `frame` covering the window (or, when infinite, a land
  `fill` element) from which water elements are subtracted.

### 7.9 Classification

* Connected components of land and water over `Terrain.Sample` (a 256² to 512² grid on the analysis
  window), with the current macro's labelling in Lua.
* Land by area: `continent` ≥ `classes.continent` km², `island` ≥ `classes.island`, else `isle`;
  `lake_island` if it is surrounded by water that is not sea. By default, when bounded, Azgaar's thresholds
  as a fraction of the frame (1/10, 1/1000); when infinite, absolute (10 000 km² and 1 km² by default).
  Configurable.
* Water: the element's `role` wins. Without `role`: when bounded, `ocean` if it touches the frame, `sea` if
  its area exceeds `classes.sea`, else `lake`; when infinite, `ocean` if it touches the edge of the
  analysis window (which includes the ring), `sea`/`lake` by area.
* `gulf`: sea water that enters a water `sector`/`ellipse` labelled as such.

### 7.10 Coast detail (phase 4, decided now so the API is not redone)

* The current macro's three-band warp, applied to the op's `position` (a `Shapes.Warp`): no new code.
* Roughness field (Azgaar): `r = clamp((fbm(p/scale)·0.5 + 0.5 − threshold)/(1 − threshold), 0, 1)^contrast`
  multiplies the warp amplitude. Starting values: `scale` 8 to 15 km, `threshold 0.25`, `contrast 1.5`.
* Damped away from the coast: amplitude × `exp(−|d|/L)`, so the warp does not move the inland relief.
* Per element: `roughness` and `coast` (type) in the block, blended with the weights of 7.3.

### 7.11 Bin size and cost

* `bin = max(R_mean, D/2)` clamped to [500 m, 20 km]; the element is registered in every bin closer than
  `R_i + k_i + D` to its centre.
* Target: ≤ 16 elements per bin on average; it is measured and recorded in the metadata
  (`lands.bins.mean`, `lands.bins.max`).
* Estimated cost: an element distance is about 20 to 60 operations; 16 per sample is comparable to a
  4-octave fBm.

---

## 8. Proposed API

Draft: names are fixed while implementing and documented in `docs/lua_reference.md`. Code and keys in
English.

### 8.1 Call

```lua
local land = Lands.Compose{
    seed  = 1,
    frame = { size = 24000, edge_margin = 1500 },     -- or { world_cell = 16000, region = { x0, z0, x1, z1 } }
    sea   = { margin = "compressed" },                 -- default margin
    profile = { "dome", height = 600, width = 4000 },  -- default for elements without their own profile
    elements = { ... },                                -- 8.2
    edit = { ... }, fixed = { ... }, remove = { ... }, -- 8.4
    land_fraction = 0.4,                               -- optional, bounded only (7.8)
    classes = { continent = 5000, island = 1 },        -- km², optional
}
return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
```

### 8.2 Elements

```lua
{ kind = "radial", id = "main", at = { 0, 0 }, area = 120e6, bumps = 3, amplitude = 0.25,
  profile = { "rim", rim = 450, at = 400, sigma = 300, inner = 120, width = 3000 }, smooth = 800 },
{ kind = "sector", water = true, role = "bay", at = { 0, 0 }, angle = 200, width = 25, reach = 6000 },
{ kind = "capsule", from = { 0, -4000 }, to = { 0, -11000 }, width = 2500, profile = "plateau" },
{ kind = "ellipse", water = true, role = "sea", at = { 3000, 2000 }, radii = { 2500, 1500 } },
{ kind = "scatter", id = "islets", count = 8, area = { 0.5e6, 3e6 }, spacing = 600,
  pattern = "cluster", region = { kind = "ring", at = { 0, 0 }, radius = 9000, width = 3000 },
  shape = { kind = "radial", bumps = { 2, 4 }, amplitude = { 0.1, 0.3 } }, profile = "cone" },
{ kind = "ridge", points = { { -6000, 0 }, { 0, 1500 }, { 6000, 0 } }, width = 2500, height = 900,
  spurs = true, saddles = 2 },
{ kind = "seamount", at = { 15000, 4000 }, radius = 3000, height = 1800 },
```

Common fields: `id`, `water`, `role`, `area` (or the shape's dimensions), `rotation`, `scale`,
`mirror = "x" | "z" | "both"`, `smooth` (k, m), `profile`, `margin`, `coast`, `roughness`, `name`.

`area` and the dimensions are mutually exclusive: with `area`, the size is solved with the closed formula of
7.1.

### 8.3 Presets (section 11)

`Lands.Island`, `Lands.Archipelago`, `Lands.Continent` and the new ones (`Lands.Preset{ name =
"mediterranean", ... }`) return the filled-in `Compose` table, which can be modified before building:

```lua
local t = Lands.Preset{ name = "high_island", seed = 3, size = 24000 }
t.elements[1].area = 200e6
return Lands.Compose(t):outputs{ ... }
```

### 8.4 Ids, resolved list and editing

* Declared element: its `id` or its index. Generated: `"<scatter>/<slot>"`.
* `land:elements()` returns the resolved list; the export writes it to the metadata (section 10):

  ```json
  { "id": "islets/3", "kind": "radial", "role": "isle", "area_m2": 1.42e6, "area_geometric_m2": 1.40e6,
    "centroid": [4120, -880], "radius": 690, "max_height": 212, "nearest": ["islets/1", 1340],
    "component": 4 }
  ```

* Editing:

  ```lua
  edit   = { ["islets/3"] = { scale = 1.2 }, main = { profile = "rim" } },
  fixed  = { "islets/1" },   -- does not change even if seed, count or land_fraction change
  remove = { "islets/4" },
  ```

### 8.5 Outputs

The current ones (`land_mask`, `land_height`, `bathymetry`, `surface_height`, `shore_mask`, `coast_type`,
`region_id`, `coast_sdf`). `region_id` becomes the numeric id of the dominant element (id → name table in
the metadata). New outputs (`temperature`, `moisture`, `biome`) in phase 7.

### 8.6 From instruction to parameters

| Instruction | Change |
| --- | --- |
| "An archipelago of 6 islands of 2 to 4 km²" | `scatter{ count = 6, area = { 2e6, 4e6 } }` |
| "Grouped together" | `pattern = "cluster"` |
| "Make island 3 a bit bigger" | `edit = { ["islets/3"] = { scale = 1.15 } }` (area ×1.32) |
| "Island 3 should be 5 km²" | `edit = { ["islets/3"] = { area = 5e6 } }` |
| "Cliffs higher than the centre" | `profile = { "rim", ... }` |
| "A flat island" | `profile = "flat"` or the `low_island` preset |
| "A continent with an inland sea" | `superellipse` + `ellipse{ water = true, role = "sea" }` |
| "All land, no ocean" | the `all_land` preset |
| "All land with lakes" | `all_land` + `scatter{ water = true, role = "lake" }` |
| "A peninsula to the south" | `capsule{ from = south coast, to = ..., width = ... }` |
| "Join islands 1 and 2 with an isthmus" | `smooth` on both or a `capsule` between them |
| "Separate islands 1 and 4" | `edit` with `offset`, or more `spacing` |
| "A seamount to the east" | `seamount{ at = ..., radius = ..., height = ... }` |
| "More land" | larger `land_fraction` (bounded) or larger `count`/`area` (infinite) |

---

## 9. Infinite plane and bounded maps

Common rules:

1. **Nothing depends on the requested window**: no quantiles or normalisations over what is exported.
2. **Absolute sea level** (0 m).
3. **Everything non-local has a bounded radius** (`D`, the ring of cells).
4. **Generators per world cell**, like `Water.Network` with `generate.world_cell`.

**Bounded** (`frame.size`, centred on the origin, or `frame.bounds`):

* Exact `land_fraction` by bisection (7.8).
* Frame percentages accepted (`at = { "30%", "50%" }`) and resolved to metres.
* `edge_margin`: minimum distance from the land to the edge.

**Infinite** (`frame.world_cell`):

* Generated elements are created for the cells that touch `frame.region` plus a ring. The `region` is the
  script's working window (±64 km by default, more than the viewer shows), not a limit of the world: two
  different regions that share cells get the same elements, so the world can be exported in pieces.
* The land fraction is statistical (7.8).
* Declared elements exist at their absolute coordinates.
* Future improvement, only if the `region` falls short for the viewer: generate the `scatter` elements
  directly in the shader by cell hash (the same numbers as the CPU), with no list. Not needed for phases 1
  to 3.

---

## 10. Tools for Claude to work precisely

The minimum needed for an instruction to be applied and checked without looking at images blindly:

1. **Element list in the metadata.** A small binding `engine.report(key, table)` that adds a
   `"script_report"` section to the export's `metadata.json`. `Lands.Compose` writes there the resolved list
   (8.4), the number of bins and the warnings (skipped elements, unreachable fraction).
2. **`terrain_metrics --by-id`**: extends the existing tool to read the `region_id` output and give, per id,
   area, centroid, box, maximum height and whether it touches the edge. Reuses `metrics::Measure` and the
   labelling.
3. **`tools/dev.sh lands <script> [--param k=v]...`**: exports, runs `terrain_metrics --by-id`, compares the
   script's list with what was measured (geometric against measured area, ids present) and writes a short
   summary; with `--hillshade`, the review image. Reuses the `review` subcommand.
4. **`tools/dev.sh lands-check <script> --expect count=6,area=2e6..4e6`**: instruction check (the same as
   the test does, from the terminal) so Claude validates its change before calling something done.
5. **`terrain-author` skill**: a "Lands" section with the flow (closest preset → `elements()` → `edit` →
   `lands-check` → detail) and table 8.6.
6. **Actionable errors**: every invalid key or impossible combination fails with what was expected, like
   the rest of the prelude (table-driven `check_args` in `Lands`, extensible to the rest in phase 8).

---

## 11. Priority phases (1 to 3)

Each step is a commit. Each phase ends with its documentation, images, `status.md` and `todo.md`.

### Phase 1: one island, end to end

**Goal.** Land of one or a few elements with a free profile, basic margin, ids, editing, on a bounded map
and on the infinite plane, on the `Lands` op.

| Step | Content | Reuses |
| --- | --- | --- |
| 1.1 | Unify `hasher`, `make_noise`, `smooth` into one prelude file loaded before `30_ranges` | the three current copies |
| 1.2 | `lib/shape.lib.glsl` with the distances from `shape.comp.glsl`; `Shape` includes it without changing results (`shapes_r2` goldens unchanged) | `shape.comp.glsl` |
| 1.3 | `Lands` op `coast` mode with `circle`, `ellipse`, `box`, `polygon`, `superellipse`, `radial`, `capsule`, `ring`, `sector`, `frame`; ordered CSG with smin/smax; bins; CPU reference in `sampler.cpp`; `Lands.Block` binding | `Water` pattern, `block` helper, `SmoothMinimum` |
| 1.4 | `test_geometry`: each primitive against its closed distance at chosen points; GPU against CPU; area of each primitive against its formula | `test_geometry`, `Terrain.Sample` |
| 1.5 | Minimal `Lands.Compose`: `frame`, declared `elements`, `area` → size, current outputs from the `coast` mode | output functions of `50_lands.lua` |
| 1.6 | `relief` mode: profiles of 7.4, weights of 7.3, hypsometry | `Curves.Profile` (formulas) |
| 1.7 | `seabed` mode: margin of 7.7 with `compressed`/`geologic`, `passive`/`active` presets, lagoon, `seamount`/`guyot` | `MARGINS` |
| 1.8 | Ids, resolved list, `edit`/`fixed`/`remove`, `engine.report`, `terrain_metrics --by-id`, `dev.sh lands` and `lands-check` | `terrain_metrics`, `dev.sh` |
| 1.9 | Infinite plane: `frame.world_cell` with declared elements; identical overlapping exports | `world_cell` rule |
| 1.10 | `Lands.Island` on `Compose` with presets `volcano`, `high_island`, `low_island`, `atoll`, `rim_island`, `mesa_island`; removed keys fail saying what to use | current coast type and warp |
| 1.11 | The phase's examples (13.2) with their goldens; documentation | — |

Acceptance criteria:

* Distance of each primitive exact on the coast (or, for the 1st-order ones, error < 1 % of the size).
* Area of each primitive (without detail) within 1 % of its closed formula.
* `edit{ scale = 1.2 }` multiplies the area by 1.44 ± 2 %.
* `rim`: maximum height closer than `at + 2σ` to the coast and above the interior; `dome`: in the interior.
* `all_land` and `ocean_only` export without error with `land_mask` 1 and 0 across the whole map.
* Infinite: two overlapping exports identical byte for byte in the common area; different section sizes
  identical.
* Default island script in under 0.5 s in Release (6 s today).
* Full suite green in Debug and Release.

### Phase 2: archipelagos

| Step | Content | Reuses |
| --- | --- | --- |
| 2.1 | Bounded `scatter`: log-uniform sizes, dart throwing with a grid, `spacing`, `edge_margin`, `region`; ids per slot | `scattered` layout |
| 2.2 | Infinite `scatter`: per-cell Poisson with hash priority | `world_cell` |
| 2.3 | Patterns `cluster`, `line`, `arc`, `ring`, `grid`, `centers` | current `hotspot`, `arc`, `atoll` |
| 2.4 | `land_fraction` by bisection (bounded) and by calibration (infinite) | `Terrain.Sample` |
| 2.5 | Classification of 7.9 | current labelling |
| 2.6 | `Lands.Archipelago` as a preset: `scattered`, `cluster`, `hotspot`, `arc`, `atoll`, `shattered`, `chain` (islands side by side, `chain_width`) | — |
| 2.7 | The phase's examples and goldens (`lands_archipelago_*` regenerated) | `tests/datasets` |

Acceptance criteria:

* "N islands of A₁ to A₂ km²": N islands in the list, all with geometric area in the range, and `--by-id`
  measures the same within 3 %.
* Minimum spacing respected for every pair (minimum channel of `metrics::Measure` ≥ `spacing` − 1 sample).
* `count` from N to N + 1: the first N with the same centroid and area.
* Infinite: the same island, equal from two windows; land fraction measured over 200 × 200 km within
  ±10 % of the target.
* `chain` and `arc` with `chain_width` reach the requested fraction (today they stop at 6 to 12 %).

### Phase 3: continents and other lands

| Step | Content | Reuses |
| --- | --- | --- |
| 3.1 | Generated relief in the `relief` mode: `ridge` with `spurs` and `saddles`, `massif`, `volcano`, `depression`, `escarpment`, `valley` | `Landforms`, `sample_spline` |
| 3.2 | Presets `continent`, `pangea`, `old_world`, `mediterranean`, `inland_sea`, `peninsula`, `isthmus`, `all_land`, `lakeland`, `fractious`, `taklamakan`, `ocean_only` as compositions (table below) | — |
| 3.3 | `edge_margin` and frame percentages | — |
| 3.4 | `Lands.Continent` as a preset; `lands_continent` golden regenerated | — |
| 3.5 | Measure `docs/references/{islands,archipelagos,continents}` with `terrain_metrics` and set acceptance bands per type | `terrain_metrics` |
| 3.6 | The phase's examples, goldens, documentation | — |

Composition of each preset (all with primitives; the numbers are starting points and editable):

| Preset | Composition |
| --- | --- |
| `continent` | `superellipse` (n 2.5 to 4) + 2 to 4 `radial` lobes joined with `smooth`; 1 to 3 `ridge` with `spurs`; 0 to 2 `sector` gulfs; passive margin and one active one with a trench |
| `pangea` | 4 `superellipse` blocks in the corners + central band, joined; partial water `trench` (inland seas) |
| `old_world` | 3 large blocks; 2 to 3 water `band` (straits); long `ridge` |
| `mediterranean` | land `frame` + elongated water `ellipse` (`role = "sea"`) + islands by `scatter` inside the sea |
| `inland_sea` | continental block + inland water `superellipse` |
| `peninsula` | block on one edge + `capsule`/`band` towards the sea + islands at the tip |
| `isthmus` | two blocks + narrow `capsule` between them |
| `all_land` | land `frame` covering the window (or `fill` when infinite); relief by elements |
| `lakeland` | `all_land` + `scatter{ water = true, role = "lake" }` |
| `fractious` | 12 to 15 medium `radial` by `scatter` with large `smooth` + water `band` cutting them |
| `taklamakan` | `all_land` with `ridge` on the four edges and a central `depression` (closed basin) |
| `ocean_only` | no land; `seamount`, `guyot`, no margin; abyssal plain |

Acceptance criteria:

* `mediterranean`: a single inland body of water classified `sea`, not touching the frame.
* `all_land`: `land_mask = 1` across the whole map; `lakeland`: the lakes with the requested area.
* A peninsula of width W measures W ± 5 % on its axis.
* Range: maximum crest height ± 2 % of the requested one; spurs at `spacing` ± 10 %.
* Shape metrics within the bands measured on the references of the type.

---

## 12. Later phases (4 to 8)

### Phase 4: detail on the shapes

1. Three-band coast warp with a roughness field (7.10), per element.
2. Relief noise with amplitude in metres tied to the inward distance (heman: bounded `s·n`) and damped on
   the peaks (`(1 − h/H)^4`, heman).
3. `field` element: noise threshold with `inflate`, `round` and a growing threshold
   (`n − (1 − inflate)·d² < 0`, mapgen2; `c > ratio·(1 + d²)`, voronoi-map), for fully procedural coasts
   when asked for.
4. `Erosion.Gullies` and `Erosion.StreamPower` (via `Terrain.Sample` → `StreamPower` → `Terrain.Grid`, as
   now, with the `world_cell` rule for the infinite case); `Ranges.Carve` for ranges.
5. Exact distance (Felzenszwalb + CPCF, bounded to `D`) in C++ only if erosion moves the coast and it has
   to be measured again.
6. Fetch: from each coast point, the open-water distance upwind, by ray marching over the `coast` mode on
   the analysis grid.
7. Validation: maximum coast displacement and height amplitude within what was declared.

### Phase 5: hydrology

1. `Water.Network` and `Water.Drain` on the composed surface (they already analyse any terrain with the
   sampler): lakes and rivers as water. Closes "the macros do not drain".
2. Closed, salt, dry and frozen lakes by evaporation and inflow balance (Azgaar).
3. Inland seas (`role = "sea"`) as bodies of water with their own level, not as lakes.
4. River width `∝ flow^0.7`.
5. Drainage weighted by precipitation once phase 7 exists.

### Phase 6: full ocean

1. Margin per coast stretch (not only per element).
2. Seabed with primitives as elements with an id:

   | Shape | Primitive |
   | --- | --- |
   | Trench | `trench` on an `arc` |
   | Mid-ocean ridge | long `ridge` with a central rift (narrow `trench`) and transform faults as offsets per stretch |
   | Basin | `depression` |
   | Submarine canyon | `band` from the shelf edge, growing depth |
   | Reef | shallow `ring` |

3. Seamounts and guyots by `scatter`.
4. Detail: abyssal hills with amplitude ∝ depth (heman's `emboss`).
5. Future: sediment and fans, submarine erosion, atoll by subsidence.

### Phase 7: climate and biomes

1. Raise `kMaxOutputs` to 16 (or `outputs = { ... }`).
2. Temperature by latitude (Azgaar's segments) and −6.5 °C/km; latitude by world coordinate.
3. Precipitation: wind march with recharge over water, orographic rain and rain shadow (Azgaar), with a
   bounded radius.
4. Moisture `1 − (d/max)^0.5` from fresh water, `max` fixed in metres.
5. Biomes by Whittaker table (mapgen2), as an optional output.

### Phase 8: infrastructure

* `engine.memo(key, fn)` or `Terrain.Bake` if the viewer suffers.
* Table-driven `check_args` across the whole prelude.
* Golden failure message with box and maximum difference.
* Shader generation of the `scatter` elements (9) if the `region` falls short.

---

## 13. Example catalogue

### 13.1 Common rules

* Location: `assets/scripts/lands/` (they replace the current ones; the current names stay as presets).
* Header of each script: what it shows, parameters (`params.*`), viewer and export command, with the
  recommended resolution for that size.
* All accept `--param seed=N` and the type's own parameters (`count`, `area`, `size`...).
* The viewer shows about 28 km: examples up to 24 km are seen whole; larger ones are reviewed with
  `tools/dev.sh review` (hillshade) and in the viewer by area with `--target`.
* Each example has, at least, a golden in `tests/datasets/lands_<name>/` at 256² (or 512² if the shape
  needs it), with `tolerance 0` and performance measurement, like the current ones.
* Infinite examples also have an overlap test and an id stability test.

Reference resolutions by size (viewer with `--resolution`; golden at 256²):

| Size | Viewer | Golden (256²) |
| --- | --- | --- |
| 4 km (islet, small atoll) | 2 m | 16 m |
| 12 km (small island) | 4 m | 48 m |
| 24 km (island, small archipelago) | 8 m | 96 m |
| 48 km (archipelago) | 16 m | 192 m |
| 96 km (continental region) | 32 m (by area) | 384 m |
| 192 km (continent) | 64 m (by area) | 768 m |

### 13.2 Phase 1: islands (bounded)

| Script | What it shows | Size |
| --- | --- | --- |
| `island_dome.lua` | radial island with a `dome` profile | 24 km |
| `island_rim.lua` | cliffs higher than the interior (`rim`) | 12 km |
| `island_plateau.lua` | plateau island (`plateau`) | 12 km |
| `island_caldera.lua` | volcano with a caldera and an inner lagoon | 12 km |
| `island_tilt.lua` | cuesta: high on one side, beach on the other | 24 km |
| `island_low.lua` | low island (`flat`), cay style | 4 km |
| `island_volcano.lua` | cone with a crater, active margin with a trench | 24 km |
| `island_bay.lua` | island with bays (water `sector`) and a peninsula (`capsule`) | 24 km |
| `atoll.lua` | ring of islets and lagoon | 4 km |
| `island_edit.lua` | the same island with `edit` (scale, profile) and `fixed`, to see editing | 24 km |
| `all_land.lua` | all land with relief by elements | 24 km |
| `ocean_only.lua` | all sea: no margin, seamounts and guyots | 48 km |

### 13.3 Phase 1: infinite plane

| Script | What it shows | World cell |
| --- | --- | --- |
| `infinite_islands.lua` | islands declared at far coordinates (±100 km) to test anchoring to the world | 16 km |

### 13.4 Phase 2: archipelagos

| Script | What it shows | Size / cell |
| --- | --- | --- |
| `archipelago_scattered.lua` | `count` islands from a range of areas | 48 km |
| `archipelago_cluster.lua` | clusters (Neyman–Scott) | 48 km |
| `archipelago_hotspot.lua` | shrinking chain | 48 km |
| `archipelago_arc.lua` | island arc with a trench | 96 km |
| `archipelago_atoll.lua` | scattered atolls | 24 km |
| `archipelago_shattered.lua` | land broken by channels | 48 km |
| `archipelago_chain.lua` | wide chain that reaches the requested fraction | 48 km |
| `archipelago_centers.lua` | islands at given positions | 24 km |
| `infinite_archipelago.lua` | infinite archipelago by cells | 16 km |
| `infinite_cluster.lua` | infinite clusters | 24 km |

### 13.5 Phase 3: continents and other lands

| Script | What it shows | Size / cell |
| --- | --- | --- |
| `continent.lua` | block with lobes, ranges with foothills, gulfs, mixed margin | 192 km |
| `pangea.lua` | supercontinent with inland seas | 192 km |
| `old_world.lua` | blocks separated by straits | 192 km |
| `mediterranean.lua` | inland sea with islands | 96 km |
| `inland_sea.lua` | continent with a closed inland sea | 96 km |
| `peninsula.lua` | peninsula with islands at the tip | 48 km |
| `isthmus.lua` | two masses joined by an isthmus | 48 km |
| `lakeland.lua` | all land with lakes | 24 km |
| `fractious.lua` | fragmented lands | 96 km |
| `taklamakan.lua` | closed basin surrounded by mountains | 96 km |
| `infinite_continents.lua` | infinite continents (large elements per cell) | 128 km |
| `infinite_lakeland.lua` | infinite land with lakes | 16 km |

### 13.6 Gallery

`assets/scripts/gallery.lua` generates one plate per type for `docs/images/lands/` and `lua_reference.md`.

---

## 14. Validation and tests

| Test | What it checks | Where |
| --- | --- | --- |
| Primitive distances | closed values at chosen points; GPU = CPU | `test_geometry` |
| Primitive areas | closed formula against `Terrain.Sample` | `test_geometry` |
| CSG | order, smin with `k` (error ≤ `k/4`), subtraction | `test_geometry` |
| Profiles | position and value of the maximum per preset; continuity between elements | `test_lands` |
| Margin | shelf, slope and abyssal depth against what was requested | `test_lands` |
| Instructions | one case per row of 8.6, with the resolved list and `metrics::Measure`/`--by-id` | `test_lands` |
| Id stability | `count` N → N + 1, `seed` of another generator, `fixed` | `test_lands` |
| Infinite plane | identical overlapping exports; same ids from two windows; statistical fraction | `test_lands` (exporting like `test_cli`) |
| Sections | different section sizes identical | `test_seams` / `test_cli` |
| Goldens | every example in the catalogue, `tolerance 0`, performance | `test_datasets` |
| Viewer | one bounded and one infinite example captured with `--screenshot` contain land and change with the camera | `test_viewer` |
| Errors | invalid keys, impossible combinations, `world_cell` too small: message and script line | `test_lua_errors` |
| References | bands per type measured in `docs/references/` | `test_lands` |
| Time | script time per golden recorded; no phase worsens it without justification | `test_datasets` |

All suites in Debug and in Release, one at a time, before every commit that touches the engine.

---

## 15. Risks and open decisions

| Topic | Risk or decision | Proposal |
| --- | --- | --- |
| New op in C++/GLSL | layer change (composition leaves pure Lua) | justified by the node limit and the cost; same pattern as `Water`. Confirm before step 1.3 |
| `engine.report` | new binding in the engine | small; confirm before step 1.8 |
| `Lands.*` compatibility | current goldens and scripts change | regenerate goldens; removed keys fail with the alternative |
| 1st-order distances | error far from the coast in `radial`/`superellipse` | exact on the coast; the profile absorbs the rest; tests with a declared tolerance |
| `smooth union` | not an exact distance in the blend | error ≤ `k/4`; acceptable for shape; measure with `Terrain.Sample` |
| Bins with `geologic` margins | a `D` of 100 km makes every element fall in every bin | measure; if it exceeds 16 per bin, a separate coarse bin just for the margin |
| Land fraction when infinite | statistical only | document it and measure it over large windows |
| `region` when infinite | the element list covers the working window | ±64 km by default; shader generation in phase 8 if needed |
| `kMaxOutputs = 8` | Lands uses 7 | raise to 16 in phase 7 |
| Suite time | more goldens | goldens at 256² and a pointwise op: they should cost less than the current ones (6 to 27 s of script each) |

---

## 16. Workflow

* **One commit per step**, never `--amend`; single-line Conventional Commits.
* If a commit changes the state of a milestone or an item, it updates `docs/status.md` and `docs/todo.md` in
  the same commit.
* Iterate with `tools/dev.sh test -L fast`; before every commit that touches the engine, full suite in Debug
  and in Release, **one suite at a time** (they share `out/`).
* `volume_r3` in `test_datasets` can fail on time in Release: repeat before investigating.
* Build with `build.cmd` (from bash, `cmd //c` with a Windows path); `ctest` by its full path.
* Code in English; no `new`/`delete`/`malloc` in the engine; `ENGINE_ASSERT` with no side effects.
* A new op needs its entry in `kOps` in `OpKind` order (there is a `static_assert`), its shader in
  `engine_shaders` and its reference in `sampler.cpp`.
* When closing each phase: examples, goldens, images, `lua_reference.md`, the `terrain-author` skill and
  this plan.

---

## 17. References

Repositories analysed:

* mapgen2: <https://github.com/redblobgames/mapgen2> (`water.js`, `elevation.js`, `rivers.js`,
  `moisture.js`, `biomes.js`, `noisy-edges.js`); Amit Patel's article "Polygonal Map Generation for Games".
* heman: <https://github.com/prideout/heman> (`include/heman.h`, `src/generate.c`, `src/ops.c`,
  `src/distance.c`).
* voronoi-map-js: <https://github.com/rjanicek/voronoi-map-js> (`src/island-shape.js`, `src/map.js`,
  `src/noisy-edges.js`, `src/lava.js`).
* Fantasy-Map-Generator: <https://github.com/Azgaar/Fantasy-Map-Generator>
  (`src/data/heightmap-templates.ts`, `src/model/physical/heightmap-generator.ts`, `features-generator.ts`,
  `lakes.ts`, `river-generator.ts`, `temperature-generator.ts`, `precipitation-generator.ts`,
  `coastline-generator.ts`, `ocean-generator.ts`, `biomes-generator.ts`).

Algorithms cited:

* Í. Quílez, 2D distance functions (`sdSegment`, `sdUnevenCapsule`, `sdArc`, `sdPie`, polynomial smin),
  iquilezles.org.
* R. Bridson, "Fast Poisson Disk Sampling in Arbitrary Dimensions" (2007).
* L.-Y. Wei, "Parallel Poisson Disk Sampling" (2008).
* Neyman–Scott and Thomas processes (clustered point processes); Boolean model for coverage.
* P. Felzenszwalb, D. Huttenlocher, "Distance Transforms of Sampled Functions" (2012).
* Sean Barrett's ambient occlusion by horizon sweeps (nothings.org).

YATE files involved:

* `assets/lua/prelude/10_shapes.lua`, `20_landforms.lua`, `30_ranges.lua`, `40_dunes.lua`, `45_water.lua`,
  `46_network.lua`, `50_lands.lua`
* `assets/shaders/ops/shape.comp.glsl`, `water.comp.glsl`, `arith.comp.glsl`, `assets/shaders/lib/`
* `src/terrain/kernels.cpp`, `src/terrain/sampler.cpp`, `src/terrain/graph.cpp`, `src/lua.cpp`,
  `include/engine/terrain/graph.hpp`, `include/engine/terrain/export.hpp`
* `assets/scripts/lands/*.lua`, `assets/scripts/gallery.lua`
* `tests/unit/test_geometry.cpp`, `test_lands.cpp`, `test_cli.cpp`, `test_viewer.cpp`, `test_seams.cpp`,
  `test_lua_errors.cpp`, `terrain_metrics.hpp`, `tests/tools/terrain_metrics.cpp`, `tests/datasets/lands_*`
* `tools/dev.sh`
* `docs/status.md`, `docs/todo.md`, `docs/lua_reference.md`, `docs/references/`, `docs/images/lands/`
* `.claude/skills/terrain-author/SKILL.md`
