# Status

Where the engine is right now. The reasoning behind every decision, and every defect found along the
way is in `docs/design.md` (standing decisions) and `docs/archive/decisions.md` (the history); open work is in `docs/todo.md`.

| # | Milestone | State |
| --- | --- | --- |
| 1 | Skeleton | **done**, accepted |
| 2 | Memory system | **done**, accepted |
| 3 | Vulkan context in Headless mode | **done**, accepted |
| 4 | First export | **done**, accepted |
| 5 | Graph and compiler, without Lua | **done**, accepted |
| 6 | Lua bindings | **done**, accepted |
| 7 | Sections and streaming | **done**, accepted |
| 8 | Iterative kernels | **done**, accepted |
| 9 | Viewer (Graphics mode) | **done**, visually reviewed |
| 10 | Viewer water | **done**, visually reviewed |
| 11 | Shapes, profiles and landforms | **done**, visually reviewed |

## What exists

Detail of each call is in `docs/lua_reference.md`; why each piece is the way it is, in `docs/design.md`.

* **Executables.** `terrain_export` (Headless) writes 16-bit PNG heightmaps, 16-bit RGB vector maps and raw
  `f32` volumes with a metadata sidecar. `terrain_viewer` (Graphics) draws the same graph as a GPU-displaced
  grid: orbit camera (`--view`, `--target`), a ring of streamed tiles with rings of detail beyond it
  (`--max-level`), `--follow`, `--map-scale`, 4x MSAA, vsync, hot reload, `--screenshot`, analytic sea level
  (`--water`, off by default).
* **Ops.** Nineteen ops: `Const`, `Coords`, `Noise` (simplex, ridged, billow, cellular), `Normals`, `Arith`,
  `Curve`, `Blend`, `SlopeMask`, `Vector`, `Blur`, `Gradient`, `ThermalErosion`, `HydraulicErosion`,
  `Transform`, `Shape`, `Gullies`, `Grid`, `Water` and `Lands`; plus `Erosion.StreamPower`, a whole-grid CPU
  simulation.
* **Script API.** `Noises.*`, `Terrain.*`, `Masks.*`, `Combine.*`, `Curves.*`, `Filters.*`, `Vec`,
  `Erosion.*`, `Geometry.*`, and the Lua prelude (`assets/lua/prelude/`, loaded before every script):
  `Shapes.*`, `Landforms.*`, `Ranges.*`, `Dunes.*`, `Water.*`, `Lands.*`.
* **Shapes.** Five reference terrains in `assets/scripts/shapes/` (volcanic island, mountain range, canyon,
  mesas, coast), previews in `docs/images/shapes/`.
* **Lands** (`Lands.Compose`, `assets/scripts/lands/`): elements with ids (land, water, sea-floor forms)
  combined in one `Lands` op, each land raised by its own profile, coast, shore and margin from one signed
  distance, edits by id, the resolved elements reported to the sidecar (`tools/dev.sh lands`, `lands-check`).
  Bounded maps or the unbounded world (`frame.world_cell`, `Lands.World`). Presets: `Lands.Island`,
  `Lands.Archipelago`, `Lands.Continent` (land presets as compositions). Outlines come from `Lands.Template`,
  a port of Azgaar's heightmap templates, with drainage fjords, beaches and an ocean floor; the presets with
  a template draw it by default, `outline = "shapes"` builds the compositions. The raster passes are C++
  (`Raster.*`). 33 golden datasets `lands_*`, previews in `docs/images/lands/`. Phases 1 to 3 are done;
  the outlines (`docs/todo.md`) come before phase 4 (`docs/roadmap.md`).
* **Water.** `Water.Drain` (breach-and-fill, lakes, rivers) and `Water.Network` (rivers, lakes and the sea as
  analytic elements, each kind shaping its valley or basin; `water_showcase.lua`); the viewer draws the
  `lakes` and `rivers` outputs as water.
* **Ranges and dunes.** `Ranges.Carve` (`assets/scripts/ranges/`: sierra, arc, parallel, knot) and
  `terrains/alpine.lua`; `Dunes.Field` in three styles, `"procedural"` (default, about 0.6 s),
  `"simulated"` (`Erosion.Wind`, periodic) and `"star"`; previews in `docs/images/ranges/` and
  `docs/images/dunes/`.
* **Sample terrains.** Nine in `assets/scripts/terrains/` (alpine, mesa, cliffs, crater, island, badlands,
  riverlands, dunes, rolling); `assets/scripts/gallery.lua` generates the reference's plates.

## Verification

Both configurations build warning-free with MSVC 19.44 at `/W4`, and `ctest` has 21 suites:
`test_allocators`, `test_mapping`, `test_errors`, `test_compiler`, `test_lua_errors`,
`test_output_files`, `test_lands`, `test_relief`, `test_hydrology`, `test_water`, `test_compose`,
`test_cli`, `test_geometry`, `test_library`, `test_compute_roundtrip`, `test_noise`, `test_kernels`,
`test_seams`, `test_erosion`, `test_viewer`, `test_datasets`. On the last run Release passed all of them. Run one
suite at a time: two runs share `out/` and fail each other.

Four of those look at something the engine did not itself produce, which is the property the earlier
suite lacked:

* `test_output_files` checks a written PNG against values worked out by hand (`0.0` over
  `[-100000, 100000]` must be the bytes `80 00`), decoded with libspng, and pins SHA-256 to its FIPS
  vectors. It fails when the original byte-reversal is reintroduced.
* `test_cli` runs `terrain_export` and `terrain_viewer` as a user would and checks exit codes, error
  text and the files left behind, including that a different section size does not change a byte of the
  heightmap. It is where the viewer was found to exit 0 on a missing script.
* `test_geometry` exports scripts built from the public API with `--raw` and compares the floats with
  closed-form values: circle, box, rotation about a pivot, a polygon whose vertices sit on samples, a
  concave polygon, profiles, every landform, and that a prelude error names the script's line.
* `test_viewer` captures a rendered frame and checks it holds terrain and changes when the camera moves.
  It fails when the depth buffer is cleared against the reversed-Z convention.

Every GPU test also fails if the Vulkan debug messenger reported anything at error severity, so the
suite doubles as a validation run. Everything GPU-side ran on a GeForce GTX 1070 reporting Vulkan
1.4.312, with a dedicated compute queue family and calibrated timestamps.

**The 16k acceptance run** is the one check the suite does not perform, because it takes two and a half
minutes and writes 1.1 GB. Three exports of `basic.lua` at 16384², twice at section 512 and once at
1024, produce byte-identical `height.png` and `normals.png`; only `metadata.json` differs, in its
timings, paths and `section_size`. The command is in `docs/commands.md`, and the self-hosted GPU job in
`.github/workflows/ci.yml` runs it.

## What the numbers say

Measured in the session that closed milestone 11, on the GTX 1070, unless the row says otherwise.

| | |
| --- | --- |
| 4096 square, `basic.lua`, section 512 | 0.94 to 0.96 s with the parallel deflate, 2.91 to 2.97 s with the single-thread one; same 109.2 MB; 3.1x |
| 8192 square, same | 5.4 to 14 s now, 19 to 23 s before; the spread is the HDD taking writes, not the encoder (deflate is 3.4 s of wall time at 4 threads) |
| 16384 square, `basic.lua` | outputs byte-identical across section sizes 512 and 1024; 70 s at 512, 123 s at 1024 on a drive already busy |
| Viewer | 0.7 to 0.9 ms a frame at 1280x720, 37 to 49 tiles on screen |
| Resolution independence, thermal erosion | 4, 8 and 16 m differ by 0.06 to 0.24 m on average (0.8 to 2.2 m before iterations were scaled) |
| Resolution independence, hydraulic erosion | 3.4 to 6.4 m on average (6.7 to 16.0 m before); channels at the scale of the cell cannot match across spacings |
| Peak VRAM per section | 1.8 MiB at section 512 for `basic.lua` (earlier measurement) |
| Noise far from the origin | worst derivative error 4.9% (`R2`) and 19.8% (`R3`) at 5·10⁵ m (earlier measurement) |

The encoder is why the exporter evaluates one section at a time: overlapping them would hide work already
hidden behind the wait for deflate.

## Known defects

Open, with the measurements, in `docs/todo.md`:

* **Hydraulic erosion is only approximately resolution independent** (reference 2 m; channels at the scale
  of the cell differ; finer than 2 m the count is capped at 96) and leaves a one-sample ripple that
  `Filters.Blur{ radius = 1 }` clears.
* **CI is red** on Windows ("Tests that need no device", not reproducible locally) and Linux has never built.
* **`test_viewer` "distance picks the level"** passes in Release since it waits for the tiles; Debug not rechecked.
* **Water at the ring's edge** shows a plate where the surface stands above a lower floor.
* **One GPU, one vendor, one OS.**

Vulkan 1.4 is the target on purpose (spec section 5), though no 1.4-only feature is used. Lowering the minimum
to 1.3 is one constant and the shader target environment: a product decision rather than a defect.

## Next

No milestone is in progress. Milestones 1 to 11 are done. `docs/todo.md` is the work queue. The op catalogue
draft (`terrain_engine_spec_terrain_standard.md`) lives on the Desktop, outside the repository; nothing in the
code depends on it.
