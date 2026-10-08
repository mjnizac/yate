# Status

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
| 9 | Viewer (Graphics mode) | **done**, pending visual review |

## Verification

Both configurations build warning-free with MSVC 19.44 at `/W4`, and `ctest` passes 12 of 12 in each:

```
test_allocators ......... Passed
test_mapping ............ Passed
test_errors ............. Passed
test_compiler ........... Passed
test_lua_errors ......... Passed
test_compute_roundtrip .. Passed
test_noise .............. Passed
test_kernels ............ Passed
test_seams .............. Passed
test_erosion ............ Passed
test_viewer ............. Passed
test_datasets ........... Passed
```

Every GPU test also fails if the Vulkan debug messenger reported anything at error severity, so the
suite doubles as a validation run.

Everything GPU-side ran on a GeForce GTX 1070 reporting Vulkan 1.4.312, with a dedicated compute
queue family and calibrated timestamps available.

The pool invariants the Tracy memory view would have been read for are asserted by the suite instead:
`test_allocators` sweeps every named CPU pool and `test_compute_roundtrip` all seven VRAM categories for
their Tracy name, `reserved >= used`, `peak >= used` and a consistent allocation count. What is left for
a human is a one-off look to confirm the pools appear in the profiler under those names.

The 16k acceptance run is the one check the suite does not perform, because it takes two and a half
minutes and writes 1.1 GB. It is described under milestone 5 and the command is in `docs/commands.md`.

## 1. Skeleton

Build system with an `OBJECT` library linked as a DLL or a static library, generated export header,
`FetchContent` dependencies pinned to release tags, build-time `glslc` compilation, the assertion
macro family, asynchronous spdlog logging mirrored into Tracy, bit-flag exit codes, the platform
module, and `Application` with its layer stack.

`terrain_viewer` exists as of milestone 9 and opens a window, but it does not draw terrain yet.

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
passes buys about 5% of size for 4.2x the time. `up` is the default filter, overridable with `--png-level`
and `--png-filter`; the default level is 2, for the reason in the zlib-ng section below. The dataset goldens did not have to change, because the runner
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

**What a trace looks like now.** The script's own line numbers reach the GPU timeline, because the
source location a binding captured travels on the node, into the dispatch, into the Tracy zone name.
A 10-node script from `blended_r2` reads as `Noise:11`, `Noise:19`, `SlopeMask:28`, `Blend:30`,
`Curve:32`, `Const:37`, `Arith:37`, `Normals:40` on the GPU track, so a slow op points at the line
that asked for it. Running the script itself costs 1.14 ms.

### Not in this milestone

- `Erosion.Hydraulic` is milestone 8.
- Hot reload is viewer work (milestone 9). Reloading is already just another `RunScript` call, which
  creates and destroys its own state.
- `Terrain.Normals` has no `R2->R1` overload: it would need a finite-difference gradient op.

## 7. Sections and streaming

**Seams and determinism, pinned.** `test_seams` is the acceptance test, and it compares f32 samples
straight out of the section buffers rather than encoded PNGs, because 16-bit quantization would hide
exactly the one-LSB differences it exists to find. Every case is bit-identical: `R2` noise as one
128x128 section against 4x4 sections of 32, a region of 100x70 that is not a whole number of sections,
blur radii 1, 3 and 8, a three-dispatch graph with a slope mask and a blend, `R3` noise and blur as one
32³ brick against 4x4x4 bricks of 8, and the same graph evaluated twice.

**The 16k acceptance run.** A 16384x16384 export of `basic.lua` with normals, run three times: twice
with 512-sample sections and once with 1024. All three `height.png` and all three `normals.png` come out
with the same SHA-256, so 268 million samples agree across both the repeat and the section layout. Only
`metadata.json` differs, in its timings, its output paths and `section_size`. It takes about 50 s per run
at section 512 and writes 1.1 GB of PNG.

**Encoding moved off the main loop, because that is where the time is.** Measured first: on a
4096x4096 export the GPU accounts for 21 ms and the readback for 76 ms of 3846 ms, so overlapping
dispatches with the CPU could chase under 4%. Encoding was 96%. Each PNG output now has its own worker
thread and two bands: one is filled from the readback ring while the other is deflated, and the two
outputs of a typical job no longer wait on each other. The same export went from 3846 ms to 2417 ms,
with the dataset goldens still bit-exact.

The sidecar reports `encode_stall` alongside `encode_and_write`, which is the number that says whether
the pipeline is deep enough: 1974 ms of the 2417 ms are still the main loop waiting for a free band, so
the critical path is now one output's own deflate stream. Two bands is enough; going deeper would not
help, because a deflate stream is sequential and cannot be split.

**Sections in flight.** One on the GPU, two bands per output on the CPU. One is enough for the reason
measured above, and the engine now says so out loud: before allocating anything it compares the planned
VRAM per section against the device-local budget, logs how many would fit, and refuses a job that
cannot run with the numbers rather than failing inside VMA:

```
a 16384 section of this graph needs 7168 MiB of VRAM but only 6453 MiB of 6453 MiB are available;
use a smaller --section
```

**`--tiles`.** One file per section instead of one stitched image: `height_0_256.png` for a tile,
`height_0_16_0.raw` for a brick. A tile needs no band and no stitching, because its rows are already
contiguous in the readback buffer and it is a complete image, so it is opened, written and closed in
the loop. A tile of a region is byte-identical to the stitched export of that same region, which is how
the mode is checked. The cost is that encoding goes back on the main loop: one deflate stream per tile
cannot be fed from a worker that owns a single stream.

**The f32 range is now a stated number, not a worry.** The jitter an f32 world coordinate carries works
out to about `2e-7 * |world|` metres, independent of frequency, which is the 0.05 m that `test_noise`
measures at 5e5 m. The exporter warns when that passes a tenth of a sample:

```
the bounds reach 999256 m from the origin, where an f32 coordinate carries about 0.200 m of jitter
against a 1.000 m sample
```

That makes the limit visible where it matters and costs nothing. The periodic wrap that would push it
further is still in `docs/todo.md`, now with a concrete reason to leave it there: a 16k map at 1 m
resolution reaches 8192 m, where the jitter is 0.0016 m.

## 8. Iterative kernels

Thermal and hydraulic erosion, plus the machinery an iterative op needs and the one op in the engine
that is not analytic.

**One dispatch, many iterations.** A node declares an iteration count, and the evaluator runs its kernel
that many times over a ping-ponged pair of buffers with a barrier between each pair. The pair is one or
two buffers, never one per iteration: the last iteration always writes the node's own output, whichever
way the parity falls, so the rest of the graph is planned around a single buffer.

**The halo argument, which is the whole correctness story.** Material moves one cell per iteration, so
the influence radius *is* the iteration count. That becomes the halo, and the intermediate state is
computed over `halo + radius` rather than `halo`: a sample is wrong after one iteration if a neighbour it
needed lay outside the computed region, and that wrongness walks one cell inward per iteration, so after
`radius` iterations it has reached exactly the boundary of `halo` and no further. One less and the
outermost ring of the output would be subtly wrong — which is to say, there would be a seam.

`test_seams` holds that down for 1, 2, 3, 8 and 24 thermal iterations and 1, 2, 3 and 12 hydraulic ones,
in `R2` and `R3`, all bit-identical between one section and 4x4. It earned its keep immediately: it found
two real bugs in this milestone, one of which only showed in `R3`.

**Order-independent by construction.** Both schemes compute every transfer from one pair of cells alone,
so what one cell sends is exactly what the other receives and no sample depends on the order cells are
visited. The usual outflow formulation normalises a cell's outflow by its own total, which means a cell
can only know what it receives by recomputing a neighbour's total, which needs that neighbour's
neighbours: a two-cell radius and twice the halo per iteration. Avoiding that normalisation is why
hydraulic erosion costs one cell per iteration rather than two.

**Hydraulic erosion carries more than it produces.** It takes a height, carries height, water and
sediment through a three-component state the rest of the graph never sees, and hands back a height. The
first and last iterations therefore read and write different shapes from the ones in between, which the
kernel resolves from the iteration index and count the evaluator writes into reserved parameter words.

**`Terrain.Gradient`.** Erosion produces a height with no closed form to differentiate, so normals from
eroded terrain need a measured gradient. This is the only op in the engine that is not analytic, and it
is deliberately explicit in a script rather than inserted behind one: everything that *has* an analytic
derivative still carries one.

**Verification.** `test_erosion` checks the properties a plausible bug breaks rather than a golden image,
because an iterative op has no CPU reference worth maintaining:

- It changes the field. A wrong ping-pong parity would hand back the input untouched.
- It conserves material, in the only form a window of a larger computation can show. Material genuinely
  crosses the window border, so the total is not expected to hold; what must hold is that the drift is a
  *border* effect, which means it shrinks like 1/L as the window grows. Measured 0.0026 at 64 and 0.0011
  at 128, a ratio of 0.42 against the 0.5 the law predicts. A per-sample leak from a sign error or a
  double-counted gift would not shrink at all.
- It reduces slope, monotonically with iterations: 0.361 bare, 0.291 at 2, 0.184 at 16, 0.119 at 48. The
  same arithmetic with the give and receive terms swapped would sharpen the terrain and still pass the
  conservation check.
- A talus above every slope present changes nothing, bit for bit, which is what says the threshold is a
  threshold and not a scale factor.
- The builders refuse what is unstable or unrepresentable, and the compiler refuses a chain whose halo
  would not fit.

### What this milestone found

- **A registry indexed by ordinal, checked only for length.** `kOps` is indexed by the `OpKind` value and
  the only assertion was on its size, so inserting an op in the wrong place silently handed every later
  op a different kernel. It turned thermal erosion into a no-op, because it ran the hydraulic shader with
  thermal parameters and every rate landed on zero. There is now a `constexpr` check that each entry sits
  at its own ordinal.
- **`Mapping{}` is a valid mapping.** The compiler asked `IsValid(node.stateMapping)` to mean "did the op
  set one", and a default-constructed `Mapping` is a perfectly valid `R2 -> R1`, so an `R3` node was given
  `R2` state buffers. The sentinel is gone: every iterative builder states its state shape, and the
  compiler rejects one that does not. The seam test caught this in `R3` only.
- **One iteration is a special case.** An iterative op asked for exactly one iteration takes the plain
  single-dispatch path, where the evaluator's loop never runs and never wrote the iteration words.
  Hydraulic erosion read that as "neither first nor last" and wrote its three-component state into a
  one-component buffer. The words are filled by the compiler now, so the single-iteration path is correct
  by construction.
- **A graph is a stack hazard.** Fixed-capacity storage for 1024 nodes makes a `Graph` around 150 KiB,
  and the seam test crossed the 1 MiB stack the moment it grew its ninth one. Both erosion tests now
  build one graph per function, and `graph.hpp` asserts the size so a future increase to `kMaxNodes`
  has to be a decision about the stack as well as about memory. It only showed in Debug, where nothing
  is elided.
- **The regression gate used the median of two passes.** Interference only ever makes a pass slower, so
  the distribution has a hard floor and a long tail; a 25 ms case read 31 ms inside a full suite run and
  24 ms on its own, a 37% "regression" caused by the test before it. The baseline comparison now uses the
  fastest pass, which is the only number that reflects the code rather than the machine.

## 9. Viewer

`terrain_viewer` runs a script, keeps a ring of tiles around the camera resident on the GPU and draws
them as a displaced grid, with an orbit and fly camera, a parameter panel, an error panel and hot
reload.

```
terrain_viewer --script assets/scripts/basic.lua --resolution 2 --section 128 --ring 3
```

Left-drag turns, middle-drag pans, the wheel zooms, F switches between orbit and fly, WASDQE moves in
fly mode and shift goes faster. R reloads, which also happens on its own when the file changes, and T
toggles wireframe.

### Window, swapchain and the frame loop

**The ordering problem at startup.** A `VkSurfaceKHR` needs an instance. Choosing a physical device needs
a surface, because presentation support is part of what makes a device acceptable in Graphics mode. So
the caller cannot have a surface before calling `Context::Create`, and the context cannot pick a device
before the caller has one. `ContextCreateInfo` therefore takes an optional `createSurface` callback: the
context builds its instance, asks for the surface, and only then looks at devices. A plain function
pointer rather than a `std::function`, because the context is constructed before the allocators a
capturing callable would want.

**GLFW is asked, not told.** The instance extensions for presentation come from
`glfwGetRequiredInstanceExtensions` rather than from a `#if defined(_WIN32)` block, which both removes a
platform conditional from `vulkan/context.cpp` and means the right answer on a platform nobody tested.

**One depth buffer, two frames, N images.** The render pass has two external subpass dependencies, not
one: colour waits on the presentation engine for the image just acquired, and depth waits on the previous
frame's fragment tests, because there is a single depth buffer shared by every frame in flight. Without
the second one, frame N's depth clear races frame N-1's depth tests.

**Reversed depth.** Near is 1, far is 0, the pass clears depth to 0 and the pipeline compares with
`GREATER_OR_EQUAL`. A float depth buffer keeps most of its precision near zero, and reversing the range
puts that precision where the geometry is close, which is what stops the horizon z-fighting when terrain
stretches to the far plane.

### The renderer

**No mesh.** Each vertex derives its grid position from `gl_VertexIndex` and reads the height and the
normal through a buffer device address, exactly as the compute kernels pass buffers to each other. Six
vertices per quad rather than an index buffer, because an index buffer is a second allocation describing
what the vertex index already encodes. Nothing is uploaded and nothing is copied per frame.

**Resident tiles that follow the camera.** A ring of `(2r + 1)^2` tiles is kept around whatever the
camera is looking at. Each tile is evaluated once into its own pair of section-pool slots; the evaluator
reuses one set of buffers for every section, so the tile's copy is what makes it outlive the next tile's
dispatches, and a tile is a `samples x samples` field with no padding.

As the camera moves, tiles that left the ring are evicted and the ones that entered are evaluated
nearest-first, at most `--tiles-per-frame` of them. The bound is the point: a hole at the edge of the
ring for a few frames is a far better failure than a hitch every time the camera crosses a tile border.
A load and a reload ignore the budget, because showing a half-built ring right after a deliberate act
would look like a failure rather than like streaming.

The ring is centred on what the camera is *about* — the orbit target, or the eye in fly mode — not on
the eye. In orbit the eye can be kilometres from what is being examined, and centring on it would stream
in terrain behind the camera while the terrain being looked at fell out of the ring.

**One level for the whole ring, chosen by distance.** Sample spacing is `resolution * 2^level`, and the
level comes from how far away the ground is: the orbit radius in orbit mode, the height above the terrain
in fly mode, because that is the measure that makes climbing coarsen the terrain and descending refine
it. The ideal level has to disagree with the active one for fifteen frames before the ring switches,
since a switch evicts and re-evaluates everything and a camera hovering near a threshold must not
stutter.

Per-tile levels are the usual answer and they bring the usual problem: two levels meeting on screen
sample the terrain at different spacings, so their shared edge does not line up and the crack has to be
hidden with skirts. This engine's whole claim is that section borders are bit-identical, and shipping a
renderer that visibly cracks between levels would contradict it. With one global level nothing ever
meets a different level: the ring switches as a whole, which is a transition in time rather than a seam
in space.

A level change recompiles the graph, because the sample spacing is baked into the parameter block of
every op that needs a world position. That costs 0.05 ms and creates no pipeline, since the pipeline map
is keyed on op and variant. Tile origins are multiples of the tile size in that level's sample units, so
a level's lattice is a subset of the one below it and changing level does not shift the terrain sideways.

**Grid density follows distance too, separately.** Inside one level, a tile more than four tile-widths
away drops to half the drawn grid, past eight to a quarter, past sixteen to an eighth. That is a
rendering decision on top of the evaluation one: the data is the same, only the number of triangles
changes.

**Frustum culling** from the six planes of the view-projection matrix against each tile's box, where the
height range comes from what the script declared rather than from the data: that can only make the box
too large, which is the only direction that cannot cull something visible.

**Wireframe** on `T`, which is the only way to see what either distance rule actually chose. It needs
`fillModeNonSolid`, the one *optional* device feature the engine asks for: a device without it keeps
working and loses the mode.

### Hot reload

The script's modification time is polled four times a second with a 150 ms debounce, because an editor
writes a file in several steps and reloading on the first one reads a half-written script.

The CPU half of a reload — running the script and resolving its outputs — happens on a worker thread.
The GPU half stays on the main thread, because the queue and the section pool are not synchronized for
concurrent use and making them so would be a large change to serve one feature. The previous terrain
keeps rendering until the new one has been both compiled *and* prepared, so a broken edit leaves the
terrain on screen and puts the error in the panel:

```
terrain.lua:12: [script] Noises.fBm does not take 'persistance'; it accepts domain, kind, frequency, ...
```

**A script written for the exporter works unchanged.** If it produces only a height, the viewer appends
the two nodes it needs — a measured gradient and the normals from it — rather than refusing to show it.

### The UI, and why Dear ImGui

The spec asks for an auto-generated parameter UI and an on-screen error panel without naming a library,
so the choice was open. The alternative considered was drawing the panels with the engine's own pipeline
and an embedded bitmap font, adding no dependency. It was rejected on the work it buys nothing for: a
legible font is 95 hand-authored glyphs, and sliders and text entry would all be written from scratch,
while ImGui ships its own atlas and a Vulkan backend that manages its descriptor sets internally — so it
needs no image upload or sampler support from the engine, which has neither.

It is confined to `src/render/ui.cpp` the way Sol2 is confined to `lua.cpp`, so no UI type reaches a
header, and its allocations are routed through `CPU/General` instead of becoming another untracked pool.

"Auto-generated" means exactly that: the engine does not know what a parameter means, only its name and
its text, so a numeric one gets a drag field whose step scales with its value and anything else gets a
text box. Editing one triggers a reload, because the script is what turns a parameter into terrain.

### What the tests check, and what they cannot

`test_viewer` is 56 checks: the surface exists, the swapchain is at least double buffered with a valid
render pass, frames are acquired and presented, three forced resizes rebuild everything, the loop still
runs afterwards, a script loads into a complete ring, **something is actually drawn** after culling, a
changed script reloads, a script with no normals output still loads, a broken script leaves the load
count where it was with a non-empty error, moving the camera twenty tiles re-evaluates the whole ring and
leaves nothing missing, and climbing to 20 km coarsens the level. On a machine with no display it returns
77 and ctest reports it as skipped.

The "something is actually drawn" check is the one that earns its place: a culling bug that rejected every
tile looks exactly like a successful empty frame, which is how that whole class of error hides.

**Still a human's job.** Nothing here says the picture is *right*. Whether the terrain looks like terrain,
whether the shading reads, whether the camera feels right, whether a level change is visible when it
happens — none of that is in reach of a test, and it is the part that needs looking at.

### The bugs this milestone found

- **`renderFinished` cannot be a per-frame semaphore.** It started out next to `imageAvailable` and the
  fence, which is wrong and only validation says so: the semaphore is signalled by the submit and waited
  on by the *present*, so it stays in use until the presentation engine is done with the image, which the
  frame fence knows nothing about. With three images and two frame slots, a slot comes round again while
  the present that used its semaphore is still pending. It is now one per swapchain image, because an
  image can only be reused after being acquired again. Release never showed it; validation is Debug-only
  here.
- **`Stop()` during `OnAttach` was discarded.** A layer's `OnAttach` runs inside `PushLayer`, before
  `Run`, and `Run` set `running` to true unconditionally. The viewer failing to load its script looked
  like a successful run of sixty empty frames. `Stop` now records the request when there is no loop to
  stop — and only then, because setting the flag unconditionally made the first `StopAfter` poison every
  later `Run`.
- **A requested output is not necessarily a tight buffer.** A value some other op reads with a radius
  carries that halo even when it is also an output, and in a script that takes the gradient of its own
  height the height buffer ends up with the whole erosion chain's halo: a 202x202 buffer where the tile
  wants 128x128. The copy into a tile now takes the interior row by row.
- **The one-shot command pool filled up in a viewer.** Per-frame GPU zone collection submits on the
  compute queue, and the viewer's frames go through the *graphics* queue, so nothing ever called
  `WaitTimeline` on the compute one and the pool was exhausted after sixteen frames. `BeginOneShot` now
  polls the timeline and recycles before giving up, which makes the pool self-healing for every caller
  rather than only for the ones that happen to wait.
- **An idle device does not release a command buffer's references.** A recorded command buffer keeps
  referencing the pipelines, buffers and descriptor sets it mentions until it is reset, and layers detach
  in reverse push order, so the viewer tore down its pipeline and the UI's resources while the window
  layer's last recording still named them. `Swapchain::ResetFrames` exists for exactly that moment.
- **`IMGUI_IMPL_VULKAN_NO_PROTOTYPES=0` still means "no prototypes".** The backend tests whether the
  symbol is defined, not what it is defined to.
- **The UI frame cannot be built in the layer update.** The window layer owns the frame and calls the
  recorder from inside its own update, which runs *before* the viewer's, so `NewFrame` landed after the
  `Render` that consumes it. The whole UI frame now lives in the recorder, which removes the ordering
  question instead of answering it.
- **A callback has one owner.** The scroll wheel arrives as a callback, and the UI backend installs its
  own that chains to whatever was there first. Installing the engine's afterwards replaced it and the
  panels stopped scrolling. `Input::Attach` is now called before the UI comes up, and says why.
- **`VK_POLYGON_MODE_LINE` needs a feature.** Creating the wireframe pipeline without `fillModeNonSolid`
  is a validation error rather than a clean failure, so the feature is requested as the engine's first
  optional one and the pipeline is only attempted when the device has it.

### Not carried out, deliberately

- **A second `WindowLayer` is refused.** The main window's surface is what chose the physical device, so
  a second window cannot make that choice and has to be checked against it. A terrain viewer with one
  viewport has nothing to gain, so the refusal is explicit and the message says why. The input module no
  longer assumes one window, which was the other obstacle.
- **Per-tile levels**, for the reason above: they would put a visible crack on screen in an engine whose
  one firm claim is that section borders are bit-identical.

## After milestone 9

Work that followed from the todo list rather than from a milestone.

### Channel names belong to the op

The Lua bindings used to know two channel names, `value` and `gradient`, written into the index
metamethod. That was fine while the only multi-channel op was noise and wrong the moment there was
another: an op whose second channel is water and sediment had no way to be named, and the bindings would
have cheerfully accepted `.gradient` on it. Names now come from the op registry, which is where the rest
of the engine already looks for what an op is, and the error lists what the op does offer rather than a
fixed pair.

### Hydraulic erosion exposes its water and sediment

`Erosion.Hydraulic` has a second channel, `flow`, carrying the water and the sediment as they stood after
the last iteration. The simulation already had both; it was discarding them. They are a wetness map and a
sediment map, which is what a renderer wants for deciding where rock gives way to silt.

It is the first second channel in the engine that is not a gradient, and the first on an *iterative* op,
so it is also the first thing to exercise the channel mask that far: the kernel writes channel 1 only
when a consumer asked for it, and only on the last iteration. `test_erosion` checks the mask is `0x3`
with both outputs requested and `0x1` with one, that the planner gives channel 1 no buffer in the second
case, and that the buffer count really is lower. The `hydraulic_flow_r2` dataset case decodes both files
and reproduces them bit-exactly.

### A silent limit in the error reporting

Every `SourceLocation` points into a table of interned chunk names, which is never emptied because a
compiled graph can outlive the state that produced it — the viewer keeps one rendering while it compiles
the next. The table held sixteen entries, and `test_lua_errors` loads twenty-seven scripts: every case
after the sixteenth reported its errors against `?` instead of its own file, and nothing noticed, because
no check looked at the file name.

The table is sixty-four entries now and warns once if it is ever exhausted, and the error tests check
that every message names its script. That is seventeen new checks whose only job is to catch this class
of thing.

### Streaming, measured

The todo asked for numbers before restructuring the viewer's evaluation. On the sample script at 128
samples and level 2:

| | |
| --- | --- |
| one tile, end to end | 954 microseconds |
| of which command recording | 326 microseconds |
| of which GPU work | about 200 microseconds |
| the rest: submit, wait, two buffer copies | about 430 microseconds |
| a full ring of 49 tiles | 47 milliseconds |

So at the default two tiles per frame, streaming costs about 1.9 ms while the camera crosses a tile
border, which is 11% of a 60 Hz budget and only while moving. Two thirds of a tile's cost is *not* GPU
work, which does suggest batching the frame's tiles into one submission would help — but it would save a
few hundred microseconds on a path that is already under a tenth of a frame, and it needs a barrier
between tiles that share the evaluator's buffers. Left alone, with the numbers written down.

A level change refills the whole ring, so it costs those 47 ms: three frames at 60 Hz, on a deliberate
zoom. The alternative is streaming the new level under the usual budget, which trades a three-frame hitch
for half a second of mostly-missing terrain. The hitch is the better failure.

### The halo of a reused slot

A chain of two ops that each carry a radius gave different numbers depending on the section size, and had
done since milestone 5. It was not synchronisation: a full `ALL_COMMANDS` barrier before every dispatch
changed nothing, and the numbers were identical run to run. It was the stride the output was *read* with.

`PlannedBuffer` carries a `mapping`, a `halo` and a `bytes`, and the planner fills them when it creates a
slot. It then reuses that slot for any later value whose size class fits, and does not touch those three
fields — only `sizeClass` is a property of the slot rather than of its first occupant. Reading an output
back with `buffers[output.buffer].halo` therefore strided it by whatever halo belonged to some unrelated
earlier value. Which slots get reused depends on the buffer sizes, which depend on the section size, so
the wrong stride appeared and disappeared with exactly the parameter a seam check varies: one 128-sample
section disagreed with 4x4 sections of 32 over 13800 of 16384 samples, deterministically.

A single op never hits it, because nothing has been released yet for it to inherit. That is why every
seam check passed: they were all one op on its own.

`CompiledOutput` now carries the halo of the dispatch that wrote it, which is unambiguous, and the
exporter, the viewer and the test helper take it from there. The three descriptive fields on
`PlannedBuffer` say in a comment that they describe the slot's first occupant and nothing else.
`CheckChainedOpSeams` covers eight chains — blur over blur at four radius pairs, and a thermal pass over a
blur, over a thermal pass and over hydraulic erosion, which is the one producer with a second channel.

A second, smaller bug came out of the same investigation: the write-after-read scan in `EmitBarriers`
looked at a dispatch's inputs and outputs but not its scratch buffers, so an iterative op's ping-pong
slots could be reused without a barrier. No check ever caught it producing a wrong number — the
iteration loop's own barriers cover the common case — but it was unguarded for the first dispatch to
inherit a released scratch slot, and the ordering a seam check depends on should not rest on that.

### zlib-ng, and why the default level moved to 2

Encoding is where an export spends its wall time, so the deflate implementation is worth more than the
level knob. zlib-ng 2.2.2 in `ZLIB_COMPAT` mode is a drop-in: the `basic_fbm` case went from 82 ms of
encoding at stock zlib's best setting to 37 ms at zlib-ng's, same pixels.

It does change the level choice, because zlib-ng's level 1 compresses visibly worse. Measured on
`basic.lua` at 2048 with both outputs, the whole export:

| Level | Total | Encoder stall | Bytes |
| --- | --- | --- | --- |
| 1 | 310 ms | 179 ms | 21433 KiB |
| **2** | **428 ms** | **277 ms** | **16741 KiB** |
| 4 | 682 ms | 449 ms | 16494 KiB |
| 6 | 1218 ms | 869 ms | 16187 KiB |

Level 2 is the knee and the default: it is the last level where the size still pays for the time, costing
38% more wall time than level 1 for 22% fewer bytes, where level 4 costs another 59% for 1.5%. A job that
wants throughput over size asks for `--png-level 1` and knows what it is buying.

The goldens did not move across any of this: the runner compares decoded samples, which is what makes a
compressor swap a non-event.

One wrinkle in the build: zlib-ng declares `zlibstatic` as an alias of `zlib`, and CMake refuses an alias
of an alias, so `engine_dependencies.cmake` resolves `ALIASED_TARGET` before creating `ZLIB::ZLIB`.

### The readback ring has to grow

At section 1024 the 16 MiB readback ring fitted a halo-free export to the byte, and erosion's halo pushed
it 628 KiB over, which failed the export outright. `RingBuffer::EnsureCapacity` reallocates the ring,
rounded up to a MiB, when nothing in it is live; the exporter reserves twice one section's outputs up
front, so a job either fails at startup or does not fail. The 1024 acceptance run above reports a 51 MiB
readback pool, which is the growth happening.

### Two decisions that stayed decisions

**One section in flight stays.** Milestone 7 chose not to overlap sections because the GPU was half a
percent of an export; milestone 8 made erosion expensive enough to ask again. Measured on `basic.lua`,
24 hydraulic iterations and 12 thermal:

| | Total | GPU | Readback | Encoder stall |
| --- | --- | --- | --- | --- |
| 2048, section 512 | 441 ms | 41 ms (9%) | 60 ms | 231 ms (52%) |
| 2048, section 1024 | 518 ms | 33 ms (6%) | 51 ms | 219 ms (42%) |
| 16384, section 512 | 50.0 s | 2.31 s (5%) | 3.32 s | 43.1 s (86%) |

The GPU share went from half a percent to single digits, which is still not where the time goes. The
exporter spends its wall time waiting for the PNG encoder, and overlapping sections would hide work that
is already hidden behind that wait. Revisit if an output format arrives that encodes as fast as the GPU
evaluates.

**No shared-memory thermal kernel.** The question was whether a four-tap stencil re-reading its
neighbours is wasting bandwidth. Measured at section 1024, taking the fastest of three runs:

| Iterations | GPU |
| --- | --- |
| 8 | 1.14 ms |
| 16 | 1.75 ms |
| 32 | 3.06 ms |

Both slopes agree: 0.077 and 0.082 ms per iteration, which against one read and one write of the padded
state is 116 GB/s, or 45% of the GTX 1070's 256 GB/s. That figure is what settles it — if each of the
four taps were reaching DRAM, the real traffic would be two and a half times the model and the card would
have to be running at 290 GB/s, which it cannot. So the redundant taps are already being served by cache,
and a shared-memory tiling would remove traffic that never leaves the chip while adding a barrier per
iteration. The remaining gap to peak is latency and occupancy, which tiling does not address.

### The viewer drew every tile and showed none of them

First time anyone opened the viewer on `basic.lua`, the window came up with the panels over an empty
background. The draws were all there — `tilesDrawn` was the whole ring — and the GPU was doing the work.

The projection is reversed: near is 1, far is 0, so a float depth buffer spends its precision where the
geometry is close, and the pipeline compares with `GREATER_OR_EQUAL`. `camera.hpp` says in as many words
that the pass clears depth to zero to match. The pass cleared it to 1.0. With that clear and that compare
the only fragment that can pass is one sitting exactly on the near plane, so every tile was drawn and
every fragment discarded. The panels survived because the UI pipeline has no depth test, which is what
made it look like a viewer that had not loaded anything rather than one that had.

Nothing in the suite could have caught it. `test_viewer` checks that the ring is evaluated, that the tiles
pass frustum culling and that the draws are recorded, and all of that was true. No check looks at a
pixel, because the swapchain images are created `COLOR_ATTACHMENT` only and there is no readback path to
look through. That is the gap, and it is in `docs/todo.md` rather than fixed here.

## The visual review of milestone 9

The viewer opened and the terrain in it was wrong in four separate ways, none of which any check in the
suite could see. They are listed here in the order they were found, because each one hid the next.

### A screenshot, first

Nothing could be judged without being able to look at a frame from a script, so `--screenshot <file>`
came first: the presented swapchain image is copied into a host buffer inside the frame's own command
buffer, after the render pass, and written as an 8-bit PNG. It needs `TRANSFER_SRC` on the swapchain
images, which is requested only when the surface offers it. With no `--frames` it defaults to 90, because
the first frames of a viewer are a half-streamed ring and a picture of those says nothing.

This is the tool the whole rest of this section depended on. Every check in `test_viewer` passed while the
screen was empty, because they all stop one step short of a pixel.

### Every 16-bit PNG the engine ever wrote was byte-reversed

The heightmaps looked like white noise at 8 m sampling and like interference rings at 1 m. The values
themselves were fine: the exporter reported no clamped samples, and `test_noise` measures the kernel
against a CPU reference to within 1e-6. It was the file.

PNG stores 16-bit samples big-endian and the writer swapped each one before handing the row to libspng.
libspng does that conversion itself, so the two cancelled and the file came out little-endian. Measured
rather than argued: a field whose values are all near zero, declared over a range of ±100000, must appear
in the file as `80 00`, and it appeared as `00 80`.

What that does to a picture is the giveaway in hindsight. The low byte lands in the high one, so a height
that changes slowly across the image produces a fast sawtooth — white noise where the field is rough,
contour rings where it is smooth.

The dataset runner could not catch it. It compares decoded samples, which is what makes it immune to a
compressor swap, but both sides of the comparison are PNGs the engine wrote, so a consistent error in
writing them is invisible. One corroboration came for free when the goldens were re-recorded:
`blended_r2/mask.png` was the only file that did not change, because a mask is almost all `0x0000` and
`0xFFFF`, and those two are the same byte-reversed.

### Hydraulic erosion had no setting that both worked and held together

With the sample script's rainfall it moved about a metre of bed across 400 m of relief — nothing. Turn the
rain up and every sample clamped: 65536 of 65536 at 0.5 m per iteration.

The capacity term was the cause. It read

```
steepest = max(steepest, abs(centre.height - neighbour.height))
```

over all four neighbours, uphill ones included, so a cell at the foot of a slope was given the carrying
capacity of the slope above it. It eroded although nothing descended there, which dug a pit, which
steepened the drop into the pit, which eroded harder. The capacity now comes from the drop the outflow
actually descends, and a cut is bounded by half the drop to the lowest neighbour so one iteration can
never take a cell below what surrounds it.

Measured over the same graph, 24 iterations at 8 m sampling:

| Rain per iteration | Before | After |
| --- | --- | --- |
| 0.02 m | 0 clamped, relief 391 m (no effect) | 0 clamped, relief 358 m |
| 0.2 m | 56041 clamped, relief 3081 m | 0 clamped, relief 350 m |
| 0.5 m | all 65536 clamped | 0 clamped, relief 338 m |
| 10 m | all 65536 clamped | 0 clamped, relief 341 m |

It saturates past about 0.5 m rather than diverging, which is the bound doing its job.

### A tile drew one interval less than it occupied

A dark line along every tile border, with the background showing through. Tile origins stepped by
`samples * spacing` while a tile of N samples spans N-1 intervals, so 8 m of every 1024 was never drawn.
Origins step by `samples - 1` now, which makes a tile's last sample its neighbour's first: the same world
position in both, and the evaluator is position-based, so the two surfaces meet exactly rather than
closely.

The per-tile grid density went at the same time, for a related reason. It reduced the drawn vertices of a
distant tile by a power of two, and two densities meeting along an edge is a T-junction — the coarse side
spans the edge with a chord between every nth sample while the fine side follows each one. Distance is
already handled, and handled once, by the level that chooses the sample spacing. Drawing every tile at
full density costs nothing measurable here: 0.72 ms a frame against 0.63.

### The sample script was asking for spikes

With the files finally readable, `basic.lua` still produced a field of spikes, and that one was not a bug.

A sine of amplitude A and wavelength L has a maximum slope of 2*pi*A/L, and with lacunarity 2 and
persistence 0.5 every octave of an fBm contributes the same slope, so the stack is about six times that.
At 400 m of amplitude over the 500 m wavelength the script asked for, that is a 62-degree mean slope
measured at 8 m sampling. The frequency is 0.0002 now, a 5 km wavelength, which measures 11 degrees.

The second half was the slope mask. `Masks.Slope` read the base's own analytic gradient, and
differentiating an fBm weights each octave by its frequency, so the gradient of a six-octave field is
dominated by the finest octave and the mask flickers between 0 and 1 from sample to sample. Blending two
surfaces hundreds of metres apart with a flickering weight is its own roughness generator:

| Octaves feeding the mask | Mean slope | Relief |
| --- | --- | --- |
| 6 | 38.0 deg | 528 m |
| 3 | 13.6 deg | 528 m |
| 2 | 12.1 deg | 528 m |

Three octaves, with the relief untouched. "Where is the terrain steep" is a question about its shape, not
about its roughness.

The script as it stands measures 12.1 degrees of mean slope over 624 m of relief across 8 km.

### What this says about the test suite

Every one of the four was invisible to twelve passing tests, and for the same reason in each case: the
suite checks the step before the one that was wrong. It verifies that the noise kernel matches a CPU
reference, and the file was written wrong. It verifies that tiles are evaluated, cleared the frustum and
were recorded as draws, and the fragments were discarded. It compares PNGs the engine wrote against PNGs
the engine wrote. The gap is listed in `docs/todo.md`.
