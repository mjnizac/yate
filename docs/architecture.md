# Architecture

## Layering

```
executables        terrain_export (Headless)        terrain_viewer (Graphics)                
                             |                                |
engine public API    engine::init / shutdown, Application, Layer, Error, log macros
                             |
engine internals     terrain/  (graph, compiler, kernels, evaluator, output writer)
                     render/   (viewer rendering)
                     vulkan/   (context, device, VMA allocator, section pool, pipelines)
                     memory/   (general, arena, pool, Tracy reporting)
                     platform/ (the only OS-specific code)
```

Everything a `.cpp` exposes beyond itself lives in a header under `include/engine/`. Internal
declarations sit inside `#ifdef IS_ENGINE`, and `IS_ENGINE` is a PRIVATE compile definition of the
engine target, so executables never see them. `src/` holds only `.cpp` files.

## Shapes

A shape is a function of a *position field*, an `R2 -> R2` value in metres, and its output is a signed
distance. Nothing about a shape reads the world: `Terrain.Coords` is just the default position. That is
what makes the shape ops pointwise, with a CPU reference, a halo of zero and bit-exact tiling for
free, and it is why a warp is `Coords + noise` and needs no op of its own.

Polygons are a chain of `Shape` nodes because a kernel has ten parameter words and no buffers. Each
node folds two segments into an accumulator (distance so far in `x`, crossing parity in `y`) and a
resolve node turns it into a signed distance. The count of segments in a node is a variant field, not a
parameter word: the compiler writes words 8 and 9 of every node with the iteration count and index.

The higher level, `Shapes.*` and `Landforms.*`, is Lua loaded before the script (`assets/lua/prelude/`,
step 8 of the startup order). It composes the ops and adds none.

## Viewer water

Water is a property of the viewer, not of the graph, and it is not a surface drawn over the terrain: the
terrain fragment shader works out the water in front of itself from the eye position, its own position
and the level, which pass in the terrain push block. A second plane at nearly the same depth would
z-fight along every shoreline. A single quad at the cleared depth covers only the pixels no terrain
reached. `docs/spec.md` section 12 has the contract.

## Run modes

The mode is chosen once, in `AppInfo::mode`:

- **Graphics** (viewer): the GPU is selected for rendering and presentation. The main window and
  its surface exist before device selection, so presentation support can be verified.
- **Headless** (export): the GPU is selected for compute. No window system is touched.

Every system except windowing, swapchain, render passes and presentation behaves identically in
both modes.

## Startup order

`engine::init` runs these steps, logging the start and end of each one. `engine::shutdown` runs the
exact reverse.

1. Platform (`platform::Init`): host description, executable directory, main thread name.
2. Tracy: the session is named with `TracyAppInfo`.
3. Logging (`log::Init`): two asynchronous loggers, `engine` and `app`, on one worker thread.
4. Memory system (`memory::Init`): `CPU/General`, `CPU/Lua`, `CPU/Frame arena`.
5. *Graphics only:* GLFW, main window, surface. **Milestone 9.**
6. Vulkan context: instance, debug messenger, device, queues, VMA allocator, section pool,
   staging and readback rings, pipeline cache.
7. *Graphics only:* swapchain, render pass, depth buffer, framebuffers. **Milestone 9.**
8. Lua runtime. **Milestone 6.**
9. Application and its layer stack.

The host banner is logged by `engine::init`, not by `platform::Init`, because logging is not up yet
at step 1.

## Application and layers

`Application` owns an ordered stack of `Layer`s and calls `OnUpdate()` on each one per frame or
tick. `PushLayer<T>(args...)` constructs the layer inside engine-owned storage and returns `T&`.
Layers are non-copyable and non-movable.

At the end of every tick the loop resets the frame arena, refreshes the CPU memory plots, queries
the VRAM budgets and collects the Tracy GPU zones. In Graphics mode it also emits `FrameMark`.

## Where the data lives

- **CPU:** every allocation goes through the memory system. `operator new` is replaced per module
  and routed to `CPU/Untracked new`, so nothing is invisible to Tracy.
- **VRAM:** VMA is the only path to `vkAllocateMemory`. Buffers carry a category, which is also
  their Tracy pool name. Section values are sub-allocated from large per-domain blocks with VMA
  virtual blocks, in power-of-two size classes derived from the mapping, section size and halo.
  `R2` and `R3` values never share a block.

## Kernel interface

Each graph node maps to one precompiled compute kernel. Kernels take no descriptor sets: inputs
and outputs arrive as buffer device addresses inside a 128-byte push-constant block
(`vulkan::KernelPushConstants`), together with the integer section origin, the section extent, the
halo, the domain, a channel mask and a 10-word parameter block.

Workgroup sizes are fixed project-wide and come from specialization constants 0..2, so one SPIR-V
module serves both domains: 8x8x1 for `R2`, 4x4x4 for `R3`. Op variants use specialization ids 3
and up. `assets/shaders/lib/kernel.lib.glsl` implements the GLSL side and keeps the buffer load/store
boilerplate separate from the math, so kernel fusion stays possible later.

World positions are always derived from the integer section origin plus a local offset, which is
what makes neighbouring sections agree bit-exactly at their borders.

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

Both configurations build warning-free with MSVC 19.44 at `/W4`; the suites are listed in `docs/status.md`.

Four suites look at something the engine did not itself produce, which is the property the earlier
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

