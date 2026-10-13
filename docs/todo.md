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
- [ ] **No lakes or rivers as water**: drowned valleys that do not reach the sea count as lagoons in the
      metrics (10 to 30 on a default island).
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
