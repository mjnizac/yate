# Status

Where the engine is right now. The reasoning behind every decision, and every defect found along the
way, is in `docs/decisions.md`; open work is in `docs/todo.md`.

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

Two executables over one engine: `terrain_export` (Headless) writes 16-bit PNG heightmaps, 16-bit RGB
vector maps and raw `f32` volumes with a metadata sidecar; `terrain_viewer` (Graphics) draws the same
graph as a GPU-displaced grid with an orbit camera (`--view`, `--target`), a ring of streamed tiles,
hot reload, `--screenshot`, and the analytic sea level (`--water`, off by default).

Seventeen ops: `Const`, `Coords`, `Noise` (simplex, ridged, billow, cellular, with an optional
`position`), `Normals`, `Arith`, `Curve`, `Blend`, `SlopeMask`, `Vector`, `Blur`, `Gradient`,
`ThermalErosion`, `HydraulicErosion`, `Transform`, `Shape`, `Gullies` and `Grid`, plus
`Erosion.StreamPower`, a whole-grid simulation on the CPU. The script API is
`Noises.*`, `Terrain.*`, `Masks.*`, `Combine.*`, `Curves.*`, `Filters.*`, `Vec`, `Erosion.*`,
`Geometry.*`, and the Lua prelude's `Shapes.*`, `Landforms.*`, `Ranges.*` (the alpine macro) and `Dunes.*`
(the dune macro), documented with pictures in
`docs/lua_reference.md`.

Five reference terrains built from shapes in `assets/scripts/shapes/` (volcanic island, mountain range,
canyon, mesas, coast), with previews in `docs/images/shapes/`. The shape layer is `Geometry.*` (C++ ops), plus
`Shapes.*` and `Landforms.*`, which are Lua in `assets/lua/prelude/` that the engine loads before every
script.

Dune fields from `Dunes.Field`, the dune macro (`terrains/dunes.lua`, `terrains/star_dunes.lua`), in
three styles: `"procedural"` (default, analytic, no repetition, about 0.6 s), `"simulated"`
(`Erosion.Wind`, periodic) and `"star"`; previews in `docs/images/dunes/`.

Islands, archipelagos and continents from `Lands.Island`, `Lands.Archipelago` and `Lands.Continent`, the
lands macro (`assets/scripts/lands/`): one signed coast distance, a margin with a shelf and a slope, a
coast type, four archipelago layouts and a continent with ranges; accepted against `terrain_metrics`
numbers and four golden datasets (`lands_*`); previews in `docs/images/lands/`.

Mountain ranges from `Ranges.Carve` in `assets/scripts/ranges/` (sierra, arc, parallel, knot) and
`terrains/alpine.lua`, with previews in `docs/images/ranges/`.

Eight sample terrains in `assets/scripts/terrains/` cover what that op set can express — alpine, mesa,
crater, island, badlands, riverlands, dunes, rolling — and `assets/scripts/gallery.lua` generates the
reference's plates.

## Verification

Both configurations build warning-free with MSVC 19.44 at `/W4`, and `ctest` passes 15 of 15 in each:
`test_allocators`, `test_mapping`, `test_errors`, `test_compiler`, `test_lua_errors`,
`test_output_files`, `test_cli`, `test_geometry`, `test_compute_roundtrip`, `test_noise`,
`test_kernels`, `test_seams`, `test_erosion`, `test_viewer`, `test_datasets`.

Three of those look at something the engine did not itself produce, which is the property the earlier
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

* **Hydraulic erosion is only approximately resolution independent.** Iterations and rates are scaled to
  the spacing (reference 2 m) and the amount of erosion agrees within a few metres, but the channels it
  carves are at the scale of the cell and differ. Finer than 2 m the count is capped at 96.
* **Hydraulic erosion leaves a one-sample-scale ripple**; a `Filters.Blur{ radius = 1 }` after it clears
  it. Adding diagonal neighbours was tried and measured worse.
* **CI is red.** Windows has failed in "Tests that need no device" since milestone 11 and does not
  reproduce locally; Linux has never built. Both need the log of the failing step.
* **Water at the ring's edge** shows a plate where the surface stands above a lower floor (todo).
* **One GPU, one vendor, one OS.**

Vulkan 1.4 is the target on purpose (spec section 5), though no 1.4-only feature is used: the 1.4
feature struct is queried and enabled and none of its members is required. Lowering the minimum to 1.3
is one constant and the shader target environment, and it is a product decision rather than a defect.

## Next

No milestone is in progress. Milestones 1 to 11 are done. `docs/todo.md` is the work queue. The op
catalogue draft (`terrain_engine_spec_terrain_standard.md`) lives on the Desktop, outside the repository,
and is not a repo document: nothing in the code depends on it.
