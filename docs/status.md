# Status

| # | Milestone | State |
| --- | --- | --- |
| 1 | Skeleton | **done**, accepted |
| 2 | Memory system | **done**, accepted |
| 3 | Vulkan context in Headless mode | **done**, accepted |
| 4 | First export | **done**, accepted |
| 5 | Graph and compiler, without Lua | in progress |
| 6 | Lua bindings | not started |
| 7 | Sections and streaming | not started |
| 8 | Iterative kernels | not started |
| 9 | Viewer (Graphics mode) | not started |

## Verification

Both configurations build warning-free with MSVC 19.44 at `/W4`, and `ctest` passes 6 of 6 in each:

```
test_allocators ......... Passed
test_mapping ............ Passed
test_errors ............. Passed
test_compiler ........... Passed
test_compute_roundtrip .. Passed
test_noise .............. Passed
test_datasets ........... Passed
```

Everything GPU-side ran on a GeForce GTX 1070 reporting Vulkan 1.4.312, with a dedicated compute
queue family and calibrated timestamps available.

Still open: capture a Tracy session and confirm the memory view shows every CPU and VRAM pool with
correct reserved and used values. The events are emitted; nobody has looked at the graphs yet.

## 1. Skeleton

Build system with an `OBJECT` library linked as a DLL or a static library, generated export header,
`FetchContent` dependencies pinned to release tags, build-time `glslc` compilation, the assertion
macro family, asynchronous spdlog logging mirrored into Tracy, bit-flag exit codes, the platform
module, and `Application` with its layer stack.

`terrain_viewer` does not exist yet: Graphics mode is rejected by `engine::init` with an error that
points here, because the window system arrives in milestone 9.

## 2. Memory system

`Allocator` interface with stats and a `std::pmr` adapter; `GeneralAllocator` over mimalloc,
`ArenaAllocator` over chunks, `PoolAllocator` with a free list, and the `lua_Alloc`-compatible hook
over a dedicated `CPU/Lua` allocator. Global `operator new`/`delete` are replaced per module and
routed to `CPU/Untracked new`.

Tracy reporting follows spec section 7.4: two pools per allocator, a `<name> unused` plot, arenas
reporting used bytes as a plot instead of per-bump events, reallocation reported as free-then-alloc,
and a leak check on destruction.

Known limitation: mimalloc does not expose its segment reservations per allocator instance, so
`GeneralAllocator` reports `reserved == used` and its `unused` plot stays at zero.

## 3. Vulkan context in Headless mode

Vulkan 1.4 instance with merged `{name, required}` requests, debug messenger chained into instance
creation, compute-first device selection that logs every candidate with its score or its rejection
reason, UUID forcing, and a `Queue` wrapper with a command pool, a timeline semaphore and a Tracy
GPU context.

Required features: `synchronization2`, `maintenance4`, `timelineSemaphore`, `bufferDeviceAddress`,
`scalarBlockLayout` and `shaderInt64`. The last two matter specifically to the kernel interface:
64-bit buffer references, and `LocalSizeId` so the workgroup size can come from specialization
constants.

VMA is the only path to `vkAllocateMemory`. The section pool keeps one large `VkBuffer` per domain
and block, sub-allocated with VMA virtual blocks in power-of-two size classes derived from the
mapping, section extent and halo; `R2` and `R3` values never share a block. Staging and readback are
persistently mapped FIFO rings. Peak bytes are tracked per category for the metadata sidecar.

## 4. First export

`terrain_export` evaluates a job section by section and writes 16-bit PNGs plus a JSON sidecar.

**Noise with analytic derivatives.** `assets/shaders/lib/noise.glsl` implements 2D and 3D simplex
noise whose value *and* exact gradient come out of one evaluation, by differentiating the kernel in
closed form. Fractal accumulation carries the gradient through the chain rule. `test_noise` compares
the analytic gradient against a central difference of the value channel over 4096 samples and finds a
worst-case disagreement of **0.03% of the RMS gradient**, which is the truncation error of the finite
difference rather than an error in the derivative.

**Two op kernels chained.** `ops/fbm.comp` writes the value on channel 0 and the gradient on
channel 1; `ops/normals.comp` turns that gradient into a unit surface normal with one
multiply-and-normalize. No finite differences, no halo, no neighbour reads. The two dispatches are
separated by a buffer memory barrier, which is the pattern the evaluator will generalise.

**Banded evaluation.** Sections are evaluated into a full-width band held in CPU RAM, and the band is
streamed into the PNG row by row, so a 16k map never sits in memory. Every section is dispatched over
the full section extent and only its useful interior is kept, so an edge section computes exactly
what an interior one does.

**Output.** 16-bit grayscale for `R2 -> R1` normalized from the declared range, 16-bit RGB for
`R2 -> R2` and `R2 -> R3` remapped from [-1, 1], raw `f32` for `R3`. Clamped samples are counted and
warned about, because clamping almost always means the declared range is wrong. The sidecar records
the script hash, bounds, timings, peak CPU and per-category VRAM, and the environment.

**Dataset runner.** `test_datasets` runs each case under `tests/datasets/`, compares the *decoded*
samples rather than the file bytes, writes a difference image on a mismatch, and compares timings
against `tests/baselines/<machine-id>.json`, recording one when there is none. History is appended to
`tests/history/<machine-id>.jsonl` with the commit hash. Performance comparison is skipped in Debug
builds. `--update-golden <case>` is the only way expected outputs change.

The first case, `basic_fbm`, is a 512x512 heightmap plus normals over 2x2 sections, and reproduces
**bit-exactly**.

### Not in this milestone

- `--tiles` is rejected with a clear error; one file per tile arrives with streaming (milestone 7).
- `graph_compilation` in the sidecar is 0 until there is a graph to compile (milestone 5).
- `gpu` in the sidecar is the total of the per-section timestamp pairs. The per-op breakdown needs the
  evaluator (milestone 5).
- Encoding dominates the wall time: a 2048x2048 export with normals takes about 5.7 s in Release, of
  which the GPU accounts for 3.2 ms and zlib for nearly all the rest.

## 5. Graph and compiler (in progress)

The graph, the compiler, the op registry and the evaluator are in, and `terrain_export` runs off the
compiled graph rather than a hardcoded pair of dispatches. `docs/todo.md` lists what is left before
the milestone can be called accepted.

**Graph.** Lazy, fixed-capacity node storage with typed handles, multi-output channels and a source
location on every node. Each builder validates its inputs' mappings on the spot, so a mismatch is
reported at the line that caused it rather than surfacing later:

```
terrain.lua:42: [validation] Normals expects R2->R2 input, got R2->R1
```

**Compiler.** Validation, constant folding, common-subexpression elimination by structural hash,
dead-node removal, classification, halo propagation, topological scheduling and buffer planning with
liveness-based reuse. Every stage is its own Tracy zone. Liveness is tracked per channel, not per
node, so a multi-output op whose value feeds an output while its gradient is consumed and finished
does not pin the gradient buffer for the whole section.

`test_compiler` covers it with 140 checks: the exact error text and line for every mapping mismatch,
broadcasting, folding counts, CSE merging identical noise nodes while leaving different ones alone,
dead nodes and unrequested channels dropped, and a seven-link chain of same-mapping curves planned
into **two** buffers.

**Ops.** Nine kernels on one uniform interface: `Const`, `Coords`, `Noise`, `Normals`, `Arith`,
`Curve`, `Blend`, `SlopeMask` and `Vector`. Variants come from byte-packed fields in a node's
`variant`, each mapping to one specialization constant, so a change of code shape costs a pipeline
and no shader compilation.

**fBm is generic over its base function.** `lib/fbm.lib.glsl` holds the algorithm once and names no
base function; `ops/fbm.comp.glsl` lists the instantiations. Ridged and billow are base functions in
their own right rather than a second axis, each carrying its own exact derivative. Parameters are the
point, the octave count, the amplitude, the persistence, the frequency and the lacunarity, with the
last three required to be strictly positive.

**Verification.** `test_noise` checks three independent things with 153 checks: that the analytic
derivative is the derivative (a central-difference sweep on the CPU, worst best-case error 0.02% to
0.09% across all six base-and-domain combinations), that the kernel matches the CPU reference sample
for sample (0 divergent samples and a worst error around 1e-6 for simplex near the origin), and that
one octave stays inside its declared band.

### What that verification found

- **3D simplex used the wrong kernel radius.** `r^2 = 0.6` is the classic constant and overshoots the
  critical radius, so the corner being swapped at a cell boundary still had a nonzero contribution
  and the noise was discontinuous. The fractal chain-rule check went from a 28% worst error to
  0.08% on `r^2 = 0.5`. The 3D normalization constant was recalibrated from 32 to 78 to match the
  smaller support, measured rather than guessed.
- **The pipeline map was being rebuilt per job.** It now lives with the device, as the spec intends.
- **Reordering a float multiply changed the output by one 16-bit unit.** The dataset golden caught it,
  which is exactly what it is for.
- **f32 coordinates lose precision far from the origin.** Quantified in `docs/todo.md`.
