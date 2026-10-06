# System Specification: Minimalist High-Performance Terrain Engine

## 1. Purpose

A GPU-driven terrain generation engine. Lua scripts describe terrain as a lazy graph of operations (noises, erosion, masks, combinators). The engine compiles that graph into a sequence of Vulkan compute dispatches, evaluates it section by section, and exports 16-bit PNG heightmaps plus metadata. An interactive viewer allows real-time preview and parameter tweaking. A headless executable performs batch export.

The export path is the product. The viewer is a tool for iterating on it.

---

## 2. Core Principles

* **Minimalism:** keep the codebase lean. Add an abstraction layer only when it removes real duplication or isolates a real dependency. No speculative generality.
* **All shared declarations live in `include/`:** any declaration that must be visible outside the `.cpp` that implements it goes in a header under `include/engine/`, whether it is public API or engine-internal. Engine-internal declarations are wrapped in `#ifdef IS_ENGINE ... #endif` (see §6). `src/` contains only `.cpp` files.
* **Two run modes, chosen at init:**
  * **Graphics mode** (viewer): graphics first. The GPU is selected for rendering and presentation.
  * **Headless mode** (export): compute first. No window system is initialized and the GPU is selected for compute.
  * Every system except windowing, swapchain, render passes and presentation must work identically in both modes.
* **Everything is measured:** all CPU time, GPU time, CPU memory and VRAM goes through Tracy. No allocation, buffer or compute pass is invisible to the profiler (see §7 and §13).
* **Deterministic output:** the same script, seed and bounds always produce the same heightmap, regardless of section size, section order or which executable runs it.

---

## 3. Language, Toolchain and Portability

* **C++23**, CMake + Ninja.
* Must compile with **Clang, GCC and MSVC**. No compiler extensions in shared code (`__int128`, `__SIZE_TYPE__`, etc.). Define project integer aliases (`u8_t`, `u16_t`, `u32_t`, `u64_t`, `i8_t`…`i64_t`, `f32_t`, `f64_t`, `usize_t`, `isize_t`, `b8_t`) on top of `<cstdint>` / `<cstddef>`, with `static_assert` on their sizes.
* Include paths must match real casing (`<GLFW/glfw3.h>`, not `<glfw/glfw3.h>`) so the project builds on case-sensitive filesystems.
* Platform-specific code is confined to a single small module (`src/platform/`), never scattered behind `#ifdef`s in engine code. The only exception is the debug-break inside the assertion macros.

### Error Handling Policy

* Fallible engine operations return `std::expected<T, engine::Error>`. `Error` carries a code, a human-readable message and an optional source location (script file and line, see §11).
* Invariants are checked with assertion macros:
  * `ENGINE_ASSERT(cond, msg, ...)` aborts in debug builds.
  * `ENGINE_ASSERT_RETURN(ret, cond, msg, ...)` logs and returns `ret`.
  * `ENGINE_ASSERT_X(ret, cond, { cleanup }, msg, ...)` runs a cleanup block before returning.
* Every Vulkan call is wrapped in `VK_CHECK`, `VK_CHECK_RETURN` or `VK_CHECK_X`. These macros integrate with the assertion system and log the `VkResult` name.
* **Never throw across a C boundary.** This covers GLFW callbacks, Vulkan debug callbacks, the Lua allocator and Lua C functions. Callbacks record the error (log entry plus a flag or error slot) and return normally. Sol2 bindings use protected calls.

### Process Exit Codes

Executables return bit-flag exit codes, so a combined failure is still readable:

* `FAILED_TO_INITIALIZE_ENGINE`
* `FAILED_TO_LOAD_SCRIPT`
* `FAILED_TO_COMPILE_GRAPH`
* `FAILED_TO_EVALUATE`
* `FAILED_TO_EXPORT`
* `FAILED_TO_SHUTDOWN`

---

## 4. Build System (CMake)

* The engine sources build into an `OBJECT` library. An option (`ENGINE_BUILD_SHARED`, default ON) links those objects as either a shared library (DLL) or a static library.
* The export macro header is generated with `generate_export_header` into `${build}/generated/include/engine/api.h`. Only symbols explicitly marked with the export macro cross the DLL boundary.
* All dependencies are fetched with `FetchContent`, **pinned to a release tag**. Never use `master` or `main`.
* **Executables:**
  1. `terrain_viewer`: runs in **Graphics mode**. Window, Vulkan presentation, camera controls, live preview, parameter panel, hot reload.
  2. `terrain_export`: runs in **Headless mode**. CLI for batch processing: runs a script, evaluates the requested bounds, and writes files to disk. It must run on a machine with no display.
* **CMake options:**
  * `ENGINE_VULKAN_VALIDATION`: validation layers and debug messenger. ON in Debug.
  * `ENGINE_VULKAN_VALIDATION_VERBOSE`: include info and verbose messages.
  * `ENGINE_TRACY`: Tracy client. ON by default.
  * `ENGINE_TRACY_CALLSTACKS`: capture callstacks on allocations. Debug only, because it is expensive.
* **Shaders:**
  * All shaders (op kernels, iterative kernels such as erosion, viewer rendering, utilities) are compiled at build time with `glslc` into `.spv` files placed next to the executables.
  * There is no runtime shader compilation in this version (see §9).
* Logging verbosity is stripped at compile time: TRACE in Debug, INFO in Release.

**Dependencies (pinned):**

| Purpose | Library |
| --- | --- |
| Windowing (viewer only) | GLFW |
| Math | GLM |
| Logging | spdlog |
| Profiler client | Tracy, as a **shared** library so the DLL and the executables share one profiler instance |
| VRAM allocation | Vulkan Memory Allocator (VMA) |
| General-purpose CPU heap | mimalloc |
| Lua runtime | Lua 5.4 |
| Lua bindings | Sol2 |
| 16-bit PNG writing | libspng |

---

## 5. Runtime Architecture

### Lifecycle

The engine exposes `engine::init(const AppInfo&)` and `engine::shutdown(Application*)`. `AppInfo` includes the run mode (`Graphics` or `Headless`) and, in Graphics mode, the main window description. Executables are thin: they parse arguments, create the application, push layers and call `Run()`.

**Startup order:**

1. Platform initialization.
2. Tracy.
3. Logging.
4. Memory system (§7).
5. *Graphics mode only:* GLFW initialization, main window creation (hidden) and its `VkSurfaceKHR`. The surface must exist before device selection so presentation support can be verified.
6. Vulkan context: instance, debug messenger and device, selected according to the run mode (see "Vulkan Context").
7. *Graphics mode only:* swapchain, render pass, depth buffer, framebuffers and per-frame sync objects.
8. Lua runtime.
9. Application.

Shutdown runs in exact reverse order. Each step logs its own start and end.

### Application and Layers

* `Application` owns an ordered stack of `Layer`s. Each frame (viewer) or tick (export), the application calls `OnUpdate()` on every layer in order.
* Layers receive a reference to the application. They are non-copyable and non-movable.
* `PushLayer<T>(args...)` constructs the layer in place and returns a typed handle.
* Typical stacks:
  * Viewer: `WindowLayer` → `ViewerLayer` (camera, input, preview, UI).
  * Export: `ExportLayer` only. It stops the application when the job finishes.
* **Windowing (Graphics mode only):**
  * GLFW is initialized by `engine::init` and terminated once by `engine::shutdown`, after every window has been destroyed. In Headless mode GLFW is never initialized.
  * The first `WindowLayer` adopts the main window created at init. Additional `WindowLayer`s create their own windows on the same device; if the device cannot present to a new surface, creation fails with a clear error.
  * Window close requests `Application::Stop()`.
  * A window is shown only after its first frame has been presented, to avoid a white flash.

### Vulkan Context

* Targets **Vulkan 1.4**. Instance and device request API version 1.4, and a physical device that does not support it is rejected. Core features used: synchronization2, timeline semaphores, buffer device address, scalar block layout.
* **Rendering uses the classic render pass path:** `VkRenderPass`, `VkFramebuffer` and `vkCmdBeginRenderPass`. Dynamic rendering is not used.
  * The viewer's main render pass has one color attachment (swapchain format) and one depth attachment. Color: clear on load, store. Depth: clear on load, don't care on store. Final color layout is `PRESENT_SRC_KHR`.
  * Subpass dependencies from `VK_SUBPASS_EXTERNAL` cover the color-attachment-output and early-fragment-test stages, so the swapchain image acquisition and the depth reuse are synchronized correctly.
  * One framebuffer per swapchain image.
  * Graphics pipelines are created against this render pass. Any future render pass must be *render-pass compatible* if it reuses those pipelines.
* **Extension and layer requests** are expressed as `{name, required}` pairs:
  * A missing *required* entry is an initialization error.
  * A missing *optional* entry is a logged warning.
  * Duplicate requests are merged, keeping the strictest `required` flag.
  * Surface and swapchain extensions are requested only in Graphics mode.
* **Physical device selection depends on the run mode:**
  * **Graphics mode (graphics first):**
    * Required: a queue family with graphics support that can present to the main surface, the swapchain extension, and at least one surface format and present mode.
    * Score: discrete GPU first, then integrated, then others. Ties are broken by device-local memory size.
    * Terrain compute work uses a dedicated compute family (compute without graphics) if one exists, for async compute. Otherwise it shares the graphics family.
  * **Headless mode (compute first):**
    * Required: a compute-capable queue family. No surface or swapchain support is checked or requested.
    * A dedicated compute family is preferred.
    * Score: discrete GPU first, then device-local memory size.
  * In both modes a specific device can be forced by UUID through CLI or config.
  * Every candidate device is logged with its score, or with the reason it was rejected.
* **Swapchain** is recreated on resize or on `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR`. Recreation rebuilds the swapchain, depth buffer and framebuffers. The render pass is rebuilt only if the surface format changed.
* Handles passed between modules are typed. Never use `void*` for ownership. Ownership is RAII with explicit move-only wrappers.

### Logging

* spdlog uses an **asynchronous** logger with a dedicated worker thread. Producer threads never block on I/O.
* There are two loggers, `engine` and `app`, selected automatically by a compile definition inside the engine.
* Macros: `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL`. Every macro must be defined in every build configuration, as a no-op when stripped.
* Log entries are mirrored into Tracy with `TracyMessage` so they appear on the timeline.
* `terrain_export` can additionally emit **JSON lines** (`--log-format=json`) so tools and LLMs can parse errors reliably.

---

## 6. Code Conventions

* **Header placement:**
  * Every declaration used outside the `.cpp` that implements it lives in a header under `include/engine/`. This applies equally to public API and to engine-internal code.
  * Engine-internal declarations are wrapped in `#ifdef IS_ENGINE ... #endif`. `IS_ENGINE` is a **PRIVATE** compile definition of the engine target, so executables and other consumers never see internal declarations.
  * A header may contain only public declarations, only internal ones (the whole content inside the guard), or both. In mixed headers, public declarations come first, followed by a single internal block at the end.
  * A public declaration must never depend on a type that is declared only inside an `IS_ENGINE` block.
  * Third-party headers needed only by internal declarations (Vulkan, VMA, Sol2, spdlog internals, mimalloc…) are included **inside** the guard, so consumers do not need those include paths.
  * Public symbols that cross the DLL boundary use the export macro. Internal symbols are never exported.
  * Declarations used only inside one `.cpp` stay in that `.cpp`, in an anonymous namespace.
* **Include order:** each `.cpp` starts with the header it implements. Then, separated by blank lines and in this order: other engine headers (`<engine/...>`), third-party headers, standard library headers. All engine headers are included with angle brackets.
* **Nullability:** use references for non-null parameters. A raw pointer means "may be null" and must be checked. Owning raw pointers are forbidden.
* **Heap memory:** never call `malloc`, `free`, `new` or `delete` directly in engine code. All memory comes from the memory system (§7).
* **Containers:** use `std::pmr` containers in engine systems, backed by engine allocators.
* **Profiling coverage:** every non-trivial function in a hot or long-running path opens a Tracy zone (`ZoneScopedN`). Every GPU pass opens a Tracy GPU zone.
* **Documentation:** `AGENTS.md` at the repository root is a short map pointing to `docs/`:

| File | Content |
| --- | --- |
| `docs/architecture.md` | Overall layering and runtime flow |
| `docs/conventions.md` | Code conventions |
| `docs/commands.md` | How to configure, build, run and test |
| `docs/lua_api.md` | Script API reference |
| `docs/status.md` | Current state of the project |
| `docs/roadmap.md` | Medium and long-term direction |
| `docs/todo.md` | Concrete open tasks |

  `status.md` and `todo.md` must be updated whenever a milestone (§15) changes state.

---

## 7. Memory Management and Tracy Integration

**Goal:** at any moment, Tracy must show for both CPU RAM and VRAM:

* how much memory is in use,
* what it is used for (named category),
* when it was allocated and freed,
* how much each allocator has **reserved but is not currently using**.

### 7.1 Allocator Interface

Every allocator implements a common interface:

```cpp
class Allocator {
public:
    virtual void* Allocate(usize_t size, usize_t align) = 0;
    virtual void  Free(void* ptr) = 0;
    virtual AllocatorStats Stats() const = 0; // reserved, used, peak, allocation_count
    const char* Name() const;                 // stable string literal, used as Tracy pool name
};
```

Each allocator also exposes a `std::pmr::memory_resource` adapter, so `std::pmr` containers can use it.

### 7.2 CPU Allocators

| Allocator | Implementation | Used for |
| --- | --- | --- |
| `GeneralAllocator` | Wraps **mimalloc** | Long-lived, variable-size objects |
| `ArenaAllocator` | Hand-written. Linear bump allocator over chunks, reset as a whole | AST construction, graph compilation, per-frame scratch |
| `PoolAllocator` | Hand-written. Fixed-size slots with a free list | AST nodes, section descriptors, small fixed-size records |
| `LuaAllocator` | `lua_Alloc` function passed to `sol::state(panic, alloc, userdata)`, forwarding to a dedicated `GeneralAllocator` instance named `CPU/Lua` | All memory used by the Lua runtime |

* **Global `operator new` / `operator delete`:** override once per module, routing to a `GeneralAllocator` named `CPU/Untracked new`. Anything that escapes the engine allocators still shows up in Tracy, and its size is the to-do list for routing it properly.
* **Module boundary rule:** memory allocated inside the DLL is freed inside the DLL, and the same applies to each executable.

### 7.3 GPU Allocators

* **VMA** is the only path to `vkAllocateMemory`.
* Buffers and images are created through engine wrappers that require a **category**:
  * `VRAM/Section buffers R2`
  * `VRAM/Section buffers R3`
  * `VRAM/Intermediate`
  * `VRAM/Staging`
  * `VRAM/Readback`
  * `VRAM/Viewer`
  * `VRAM/Uniforms`
* **Buffer sizes depend on the mapping type (§8).** For a value of mapping `Rn→Rm` evaluated on a section with halo `h`:
  * `R2→Rm`: `(sx + 2h) × (sz + 2h) × m × bytes_per_component`
  * `R3→Rm`: `(sx + 2h) × (sy + 2h) × (sz + 2h) × m × bytes_per_component`
  * Components are stored as tightly packed `f32` using scalar block layout, so an `R2→R3` sample takes exactly 12 bytes, not 16.
  * The halo of each value comes from halo propagation (§9), so two values of the same mapping can have different sizes.
* **R3 sections are much smaller than R2 sections.** One `R3→R1` value on a 512³ section is 512 MiB, while one `R2→R1` value on a 512² section is 1 MiB. Default section sizes are configured separately per domain (for example 512² for R2 and 64³ for R3).
* **Section buffer pool:**
  * One large `VkBuffer` per memory type.
  * Sub-allocated with a **VMA virtual block** (`vmaCreateVirtualBlock`).
  * Slots are grouped in **size classes** computed from the mapping sizes above. R2 and R3 values never share a size class, and a value is never placed in a slot smaller than its computed size.
  * The pool grows by whole blocks and only shrinks on explicit trim.
* **Staging and readback:** persistently mapped ring buffers, sized for at least two sections in flight.

### 7.4 Tracy Reporting Rules

**1. Named pools**

* Every allocation is reported with `TracyAllocN(ptr, size, name)` and freed with `TracyFreeN(ptr, name)`.
* `name` is the allocator's stable string literal (`CPU/General`, `CPU/AST arena`, `VRAM/Section buffers R2`, …).
* Tracy identifies pools by pointer, so the name must always be the **same pointer**.

**2. Reserved vs. used: two pools per allocator**

* `"<name> [reserved]"`: one event per block obtained from the OS or the driver.
  * CPU: arena chunks, pool pages, mimalloc segments if exposed.
  * GPU: every `VkDeviceMemory` block, reported through `VmaDeviceMemoryCallbacks::pfnAllocate` and `pfnFree`.
* `"<name>"`: one event per live sub-allocation handed to callers.
* A plot `"<name> unused"`, computed as reserved minus used, configured with `TracyPlotConfig(..., tracy::PlotFormatType::Memory, ...)`. It is updated whenever either value changes, and at least once per frame or tick.

**3. High-frequency allocators (arenas)**

* Report chunk reservations as events.
* Report *used bytes* as a plot instead of per-bump events, to keep trace size sane.
* An arena reset emits a single plot update.

**4. Reallocation**

* A reallocation is reported as free(old) followed by alloc(new).
* This applies to `lua_Alloc` too: `nsize == 0` is a free, `ptr == nullptr` is an alloc, anything else is both.

**5. GPU allocation keys**

* GPU sub-allocations have no CPU address, so the `VmaAllocation` handle (or the virtual allocation handle) is used as the Tracy pointer key.
* The key must be unique while the allocation is alive.

**6. Budgets**

* Each frame or tick, `vmaGetHeapBudgets` is queried.
* `VRAM heap N usage` and `VRAM heap N budget` are plotted in memory format.
* Usage above 90% of budget logs a warning and makes the evaluator reduce the number of sections in flight.

**7. Callstacks**

* With `ENGINE_TRACY_CALLSTACKS`, allocation events use the `...S` variants (`TracyAllocNS`) with a configurable depth.

**8. Shutdown leak check**

* Every allocator asserts `used == 0` on destruction.
* On failure, it logs its name, live allocation count and bytes.

**9. Session mode**

* Do not build with `TRACY_ON_DEMAND` for memory profiling sessions. Events that happen before the profiler connects would be lost, and the memory graphs would be wrong.

---

## 8. Lua and Sol2 Integration (Lazy Graph Construction)

* **Runtime:** Lua 5.4 through Sol2.
* **One translation unit:** all bindings, type checks and C++ ↔ Lua bridges live in **`src/lua.cpp`**. This contains Sol2's compile cost in one translation unit. No Sol2 type appears in any public header.
* **Lazy evaluation:** script functions perform **no math**. Each call appends a node to a typed graph and returns a lightweight handle.
* **Mappings:** every value in the graph has a mapping type `Rn→Rm`, where `n` is the domain dimension and `m` the number of components (1 to 4).
  * Domain `R2` is `(x, z)`. Domain `R3` is `(x, y, z)`.
  * Common mappings:

    | Mapping | Example |
    | --- | --- |
    | `R2→R1` | Heightmap, mask |
    | `R2→R2` | Gradient of a heightmap, flow field |
    | `R2→R3` | Normal map |
    | `R3→R1` | Density field |
    | `R3→R3` | 3D vector field, for example a domain warp |

  * Every op declares the mappings it accepts and the mapping it produces. Pointwise arithmetic is generic over `Rn→Rm` when both operands match, and broadcasts an `Rn→R1` operand over `Rn→Rm`.
  * Components are extracted with `.x`, `.y`, `.z`, `.w` (producing `Rn→R1`) and assembled with `Vec.Combine{ ... }`.
  * Mapping mismatches are compile errors (§11), not runtime surprises.
  * The mapping type also determines buffer sizes in VRAM (§7.3).
* **Multi-output handles:** a node can expose several channels, each with its own mapping. For example, a 2D `Noises.fBm` exposes `value` (`R2→R1`) and an analytic `gradient` (`R2→R2`). The script picks channels explicitly: `h.value`, `h.gradient`. Unused channels are never computed.
* **Semantic macros:** a high-level API designed for readability by humans and LLMs. Parameters use named tables and every parameter has a documented default.

  ```lua
  local base   = Noises.fBm{ kind = "simplex", frequency = 0.002, octaves = 6, lacunarity = 2.0, gain = 0.5 }
  local ridges = Noises.Ridged{ frequency = 0.004, octaves = 4 }
  local h      = Combine.Blend{ a = base, b = ridges, mask = Masks.Slope{ input = base, min = 0.2, max = 0.6 } }
  local eroded = Erosion.Hydraulic{ input = h, iterations = 200, rain = 0.01, evaporation = 0.02 }
  local normals = Terrain.Normals{ input = eroded.value }          -- R2→R1 in, R2→R3 out
  return {
    height  = { value = eroded.value, range = { -200, 1800 } },
    normals = { value = normals },
  }
  ```

* **Source locations:** when a node is created, the binding captures the calling script line with `debug.getinfo` and stores it in the node. Every validation or compilation error must report it.
* **Entry point:** a script defines `main(params)`. `params` contains:
  * `seed`
  * `bounds`: world-space minimum and maximum
  * `resolution`: meters per pixel
  * user-defined parameters exposed to the viewer UI

  `main` returns a table that maps output names to output descriptions: the graph handle and, for scalar outputs, the value range used for normalization (§12).

---

## 9. Graph Compiler (One Kernel per Node)

The compiler turns the lazy graph into an ordered list of GPU dispatches. **Each node maps to one precompiled compute kernel.** There is no runtime shader generation in this version.

Kernel fusion (generating one shader per region of nodes) is deliberately postponed. It will be evaluated once this version produces real measurements (see "Future: kernel fusion" at the end of this section). The design below must not prevent adding it later.

### Stage 1 — Validation

* Check mapping types, channel availability, parameter ranges and cycles. Cycles are impossible through the API, but the compiler still asserts there are none.
* Errors carry the node's source line.

### Stage 2 — Canonicalization

* **Constant folding.**
* **Common-subexpression elimination**, using a structural hash of `(op, inputs, parameters)`. Two identical noise nodes must run once.
* **Dead-node removal:** nodes that do not reach any requested output are dropped.

### Stage 3 — Node classification

Every op declares its class. The class drives halo propagation and will be the basis for fusion later.

| Class | Definition | Examples | Halo |
| --- | --- | --- | --- |
| **Pointwise** | Each output pixel depends only on the same pixel of its inputs, plus world coordinates | Noises, arithmetic, curves, clamps, blends, analytic gradients | 0 |
| **Neighborhood** | Reads a fixed radius `r` around the pixel | Finite-difference slope, blur, curvature | `r` |
| **Iterative / global** | Many iterations with data movement | Hydraulic erosion, thermal erosion, flow accumulation | Declared by the op (effective influence radius) |

### Stage 4 — Halo propagation

* Starting from the requested outputs and walking backwards, each node's output accumulates the sum of the radii of all downstream consumers. This defines how much padding each section must compute (§10).
* Because nothing is fused, a pointwise node feeding a neighborhood node is computed over the enlarged area directly.

### Stage 5 — Kernel library

* Every op has a **hand-written GLSL compute shader** in `assets/shaders/ops/`, compiled at build time with `glslc`. Shared code (noise functions, hashing, math helpers) lives in `assets/shaders/lib/` and is included by the op shaders.
* **All op kernels follow one uniform interface:**
  * Inputs and outputs are bound as storage buffers through **buffer device address**, passed in push constants. This avoids per-dispatch descriptor set management.
  * Push constants also carry the section origin (integer), section size per axis, domain dimension, halo and the node's parameter block.
  * Workgroup size is fixed project-wide (for example 8×8 for R2, 4×4×4 for R3).
* **Numeric parameters** (frequency, gain, thresholds, octave count…) go in push constants or a parameter buffer. Changing them never creates a pipeline; it only re-dispatches.
* **Variants** that change code shape (for example noise kind) are either separate shader files or **specialization constants**. Specialization constants create a new pipeline from the same SPIR-V, with no shader compilation.
* Pipelines are created once at startup (or lazily on first use) and kept in a pipeline map keyed by `(op, variant)`. A `VkPipelineCache` is persisted to disk.
* Multi-output ops (for example noise returning value and analytic gradient) write all their channels in the same dispatch. Channels that no consumer requests are not written; the op receives a channel mask in its push constants.

### Stage 6 — Scheduling and buffer planning

* Nodes are ordered topologically. One node = one dispatch, with a buffer memory barrier between dependent dispatches (synchronization2).
* **Buffer planning is essential in this version**, since every node produces an intermediate buffer:
  * A liveness analysis assigns physical buffers from the section pool and reuses a buffer as soon as its last consumer has run. A buffer is only reused for a value whose computed size (mapping, section size and halo, §7.3) fits in it.
  * Peak intermediate VRAM per section is computed **before** evaluation and logged. The evaluator uses it to decide how many sections fit in flight.
* All dispatches of one section are recorded in a single command buffer.

### Instrumentation

* Every compiler stage is a Tracy zone: validation, CSE, classification, halo propagation, scheduling, buffer planning, pipeline creation.
* **Every dispatch has its own Tracy GPU zone, labeled with the op name and the node's script line.** This is the data that will decide whether fusion is worth it.
* Compile statistics (node count before/after CSE, dispatch count, peak intermediate VRAM per section) are logged and plotted.

### Future: kernel fusion (not in this version)

Kernel fusion would merge chains of pointwise nodes into one generated shader compiled at runtime. It will be considered only if measurements on real scripts show that it matters. The metrics to collect:

* GPU time spent in pointwise dispatches vs. total GPU time per section.
* Intermediate VRAM per section, and how many sections in flight it costs.
* Memory bandwidth per section (from the dispatch timings and buffer sizes).

To keep that door open:

* Pointwise op shaders keep their math in reusable functions inside `assets/shaders/lib/`, separated from the buffer load/store boilerplate. A future code generator could then chain those functions instead of rewriting them.
* The compiler keeps classification and halo propagation as separate stages, so fusion can be inserted between them and scheduling.

---

## 10. Sections, Streaming and Determinism

* **Sections:** the requested bounds are split into fixed-size sections. Each section is evaluated with its halo, and only the interior is kept.
  * `R2` graphs use 2D tiles, for example 512×512 (configurable, power of two).
  * `R3` graphs use 3D bricks, for example 64×64×64. The vertical extent comes from the `y` range of the bounds. Halos apply on all three axes.
* **World coordinates:**
  * Kernels compute positions from an **integer section origin** plus a local pixel offset.
  * Noise hashing operates on the integer lattice.
  * This avoids float precision loss far from the origin and guarantees that neighboring sections agree exactly at their borders.
* **Seam test:** evaluating a region as one section, or as N×N sections, must produce bit-identical interiors. This is an automated test (§14).
* **Pipelining:**
  * While section *k* is being computed, section *k−1* is read back through the staging ring and section *k−2* is encoded and written to disk on a CPU worker.
  * Synchronization uses timeline semaphores.
* **Memory pressure:** the number of sections in flight depends on the planned VRAM per section (§9, stage 6) and the current VMA budget (§7.4, rule 6).

---

## 11. Hot Reload and Error Reporting

* **File watching:**
  * The viewer polls the active script and its `require`d modules for modification time changes, with a short debounce of about 150 ms.
  * No OS-specific watch API is needed.
* **Reload:**
  * Reloading re-runs the script in a fresh Lua state and recompiles the graph on a worker thread.
  * The previous valid graph keeps rendering until the new one is ready.
  * If the graph structure did not change and only numeric parameters did, the reload only updates push constants and parameter buffers. No pipeline is created.
* **Error format:** every error is reported as `file:line: [stage] message`. Example:

  ```
  terrain.lua:42: [validation] Erosion.Hydraulic expects R2→R1 input, got R3→R1
  ```

  * The viewer shows errors in an on-screen panel.
  * `terrain_export` prints them to stderr (or as JSON lines) and exits with the matching exit code.
  * Errors must be specific enough for an LLM to fix the script without reading engine code.

---

## 12. Execution and I/O

### `terrain_export` CLI

```
terrain_export --script terrain.lua --seed 1234 \
               --min 0,0 --max 16384,16384 --resolution 1.0 \
               --section 512 --out out/ [--param key=value ...] [--log-format json]
```

### Outputs

* **File format by mapping:**
  * `R2→R1` (heightmaps, masks): 16-bit grayscale PNG.
  * `R2→R2` and `R2→R3` (gradients, normal maps): 16-bit RGB PNG. `R2→R2` writes blue as 0. Normals are remapped from `[-1, 1]`.
  * `R3→Rm` (volumes): raw little-endian `f32` files, one per brick or stitched. Dimensions and component count are written in the metadata sidecar.
* PNGs are written with **libspng**.
  * Rows are streamed progressively, so a full large map never has to sit in CPU RAM.
  * Output is either one stitched image or one file per tile (`--tiles`).
* **Normalization (scalar PNG outputs):**
  * The value range is declared by the script, as `range = {min, max}` in the output description.
  * Values are mapped linearly to `[0, 65535]`, with clamping.
  * A clamped pixel count is reported in the log, since clamping usually means the declared range is wrong.
* **Metadata sidecar (`.json`):**
  * script path and hash
  * seed
  * bounds
  * resolution
  * height range
  * section size
  * engine version
  * per-output mapping type and file format
  * **timings:** total wall time, graph compilation, pipeline creation, GPU time per op (summed over all sections), readback, encoding and writing
  * **peak memory:** peak CPU RAM and peak VRAM, per allocator category
  * **environment:** GPU name, driver version, Vulkan API version, CPU, OS, build type and engine commit hash

  The timing and environment fields are what the performance regression tests compare (§14).

### Viewer

* Renders the current heightfield as a GPU-displaced grid. The vertex shader samples the compute output directly, so no mesh is uploaded.
* Camera: orbit and fly modes.
* Input system abstracted from GLFW key codes.
* Frustum culling of sections.
* Auto-generated UI for user parameters.
* Evaluation is restricted to visible sections at a resolution appropriate to camera distance.

---

## 13. Profiling (Tracy)

* **CPU:** zones on all engine stages. Thread names are set for every thread: main, log, pipeline creation, IO workers.
* **GPU:**
  * A `TracyVkContext` per queue, calibrated when `VK_EXT_calibrated_timestamps` is available.
  * A GPU zone per dispatch, labeled with the op name and the node's script line.
  * Zones are collected each frame or tick.
* **Frames:**
  * `FrameMark` per viewer frame.
  * `FrameMarkNamed("Section")` per evaluated section in export.
* **Plots:**
  * all memory plots from §7
  * sections in flight
  * readback bandwidth
* **Shared client:** the Tracy client is a shared library, so the DLL and the executables report into the same session.

---

## 14. Testing

Tests run with `ctest`.

### Unit and behavior tests

| Test | Description |
| --- | --- |
| **Seam test** | Single-section vs. tiled evaluation must give bit-identical interiors, for R2 tiles and R3 bricks |
| **Determinism test** | Same script, seed and bounds twice must give identical hashes |
| **Compiler tests** | Small graphs → expected dispatch count, mapping checks, halo sizes, CSE results, buffer reuse and peak VRAM |
| **Allocator tests** | Every allocator returns to `used == 0`. Arena reset and pool reuse behave correctly. Reserved ≥ used at all times |
| **Lua error tests** | Malformed scripts produce the exact expected `file:line: [stage]` message |
| **Headless smoke test** | `terrain_export` runs a sample script on a machine without a display |

### Dataset tests (golden outputs)

Each test case is a directory under `tests/datasets/<case>/`:

```text
tests/datasets/<case>/
├── script.lua             # The terrain script under test
├── case.json              # seed, bounds, resolution, section sizes, params, tolerances, perf thresholds
└── expected/
    ├── <output files>     # Precomputed results (PNG / raw volumes)
    └── metadata.json      # Metadata produced when the expected results were generated
```

* **Coverage:** at least one case per op, cases for every common mapping (`R2→R1`, `R2→R2`, `R2→R3`, `R3→R1`), a multi-section case that crosses section borders, erosion cases, and one large realistic script.
* **Comparison:**
  * The runner executes each case and compares the **decoded data** (pixels or floats), not file bytes, since PNG encoding details may differ.
  * Default is exact equality. A case may declare a tolerance in `case.json` (maximum absolute difference in 16-bit units or in `f32` ULPs) when results are known to vary between GPU vendors.
  * On failure, the runner reports the maximum difference and the number of differing samples, and writes a difference image to the test output directory.
* **Updating expected results:** only through an explicit command (`test_datasets --update-golden <case>`), never automatically. The commit that updates them must explain why the output changed.

### Performance regression tests

After a dataset case passes its correctness check, its new metadata is compared against a timing baseline:

* **Baselines are machine-specific.** They live in `tests/baselines/<machine-id>.json`, where `machine-id` is a hash of GPU name, driver version, CPU and build type. If there is no baseline for the current machine, the run records one and passes with a notice.
* **Measurement:** each case runs one warm-up pass and then N measured passes (default 5). The median is used.
* **Compared values:** total time, each stage from the metadata timings, GPU time per op, and peak RAM / VRAM.
* **Thresholds** (configurable per case): more than 10% slower is a warning, more than 25% slower is a failure. Memory peaks use the same rule. Being faster than the threshold prints a notice suggesting a baseline update.
* **History:** every run appends its results, with the commit hash, to `tests/history/<machine-id>.jsonl`, so trends are visible across commits.
* Performance tests run only in Release builds. In Debug builds they are skipped.

---

## 15. Implementation Order

### Development workflow

* **One commit per step.** Every concrete step of a milestone ends with its own commit, not only the milestone as a whole.
* **Never amend.** If a bug is discovered after a commit, it is fixed in a **new** commit (`fix: ...`) whose message names the commit hash that introduced the bug. No `git commit --amend`, no rebasing or squashing of existing commits, no force pushes.
* **Every commit builds and passes the existing tests.**
* Commit messages follow Conventional Commits: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `chore`.
* When a step changes the state of a milestone, the same commit updates `docs/status.md` and `docs/todo.md`.

### Milestones

Each milestone ends with its acceptance criterion met and `docs/status.md` updated.

1. **Skeleton.**
   Build system, project types, assertion macros, async logging, Tracy, exit codes.
   *Accepted when:* the empty application starts and shuts down cleanly, with zones visible in Tracy.

2. **Memory system.**
   General, arena, pool and Lua allocators. pmr adapters. Tracy pools and reserved/unused plots. Shutdown leak check.
   *Accepted when:* Tracy's memory view shows every pool with correct reserved and used values.

3. **Vulkan context in Headless mode.**
   Vulkan 1.4 instance, messenger, compute-first device selection. VMA with Tracy integration. Section pool with mapping-based size classes, staging rings.
   *Accepted when:* a trivial compute kernel writes a buffer that is read back correctly, and VRAM pools appear in Tracy.

4. **First export.**
   One hand-written simplex kernel, readback, and a 16-bit PNG through libspng with its metadata sidecar.
   Dataset test runner and performance baseline recording.
   *Accepted when:* `terrain_export` produces a valid 16-bit PNG on a machine with no display, and the first dataset case passes and records its baseline.

5. **Graph and compiler, without Lua.**
   Graph built from C++. Validation, CSE, classification, halo propagation, op kernel library, pipeline map, scheduling, buffer planning.
   *Accepted when:* compiler tests pass, each op kernel matches a CPU reference implementation on small inputs, and every op has its dataset case.

6. **Lua bindings.**
   `lua.cpp`, semantic macros, source locations, the `main(params)` entry point.
   *Accepted when:* Lua error tests pass.

7. **Sections and streaming.**
   Halo propagation, pipelined evaluation, progressive PNG writing, budget-aware sections in flight.
   *Accepted when:* seam and determinism tests pass on a 16k × 16k export.

8. **Iterative kernels.**
   Hydraulic and thermal erosion with correct halo declarations.

9. **Viewer (Graphics mode).**
   Graphics-first device selection, window layer, swapchain, render pass and framebuffers, displaced-grid rendering, camera, input, parameter UI, hot reload.

---

## 16. Directory Structure

Headers marked *(internal)* have their content inside `#ifdef IS_ENGINE`. Headers marked *(mixed)* have public declarations followed by an internal block. `src/` contains only `.cpp` files.

```text
root/
├── AGENTS.md                      # Short map pointing to docs/
├── CMakeLists.txt                 # Root build: engine library + both executables
├── docs/                          # architecture, conventions, commands, lua_api, status, roadmap, todo
├── assets/
│   ├── shaders/
│   │   ├── ops/                   # One compute shader per graph op (noises, math, masks, erosion...)
│   │   ├── lib/                   # Shared GLSL includes (noise, hash, math), kept fusion-friendly
│   │   └── viewer/                # Terrain rendering shaders
│   └── scripts/                   # Sample Lua scripts
├── include/
│   └── engine/
│       ├── engine.hpp             # init / shutdown, run modes, exit codes
│       ├── application.hpp        # Application lifecycle and layer stack
│       ├── layer.hpp              # Layer base class
│       ├── window_layer.hpp       # Window management layer (mixed)
│       ├── common.hpp             # Project types and utility macros
│       ├── assert.hpp             # ENGINE_ASSERT family
│       ├── error.hpp              # engine::Error and std::expected aliases
│       ├── log.hpp                # Logging macros (mixed: engine logger is internal)
│       ├── prelude.hpp            # Convenience include for executables
│       ├── lua.hpp                # Lua runtime entry points (internal)
│       ├── platform.hpp           # OS-specific helpers (internal)
│       ├── memory/
│       │   ├── allocator.hpp      # Allocator interface, stats, pmr adapters
│       │   ├── general.hpp        # mimalloc-backed allocator (internal)
│       │   ├── arena.hpp          # (internal)
│       │   ├── pool.hpp           # (internal)
│       │   └── tracy_memory.hpp   # Helpers for Tracy pools and reserved/unused plots (internal)
│       ├── vulkan/                # All internal
│       │   ├── vk.hpp             # VK_CHECK macros
│       │   ├── context.hpp        # Instance, messenger, mode-dependent device selection
│       │   ├── device.hpp
│       │   ├── allocator.hpp      # VMA setup, device memory callbacks, budgets
│       │   ├── buffer_pool.hpp    # Section pool (VMA virtual blocks, size classes), staging/readback rings
│       │   ├── swapchain.hpp      # Swapchain, depth buffer, framebuffers
│       │   ├── render_pass.hpp    # Main VkRenderPass
│       │   └── window.hpp         # Surface creation
│       ├── render/
│       │   └── renderer.hpp       # Viewer rendering interface (mixed)
│       └── terrain/
│           ├── mapping.hpp        # Rn→Rm mapping types and size computation
│           ├── graph.hpp          # Graph handles and channels (mixed: node storage is internal)
│           ├── export.hpp         # Export job description and entry point
│           ├── compiler.hpp       # (internal)
│           ├── kernels.hpp        # (internal)
│           ├── evaluator.hpp      # (internal)
│           └── output_writer.hpp  # (internal)
├── src/
│   ├── engine.cpp                 # Startup/shutdown sequence
│   ├── application.cpp
│   ├── window_layer.cpp
│   ├── log.cpp                    # Async logging + Tracy message mirroring
│   ├── lua.cpp                    # The single Lua <-> C++ translation unit
│   ├── platform/                  # The only place for OS-specific code
│   ├── memory/
│   │   ├── general.cpp
│   │   ├── arena.cpp
│   │   ├── pool.cpp
│   │   └── new_delete.cpp         # Global operator new/delete routing
│   ├── vulkan/
│   │   ├── context.cpp
│   │   ├── device.cpp
│   │   ├── allocator.cpp
│   │   ├── buffer_pool.cpp
│   │   ├── swapchain.cpp
│   │   ├── render_pass.cpp
│   │   └── window.cpp
│   ├── render/
│   │   └── renderer.cpp           # Displaced-grid terrain rendering
│   └── terrain/
│       ├── mapping.cpp
│       ├── graph.cpp              # Node storage, handles, source locations
│       ├── compiler.cpp           # Validation, CSE, classification, halo propagation
│       ├── kernels.cpp            # Op registry, pipeline map, specialization constants, pipeline cache
│       ├── evaluator.cpp          # Scheduling, buffer planning, command recording, section pipelining
│       └── output_writer.cpp      # libspng 16-bit PNG, raw volumes, JSON sidecar
└── tests/
    ├── unit/                      # Compiler, allocator, Lua error, seam and determinism tests
    ├── datasets/<case>/           # script.lua, case.json, expected/ (outputs + metadata.json)
    ├── baselines/                 # <machine-id>.json timing and memory baselines
    ├── history/                   # <machine-id>.jsonl results per run, with commit hash
    └── runner/                    # test_datasets: golden comparison + performance regression
```
