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

Start `tracy-profiler` before the process you want to capture: the build does not use
`TRACY_ON_DEMAND`, so events are produced from startup and the memory graphs are complete.
