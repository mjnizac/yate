# Open tasks

Only what is open, in the order to do it. Finished work lives in `docs/design.md` (standing decisions),
`docs/archive/decisions.md` (history, including the closed items this file used to carry) and the git log.
Directions that are not tasks yet are in `docs/roadmap.md`.

## 1. Blocks trust in the suite

- [ ] **CI is red on Windows since milestone 11, and Linux has never built.** `ci.yml` has run on GitHub
      (read from the public API; the logs need admin rights). Run `9dba657` passed on Windows. Runs `39092a3`
      and `ccd62ee` fail in "Tests that need no device" on both Windows configurations, and every Linux job
      fails in "Build". The Windows failure does not reproduce here (a clean clone built as the workflow does
      passes all six suites). The failing step's log names the suite; the same goes for Linux.
- [ ] **`test_viewer` "distance picks the level"** waits up to 10 rounds of 60 frames for
      `MissingTiles() == 0`; passes in Release, Debug not rechecked.
- [ ] **One GPU, one vendor, one OS.** Everything is validated on a GTX 1070 on Windows. Queue family
      selection, memory types, readback and the swapchain are where AMD and Intel differ; it needs hardware.

## 2. Lands: outlines like the references (first priority)

The 13 images in `docs/references/{islands,archipelagos,continents}` are the result wanted for islands,
archipelagos and continents (decision of 2026-10-28); the outlines of the four reviewed generators are the
minimum bar, and what stands on the land comes later. What was done (`Lands.Template`, fjords by drainage,
the ocean floor per map) is in `docs/archive/decisions.md`, "Closed work moved out of todo.md".

- [ ] The finest scale of the coast is smoother than the references': the raster holds 4 pixels a cell
      (detail 8 gives 1.17 to 1.22 at 14 s), and below it only the warp acts.
- [ ] Speed: about 4 s of Lua a 24 km island with fjords and ocean floor, paid again at every level change in
      the viewer. Left in Lua: the strata's per-pixel loops, the shelf and the drowned land's raster.
- [ ] `taklamakan` has about 0.09 less land than Azgaar's (its hills hug the edges of a square map).
- [ ] The archipelago references (`8u0yqm3yabcy.png`, `images.jpg`, `images1.jpg`) are painted maps: extract
      land masks by the sea's colour and measure an archipelago band. mapgen2 and heman were not run beside it.
- [ ] A count of masses above a share of the land in `terrain_metrics` (the JPEG speckle makes the raw
      count meaningless), and its band.
- [ ] `Lands.World` (`assets/scripts/lands/infinite_world.lua`) has goldens for the compositions only;
      `frame.world_cell` takes only compositions.
- [ ] `region_id` of a template reads a few samples below 1 inland of some coasts (`Curves.Terrace` over the
      interpolated ranks).
- [ ] **Relief on the land (after the outlines).** Templates: the ground is interpolated whole-number heights,
      combed into streaks in the hillshade, and 4 to 9 % of it is closed depressions. Compositions: the interior
      is a low plateau, so the continents' hypsometric integral is 0.10 against 0.15 to 0.62 on the references;
      the presets need massifs and uplands. Ranges read drawn (a nearly straight three-point spline, straight
      capsule spurs at even intervals) and show a regular ripple on the crest's flanks.
- [ ] **Phase 4, detail on the shapes** (`docs/roadmap.md`): relief noise bound to the distance inland, erosion,
      exact distance if erosion moves the coast, fetch.

## 3. Correctness

- [ ] **Coarse levels are spiky.** A 16 m preview runs one erosion iteration and shows the noise's finest
      octaves (about two samples wide) as spikes (99th percentile slope 1.6 against 1.2 on the volcano). The fix,
      limiting a noise's octaves to what the spacing resolves, would make noise values differ between spacings.
- [ ] **Hydraulic erosion leaves a one-sample ripple** (`Filters.Blur{ radius = 1 }` clears it). Diagonal
      neighbours were tried and rejected on measurement (`docs/design.md`); the cause is in the per-cell
      erosion and deposition step (`taken`/`dropped` in `hydraulic_erosion.comp.glsl`).
- [ ] **The open sea ends at the ring.** A surface at 0 m above a floor at -50 m shows a plate whose far edge
      floats above the terrain's; extending it to the far plane would paint sea over unevaluated world.
- [ ] **The macros do not drain**: `Lands`, `Ranges` and the cliffs leave their pits (a rim island's interior
      is a closed basin); give them `Water.Drain` and their own `lakes` and `rivers`.
- [ ] A lake's shore cliff stands above a low plain round it (`all_land.lua`, `lake_b`: a ring 17 m deep).
- [ ] Debug shaders are built unoptimized, so a golden sample on a discontinuity can differ between builds by
      one step; `lands_mediterranean` is sampled on a shifted grid for it.
- [ ] Bounding circles are crude for long shapes (capsules, bands): scatters keep them further apart than needed.
- [ ] Uniform and cluster in the world keep fewer than `count` (Wei's rule in one pass drops every candidate
      outranked by one that is itself dropped).
- [ ] Coast type has no fetch: exposure is the normal against one wave direction.

## 4. Terrain quality (in this order; done when it is in `ctest`)

The goal is that a terrain is composed from validated examples and judged by numbers, not only by a screenshot.

- [ ] **The alpine family rebuilt in stages**, as the cliffs were (`terrains/cliffs.lua`, cases
      `tests/library/cliffs*.json`). `tests/library/alpine.json` shows the gap: spectral slope 5.3 against 3.9
      on the four reference heightmaps, so it lacks fine relief.
- [ ] **Hypotheses from World Machine and Gaea practice**, to test on the alpine case, each a prelude function
      or script change only if it measures better: irregular bed thickness by terracing a monotone remap of
      height; a convexity mask (`h - Blur(h)`) to confine detail and erosion to walls and ridges; detail octaves
      above the stream-power grid scale with an amplitude ratio from the target spectral slope; erosion passes
      ordered coarse to fine; the difference between before and after an erosion recombined as debris and
      broken terraces; terraces cracked by cells before erosion; F3 - F1 cells for cliff edges.
      The World Machine devices these lean on now exist (`Noises.Cells` with every F-style and metric,
      cone/round `Filters.Expand`, thermal `mass_balance` and `two_phase`, `Strata.Apply` with spatial
      bedding, `Curves.Ramp`), recreated against the forum threads they came from; none is in the alpine
      script yet. Since then: `multiscale` noise (the likeliest fix for the missing fine relief), hydraulic
      `hardness`, `Erosion.Coastal`, `Weathering`, `Snow`, `Masks.Curvature` and `Wetness`.
- [ ] **World Machine's multiscale erosion** (Erosion's Multiscale Synthesis and Scale Independence) needs a
      coarse-to-fine pass the sectioned graph cannot run: the erosion at a coarser grid than the job's. A
      design decision (a coarse GPU grid op, or `Terrain.Bake` in section 5), not a parameter.
- [ ] Not taken from World Machine: vector displacement (VDM, build 4059: overhangs and caves), Flow
      Restructure's drainage-mask outlets, the River device's Shields depth and its GCS library.
- [ ] **Cliffs, what is open:** no heightmap reference with a known scale exists, so its case holds guards, not
      ranges; the chutes' heads are rounded capsules; the sea cliffs are low against the escarpment; 0.3 % of the
      land is closed depressions; `strata` and `debris` outputs are not used by the viewer's palette; the
      stream-power grid covers +-6.4 km.
- [ ] **References with a known scale.** The usable alpine references are 8-bit heightmaps of unknown size and
      height, so only scale-free numbers (spectral slope, hypsometric integral) can be targets;
      `heightmap1.jpg` and `heightmap2.jpg` are shaded relief, not heights.
- [ ] **Water plan, phase 5**: texture masks, water colour by body, foam.
- [ ] `Erosion.StreamPower` only fills inside and cuts straight D8 spokes across what it fills: breach the grid
      with `Hydrology.Drain` before it (`assets/scripts/terrains/lakes_rivers.lua`).
- [ ] **Dunes:** per-direction slope histograms (the back 5 to 15 degrees, the slip face up to `repose`) are
      checked by eye; `"simulated"` repeats every `cells * cell` metres (5.8 km) and costs a few seconds;
      slopes over 40 degrees (0.03 %) are 2-sample edges of small dunes; `"star"` has not been rebuilt on the
      per-dune profile of `"procedural"`.
- [ ] **Shadows**: the strata cliffs and gullies cast none (shadows come from the ~100 m simulation grid); a
      sun-direction heightmap pass in the viewer would cover any script and the dunes' low contrast.
- [ ] Strata steps are regular close up (one bed thickness and one dip per terrain); `--detail 1` lost a piece
      of foreground once, not investigated.

## 5. Speed and tooling

- [ ] **Tiles are evaluated one submit and wait at a time** (`EvaluateTile`): batch several per submission.
- [ ] **Lua work is repeated on every level change**: the macros rebuild resolution-independent CPU grids each
      time. `engine.memo(key, fn)` cleared on hot reload, or a `Terrain.Bake` op on a coarse grid; the report
      also samples 3 x 65 k points on the CPU (60 to 130 ms) at each change.
- [ ] **One global level limits the view distance**: CDLOD-style per-tile levels with geomorphing (a design,
      after the two items above). Normals take three floats per sample in the viewer; octahedral 2 x 16-bit is a
      third of it.
- [ ] `StreamPower` costs ~145 ns per cell and step on one thread (profile first); `Erosion.Wind` has no cache.
- [ ] Export is bound by PNG encoding at 16k; measure the zlib level on 16-bit heights. `Graph::kMaxOutputs` is
      8; 16 would cost readback space only when used.
- [ ] **No introspection of the API**: an agent learns a call's keys from the docs or the error. A generated
      catalogue needs the bindings to declare parameters in data; start with a data-driven `check_args` for the
      macros, now hand-written per macro.
- [ ] User macros cannot be shared between scripts (`require`, `dofile`, `load` are removed on purpose).
- [ ] The sidecar has no slope percentiles (`test_geometry` computes them from the raw export).
- [ ] A golden failure reports a count, not where: print the bounding box and the largest difference.
- [ ] The suite takes three to six minutes, mostly macro cases paying their own simulation; a `cells` key per
      case would cut it. `docs/architecture.md` does not describe the CPU ops (`StreamPower`, `Wind`, `Grid`).
