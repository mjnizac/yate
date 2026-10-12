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

## What exists

Two executables over one engine: `terrain_export` (Headless) writes 16-bit PNG heightmaps, 16-bit RGB
vector maps and raw `f32` volumes with a metadata sidecar; `terrain_viewer` (Graphics) draws the same
graph as a GPU-displaced grid with an orbit camera, a ring of streamed tiles, hot reload and
`--screenshot`.

Thirteen ops: `Const`, `Coords`, `Noise`, `Normals`, `Arith`, `Curve`, `Blend`, `SlopeMask`, `Vector`,
`Blur`, `Gradient`, `ThermalErosion`, `HydraulicErosion`. The script API is `Noises.*`, `Terrain.*`,
`Masks.*`, `Combine.*`, `Curves.*`, `Filters.*`, `Vec` and `Erosion.*`, documented with pictures in
`docs/lua_reference.md`.

Eight sample terrains in `assets/scripts/terrains/` cover what that op set can express — alpine, mesa,
crater, island, badlands, riverlands, dunes, rolling — and `assets/scripts/gallery.lua` generates the
reference's plates.

## Verification

Both configurations build warning-free with MSVC 19.44 at `/W4`, and `ctest` passes 14 of 14 in each:
`test_allocators`, `test_mapping`, `test_errors`, `test_compiler`, `test_lua_errors`,
`test_output_files`, `test_cli`, `test_compute_roundtrip`, `test_noise`, `test_kernels`, `test_seams`,
`test_erosion`, `test_viewer`, `test_datasets`.

Three of those look at something the engine did not itself produce, which is the property the earlier
suite lacked:

* `test_output_files` checks a written PNG against values worked out by hand (`0.0` over
  `[-100000, 100000]` must be the bytes `80 00`), decoded with libspng, and pins SHA-256 to its FIPS
  vectors. It fails when the original byte-reversal is reintroduced.
* `test_cli` runs `terrain_export` and `terrain_viewer` as a user would and checks exit codes, error
  text and the files left behind, including that a different section size does not change a byte of the
  heightmap. It is where the viewer was found to exit 0 on a missing script.
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

| | |
| --- | --- |
| 16k export, erosion-heavy | 50.0 s total — GPU 2.31 s (5%), readback 3.32 s, encoder stall 43.1 s (86%) |
| Viewer | 0.6–0.7 ms a frame at 1600x900, 49 tiles; a tile costs 954 µs to stream, a full ring 47 ms |
| Peak VRAM per section | 1.8 MiB at section 512 for `basic.lua` |
| Noise far from the origin | worst derivative error 4.9% (`R2`) and 19.8% (`R3`) at 5·10⁵ m |
| Resolution independence | noise is bit-identical at 8 m and 16 m; with erosion it differs by 8.0 m on average |

The encoder figure is why the exporter evaluates one section at a time: overlapping them would hide
work already hidden behind the wait for deflate.

## Known defects

Open, with the measurements, in `docs/todo.md`:

* **Erosion iterations are per cell**, so the amount of erosion still depends on the sample spacing:
  8 m against 16 m on `basic.lua` differs by 5.45 m on average and 44.3 m at worst. The slope units are
  fixed; the propagation distance is not.
* **Hydraulic erosion leaves a one-sample-scale ripple**; a `Filters.Blur{ radius = 1 }` after it clears
  it. Adding diagonal neighbours was tried and measured worse.
* **The CI workflow has never run**, and Linux has never been built.
* **One GPU, one vendor, one OS.**

Vulkan 1.4 is the target on purpose (spec section 5), though no 1.4-only feature is used: the 1.4
feature struct is queried and enabled and none of its members is required. Lowering the minimum to 1.3
is one constant and the shader target environment, and it is a product decision rather than a defect.

## Next

No milestone is in progress. `docs/todo.md` is the work queue, and `docs/terrain_standard.md` is a
draft of where the op catalog would go next if that direction is taken.
