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
`none` or `up` the deflate stream is built in parallel (see `docs/decisions.md`); `all` uses one thread.
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
surface; the panel has the same controls and a reload keeps them. Terrain below the level takes the
water colour in proportion to the water in front of it, with a pale band where it meets the surface:

```bash
build/Release/bin/terrain_viewer --script assets/scripts/terrains/island.lua --water --screenshot out/island.png
```

`--resolution` is metres per sample at the finest level and `--section` is samples per tile, so the two
decide how much world one tile covers. `--ring` is how many tiles are kept on each side of the camera, so
3 is a 7x7 ring of 49 tiles: at 128 samples and 2 m that is about 1.8 km across and takes roughly 45 ms
to evaluate for the sample script.

The ring follows the camera and switches to a coarser sample spacing as it pulls away, up to
`--max-level` doublings. `--tiles-per-frame` bounds how much streaming one frame may pay for.

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
