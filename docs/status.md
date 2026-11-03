# Status

Where the engine is right now, kept short because every task starts here. What exists and how it is
verified: `docs/architecture.md`. Why it is built that way: `docs/design.md`. Open work: `docs/todo.md`.

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

## Now

No milestone is in progress. `docs/todo.md` is the work queue; its first priority is the Lands outlines
(section 2), after the items that block trust in the suite (section 1).

Twenty ops: `Const`, `Coords`, `Noise`, `Normals`, `Arith`, `Curve`, `Blend`, `SlopeMask`, `Vector`, `Blur`,
`Gradient`, `ThermalErosion`, `HydraulicErosion`, `Transform`, `Shape`, `Gullies`, `Grid`, `Water`, `Lands`
and `Displace`; plus `Erosion.StreamPower`, a whole-grid CPU simulation.

`ctest` has 21 suites, all passing in Debug and Release on the last run: `test_allocators`, `test_mapping`,
`test_errors`, `test_compiler`, `test_lua_errors`, `test_output_files`, `test_lands`, `test_relief`,
`test_hydrology`, `test_water`, `test_compose`, `test_cli`, `test_geometry`, `test_library`,
`test_compute_roundtrip`, `test_noise`, `test_kernels`, `test_seams`, `test_erosion`, `test_viewer`,
`test_datasets`.

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

