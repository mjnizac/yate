# Open tasks

Only what is still open. Finished work and the measurements behind it are in `docs/decisions.md`; the
full history is in the git log.

## Correctness

- [ ] **Coarse levels are spiky.** With iterations scaled to the spacing, a 16 m preview runs one
      erosion iteration and shows the noise's finest octaves, which are about two samples wide, as
      point-sampled spikes (99th percentile slope 1.6 against 1.2 before on the volcano). The honest fix
      is to limit a noise's octaves to what the spacing resolves, which would make noise values differ
      between spacings where they are bit-identical today; not done.

- [ ] **Hydraulic erosion leaves a high-frequency ripple.** A `Filters.Blur{ radius = 1 }` after the op
      clears it, and every sample terrain does that. Adding the four diagonal neighbours (weight
      1/sqrt(2), `flow_rate` bound lowered to 0.146) was tried and **rejected on measurement**: the
      mean |second difference| went from 5.3 to 6.4 along the axes and 7.8 along the diagonals, and the
      axis-to-diagonal ratio flipped from 1.36 to 0.83. The diagonals move where the residue lands
      rather than removing it, so the cause is in the per-cell erosion and deposition step
      (`taken`/`dropped` in `hydraulic_erosion.comp.glsl`), not in the transfer stencil.

## Testing

- [ ] **CI is red on Windows since milestone 11, and Linux has never built.** `ci.yml` has run on
      GitHub (read from the public API; the logs need admin rights). Run `9dba657` passed on Windows.
      Runs `39092a3` and `ccd62ee` fail in the step "Tests that need no device" on both Windows
      configurations, and every Linux job fails in "Build" in every run. The Windows failure does **not**
      reproduce here: a clean clone of `ccd62ee`, configured and built exactly as the workflow does, with
      CRLF checkout and with the Vulkan drivers disabled, passes all six suites. The failing step's log is
      the missing piece: it names the suite. The same goes for the Linux compile errors.

- [ ] **One GPU, one vendor, one OS.** Everything has been validated on a GTX 1070 on Windows. Queue
      family selection, memory types, readback and the swapchain are exactly the areas where AMD and
      Intel differ. It needs hardware, and the GPU job above is how it would be exercised.

- [ ] **The open sea ends at the ring.** The quad that draws water where no terrain was covers the
      footprint of the resident tiles, so a surface at 0 m above a floor at -50 m shows a plate whose far
      edge floats above the terrain's. Extending it to the far plane would paint sea over a world that
      has not been evaluated, which is the less honest of the two.

- [ ] **No introspection of the API.** An agent can learn a call's keys only from the docs or from the
      error an unknown key produces, which lists them. A generated catalogue would need the bindings to
      declare their parameters in data instead of reading them as they go.

- [ ] **User macros cannot be shared between scripts.** `require`, `dofile` and `load` are removed on
      purpose, and the prelude is a closed list of files. A script can define its own local functions;
      sharing them across scripts needs a loading mechanism and a security decision, neither made.

- [ ] **Slope statistics.** The sidecar reports min, max, mean and the count of non-finite samples. Slope
      percentiles are computed by `test_geometry` from the raw export and are not in the sidecar.

## Terrain quality (in this order)

The goal is that a terrain is composed from validated examples and judged by numbers, not only by a
viewer screenshot. Each item is done when it is in `ctest`.

- [ ] **The alpine family rebuilt in stages**, as the cliffs were (`terrains/cliffs.lua`, cases
      `tests/library/cliffs*.json`). The alpine case (`tests/library/alpine.json`) shows the measured gap:
      a spectral slope of 5.3 against 3.9 on the four reference heightmaps, so it lacks fine relief.
- [ ] **Cliffs, what is still open** after the plan from World Machine and Gaea practice (re-entrants,
      cap rock over a softer base, a lower slope carved by stream power, talus aprons, F3 - F1 cracks and
      blocks, sea cliffs and beaches, irregular beds, chutes, a texture pass, strata and debris outputs):
      no heightmap reference with a known scale exists for the family, so its case holds guards, not
      reference ranges; the chutes' heads are rounded capsules and read drawn; the sea cliffs are low
      against the escarpment; 0.3 % of the land is still closed depressions, in amphitheatre floors and
      behind a headland whose sea cliff dams a valley; the `strata` and `debris` outputs are not used by
      the viewer's palette yet; the stream-power grid covers +-6.4 km, past which the lower slope is held
      at the grid's edge.
- [ ] **Hypotheses from World Machine and Gaea practice**, to test on the alpine case next, each a
      prelude function or a script change only if it measures better. Tested on the cliffs and kept:
      strata as a sideways displacement along the slope (the plateau went from 7.2 to 1.6 degrees p90),
      provided the bed spans a few samples and the residue is continuous. Still open: irregular bed thickness by terracing a monotone remap of height; a convexity mask
      (`h - Blur(h)`) to confine detail and erosion to walls and ridges; detail octaves above the
      stream-power grid scale with an amplitude ratio set from the target spectral slope; erosion
      passes ordered coarse to fine; the difference between a terrain before and after an erosion
      recombined as debris and broken terraces; terraces cracked by cells before erosion; cells with
      F3 - F1 (more break points than the current F2 - F1) for cliff edges.
- [x] **Drainage made valid by breaching as well as filling.** `Hydrology.Drain` (Lindsay's hybrid,
      decided per depression), and `Water.Drain` in the prelude. `Erosion.StreamPower` still only fills
      inside, and cuts straight D8 spokes across what it fills: breach the grid with `Hydrology.Drain`
      before it (`assets/scripts/terrains/lakes_rivers.lua`) until it routes flats itself.
- [ ] **Vector displacement for real cliffs (roadmap, not now).** A heightfield cannot hold overhangs
      and stretches detail above about 45 degrees. World Machine 4059/4065 added vector displacement
      maps that erosion, thermal weathering and strata run on, converted back to a heightfield plus
      cliff meshes. YATE's mappings already have `R2->R3`; an offset output is the natural form.
- [ ] **References with a known scale.** The usable alpine references are 8-bit heightmaps of unknown
      size and height, so only scale-free numbers (spectral slope, hypsometric integral) can be targets;
      `heightmap1.jpg` and `heightmap2.jpg` are shaded relief, not heights. The cliffs family has no
      heightmap reference at all.
- [x] **Rivers and lakes**: `Water.Drain` and `Water.Apply` give `lakes` and `rivers` outputs, drawn by
      the viewer's water with Sea, Lakes and Rivers toggles.
- [x] **Rivers and lakes as designed elements**: `Water.Network` (drawn, from `--shapes` or generated,
      kinds of valley and basin, lakes at any level, the sea only where it reaches the edge).
- [x] **Water that rose and ran dry along its channel** (phase 1 of the water plan): confluences resolved as
      a tree without raising a level, a tributary's water giving way to its river's, floodplains filled
      before channels are cut, detail kept out of the water, duplicates dropped; `net:audit` measures it.
- [ ] **Water plan, phases 2 to 5**: meanders as sine-generated curves, cut banks and point bars, width
      from drainage area, rapids and falls, deltas; lake outlines from contours, outlets at the lowest
      shore point; a fused carve op for the node cap; generation per world cell; texture masks.
- [ ] **Generation over an unbounded world**: drawn elements work anywhere, but `generate` analyses one
      `region`; the same region gives the same rivers in the viewer and in any export inside it. Rivers
      everywhere without a region needs the analysis per fixed world cell, a river owned by the cell of
      its source.
- [x] **The graph's 1024 nodes capped a script at about twenty rivers and lakes**: the fused `Water` op
      carves each in one node, its channel measured exactly on its course; `test_water` holds 35 elements.
- [ ] **Lakes found by the generator have star-shaped outlines** (the farthest cell in 32 directions),
      which overreach in long, branching depressions.
- [ ] **The macros do not drain yet**: `Lands`, `Ranges` and the cliffs leave their pits; give them
      `Water.Drain` and their own `lakes` and `rivers`.
- [ ] **Later, if the above justifies it**: erosion by a hardness map, environmental layers for biomes
      as outputs, more cells and threads for `StreamPower`.

## Viewer and performance

- [ ] **Tiles are evaluated one submit and wait at a time** (`EvaluateTile`): batch several tiles per
      submission and add them to the cache when their timeline value is reached.
- [ ] **Lua work is repeated on every level change**: the macros rebuild resolution-independent CPU grids
      each time. Either an `engine.memo(key, fn)` cleared on hot reload, or a `Terrain.Bake` op that
      evaluates a GPU subgraph on a coarse grid, which would also remove the three `make_noise` copies.
- [ ] **One global level limits the view distance**: CDLOD-style per-tile levels with geomorphing. A
      design, after the two items above.
- [ ] **Normals take three floats per sample** in the viewer; an octahedral 2 x 16-bit encoding is a
      third of it.
- [ ] **`StreamPower` costs ~145 ns per cell and step** on one thread; profile first. `Erosion.Wind` has
      no result cache.
- [ ] **Export is bound by PNG encoding** at 16k; measure the zlib level on 16-bit heights.
- [ ] **Eight outputs per script** (`Graph::kMaxOutputs`); 16 would cost readback space only when used.

## Prelude and tooling

- [ ] **`make_noise`, the hash and `smooth` are written three times** (`30_ranges`, `40_dunes`,
      `50_lands`) with small differences.
- [ ] **Macro argument checks are hand-written per macro**; a data-driven `check_args` would make errors
      uniform and start the API catalogue.
- [ ] **A golden failure reports a count**, not where: print the bounding box and the largest difference.
- [ ] **The suite takes three to six minutes**, mostly macro cases paying their own simulation; a
      `cells` key per case would cut it.
- [ ] **`docs/architecture.md` does not describe the CPU ops** (`StreamPower`, `Wind`, `Grid`), their cost
      or their caches.

## Not yet decided

- [ ] **The standard's catalog.** The draft `terrain_engine_spec_terrain_standard.md` (on the Desktop, not in the repo) describes a 25-node catalog with macros, file
      inputs and global operations. It would be built on the items above rather than instead of them,
      and its first phase is already mostly done: per-metre erosion, a pixel check and a file-bytes
      check were its "make the current engine honest" list. Whether to take the rest is open.

## Alpine macro (`Ranges.*`)

- [ ] **Shadows come from the simulation grid** (~100 m cells): the strata cliffs and the gullies cast
      none. A sun-direction heightmap pass in the viewer would cover any script.
- [ ] **`--detail 1` lost a piece of foreground** next to the camera once; not investigated.
- [ ] **Strata steps are regular close up**: one bed thickness and one dip per terrain.
- [ ] **`test_datasets` `volume_r3` timing** fails intermittently (24 to 29 ms against a 20 ms baseline,
      the same spread the history shows for weeks); the baseline or the tolerance needs a decision.

## Lands macro (`Lands.*`)

- [ ] **No real references measured yet**: the heightmaps in `docs/references/{islands,archipelagos,
      continents}` have not been run through `terrain_metrics`; per-type acceptance numbers wait for it.
- [ ] **A chain, an arc and an atoll reach 6 to 12 % of the map**, not 70 %: the layouts keep their
      islands apart. A broader chain (islands side by side) would fill more of a square.
- [ ] **The 70 % island touches the map edge in places**: the land is cropped by the window, as a piece
      of a larger landmass; the references mostly leave a sea margin.
- [ ] **Coast type has no fetch**: exposure is the normal against one wave direction.
- [ ] **No lakes or rivers as water in the Lands macros**: drowned valleys that do not reach the sea count
      as lagoons in the metrics (10 to 30 on a default island); see "The macros do not drain yet".
- [ ] **The grid is 256 cells** (94 m at 24 km): the drainage is as fine as that; a larger map has
      coarser valleys until the grid is raised (`cells`, up to 512).

## Dune macro (`Dunes.*`)

- [ ] **No sections-parallel and sections-perpendicular profiles are measured**, only slope and height
      histograms; the back (5 to 15 degrees) and the slip face (at most `repose`) are checked by eye in
      the viewer. A per-direction histogram would make it a criterion.
- [ ] **Contrast against the references is lower**: the viewer has one fixed sun and no cast shadows, so
      dunes read softer than in the photographs. The same limit as the alpine macro.
- [ ] **`"simulated"` repeats every `cells * cell` metres** (5.8 km by default) and costs a few seconds
      the first time; it is kept as an explicit option, not the default.
- [ ] **Slopes over 40 degrees (0.03 %)** are the edges of 2 m-high small dunes at a 2 m spacing: two
      samples wide. A sampling limit, not a construction one.
- [ ] **`"star"` still uses the first pointwise pattern** (billow bend, `flats`, a thermal pass): it has
      not been rebuilt on the per-dune profile of `"procedural"`.
