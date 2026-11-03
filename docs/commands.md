# Commands

## Prerequisites

| What | Why | Where it is expected |
| --- | --- | --- |
| MSVC, Clang or GCC with C++23 | compiler | Visual Studio Build Tools 2022 (17.14+) is enough on Windows |
| CMake 3.25+ and Ninja | build | bundled with Build Tools under `Common7\IDE\CommonExtensions\Microsoft\CMake` |
| Windows SDK 10.0.26100 | UCRT headers and libs | `C:\Program Files (x86)\Windows Kits\10` |
| Vulkan SDK 1.4.x | headers, `vulkan-1.lib`, `glslc`, validation layers | `C:\VulkanSDK\<version>` |
| A GPU exposing Vulkan 1.4 | compute | driver must report api 1.4 |

Everything else (spdlog, Tracy, mimalloc, VMA) is fetched by CMake, pinned to a release tag.

### Installing the Vulkan SDK on Windows

The LunarG installer needs interactive elevation, so it cannot be fully automated. Run this from a
terminal and accept the UAC prompt:

```bash
"C:/Users/$USERNAME/AppData/Local/Temp/WinGet/KhronosGroup.VulkanSDK.1.4.363.0/vulkansdk-windows-X64-1.4.363.0.exe" --root C:/VulkanSDK/1.4.363.0 --accept-licenses --default-answer --confirm-command install
```

If the installer is not cached, `winget download KhronosGroup.VulkanSDK` fetches it first.

## Configure and build

The wrapper sets up `vcvars64`, puts the bundled CMake and Ninja on `PATH`, configures and builds:

```bash
./build.cmd Debug
```

```bash
./build.cmd Release
```

Extra arguments after the build type are forwarded to CMake:

```bash
./build.cmd Release -DENGINE_TRACY=OFF
```

Doing it by hand, from a developer prompt:

```bash
cmake -S . -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build/Debug
```

### CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `ENGINE_BUILD_SHARED` | ON | link the engine objects as a DLL instead of a static library |
| `ENGINE_VULKAN_VALIDATION` | ON in Debug | validation layers and the debug messenger |
| `ENGINE_VULKAN_VALIDATION_VERBOSE` | OFF | also include info and verbose messages |
| `ENGINE_TRACY` | ON | Tracy client, built shared so the DLL and the executables share a session |
| `ENGINE_TRACY_CALLSTACKS` | OFF | capture callstacks on allocations; expensive, Debug only |
| `ENGINE_BUILD_TESTS` | ON | build the ctest suite |

## Run

```bash
build/Debug/bin/terrain_export --script assets/scripts/basic.lua --min 0,0 --max 4096,4096 --resolution 1.0 --section 512 --out out/
```

`--log-format json` emits one JSON object per line, which is what tooling should parse.
`--device <32 hex chars>` forces a physical device; the log lists every candidate with its UUID.
`--raw` writes raw little-endian `f32` instead of a PNG: the evaluated values, unnormalised and
uncompressed, in row-major order with the dimensions in the sidecar. On an 8192 square `basic.lua` it is
13.0 s against 23.3 s for the PNG and about three times the bytes. `--max-samples <n>` refuses a job over
`n` samples before anything is allocated (default 2^40), so a mistyped bound is an error with the number
it computed instead of an export of billions of sections. Every numeric option is parsed strictly: a
trailing suffix, `nan`, `inf` and an overflow are errors that name the option and the text.
`--png-level` and `--png-filter` trade encoding time against output size; the defaults are level 2 and
the `up` filter, and `--png-level 1` is the setting for a job that wants throughput over size. With
`none` or `up` the deflate stream is built in parallel (see `docs/archive/decisions.md`); `all` uses one thread.
Run `--help` for the rest.

The sidecar's `outputs[].statistics` holds the min, max and mean of each output over its components and
the number of samples that were NaN or infinite, which a 16-bit PNG would store as terrain. A non-zero
count is also a warning in the log. `timings_ms` splits the time into script, compilation, pipeline
creation, GPU, readback, encode and write, and the time the main loop spent waiting on an encoder.

The seam and determinism acceptance check, which has to be run by hand because it takes about two and a
half minutes and 300 MB:

```bash
for n in a b; do build/Release/bin/terrain_export --script assets/scripts/basic.lua --min 0,0 --max 16384,16384 --resolution 1.0 --section 512 --out out/acc512$n; done
```

```bash
build/Release/bin/terrain_export --script assets/scripts/basic.lua --min 0,0 --max 16384,16384 --resolution 1.0 --section 1024 --out out/acc1024
```

The three `height.png` and the three `normals.png` must have identical SHA-256. `metadata.json` will not:
it records the timings, the output paths and the section size.

## Test

```bash
cd build/Debug && ctest --output-on-failure
```

One test at a time, with its own output:

```bash
build/Debug/bin/test_compute_roundtrip
```

Unit tests link the engine objects directly and define `IS_ENGINE`, so they can reach internal
allocators, the Vulkan context and the section pool. `test_compute_roundtrip` needs a Vulkan 1.4
device and the compiled shaders.

`tools/dev.sh lint` checks the memory, `ENGINE_ASSERT`, `new_delete.cpp` and push-constant rules in seconds; `tools/dev.sh headings <doc>` maps a long doc by line.

`ctest -L fast` runs the suites that take seconds (about 9 s in Release); `-L compose` is `test_compose` (about 50 s); `-L slow` the ones that export
whole terrains or open the viewer. `test_datasets --record-history` appends its timings to
`tests/history/`; a plain run leaves the tree clean.

### From bash: `tools/dev.sh`

```bash
tools/dev.sh build Release
tools/dev.sh test Release -L fast
tools/dev.sh review assets/scripts/terrains/alpine.lua --bounds -8192,-8192,8192,8192 --res 8 --level 2 --sun 180,35 --shot 20,-30,9000 --shot 0,-89,14000
```

`build` calls `build.cmd` with the Windows path and keeps only errors and warnings. `test` finds ctest
and refuses to start while another run holds `out/.ctest.lock`, since two suites share `out/`. Within one
run the suites go 4 at a time (`CTEST_PARALLEL_LEVEL`, or `-j N`; `test_datasets` stays serial for its
timings): the full suite took 350 s serial and 231 s at `-j4` in Release, about 800 s and 430 s in Debug. Both exit with the build's or ctest's status.
`review` is one terrain iteration: a raw export with warnings only, `metrics.json`, a hillshade, the
viewer shots in one process, and a four-line relief summary, all under `out/review/<script>/`.
Stream-power results are cached on disk (`<bin>/cache/`) until the engine is rebuilt, so repeating a
review within one build skips the simulation.

## Profile

Tracy does not support cross-version connections, so the profiler and the client must match.
`cmake/engine_dependencies.cmake` pins the client; check it against your tools:

```bash
tracy-capture --version
```

Start the server **before** the process you want to capture. The build does not use
`TRACY_ON_DEMAND`, so events are produced from process start and anything emitted before the
connection would be lost, which would make the memory graphs wrong:

```bash
tracy-capture -o traces/export.tracy -f
```

```bash
build/Release/bin/terrain_export --script assets/scripts/basic.lua --min 0,0 --max 2048,2048 --resolution 1.0 --section 512 --out out/ --param normals=1
```

`tracy-profiler` instead of `tracy-capture` shows it live. `tracy-csvexport <trace>` dumps the zone
statistics as text, which is the quickest way to check a trace without opening the UI.

## The viewer

```bash
build/Release/bin/terrain_viewer --script assets/scripts/basic.lua --resolution 2 --section 128 --ring 3
```

Taking a picture of a frame, which is how the renderer gets looked at without a human at the keyboard:

```bash
build/Release/bin/terrain_viewer --script assets/scripts/basic.lua --screenshot out/frame.png
```

It writes the last frame before the viewer stops, so it implies `--frames 90` unless a count is given:
the first frames show a half-streamed ring. The copy comes out of the presented swapchain image, so the
file is what the window showed.

Left-drag turns, middle-drag pans, the wheel zooms. `F` switches between orbit and fly; in fly mode
`WASD` moves, `Q` and `E` go down and up, and shift goes faster. `R` reloads the script, which also
happens on its own about a quarter of a second after the file changes. `T` toggles wireframe, which is
the only way to see which grid density and which level the distance rules actually chose.

`--view yaw,pitch,distance` starts the orbit camera there (degrees, degrees, metres; yaw 0 looks along
+z and 90 along +x, a negative pitch looks down) and `--target x,y,z` is the point it orbits. Without
them the ring is framed as before. They exist so a reference image can be regenerated from a command:

```bash
build/Release/bin/terrain_viewer --script assets/scripts/shapes/canyon.lua --view 25,-32,4200 --screenshot out/canyon.png
```

`--water` draws water at the water level, which is off by default and costs no evaluation to change.
`--water-level <m>` (default 0), `--water-color r,g,b` (each 0 to 1) and `--water-opacity <0-1>` set the
sea; the panel has the same controls and a reload keeps them. The panel also toggles the sea, the lakes
and the rivers apart: a script's outputs named `lakes` and `rivers` are water surfaces of their own (see
`Water.Apply`), drawn with the same colour. An output named `sea` is the sea's surface (see
`Water.Network`): with it the sea is drawn only where the script put it, and the panel's level is not used. Terrain below the level takes the
water colour in proportion to the water in front of it:

```bash
build/Release/bin/terrain_viewer --script assets/scripts/terrains/island.lua --water --screenshot out/island.png
```

Drawn shapes go in as GeoJSON with `--shapes`, in both executables; the viewer reloads when the file
changes (see `docs/lua_reference.md`, "Drawn shapes"):

```bash
build/Release/bin/terrain_viewer --script assets/scripts/shapes/drawn.lua --shapes assets/shapes/valley.geojson --water
```

`--resolution` is metres per sample at the finest level and `--section` is samples per tile, so the two
decide how much world one tile covers. `--ring` is how many tiles are kept on each side of the camera, so
3 is a 7x7 ring of 49 tiles: at 128 samples and 2 m that is about 1.8 km across and takes roughly 45 ms
to evaluate for the sample script.

The ring follows the camera and switches to a coarser sample spacing as it pulls away. `--max-level` adds
that many rings of detail beyond it (a clipmap), each a level coarser, twice the spacing and twice as far;
0 is the finest ring alone. Tiles stream in nearest and faced first: at least `--tiles-per-frame` a frame,
then more while `--stream-ms` (4) of the frame are not spent. The panel's "View" section changes the
resolution, the detail rings, the tiles each way and the map scale while the viewer runs.

`--follow` re-runs the script when the camera's focus has moved a quarter of the ring away, with
`params.bounds` round it, for a world built only where it is looked at (`assets/scripts/lands/infinite_world.lua`);
the old tiles stay on screen until the new ones replace them. `--map-scale <f>` draws the map `f` times
wider in x and z, heights and the water level unchanged. The viewer waits for the vertical blank by default;
`--no-vsync` draws as fast as it can, to measure.

To measure: `--drift <m/s>` moves the camera along +x by itself and `--spin <deg>` turns it that much a
frame, both the same every run; `--no-cull` draws every resident tile, to check that the culling never
hides one that shows. The viewer logs its frame times and streaming stats when it closes.

```bash
build/Release/bin/terrain_viewer --script assets/scripts/lands/infinite_world.lua --seed 1 --water --water-opacity 0.45 --follow --param budget=4 --resolution 128 --max-level 1 --ring 4
```

`--frames <n>` presents that many frames and exits. That is what makes the viewer runnable from a script
or a test, and it is how `test_viewer` drives it:

```bash
build/Release/bin/terrain_viewer --script assets/scripts/basic.lua --frames 300 --ring 2
```

A trace of the viewer is captured the same way as one of an export; the GPU zones carry the script line
of the op that produced them, so `ThermalErosion:53` is the erosion on line 53 of the script:

```bash
tracy-capture -o traces/viewer.tracy -f
```

## Measuring land and sea: `terrain_metrics`

```bash
build/Release/bin/terrain_metrics out/island_export            # a directory with height.raw and metadata.json
build/Release/bin/terrain_metrics out/x/height.raw --width 1024 --depth 1024 --resolution 8 --level 0
build/Release/bin/terrain_metrics out/island_export --json --box 2,7
```

Reads an exported height field and reports it as land (above `--level`, the sea level) and sea: land
fraction, masses and the share of the largest, lagoons (water that never reaches the edge), coast
length and per km2, box-counting dimension with its R2 over `--box kmin,kmax` cells (2^k), the narrowest
gap between masses, land height percentiles and mean slope by depth (shelf, slope, abyssal). The
library is `tests/unit/terrain_metrics.hpp`, the same code `test_lands` checks against fields with known
answers, so a number from the tool means what the test says it means. The dimension is a diagnostic per
coast type and range of scales, not a target: it changes with the resolution, the window and the level.

Measuring a reference against an example, so the numbers are comparable:

1. Resample the reference to one fixed resolution per coast type and record it with the numbers.
2. Use the reference's own sea level (0 unless it says otherwise).
3. Run `terrain_metrics <dir or .raw> --box 2,7` on both with the same window.
4. Compare per coast type and per range of scales: a ragged rocky coast sits around 1.2 to 1.3, a
   sedimentary one or an atoll rim lower. Acceptance values come from measuring references; until an
   example has some, it is accepted on structural properties only (one mass, the sign of the distance,
   identical sections), not on a number.

Third-party images and heightmaps stay out of the repository (`docs/references/.gitignore`).

### Relief, and a hillshade

```bash
build/Release/bin/terrain_metrics out/alpine --no-sea --hillshade out/alpine_shade.png --sun 180,35
```

The same run reports the relief (`tests/unit/relief_metrics.hpp`, checked by `test_relief`): slope
percentiles and the share over 35 and 45 degrees, local relief per `--block` metres, the hypsometric
integral, curvature, closed depressions, drainage density from `--channel` square metres, the spectral
slope and the axis-to-diagonal power ratio. Cells at or below `--level` are sea; `--no-sea` measures
everything. `--hillshade` writes a grey image with cast shadows from `--sun azimuth,elevation` (0 is from
the top of the image, 90 from the right; rounded to 45 degrees): it judges geometry without the viewer's
lighting. Put the sun in front of the faces you want to see; behind a 2 km wall it shades kilometres.

`--pits out.png` writes the closed depressions as a white mask, which says what made them where their
count does not.

An 8-bit reference heightmap of unknown size can only be compared on scale-free numbers: the spectral
slope and the hypsometric integral. Check first that it is heights and not shaded relief.

### Library cases

`tests/library/<name>.json` names a script under `assets/scripts`, its bounds, resolution and seed, and
metric ranges; `test_library` exports it and checks them. An entry with `"target": true` is a range taken
from references that the example does not meet yet: printed with `->`, not failed.

## Planning and implementing a change

Opus plans (plan mode), Sonnet implements through the `implementer` agent, then `reviewer` and
`build-runner` run in parallel, then the orchestrator commits. A plan is a message, not a file; its
conclusion goes into `docs/design.md` and its leftovers into `docs/todo.md`, so nothing stale is kept.

Each agent starts cold and reads its context again, so delegate only what pays for that: a change of more than
about 50 lines, or a suite of minutes (`build-runner`, in the background). A smaller change, a script, a doc or a
`-L fast` run is done by the orchestrator itself, which already has the context. Before writing a script call,
`tools/dev.sh api <name>` gives the keys it accepts from the engine itself, and the line of
`docs/lua_reference.md` to read for the rest.

A plan has these parts, each short enough to be checked:

1. **Goal**: one sentence, and what must be true afterwards.
2. **Files**: every file to touch and why; new files only if the stack's convention or Claude's reading
   needs them. Over about 150 changed lines, split it into two plans.
3. **Docs**: which of `status.md`, `todo.md`, `design.md`, `lua_reference.md` change (`tools/dev.sh docs-check`
   fails when they drift). A change that finishes a milestone updates `status.md` and `todo.md` in its commit.
4. **Removes**: the old code, test, script or doc this replaces, deleted in the same commit.
5. **Verify**: exact commands, each with its expected result (`tools/dev.sh lint`, `tools/dev.sh test Release
   -L fast`, `-R <suite>`, `tools/dev.sh review <script> ...`). Debug and Release full suites run once, before
   the commit that touches the engine.
6. **Revert**: what to undo if verification fails twice.
