# Status

| # | Milestone | State |
| --- | --- | --- |
| 1 | Skeleton | **done**, accepted |
| 2 | Memory system | **done**, accepted |
| 3 | Vulkan context in Headless mode | **done**, accepted |
| 4 | First export | **done**, accepted |
| 5 | Graph and compiler, without Lua | **done**, pending acceptance |
| 6 | Lua bindings | **done**, accepted |
| 7 | Sections and streaming | not started |
| 8 | Iterative kernels | not started |
| 9 | Viewer (Graphics mode) | not started |

## Verification

Both configurations build warning-free with MSVC 19.44 at `/W4`, and `ctest` passes 9 of 9 in each:

```
test_allocators ......... Passed
test_mapping ............ Passed
test_errors ............. Passed
test_compiler ........... Passed
test_lua_errors ......... Passed
test_compute_roundtrip .. Passed
test_noise .............. Passed
test_kernels ............ Passed
test_datasets ........... Passed
```

Every GPU test also fails if the Vulkan debug messenger reported anything at error severity, so the
suite doubles as a validation run.

Everything GPU-side ran on a GeForce GTX 1070 reporting Vulkan 1.4.312, with a dedicated compute
queue family and calibrated timestamps available.

Still open: read the Tracy memory view and confirm every CPU and VRAM pool reports correct reserved
and used values. CPU zones, GPU zones and allocation events all reach the profiler; the memory graphs
are the part nobody has looked at.

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
- Encoding dominates the wall time; see the PNG encoder settings under milestone 5 for what that
  measurement led to.

## 5. Graph and compiler

The graph, the compiler, the op registry and the evaluator are in, and `terrain_export` runs off the
compiled graph rather than a hardcoded pair of dispatches.

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

**Ops.** Ten kernels on one uniform interface: `Const`, `Coords`, `Noise`, `Normals`, `Arith`,
`Curve`, `Blend`, `SlopeMask`, `Vector` and `Blur`. Variants come from byte-packed fields in a node's
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
- **f32 coordinates lose precision far from the origin, and the first two fixes were worse.** The
  derivative sweep now runs at about 5e5 metres as well as near the origin, which is what turned a
  suspicion into a number: 4.9% worst error for `R2` and 19.8% for `R3`, against 0.03% near the
  origin. Routing the cell offset through the fractional part of the skewed coordinate is
  algebraically identical and removes the rounding that causes it, and it measured five times worse;
  `docs/todo.md` records both attempts with their numbers and why the remaining option is deferred to
  milestone 7.
- **One halo per dispatch was not enough.** Halo propagation gives a producer a wider halo than its
  own consumer needs, so a kernel's inputs and outputs can have different row strides. The push
  constant now carries one halo byte per input slot. Keeping the block at its asserted 128 bytes meant
  merging `domain` and `channelMask` into one `flags` word, which costs nothing because both were
  already read through accessors.
- **`TracyVkCollect` was called on a command buffer that was never begun.** Validation said so and the
  GPU zones never reached the profiler at all. With the collection properly recorded and submitted the
  trace grew from 31 KB to 43 KB at an identical CPU zone count.

**Neighbourhood ops and halos.** `Blur` is a separable-radius box filter that reads its input with
halo-aware indexing, which is what exercises halo propagation end to end: `test_kernels` dispatches it
with an input halo wider than the output and `test_compiler` checks the propagated radii. A producer's
halo is the maximum over its consumers of consumer halo plus consumer radius.

**Dataset coverage.** Five cases, all bit-exact: `basic_fbm` (`R2->R1` plus normals), `gradient_r2`
(`R2->R2` written as 16-bit RGB), `ridged_r2` (a different base function with normalization off),
`blurred_r2` (a nonzero halo through the whole pipeline) and `volume_r3` (`R3->R1` as a raw `f32`
volume). The runner reads the output file names from `case.json` and dispatches on the extension, so a
case declares whatever set of outputs it produces; volumes are compared sample by sample in raw units
instead of 16-bit steps, since there is no useful 2D difference image for a 3D output.

**PNG encoder settings, chosen by measurement.** Encoding was 96% of the wall time of the
`basic_fbm` case, so the filter choice and the zlib level were measured across the grid rather than
left at libspng's defaults:

| Filter | Level | Total | Bytes |
| --- | --- | --- | --- |
| all | 6 (libspng default) | 341 ms | 1072 KiB |
| all | 1 | 124 ms | 1124 KiB |
| up | 3 | 153 ms | 1091 KiB |
| **up** | **1** | **82 ms** | **1124 KiB** |
| none | 1 | 72 ms | 1273 KiB |

Terrain data is high-entropy noise, so searching every filter per row and running the slow zlib
passes buys about 5% of size for 4.2x the time. `up` at level 1 is the default, overridable with
`--png-level` and `--png-filter`. The dataset goldens did not have to change, because the runner
compares decoded samples rather than file bytes: `basic_fbm` went from 382 ms to 66 ms with the same
pixels.

**`R3` end to end.** `terrain_export` builds an `R3` graph when the job asks for it, evaluates it brick
by brick in y, z, x order and streams each brick into `RawVolumeWriter` with `fseek`, so a volume is
never fully resident.

## 6. Lua bindings

Lua 5.4 through Sol2, all of it in `src/lua.cpp`. `terrain_export` no longer builds a graph from
`--param`: it runs the script, and what the script built decides the domain, which decides the grid,
the section shape and the output formats. `docs/lua_api.md` is the reference.

**Lazy, as specified.** A binding reads its named arguments, validates them, appends one node and
returns a handle. A handle is one `Value`: a node index, a channel and a mapping, with no back
pointer, because the bindings reach the graph by capture. `.value` and `.gradient` select a channel,
`.x` to `.w` extract a component, and the four arithmetic operators plus unary minus work with a
number on either side.

**Named arguments that reject what they were not given.** `Noises.fBm{ persistance = 0.3 }` is an
error naming the key and listing the ones the op accepts. A silently ignored key is the one failure
mode of a named-parameter API that produces a plausible wrong result instead of a message, so every
key read is recorded and anything left over is reported.

**Errors land on the script line.** Every binding reads the caller's line out of the Lua stack before
it touches the graph. Two stages show up, both with the line: `[script]` when a binding rejects its
own arguments, `[validation]` when a graph builder rejects a mapping, which is the stage the spec's own
example shows.

**Nothing throws across the C boundary.** A failing binding throws a C++ exception that Sol2's wrapper
catches one frame later; the structured `Error` travels in a thread-local slot, because Lua's error
channel carries only a string. Every call into Lua is protected, and the panic handler logs and aborts
instead of throwing, since a panic returns into C.

**Memory and sandbox.** Every byte the interpreter touches comes from the `CPU/Lua` allocator, so the
script's cost is its own pool in the Tracy memory view, and `test_lua_errors` asserts the pool is empty
after the last script. Only `base`, `math`, `string` and `table` are opened: no `io`, no `os`, no
`package`, no `require`.

**Verification.** `test_lua_errors` is the acceptance test, 126 checks over 27 bad scripts and one good
one. Each case pins the stage, the line and the words the message has to contain. And the stronger
check: all five dataset cases reproduce **bit-exactly** through the Lua runtime, which is what says the
bindings build the same graph the parameter path did, node for node.

### Not in this milestone

- `Erosion.Hydraulic` is milestone 8.
- Hot reload is viewer work (milestone 9). Reloading is already just another `RunScript` call, which
  creates and destroys its own state.
- `Terrain.Normals` has no `R2->R1` overload: it would need a finite-difference gradient op.
