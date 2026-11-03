# Design: standing decisions

The conclusions an engineer must know before changing the engine, distilled from `docs/archive/decisions.md`
and `docs/archive/lands_plan.md`.
Where a later archive section reversed an earlier one, only the final rule is stated. Numbers are the ones
that justified the rule on a GTX 1070 / i7-4790.
Milestone state is in `docs/status.md`, open work in `docs/todo.md`, and the project rules (allocators,
`IS_ENGINE`, push constant, `ENGINE_ASSERT`) in `CLAUDE.md`.

## Memory and Vulkan context

- **Project memory and layout rules live in `CLAUDE.md`.** Allocator routing, Tracy pool names,
  `vulkan::Context` rebinding, VMA callbacks and the 128-byte push constant are documented there and are not
  repeated. (archive: "## 2. Memory system")
- **`GeneralAllocator` reports `reserved == used`.** mimalloc does not expose segment reservations per
  allocator instance, so its `unused` plot stays at zero. That is a known limit, not a bug. (archive: "## 2.
  Memory system")
- **Required device features are fixed.** `synchronization2`, `maintenance4`, `timelineSemaphore`,
  `bufferDeviceAddress`, `scalarBlockLayout` and `shaderInt64`; the last two exist for 64-bit buffer
  references and for `LocalSizeId` driven by specialization constants. `fillModeNonSolid` is the only optional
  feature and its absence only loses the wireframe. (archive: "## 3. Vulkan context in Headless mode")
- **VMA is the only path to `vkAllocateMemory`.** The section pool keeps one large buffer per domain and
  block, sub-allocated in power-of-two size classes; `R2` and `R3` values never share a block. (archive: "##
  3. Vulkan context in Headless mode")
- **A surface must exist before device selection.** Presentation support is part of device scoring, so
  `ContextCreateInfo` takes a plain-function `createSurface` callback that the context calls after creating
  its instance. (archive: "### Window, swapchain and the frame loop")
- **`renderFinished` is one semaphore per swapchain image, not per frame.** The present waits on it, so a
  per-frame one is reused while a present is still pending; only Debug validation shows it. (archive: "### The
  bugs this milestone found")
- **A recorded command buffer keeps its references until reset.** Layers detach in reverse push order, so
  teardown must call `Swapchain::ResetFrames` before destroying pipelines or UI resources the last recording
  names. An idle device does not release them. (archive: "### The bugs this milestone found")
- **Per-frame GPU zone collection and the one-shot pool.** Collection submits on the compute queue while the
  viewer draws on the graphics queue, so nobody waited on the compute timeline and the pool ran out after 16
  frames. `BeginOneShot` now polls the timeline and recycles before giving up. (archive: "### The bugs this
  milestone found")
- **`Stop()` before `Run` must be remembered, only then.** Recording it unconditionally poisons later runs;
  discarding it made a viewer that failed in `OnAttach` look like sixty empty frames. (archive: "### The bugs
  this milestone found")
- **A callback has one owner.** The UI backend chains the scroll callback to whatever was installed first, so
  `Input::Attach` must run before the UI comes up. (archive: "### The bugs this milestone found")
- **A second `WindowLayer` is refused.** The main surface chose the physical device, so a second window cannot
  make that choice; one viewport gains nothing from it. (archive: "### Not carried out, deliberately")
- **The readback ring grows.** `RingBuffer::EnsureCapacity` reallocates (rounded up to a MiB) when nothing in
  it is live, and the exporter reserves twice one section's outputs at start, so a job fails at startup or not
  at all. At section 1024 erosion's halo overran the 16 MiB ring by 628 KiB. (archive: "### The readback ring
  has to grow")
- **The exporter checks VRAM before allocating.** It compares the planned bytes per section with the
  device-local budget and refuses a job that cannot run, telling the user to use a smaller `--section` (a
  16384 section needed 7168 MiB of 6453 MiB). (archive: "## 7. Sections and streaming")
- **A `Graph` is 150 KiB; build one per function in tests.** With 1024 nodes the seam test overflowed the 1
  MiB stack at its ninth graph, in Debug only. `graph.hpp` asserts the size so raising `kMaxNodes` is a stack
  decision too. (archive: "### What this milestone found")
- **The Lua source-location table is never emptied.** A compiled graph can outlive the state that produced it.
  It held 16 entries, so scripts past the sixteenth reported errors against `?`; it is 64 now and warns once
  if exhausted. (archive: "### A silent limit in the error reporting")

## Export, sections and determinism

- **Sections are bit-identical by construction.** Every section is dispatched over its full extent and only
  the useful interior is kept; `test_seams` compares f32 samples straight from section buffers because 16-bit
  quantisation would hide one-LSB differences. (archive: "## 7. Sections and streaming")
- **The 16k acceptance run is the end-to-end proof.** A 16384x16384 export of `basic.lua` at sections 512 and
  1024 gives the same SHA-256 for `height.png` and `normals.png` (268 million samples); only `metadata.json`
  differs. (archive: "## 7. Sections and streaming")
- **Wall time is the PNG encoder, so sections are evaluated one at a time.** On 4096 squared the GPU took 21
  ms and readback 76 ms of 3846 ms; with erosion the GPU is still 5 to 9 % of the total at 16384 squared (2.31
  s of 50.0 s, encoder stall 43.1 s). Overlapping sections would hide work already hidden behind the wait.
  Revisit only if an output format encodes as fast as the GPU evaluates. (archive: "### Two decisions that
  stayed decisions")
- **Each PNG output has its own worker and two bands.** One band fills from the readback ring while the other
  is deflated; going deeper does not help because a deflate stream is sequential. (archive: "## 7. Sections
  and streaming")
- **The PNG is encoded in parallel chunks of 32 rows.** Chunks counted from the top of the image are deflated
  as raw deflate with a sync flush by up to four threads, written as one IDAT each, with the zlib header first
  and the Adler-32 combined at the end (pigz construction). 4096 squared went from 2.91 to 2.97 s to 0.94 to
  0.96 s; a different section size does not change a byte. libdeflate was rejected because it has no streaming
  interface. (archive: "## The PNG encoder, measured and then parallelised")
- **zlib-ng in `ZLIB_COMPAT` mode, default level 2, default filter `up`.** Level 2 is the knee: 428 ms and
  16741 KiB against 310 ms and 21433 KiB at level 1 and 682 ms and 16494 KiB at level 4 (2048 export). Filter
  `all` costs 4.2x the time for about 5 % of size. Goldens compare decoded samples, so a compressor swap never
  moves them. (archive: "### zlib-ng, and why the default level moved to 2")
- **16-bit PNG samples are big-endian and libspng does the swap.** Swapping in the writer cancelled it and
  every PNG was byte-reversed. `test_output_files` now checks a field near 0 over a +-100000 range is the
  bytes `80 00`. (archive: "### Every 16-bit PNG the engine ever wrote was byte-reversed")
- **Output encoding.** `R2 -> R1` is 16-bit grayscale from the declared range, `R2 -> R2` and `R2 -> R3` are
  16-bit RGB remapped from [-1, 1], `R3` is raw `f32`. Clamped samples are counted and warned about because
  they usually mean a wrong declared range. The sidecar also holds min, max, mean and the non-finite count per
  output (about 40 ms on 4096 squared), because a 16-bit PNG turns NaN into 0 or 65535. (archive: "## 4. First
  export")
- **`--tiles` writes one file per section.** A tile of a region is byte-identical to the stitched export of
  that region; encoding goes back on the main loop. (archive: "## 7. Sections and streaming")
- **`--raw`: write in 4 MiB pieces and seek with 64 bits.** One large `fwrite` is pathologically slow with
  this CRT (24 MiB appends ran at 25 to 80 MiB/s, 4 MiB pieces at 1.7 GiB/s), and `fseek` through a 32-bit
  `long` overwrote rows past 2 GiB. (archive: "### `--raw`, and a CRT pathology worth remembering")
- **f32 coordinates jitter by about `2e-7 * |world|` metres**, independent of frequency (0.05 m at 5e5 m,
  matching `test_noise`). The exporter warns when that passes a tenth of a sample. Routing the cell offset
  through the fractional part measured five times worse; the periodic wrap stays in `docs/todo.md`. (archive:
  "## 7. Sections and streaming")
- **Provenance is hashed on what actually ran.** The sidecar holds SHA-256 of the script file, of each
  compiled shader preceded by its name, and of the sorted resolved parameters. `HashFile` must fail loudly: it
  once returned sixteen zeros for an unreadable file. (archive: "### Provenance")
- **Command-line numbers must consume the whole token and be finite**, and a job has a sample budget (`--max
  1e9,1e9` once started a 1953125 x 1953125-section export). Exit codes are assembled in `main`, so only a
  black-box test (`test_cli`) can see `terrain_viewer` exiting 0 on a missing script. (archive: "###
  Command-line numbers")
- **The regression gate uses the fastest pass.** Interference only slows a pass, so the median of two read a
  25 ms case as 31 ms (a false 37 % regression); the fastest pass is the number that reflects the code.
  Dataset runs compare decoded samples, record history per machine id and skip timing in Debug. (archive: "###
  What this milestone found")
- **Hash-checked datasets cannot catch consistent write errors.** Both sides of the golden comparison are
  files the engine wrote, which is how the byte reversal hid. Hand-computed values and independent decoders
  are needed. (archive: "### Every 16-bit PNG the engine ever wrote was byte-reversed")

## Graph and compiler

- **Builders validate on the spot, with the script line.** A mapping mismatch is reported as `terrain.lua:42:
  [validation] Normals expects R2->R2 input, got R2->R1`, never later. (archive: "## 5. Graph and compiler")
- **Compiler stages.** Validation, constant folding, CSE by structural hash, dead-node removal,
  classification, halo propagation, topological scheduling and buffer planning with liveness-based reuse.
  Liveness is tracked per channel, not per node, so a finished gradient does not pin its buffer. (archive: "##
  5. Graph and compiler")
- **A producer's halo is the maximum over its consumers of consumer halo plus consumer radius.** An iterative
  op's radius is its iteration count; its intermediate state is computed over `halo + radius` or the outer
  ring would be wrong, which is a seam. (archive: "## 8. Iterative kernels")
- **Variants are byte-packed specialization constants.** A change of code shape costs a pipeline and no shader
  compilation; the pipeline map is keyed on op and variant and lives with the device, not per job. (archive:
  "## 5. Graph and compiler")
- **The push constant carries one halo byte per input slot.** A producer's halo can exceed its own consumer's,
  so inputs and outputs of one kernel can have different row strides. `domain` and `channelMask` were merged
  into one `flags` word to stay at 128 bytes. (archive: "### What that verification found")
- **A reused buffer slot does not describe its current value.** `PlannedBuffer.mapping`, `halo` and `bytes`
  describe the slot's first occupant only. Read an output with the halo of the dispatch that wrote it
  (`CompiledOutput` carries it). Reading the slot's halo made a chain of two radius ops depend on the section
  size (13800 of 16384 samples differed between one 128 section and 4x4 of 32). (archive: "### The halo of a
  reused slot")
- **Pointwise kernels index inputs through `InputIndex(slot, local)` and write through `SampleIndex`.** A
  value feeding both a pointwise op and a wider-halo consumer is stored wider; indexing it with the kernel's
  own layout corrupted 99.99 % of samples. `arith`, `curve`, `blend`, `slope_mask`, `normals` and `vector`
  were affected and `test_seams` now has a case per kernel. (archive: "## Pointwise kernels read their inputs
  through the input's halo")
- **An iterative op reads its producer with the producer's own halo in the first iteration.** Setting every
  iteration's input halo to the state halo is only right when the op is the producer's widest consumer;
  otherwise `Hydraulic(n)` beside `Gradient(Thermal(n))` differed between sections 128 and 512 (holes and
  arches in walls). (archive: "## Cliffs after a World Machine escarpment")
- **Barrier scans must include scratch buffers.** `EmitBarriers` once checked inputs and outputs but not an
  iterative op's ping-pong slots; the loop's own barriers hid it. (archive: "### The halo of a reused slot")
- **The op registry is indexed by ordinal and must be checked per entry.** A length-only assertion let a
  misplaced op hand every later op a different kernel (thermal erosion became a no-op). A `constexpr` check
  now verifies each entry sits at its ordinal. (archive: "### What this milestone found")
- **No sentinel mappings.** `Mapping{}` is a valid `R2 -> R1`, so "did the op set a state mapping" cannot be
  asked of it. Every iterative builder states its state shape and the compiler rejects one that does not.
  (archive: "### What this milestone found")
- **Single-iteration ops take the plain dispatch path**, where the evaluator never writes the iteration words.
  The compiler fills them, so the single path is correct by construction. (archive: "### What this milestone
  found")
- **Channel names belong to the op registry.** The bindings no longer know `value` and `gradient`; the error
  for a missing channel lists the ones the op has (`flow` for hydraulic erosion). A channel is written only
  when a consumer asked for it, and an iterative op writes its second channel only on the last iteration.
  (archive: "### Channel names belong to the op")
- **fBm is generic over its base function.** `lib/fbm.lib.glsl` holds the algorithm once, `ops/fbm.comp.glsl`
  lists the instantiations, and ridged and billow are base functions with their own exact derivatives.
  (archive: "## 5. Graph and compiler")

## Lua bindings and prelude

- **Bindings are lazy and Sol2 stays inside `src/lua.cpp`.** A handle is one `Value` (node index, channel,
  mapping) with no back pointer; the script decides the domain, the grid, the section shape and the output
  formats. (archive: "## 6. Lua bindings")
- **Named arguments reject what they were not given.** Every key read is recorded and leftovers are errors
  that list the accepted keys, because a silently ignored key (`persistance`) yields a plausible wrong
  terrain. (archive: "## 6. Lua bindings")
- **Errors land on the script line.** Two stages exist, `[script]` for binding arguments and `[validation]`
  for graph mapping. A failing binding throws a C++ exception caught by Sol2 one frame later, and the
  structured `Error` travels in a thread-local slot; nothing throws across the C boundary and the panic
  handler aborts. A VM error such as calling a nil value reads its line back out of the message (it used to
  report line 0). (archive: "## 6. Lua bindings")
- **Sandbox: only `base`, `math`, `string` and `table`; `dofile`, `loadfile`, `load`, `collectgarbage`,
  `pcall` and `xpcall` removed.** `load` would assemble code the script hash does not describe. `pcall` goes
  because Lua has no uncatchable error, so the 10 s wall-clock budget raised from a count hook could be
  swallowed in a loop (it ran 40 s until killed). (archive: "### The Lua sandbox, and an error that was not
  catchable")
- **All interpreter memory comes from the `CPU/Lua` pool**, and `test_lua_errors` asserts it is empty after
  the last script. (archive: "## 6. Lua bindings")
- **The prelude is Lua, not C++.** The engine loads its own files without exposing `load` to scripts, and the
  prelude's hash goes into the sidecar. `CallerLocation` skips frames whose chunk is `prelude/...` so errors
  name the script line, and `fail` is passed in as an argument so a script cannot call it. A landform is five
  readable lines a script author can copy. (archive: "## Shapes, and what the audit changed")
- **`Terrain.Gradient` is the only non-analytic op and is explicit in the script.** Eroded height has no
  closed form, so normals from it need a measured gradient; everything with an analytic derivative keeps
  carrying one. (archive: "## 8. Iterative kernels")
- **Erosion iteration counts in scripts are written for a 2 m reference** (see Kernels and erosion); the
  bindings convert them, in C++, because the count is also the halo and the log line belongs to the engine.
  (archive: "## Erosion scaled to the sample spacing")
- **Slope parameters are rise over run.** `Erosion.Thermal`'s `talus` and the hydraulic capacity term were per
  cell, so one script was a different terrain at every spacing; at 8 m and 16 m a thermal-only graph went from
  differing by 8.0 m on average to 0.02 m. `Terrain.Coords` returns metres. (archive: "### Slope per metre,
  `Coords` in metres")
- **Almost nothing needed a new op for shapes.** Union and intersection are `Combine.Min` and `Max`,
  subtraction is `Max` with a negation, a mask is `Smoothstep`. The genuinely missing pieces were `Transform`,
  `Shape`, six `CurveOp` and two `ArithOp` variants; profiles live in `Curve`, not in a second `Curves`.
  (archive: "## Shapes, and what the audit changed")
- **`Filters.Expand` and `Filters.Shrink` are modes of Blur** (largest or smallest value in the window), not
  new ops. (archive: "## Cliffs after a World Machine escarpment")
- **`Masks.Slope` must read a smooth field.** The analytic gradient of an fBm is dominated by its finest
  octave, so the mask flickered from sample to sample; feeding it 3 octaves instead of 6 dropped mean slope
  from 38.0 to 13.6 degrees with relief untouched at 528 m. (archive: "### The sample script was asking for
  spikes")
- **A script's slope is a budget.** A sine of amplitude A and wavelength L peaks at slope `2*pi*A/L`, and with
  lacunarity 2 and persistence 0.5 each octave adds the same slope, so the stack is about six times that.
  `basic.lua` at frequency 0.0002 (5 km wavelength) measures 11 degrees; at the old 500 m wavelength it was
  62. (archive: "### The sample script was asking for spikes")
- **ImGui is confined to `src/render/ui.cpp`** and its allocations go through `CPU/General`. The parameter
  panel knows only a parameter's name and text: numbers get a drag field scaled to the value, anything else a
  text box, and editing triggers a reload. The `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` macro tests whether it is
  defined, not what it is defined to. (archive: "### The UI, and why Dear ImGui")

## Kernels and erosion

- **Noise carries an analytic gradient from the same evaluation.** Fractal accumulation applies the chain
  rule. `test_noise` finds the analytic derivative within 0.02 % to 0.09 % of a central difference across all
  six base-and-domain combinations, and the kernel within about 1e-6 of the CPU reference. (archive: "## 5.
  Graph and compiler")
- **3D simplex uses kernel radius `r^2 = 0.5`, normalisation 78.** The classic 0.6 overshoots the critical
  radius and makes the noise discontinuous (28 % worst chain-rule error against 0.08 %). (archive: "### What
  that verification found")
- **Float multiplication order is part of the output.** Reordering one multiply changed a result by one 16-bit
  unit; the dataset golden caught it. (archive: "### What that verification found")
- **Iteration moves material one cell, so iteration count equals halo.** Thermal and hydraulic schemes compute
  each transfer from a single pair of cells, so what one cell sends is exactly what its neighbour receives and
  no sample depends on visit order. Avoiding the usual per-cell outflow normalisation keeps the cost at one
  cell per iteration instead of two. (archive: "## 8. Iterative kernels")
- **Ping-pong uses one or two buffers, never one per iteration.** The last iteration always writes the node's
  own output whichever way the parity falls. (archive: "## 8. Iterative kernels")
- **Hydraulic capacity comes from the drop the outflow actually descends**, not the steepest of all four
  neighbours, and a cut is bounded by half the drop to the lowest neighbour. Before, a foot-of-slope cell
  inherited the slope above's capacity and dug a pit: at 0.5 m rain per iteration all 65536 samples clamped;
  after, 0 clamped and relief 338 m. It saturates past about 0.5 m instead of diverging. (archive: "###
  Hydraulic erosion had no setting that both worked and held together")
- **Erosion tests check properties, not goldens.** There is no CPU reference worth maintaining, so
  `test_erosion` checks: the field changes, material is conserved as a border effect (drift 0.0026 at 64 and
  0.0011 at 128, ratio 0.42 against the 0.5 predicted), slope falls monotonically (0.361 bare, 0.291 at 2,
  0.184 at 16, 0.119 at 48 iterations), and a talus above every slope changes nothing bit for bit. (archive:
  "## 8. Iterative kernels")
- **Resolution independence: iterations scale as `(ref / s)^2`, hydraulic rates scale by `s / ref`, reference
  2 m.** Thermal erosion is explicit diffusion, so the exponent 2 is the continuum limit (mean difference
  between 4, 8 and 16 m lattices 0.05 / 0.22 m against 0.79 / 1.55 m unscaled). Hydraulic erosion also
  accumulates rain and evaporation per iteration, so scaling iterations alone made it worse (10.6 / 8.5 m);
  with rates scaled it is 3.8 / 4.1 / 5.1 m against 6.7 / 10.8 / 16.0 m. It remains an approximation.
  (archive: "## Erosion scaled to the sample spacing")
- **Iterations written = `written / (spacing / 2)^2`, minimum 1, capped at 96 below 2 m** (or at the written
  count if larger, because a chain of erosions adds halo). The effective count is logged. A coarser reference
  would make every export look less eroded than the preview; a finer one makes the preview show almost
  nothing. At 1 m `basic.lua` runs 96 and 48 iterations instead of 24 and 12 (GPU 450 ms against about 170 on
  a 2 km export). Coarse levels are spikier because nothing relaxes the finest octaves. (archive: "## Erosion
  scaled to the sample spacing")
- **No diagonal neighbours for hydraulic erosion.** Eight neighbours with 1/sqrt(2) weight and `flow_rate`
  bound 0.146 gave more roughness (mean |second difference| 5.3 along axes before; 6.4 along axes and 7.8
  along diagonals after) and flipped the anisotropy (1.36 to 0.83). The one-sample ripple is cleared with
  `Filters.Blur{ radius = 1 }`. (archive: "### Tried and rejected")
- **No per-edge talus jitter.** The terracing it was meant to stop came from per-cell units, not from the
  scheme. (archive: "### Tried and rejected")
- **No shared-memory thermal kernel.** Per iteration 0.077 to 0.082 ms at section 1024, which is 116 GB/s, 45
  % of the card's 256 GB/s; four DRAM taps would need 290 GB/s, so the cache already serves the redundant
  reads and tiling would add a barrier per iteration for nothing. (archive: "### Two decisions that stayed
  decisions")
- **Thermal erosion cannot enforce a slope limit.** It limits the difference per axis (a diagonal reaches 1.4
  times the threshold) and moves one cell per iteration. Iterations 12 to 30 left the fraction of cells over
  35 degrees at 0.74 %, 0.76 %, 0.68 %. A limit has to be in the construction. (archive: "## The dune macro")
- **Gradient and Gullies.** `Erosion.Gullies` stripe density follows the length of the gradient it is given,
  so on a steep or noisy gradient stripes fall under a sample and alias to needles; give it the gradient of a
  smooth field. (archive: "## Cliffs after a World Machine escarpment")
- **`Erosion.StreamPower` is global and CPU-side.** The GPU erosion moved 0.5 % of the relief; the drainage
  network of a real range comes from a stream-power simulation with uplift over the whole terrain that no
  section with a halo can compute, carried to sections by `Terrain.Grid`. Uplift along crest lines, not a
  dome, keeps rivers from radiating from a centre. A cache of the simulation took zooming from 8 s of script
  per level change to 9 ms of GPU. (archive: "## The alpine macro, and what building it taught")
- **Sediment against a wall.** An erosion over a wall hundreds of metres high piles sediment against its face
  and a following `Max` keeps it as arches and spikes; cap the deposit a few tens of metres above the ground.
  (archive: "## Cliffs after a World Machine escarpment")

## Viewer

- **There is no mesh.** Each vertex derives its grid position from `gl_VertexIndex` and reads height and
  normal through a buffer device address, six vertices per quad and no index buffer. Nothing is uploaded or
  copied per frame. (archive: "### The renderer")
- **Reversed depth is set in the projection only.** Near maps to 1 and far to 0, depth clears to 0, the
  compare is `GREATER_OR_EQUAL`, and the viewport is the usual `0` to `1`. Setting the viewport to `minDepth =
  1, maxDepth = 0` as well reverses twice and keeps the farther fragment: ridges in front of lower terrain
  were painted over ("transparent mountains"). The open-sea quad is emitted at `z = 0`. (archive: "## Depth
  was reversed twice")
- **The depth pass clears to 0.** A clear of 1.0 against that compare discarded every fragment while
  `tilesDrawn` was the whole ring. (archive: "### The viewer drew every tile and showed none of them")
- **The pass has two external dependencies.** Colour waits on the presentation engine, depth waits on the
  previous frame's fragment tests, because one depth buffer is shared by every frame in flight. (archive: "###
  Window, swapchain and the frame loop")
- **Tiles follow what the camera is about**: the orbit target, or the eye in fly mode, not the eye. Tiles load
  nearest-first under `--tiles-per-frame`; a hole at the ring's edge is a better failure than a hitch at every
  border. A load or reload ignores the budget. (archive: "### The renderer")
- **Tile origins step by `samples - 1`.** A tile of N samples spans N-1 intervals, so stepping by `samples`
  left 8 m of every 1024 undrawn; with the step fixed a tile's last sample is its neighbour's first and the
  surfaces meet exactly. (archive: "### A tile drew one interval less than it occupied")
- **Every tile draws at full density.** Halving the drawn grid with distance made T-junctions where densities
  met; distance is handled once, by the level. Full density cost 0.72 ms a frame against 0.63. (archive: "###
  A tile drew one interval less than it occupied")
- **The ring uses one level, chosen by distance** (orbit radius, or height above terrain in fly mode), and
  switches only after the ideal level disagrees for fifteen frames. Per-tile levels were refused because two
  spacings meeting on screen crack, contradicting the claim that section borders are bit-identical. Tile
  origins are multiples of the tile size in that level's units, so changing level does not shift terrain
  sideways. (archive: "### The renderer") Note: `docs/architecture.md` ("What exists") also lists rings of
  detail beyond the main ring (`--max-level`), which this entry does not describe.
- **A level change recompiles the graph (0.05 ms) and refills the ring (47 ms for 49 tiles, three frames at 60
  Hz).** Streaming the new level under the usual budget would trade that hitch for half a second of mostly
  missing terrain. At the default two tiles per frame, streaming costs about 1.9 ms while crossing a tile
  border. (archive: "### Streaming, measured")
- **Batching tile submissions was left alone.** One tile costs 954 microseconds end to end, 326 of command
  recording and about 200 of GPU work, and batching would save a few hundred microseconds on a path under a
  tenth of a frame. (archive: "### Streaming, measured")
- **Culling uses the declared height range**, which can only make a box too large, the one direction that
  cannot cull something visible. A tile's draw copies the interior row by row, because a requested output is
  not necessarily a tight buffer (a 202x202 buffer where the tile wants 128x128). (archive: "### The
  renderer")
- **Hot reload polls the script's mtime four times a second with a 150 ms debounce.** The CPU half (running
  the script) runs on a worker; the GPU half stays on the main thread because the queue and section pool are
  not synchronised. The old terrain stays on screen until the new one is compiled and prepared, so a broken
  edit shows an error panel and keeps the terrain. (archive: "### Hot reload")
- **A height-only script works in the viewer unchanged.** The viewer appends a measured gradient and the
  normals from it. (archive: "### Hot reload")
- **`--screenshot` copies the presented swapchain image inside the frame's command buffer** (needs
  `TRANSFER_SRC`, requested only if offered) and defaults to 90 frames because the first frames are a
  half-streamed ring. It is the tool that found four invisible defects. (archive: "### A screenshot, first")
- **Rock texture is colour only.** A normal bumped by noise read as scales or pits at every scale tried;
  relief belongs to the heightfield. Colour thresholds from slope alone draw patches wherever a slope crosses
  them (pale scree on ordinary hillsides). At 16 to 32 m per sample nothing under about 60 m survives;
  `--detail` looks closer. Push constants are full, so the shadow travels in the length of the normal.
  (archive: "## The alpine macro, and what building it taught")
- **Start-up framing is a heuristic, not a fixed point.** "Frame the whole ring, pick the level, frame again"
  does not converge because `IdealLevel` wants a tile about half the viewing distance across, so each pass
  raises the level by about two. (archive: "### Not a bug: the viewer's start-up framing")
- **What a viewer test cannot see.** `test_viewer` must look at pixels, not only at ring and draw counts: it
  captures a frame and checks it holds terrain and changes with the camera. Whether it looks like terrain is
  still a human's job. (archive: "### What the tests check, and what they cannot")

## Shapes, landforms and Lands

- **A shape is a function of a position field**, an `R2 -> R2` input defaulting to `Coords`. That gave the CPU
  reference, zero halo and exact tiling for free, made reuse a `Transform` and a domain warp `Coords + noise`.
  (archive: "## Shapes, and what the audit changed")
- **Shape accuracy.** Every new kernel agrees with its CPU reference to float precision (worst relative error
  3e-7); a 40-gon against a circle of the same radius differs by 0.308 m, exactly its sagitta `100 (1 - cos(pi
  / 40))`. A polygon is capped at 128 segments because each pair is a pass over every sample. Caves, arches,
  overhangs, `R3` shapes and rivers are out of scope (one height per x, z). (archive: "### Measured")
- **A smooth maximum of 0 and 0 is `k / 4`.** Rounding the foot of a profile that must be exactly 0 left a
  crease where two dunes of different heights met; subtracting `k / 4` fixed it. (archive: "## The dune
  macro")
- **Lands principle: simple analytic shapes first, detail after.** Every element is built first from
  signed-distance primitives combined by union, intersection and subtraction plus profiles over those
  distances; noise, warp and erosion enter only in the detail stage, with their amplitude declared in metres.
  This makes primitives local (valid on the infinite plane), resolution-independent, GPU-evaluated and
  editable by id ("make island 3 bigger" changes a radius). (archive: "lands_plan.md ## 2. Fundamental
  criterion and principles")
- **Lands principles that still hold.** Planform is separate from profile and nothing assumes the centre is
  highest; land and water compose freely at any land fraction; one model serves the infinite plane and bounded
  maps; stable ids, a resolved list in the metadata and editing by id; take the references' operations and
  parameters, not their assumptions; all randomness from a seeded hash independent of query order or window;
  units are metres and km^2. (archive: "lands_plan.md ## 2. Fundamental criterion and principles")
- **The fused `Lands` op follows the `Water` pattern**: a list of elements in one block of the grid buffer and
  one node, because composing each island from `Shapes.*` costs several nodes of 1024 and a polygon a node per
  two segments. `Shapes.*` and `Landforms.*` remain for a few hand-declared elements and for detail. (archive:
  "lands_plan.md ## 6. Architecture: the fused `Lands` op")
- **`Lands` is pointwise (halo 0), so neighbouring sections agree by construction.** It takes a `position`
  input (default `Terrain.Coords`; a `Shapes.Warp` gives coast detail with no new code), has two channels
  (`maxChannels = 2`) and three modes: `coast` (signed distance, land negative, in metres clipped to `D`, plus
  dominant land id), `relief` (height over sea level of the per-element blended land profile) and `seabed`
  (margin and seabed elements). The CPU reference in `sampler.cpp` matches the GLSL term by term, which is
  what makes `Terrain.Sample` and the GPU/CPU test comparisons work. (archive: "lands_plan.md ## 6.
  Architecture: the fused `Lands` op")
- **Per-sample cost is bin-local.** The block has a 24-value header, 32 values per element, a CSR bin index
  and a points table; a sample reads only the elements of its bin (CSG in global order: `add` with polynomial
  smin, `subtract` with smax), so cost is independent of the total and outside the bins the distance is `D`
  (open sea). (archive: "lands_plan.md ## 6. Architecture: the fused `Lands` op")
- **One field, one sign.** Everything derives from the signed distance to the coast with land negative; mixing
  conventions between products is the usual source of inverted shelves. The ocean margin is a multiple of the
  radius for `compressed` (a fixed 2.6 km shelf made one island look different at every size) and metres for
  `geologic`. (archive: "## The lands macro: islands, archipelagos and continents (superseded)")
- **Coast type is blended from several inputs** (hardness, sediment, exposure, curvature) over a wide
  transition (0.2 to 0.7). Curvature alone makes every cape a cliff and every bay a beach, and sharper edges
  saturated every preset to one type. (archive: "## The lands macro: islands, archipelagos and continents
  (superseded)")
- **Placement rules.** Islets are placed on the CPU outside the outline by a channel plus twice the warp's
  reach, so a warp can neither join them nor leave a hole. An archipelago's islands share a warp tied to the
  median island, placed after its reach is known; a warp tied to the whole group folded the small islands. A
  continent is a region with ranges from `Ranges.Carve` faded at the coast. (archive: "## The lands macro:
  islands, archipelagos and continents (superseded)")
- **Fractal dimension is a diagnostic.** Box counting over 2^2 to 2^7 cells gave 0.9 to 1.1 on every island
  and depends on scale range and grid; compare it between runs of one example, never with a literature value.
  (archive: "## The lands macro: islands, archipelagos and continents (superseded)")
- **Sea level by quantile.** `land_fraction` is exact on the grid whatever the noise did; flat ground at the
  base level counts as sea, so a layout that cannot reach the fraction gets what it has. A skirt round each
  body is needed or over 30 % of the grid sits at the base level and the quantile has no contour to choose.
  (archive: "## The lands macro, second version: generated shapes, 70 % land")
- **Default land share: 70 % for an island or continent; lower for an archipelago.** Scattered islands kept
  apart by straits fill 35 to 55 % of a square, and asking 70 % merges them into one mass with gulfs.
  `land_fraction` overrides. (archive: "## The lands macro, second version: generated shapes, 70 % land")
- **Valleys that reach the sea are the inlets.** Stream-power valleys are cut to the base level and the sea
  level cuts the coast above them, so no separate fjord step exists in that macro. (archive: "## The lands
  macro, second version: generated shapes, 70 % land")
- **History in one line.** The first Lands macro (smooth unions of discs, an outline drawn by default) was
  superseded by generated shapes with a quantile sea level, and later by the fused op fed by Azgaar-template
  outlines; only the sign convention, the margin and the products survived each change. (archive: "## The
  lands macro, second version: generated shapes, 70 % land")

## Water

The archive records no separate water milestone; the decisions below are the ones it states, and
`docs/architecture.md` ("What exists") describes the ops (`Water`, `Water.Drain`, `Water.Network`).

- **Hydraulic erosion exposes water and sediment as channel `flow`**, the state after the last iteration, a
  wetness and a sediment map a renderer uses to decide where rock gives way to silt. Written only when a
  consumer asks, and `hydraulic_flow_r2` reproduces both files bit-exactly. (archive: "### Hydraulic erosion
  exposes its water and sediment")
- **The open-sea quad sits at the cleared depth** (`z = 0` under reversed depth), so water is hidden by
  terrain that stands above it. Water makes depth bugs obvious because the part behind a ridge is the tinted
  part. (archive: "## Depth was reversed twice")
- **Water depth regression test.** `test_viewer` renders a crater seen from inside with the floor under water;
  the wall is above the level, so the frame over the wall must be identical with water on and off (13.56 codes
  of difference with the old viewport, 0.00 fixed). (archive: "## Depth was reversed twice")
- **Water bodies and coasts follow the signed-distance convention of Lands**; the resolved list classes them
  (`sea`, `lake`, `ocean`, or the role) and a water element covering a sample ends the ownership of the land
  before it, so an islet in a bay has its own id. (archive: "## The lands macro, second version: generated
  shapes, 70 % land")

## Terrain families

- **Alpine: composition before detail, and erosion is the form.** Ranges built by adding shapes read as clay
  (convex everywhere, no arêtes); carving a block with cirques left every primitive's signature (circles,
  flat floors, rings). Drainage joining larger valleys from crest to plain is what the references show, and
  only a global stream-power simulation produced it (`Ranges.Carve` / `Ranges.Finish`). (archive: "## The
  alpine macro, and what building it taught")
- **Alpine tuning came from numbers.** "Too grey" was 77 % of the massif steeper than 30 degrees because every
  hillslope sat at the talus; lowering it from 1.15 to 0.6 fixed it. A piedmont and a painted scree mask were
  judged worse and reverted; the plain around a range is flat. Terrain was judged by eye on the references, no
  metric decided it. (archive: "## The alpine macro, and what building it taught")
- **Dunes: asymmetry is the dune.** A long windward back, a short slip face at the angle of repose and a sharp
  brink; billow noise is symmetric lumps, a sawtooth along the wind gives the profile and stays pointwise.
  (archive: "## The dune macro")
- **Dunes: crescents, not ridges.** Bending the phase by billow noise across the wind makes cusps where arcs
  meet (a barchan chain); fBm bend gives sinuous ridges without horns. Small dunes bend by simplex because
  billow reads as fish scales at that size. Clipping below a fraction of the height and scaling chains by slow
  noise gives bare flats and isolated barchans. (archive: "## The dune macro")
- **Dunes: three styles.** `"procedural"` is the default (analytic, no repetition, about 0.6 s), `"simulated"`
  is `Erosion.Wind` (Werner's slab model with shadow, avalanche and reptation, periodic every `cells * cell`
  metres) and `"star"`. Simulated dunes need the right iteration count: too few leave grain, too many merge
  into a few huge dunes. Slab heights make them steep for their size, so relief is scaled down rather than
  smoothed. (archive: "## The dune macro")
- **Dune slip faces are built a few degrees under repose.** That took the fraction over 35 degrees to 0.3 %
  and over 40 to 0.03 %, where thermal iterations could not. (archive: "## The dune macro")
- **Dunes look: the light, then the geometry.** At wind 20 slip faces face the fixed sun and read as thin
  bright lines; at wind 110 the same geometry reads as soft dunes. Check the light first. Sand gets its
  palette through the `look` key of the returned table, with ripples drawn in the shader a few metres apart.
  GLSL `%` on a negative int is undefined and drew a seam where the periodic grid wrapped. (archive: "## The
  dune macro")
- **Cliffs: buttresses are a sideways displacement, not erosion.** Moving the ground horizontally by ridged
  noise at column scale (150 m) shifts the riser by the same amount at every height, so the pattern runs
  straight down the face. Strata are a terrace of height plus a tilted plane, minus the plane; the mirrored
  gradient is `u - Terrace(u, 1, sharpness 1)`. (archive: "## Cliffs after a World Machine escarpment")
- **Cliffs: rivers and coast erosion are not reproduced.** The flow map gives channels on the slopes;
  `Erosion.StreamPower` is whole-grid, which `terrains/cliffs.lua` does not use. (archive: "## Cliffs after a
  World Machine escarpment")

## Testing lessons

Defects that reached acceptance with a green suite, and the property that guards each now. The common cause:
the suite checked the step before the one that was wrong, or compared the engine's output with itself.

- **Byte-reversed 16-bit PNGs.** Guard: `test_output_files` decodes a written PNG against hand-computed bytes
  (`0.0` over +-100000 is `80 00`). (archive: "### Every 16-bit PNG the engine ever wrote was byte-reversed")
- **Viewer drew every tile and showed none (depth cleared to 1).** Guard: `test_viewer` captures a frame and
  checks it holds terrain and changes when the camera moves. (archive: "### The viewer drew every tile and
  showed none of them")
- **Depth reversed twice.** Guard: a crater seen from inside with the floor under water; the frame over the
  wall must be identical with water on and off. (archive: "## Depth was reversed twice")
- **Pointwise kernels indexing with the wrong halo (99.99 % wrong).** Guard: `test_seams` has one case per
  kernel, comparing the output with and without a wider-halo branch. (archive: "## Pointwise kernels read
  their inputs through the input's halo")
- **Chained halo ops depending on section size.** Guard: `CheckChainedOpSeams` covers eight chains (blur over
  blur at four radius pairs; thermal over blur, over thermal and over hydraulic). Single-op seam checks all
  passed because the bug needs a released slot to inherit. (archive: "### The halo of a reused slot")
- **Iterative op reading its producer with the wrong stride.** Guard: a graph with `Hydraulic(n)` beside
  `Gradient(Thermal(n))` is identical at sections 128 and 512. (archive: "## Cliffs after a World Machine
  escarpment")
- **Thermal erosion as a no-op from a misordered registry.** Guard: a `constexpr` check that each `kOps` entry
  sits at its ordinal. (archive: "### What this milestone found")
- **`R3` node given `R2` state buffers via `Mapping{}`.** Guard: no sentinel; builders state the state shape,
  and the `R3` seam case catches it. (archive: "### What this milestone found")
- **Error file names reported as `?` after 16 scripts.** Guard: the error tests check every message names its
  script (17 checks). (archive: "### A silent limit in the error reporting")
- **`terrain_viewer` exiting 0 on a missing script.** Guard: `test_cli` runs the executables as a user would
  and checks exit codes, error text and files. (archive: "### Command-line numbers")
- **Lua sandbox escape (`pcall` swallowing the time budget).** Guard: a test script `while true do
  pcall(function() while true do end end) end` must end within the budget. (archive: "### The Lua sandbox, and
  an error that was not catchable")
- **Stale golden recorded with corrupted values** (`eroded_r2/slope.png`). Guard: the corrected output is
  bit-identical to the same graph with no erosion branch, which has no halo and is right by construction.
  (archive: "## Pointwise kernels read their inputs through the input's halo")
- **Hydraulic erosion with no usable setting.** Guard: `test_erosion` checks slope falls monotonically and
  conservation as a border effect; the clamped-sample count and relief across rain values were the measurement
  that exposed it. (archive: "### Hydraulic erosion had no setting that both worked and held together")
- **Test-order interference reading as regression.** Guard: the baseline comparison uses the fastest pass;
  never run two suites at once because they share `out/`. (archive: "### What this milestone found")
- **Process-level state (`Stop`, command pools) invisible to unit tests.** Guard: Debug validation errors fail
  every GPU test, and the viewer test runs the real loop with forced resizes. (archive: "### The bugs this
  milestone found")
