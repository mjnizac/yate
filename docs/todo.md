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
- [x] **Water plan, phase 2**: sine-generated meanders with Kinoshita skew, cut banks and point bars,
      width from drainage summed at confluences, falls with plunge pools and rapids, deltas and estuaries.
- [x] **Water plan, phase 3**: lake outlines from contours, outlets at the lowest shore point, cirque
      headwalls and moraine dams, paternoster lakes, shore kinds, a lobed delta fan.
- [x] **Water plan, phase 4**: levels sampled on the course, falls found on them; the fused op.
- [ ] **Water plan, phase 5**: texture masks, water colour by body, foam.
- [x] **Generation over an unbounded world**: `generate.world_cell` snaps the analysis to fixed world
      cells plus a ring, `count` per cell; two exports touching the same cells get the same rivers
      (`test_water`).
- [x] **The graph's 1024 nodes capped a script at about twenty rivers and lakes**: the fused `Water` op
      carves each in one node, its channel measured exactly on its course; `test_water` holds 35 elements.
- [x] **Lakes found by the generator had star-shaped outlines**: now the contour of their level.
- [ ] **The macros do not drain yet**: `Lands`, `Ranges` and the cliffs leave their pits; give them
      `Water.Drain` and their own `lakes` and `rivers`.
- [ ] **Later, if the above justifies it**: erosion by a hardness map, environmental layers for biomes
      as outputs, more cells and threads for `StreamPower`.

## Viewer and performance

- [ ] **`test_viewer` "distance picks the level"** now waits up to 10 rounds of 60 frames for `MissingTiles() == 0`;
      passes in Release, Debug not rechecked. Before it failed in every Debug run and,
      from 2026-10-28, in most Release runs, with its own noise script. Find whether the build is too slow
      for the frame budget the test allows or tiles are really missing.

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

- [x] **`make_noise`, the hash and `smooth` were written three times**: the hash and `smooth` live in
      `05_common.lua` (`engine.lattice_hash`, `engine.hasher`, `engine.smooth`); each macro keeps its own
      value noise, which differs on purpose (periodic for dunes, its own lacunarity for lands).
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

## Lands macro (`Lands.*`)

The plan is `docs/lands_plan.md` (local, not versioned): shapes first, detail after.

- [x] **Phase 1, islands**: the `Lands` op and `Shapes.Set` (a list of shapes with ids in one node, through
      bins), `Lands.Compose` (areas, profiles per element with no assumed shape, lagoons, seamounts, guyots,
      active margins, edits by id, the report in metadata, bounded maps and `frame.world_cell`), island
      presets in `Lands.Island`, `terrain_metrics --by-id`, `dev.sh lands` and `lands-check`, 9 new goldens.
- [x] **Phase 2, archipelagos**: `scatter` elements (dart throwing in a bounded map, stable as the count
      grows; hash-priority Poisson per world cell in the unbounded one), patterns (uniform, cluster, line,
      arc, ring, grid, centers), `land_fraction` by rescaling the scatters, classes by area in the report,
      water scatters, atolls with their lagoons; the presets of `Lands.Archipelago`; 10 new goldens.
- [x] **The margin creased on the medial axis** between two coasts: the sea floor is now built on the
      distance with the land joined by a smooth minimum (the `margin` mode), and trenches join as a 4-norm.
- [x] **Phase 3, continents and other lands**: `ridge` elements (crest pieces meeting in saddles, spurs every
      `spacing`) and relief elements, land profiles joined by their maximum, water bodies classed in the
      report, `frame.edge_margin` and percentages, the land presets of `Lands.Continent` (the grid-built
      macro retired), the references measured into bands in `test_compose`; 11 new goldens.
- [ ] **Before phase 4: outlines like the references' (first priority).** What matters is the outline of
      the land, as the thirteen images in `docs/references/{islands,archipelagos,continents}` show it: they
      are the result wanted for now for islands, archipelagos and continents (the user's decision,
      2026-10-28), and the outlines of the four reviewed generators are the minimum bar; what stands on the
      land comes later.
      * Done (2026-10-29): `Lands.Template`, a faithful port of Azgaar's fourteen templates (his steps and
        numbers, a jittered grid of 10 000 cells, whole-number heights), whose land share over six seeds falls
        in the spread of his own generator (measured on azgaar.github.io); on it a broad bend, fjords walked
        inland, a fractal fray by his roughness field and a sub-cell warp, the exact distance; `land_fraction`
        by quantile; masses and water bodies in the report. The presets with a template draw it by default
        (`outline = "shapes"` for the compositions). Coast dimension 1.15 to 1.47, in or above the
        references' band; hypsometric integral 0.19 to 0.39 on most. The plan's discard of grid growth
        (section 5.2) is reversed for bounded maps.
      * Done (2026-10-29): fjords follow the drainage (a priority flood from the coast, the area drained
        summed down it, the valleys of the largest basins drowned upstream with their tributaries), 10 systems
        a map round two or three fjord coasts, after the fray; the land they take is given back as a rim on
        the coast, so the shares stay; each path carved at the mean of five of its pixels (no 45 degree
        stair), the walls frayed by a sixth of the coast's noise, the valley cut into the ground along it and
        past its head.
      * Done (2026-10-29): every template map has the unbounded world's ocean floor at its own scale
        (`ocean_floor`, `ocean_compose`; `ocean = false` turns it off): stairs, drowned land, guyots, ridges,
        trenches and canyons, faded to the far floor at the map's rim. 1.5 s more a map (its drowned land is
        made at two pixels a cell); `test_compose` measures without it but in one case, about 4 min in Debug
        (timeout 300).
      * The finest scale of the coast is smoother than the references': the raster holds 4 pixels a cell
        (detail 8 gives 1.17 to 1.22 at 14 s), and below it only the warp acts.
      * Speed: about 4 s of Lua a 24 km island with its fjords and ocean floor, after the triangles, the
        drainage, the labels and the distances moved to C++ (`Raster.*`); paid again at every level change in
        the viewer. What is left in Lua is the strata's per-pixel loops, the shelf and the drowned land's
        raster.
      * taklamakan has about 0.09 less land than his (its hills hug the edges of a square map, his are 4:3).
      * The archipelago references (`8u0yqm3yabcy.png`, `images.jpg`, `images1.jpg`) are painted maps: extract
        land masks by the sea's colour and measure an archipelago band. mapgen2 and heman have not been run
        beside it (only Azgaar's previews were).
      * A count of masses above a share of the land in `terrain_metrics` (the JPEG references' speckle makes
        the raw count meaningless) and its band.
      * The unbounded world from templates is `Lands.World` (`assets/scripts/lands/infinite_world.lua`);
        `frame.world_cell` still takes only the compositions. Neither has goldens or tests yet.
      * `region_id` of a template reads a few samples below 1 inland of some coasts (`Curves.Terrace` over the
        interpolated ranks).
- [ ] **Relief on the land (after the outlines, lower priority).** Templates: the ground is his whole-number
      heights interpolated, combed into parallel streaks in the hillshade, and 4 to 9 % of it is closed
      depressions (his pits and troughs, which he turns into lakes). Compositions: the interior is a low
      plateau with a few ranges, so the continents' hypsometric integral is 0.10 against 0.15 to 0.62 on the references, which
      detail noise will not close: the presets need massifs and uplands (`relief` elements of varied
      heights, as Azgaar builds land from overlapping `Hill` steps). Ranges read drawn: a three-point spline
      nearly straight, spurs as straight capsules at even intervals; they need wander along the crest and
      spurs that descend and branch. The crest's flanks show a regular ripple in the hillshades, an artefact
      of how its pieces join, to find and remove.
- [ ] **Phase 4, detail on the shapes** (`docs/lands_plan.md`): what remains of it once the outlines are done
      (relief noise bound to the distance inland, erosion, the exact distance if erosion moves the coast,
      fetch).
- [ ] **A lake's shore cliff stands above a low plain round it** (`all_land.lua`, `lake_b`: a ring 17 m deep):
      the cliff height is not held under the land behind it, as the land presets do with `cliff_height`.
- [ ] **Debug shaders are built unoptimized** (`-g` against `-O`), so a golden sample on a discontinuity can
      differ between the two by one step; `lands_mediterranean` is sampled on a shifted grid for it.
- [ ] **Bounding circles are crude for long shapes** (capsules, bands): a scatter of channels keeps them
      further apart than their outlines need, and fewer fit.
- [ ] **Uniform and cluster in the world keep fewer than `count`**: Wei's rule in one pass drops every
      candidate outranked by a conflicting one, even when that one is dropped too.
- [ ] **A rim island's interior is a closed basin**: what the profile says, but nothing drains it yet.
- [ ] **The report samples 3 x 65 k points on the CPU** (60 to 130 ms of script time); fine for exports, it
      is paid again at every level change in the viewer.
- [ ] **Coast type has no fetch**: exposure is the normal against one wave direction.
- [ ] **No lakes or rivers as water in the Lands macros**: see "The macros do not drain yet".

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
