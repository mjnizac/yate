# Status

Scope of the current session: milestones 1 to 3.

| # | Milestone | State |
| --- | --- | --- |
| 1 | Skeleton | implemented, compiles clean; link and run pending |
| 2 | Memory system | implemented, compiles clean; link and run pending |
| 3 | Vulkan context in Headless mode | implemented, compiles clean; link and run pending |
| 4 | First export | not started |
| 5 | Graph and compiler, without Lua | not started |
| 6 | Lua bindings | not started |
| 7 | Sections and streaming | not started |
| 8 | Iterative kernels | not started |
| 9 | Viewer (Graphics mode) | not started |

## Blocker

The Vulkan SDK is not installed on this machine, so the project has not been configured or compiled
yet. `find_package(Vulkan 1.4 REQUIRED COMPONENTS glslc)` is the first thing that fails. The MSVC
toolchain, the Windows SDK, CMake and Ninja are all present and working.

See `docs/commands.md` for the one command that installs it; it needs an interactive UAC prompt,
which is why it could not be scripted.

Every translation unit has been compile-checked with MSVC 19.44 at /W4 against the fetched
dependencies and the Vulkan 1.4 headers, and is clean. What has not happened yet is linking,
shader compilation with `glslc`, and running anything: all three need the SDK.

## 1. Skeleton

Build system with an `OBJECT` library linked as a DLL or a static library, generated export header,
`FetchContent` dependencies pinned to release tags, build-time `glslc` compilation, the assertion
macro family, asynchronous spdlog logging mirrored into Tracy, bit-flag exit codes, the platform
module, and `Application` with its layer stack.

`terrain_export` parses the full CLI documented in the spec and runs the real update loop.
`terrain_viewer` does not exist yet: Graphics mode is rejected by `engine::init` with an error that
points here, because the window system arrives in milestone 9.

## 2. Memory system

`Allocator` interface with stats and a `std::pmr` adapter; `GeneralAllocator` over mimalloc,
`ArenaAllocator` over chunks, `PoolAllocator` with a free list, and the `lua_Alloc`-compatible hook
over a dedicated `CPU/Lua` allocator. Global `operator new`/`delete` are replaced per module and
routed to `CPU/Untracked new`.

Tracy reporting follows the rules in spec section 7.4: two pools per allocator (`<name>` for live
sub-allocations, `<name> [reserved]` for blocks obtained from the OS or the driver), a
`<name> unused` plot, arenas reporting used bytes as a plot instead of per-bump events,
reallocation reported as free-then-alloc, and a leak check on destruction.

Known limitation: mimalloc does not expose its segment reservations per allocator instance, so
`GeneralAllocator` reports `reserved == used` and its `unused` plot stays at zero.

## 3. Vulkan context in Headless mode

Vulkan 1.4 instance with merged `{name, required}` layer and extension requests, debug messenger
chained into instance creation, compute-first physical-device selection that logs every candidate
with its score or its rejection reason, UUID forcing, a `Queue` wrapper with a command pool, a
timeline semaphore and a Tracy GPU context (calibrated when `VK_EXT_calibrated_timestamps` exists).

VMA is the only path to `vkAllocateMemory`. Device-memory blocks are reported through
`VmaDeviceMemoryCallbacks` into `VRAM/Device memory [reserved]`; buffers carry a category which is
also their Tracy pool name; heap budgets are plotted every tick and a heap above 90% flips the
allocator into a pressure state the evaluator will read.

The section pool keeps one large `VkBuffer` per domain and block, sub-allocated with VMA virtual
blocks in power-of-two size classes derived from the mapping, section extent and halo. `R2` and
`R3` values never share a block. Staging and readback are persistently mapped FIFO rings.

Two op kernels exist: `ops/constant.comp` and `ops/coords.comp`, both on the uniform kernel
interface. `test_compute_roundtrip` is the acceptance check: it dispatches `coords` into a section
slot at an origin far from zero with a halo of 2, reads it back and verifies every padded sample,
then checks the whole VRAM accounting returns to zero.
