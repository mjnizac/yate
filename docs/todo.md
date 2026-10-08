# Open tasks

## Findings to act on

- [x] **f32 coordinate precision far from the origin: measured, decided, limit reported.** A derivative
      sweep about 5e5 metres out is now part of `test_noise`, and it is the check that actually
      answers the question: the worst best-case error is 4.9% for `R2` and 19.8% for `R3`, against
      0.03% and 0.03% near the origin. The cause is the rounding of the unskewing term, which
      displaces the cell origin by around 1e-4 noise units; the displacement is not smooth between
      neighbouring samples, so the field carries argument jitter of about 0.05 m at a frequency of
      2e-3. Output at 1 m resolution never sees it. What a sub-metre derivative reports there does
      not mean anything.

      Two mitigations were tried and measured:

      - Reconstructing the cell offset from the fractional part of the skewed coordinate,
        `d0 = unskew(skewed - floor(skewed))`, which is algebraically identical and removes the
        unskewing rounding entirely. **Rejected: measurably worse.** The far sweep went from 4.9% to
        28% for `R2` and 19.8% to 109% for `R3`, and even the near-origin sweep went from 0.033% to
        0.094%, because `frac` is quantized at `ulp(skewed)` where the old difference is quantized at
        the finer `ulp(p)`.
      - Wrapping the noise-space coordinate by a power of two, which is the only option that shrinks
        every magnitude involved. It costs periodicity per octave: with a wrap of K noise units, the
        finest octave of a 6-octave fBm at frequency 2e-3 and lacunarity 2 repeats every K/0.064
        metres, so K = 256 repeats every 4 km.

      **Decided in milestone 7: not implemented, and the limit is stated instead.** The jitter works
      out to about `2e-7 * |world|` metres regardless of frequency, which matches the 0.05 m measured
      at 5e5 m. A 16k map at 1 m resolution reaches 8192 m, where that is 0.0016 m, so nothing the
      engine can export today is affected. The exporter warns when the jitter passes a tenth of a
      sample, so the limit is visible rather than lurking. Revisit only if a job genuinely needs
      coordinates past a few hundred kilometres, where the periodicity would be the lesser evil.

## Next

- [x] **Memory pools: asserted instead of eyeballed.** The item was "go and look at the Tracy memory
      view", which is a check that happens once and then stops happening. The invariants the view is
      drawn from are now in the suite instead: `test_allocators` sweeps every named CPU pool for its
      Tracy name, `reserved >= used`, `peak >= used` and an allocation count consistent with the bytes,
      and checks that `PeakCpuBytes` is never below their sum; `test_compute_roundtrip` sweeps all
      seven VRAM categories for the same invariants, that each name is grouped under `VRAM/`, and that
      the categories never account for more than the driver handed VMA, which is what would catch a
      category counted twice or against the wrong pool. What is left for a human is a one-off look to
      confirm the pools *appear* in the profiler under those names; the values no longer need looking
      at.
- [x] **`CPU/Untracked new` is third-party static state, not a leak.** Measured rather than assumed:
      a 4-section export and a 256-section one both end with exactly 1321536 bytes in 92 allocations,
      so it does not scale with work. It is spdlog's async queue, VMA's internals and Sol2's type
      registry, none of which take an allocator. Routing them would mean forking a dependency for
      1.3 MB of process-lifetime memory, so this stays reported as its own pool and is not pursued.

## Milestone 6 — follow-ups

- [x] **A scalar operand is now an immediate, not a buffer.** `Arith` gained a second variant field
      saying which operand was folded, the value travels as float bits in a parameter word, and a new
      compiler stage rewrites a node whose operand is a one-component constant. `blended_r2` drops from
      10 dispatches to 8 and loses the 0.16 ms the two `Const` dispatches cost, with the golden still
      bit-exact. Both sides fold, so unary minus (`0 - x`) is covered as well as `x * k`.

      The guess that it would save VRAM was wrong, and the measurement says so: peak stayed at 2048 KiB
      per section. The planner was already reusing those buffers for later values, so the peak is set by
      how many values are live at once, not by how many constants the graph mentions.

- [x] **`Terrain.Gradient` exists, and it is explicit.** Erosion produces a height with no closed form
      to differentiate, so the decision could not be deferred past milestone 8. It is a neighbourhood op
      of radius 1 over central differences, and it is the only op in the engine that is not analytic.
      What it is *not* is implicit: `Terrain.Normals` still takes a gradient and names `Terrain.Gradient`
      in its error rather than inserting one behind the script's back, so a script that could have used
      an exact derivative and did not is visible in the source.
- [x] The interned chunk-name table is shared by every Lua state, which is the point of interning, and
      it is now the only piece of mutable state in `lua.cpp` that is not per-script, so it takes a
      mutex. Without it a reload on a worker thread could hand out a view of a half-copied entry. The
      lock is taken a handful of times per script and never on a hot path.

## Carried over

- [x] **`SectionPool::Trim` compacts freely now.** A slot names its block by a never-reused id instead
      of by its position, so trimming empty blocks can move the survivors down without invalidating a
      live slot. The assert that used to guard it only held because trims happened on an empty pool,
      which is the restriction that would have bitten as soon as the evaluator released one graph and
      kept another. `test_compute_roundtrip` holds a slot in one block, trims another away, and checks
      the survivor still resolves and releases cleanly.
- [x] **`GeneralAllocator` keeps reporting `reserved == used`, deliberately.** The alternative was a
      private `mi_heap_t` per allocator, and the reason not to has become concrete rather than a matter
      of taste: a `mi_heap_t` is not thread-safe, and `CPU/General` is now allocated from several
      threads at once, because each PNG output encodes on its own worker and its bands come from there.
      Buying an accurate `unused` plot would mean a lock on every allocation in the engine. Revisit only
      if mimalloc gains per-heap segment callbacks.
- [x] **`ENGINE_ASSERT` now type-checks its condition in Release.** It used to expand to `((void)0)`,
      so a Release-only build never compiled the condition or the message at all and either could rot
      silently. Both now sit inside `sizeof`, which never evaluates what it measures: zero code, and a
      renamed member or a format string that stopped matching its arguments fails the Release build.
      Every assertion in the engine still compiles, which is how the change was checked.

      It does not detect a side effect inside a condition. C++ cannot decide that, so the rule stays a
      rule; what the macro can own is the guarantee that Release behaviour is "never evaluated" rather
      than "never compiled".
- [x] `ENGINE_SIMPLEX2_SCALE` and its 3D counterpart are now measured across all six base-and-domain
      combinations by `test_noise`, which prints the band each one produces and fails on a gross
      break. Both constants were recalibrated from that measurement rather than guessed.

## Milestone 8 — follow-ups

- [x] **Hydraulic erosion exposes its water and sediment** as channel 1, named `flow`. The naming was
      indeed the part worth thinking about, and the answer was that the Lua bindings should not hold the
      names at all: they come from the op registry now, so `noise.gradient` and `carved.flow` resolve
      through one mechanism and a wrong name is told what the op actually offers. The channel mask does
      the rest — a script that only wants a height pays for no second buffer.
- [x] **One section in flight stays, measured against an erosion-heavy script.** `basic.lua` with 24
      hydraulic iterations and 12 thermal: at 2048 the GPU is 41 ms of a 441 ms total (9%) at section 512
      and 33 of 518 (6%) at 1024; at 16384 it is 2.31 s of 50.0 s (5%). The encoder stall is 52% and 86%
      of those totals. So the GPU share did rise from half a percent to single digits and it still is not
      where the time goes — overlapping sections would hide work already hidden behind the wait for the
      PNG encoder. Revisit if an output format arrives that encodes as fast as the GPU evaluates.
- [x] **No shared-memory thermal kernel: the redundant taps never reach DRAM.** Measured at section
      1024, fastest of three runs: 1.14 ms at 8 iterations, 1.75 at 16, 3.06 at 32. Both slopes agree at
      0.077 and 0.082 ms per iteration, which against one read and one write of the padded state is
      116 GB/s, 45% of the GTX 1070's 256 GB/s. That is what settles it: if each of the four taps were
      reaching DRAM the real traffic would be two and a half times the model, so the card would have to be
      running at 290 GB/s, which it cannot. The taps are cache-served already, and a tiling would remove
      traffic that never leaves the chip while adding a barrier per iteration. The gap to peak is latency
      and occupancy, which tiling does not address.

## Milestone 9 — follow-ups

- [x] **Evaluation follows the camera.** A ring of tiles around what the camera is looking at, streamed a
      couple at a time as it moves, at a sample spacing chosen from how far away the ground is. One
      global level rather than per-tile, so two levels never meet on screen and there is no crack to hide
      with skirts — which would have contradicted the engine's one firm claim about seams. A level change
      recompiles the graph, which is 0.05 ms and no new pipeline.
- [x] **Wireframe on `T`.** It needed `fillModeNonSolid`, which is now the engine's only optional device
      feature; without it the mode is unavailable and the viewer says so instead of producing a
      validation error.
- [x] **The scroll accumulator no longer assumes one window**, and more importantly no longer replaces
      the UI backend's own scroll callback: `Input::Attach` runs before the UI comes up, so the UI chains
      to it rather than the other way round.
- [x] **Streaming cost measured, and left alone.** A tile is 954 microseconds end to end: 326 of command
      recording, about 200 of GPU work, and about 430 of submit, wait and two buffer copies. At two tiles
      a frame that is 1.9 ms, or 11% of a 60 Hz budget, and only while the camera crosses a tile border.
      Two thirds being overhead rather than GPU work does argue for batching a frame's tiles into one
      submission, but it would save a few hundred microseconds on a path already under a tenth of a
      frame, and it needs a barrier between tiles sharing the evaluator's buffers. The numbers are in
      `docs/status.md`; revisit if a heavier script makes a tile cost materially more.
- [x] A level change refills the ring, which is 47 ms, or three frames at 60 Hz, on a deliberate zoom.
      Streaming the new level under the usual budget instead would trade that for half a second of
      mostly-missing terrain. The hitch is the better failure, so it stays.

## Found during the milestone 9 visual review

- [x] **The viewer can be looked at.** `--screenshot <file>` copies the presented swapchain image out of
      the frame's own command buffer and writes it as an 8-bit PNG, which is what made the visual review
      possible at all. The *check* is still missing, below.
- [ ] **No check looks at a rendered pixel.** The readback exists now, so a check that a frame of terrain
      is not uniformly the clear colour, and that it changes when the camera moves, is a few lines in
      `test_viewer`. It would have caught the reversed-Z depth clear on the day it was written, and the
      tile gap on the day after. Everything the suite asserts about the viewer today stops one step short
      of a pixel.
- [ ] **Nothing checks a written file against a value, only against another written file.** The dataset
      runner compares decoded samples, which is what makes it survive a compressor swap, but both sides
      are PNGs the engine produced, so it was blind to every 16-bit sample being byte-reversed for the
      whole life of the project. One check that writes a known constant over a known range and asserts
      the bytes in the file closes it.
- [ ] **The startup framing ignores the level it is about to pick.** `Frame` is called with
      `sectionSize * resolution * (ring + 1)`, which is the ring's radius at level 0. `IdealLevel` then
      reads the resulting distance and picks a level, and at the defaults that is level 2, where a tile is
      8 m per sample and the ring actually spans 3584 m rather than the 1024 m it was framed for. The
      camera ends up inside a ring four times larger than intended. It is not wrong enough to hide
      anything — the terrain fills the view from 2128 m out — but the number is a lie and the fix is to
      frame, pick the level, then reframe on the radius that level implies.

## Found during the milestone 9 visual review

- [ ] **The erosion ops measure slope per cell, so the terrain changes shape with the sample spacing.**
      `Erosion.Thermal`'s `talus` is a height difference between neighbours, and hydraulic erosion's
      capacity term is too. Halve the spacing and the same drop is half as many metres per metre, so the
      same script describes different terrain. Measured on `basic.lua` over the same region at 8 m and at
      16 m, comparing the samples that share a world position: the noise alone is **identical**, to the
      bit, and with erosion the mean difference is 8.0 m and the worst is 94.7 m.

      That is not an abstract wart. The viewer picks its sample spacing from camera distance, so the
      terrain visibly changes shape when the ring changes level — the one thing the single global level
      was chosen to avoid.

      The fix is to divide the neighbour difference by the sample spacing in both kernels and let `talus`
      mean a slope rather than a drop. Both already have a parameter word free, and `ResolutionParamWord`
      is the mechanism. It changes what `talus` means to a script, which is a published convention, so it
      is written down here rather than done.
- [ ] **The viewer's startup framing still ignores the level it is about to pick.** Unchanged from the
      previous entry: `Frame` is called with the ring radius at level 0, `IdealLevel` reads the resulting
      distance, and at the defaults the ring then spans 3584 m rather than the 1024 m it was framed for.
      Nothing is hidden by it, but the number is a lie.
- [ ] **`basic.lua` declares a range it does not use.** The clamp is at -600 and 800 while the terrain
      measures -173 to 166 at 8 m sampling, so most of the 16-bit depth is spent on heights that never
      occur. Narrowing it is a one-line change, but the right number depends on the amplitude, which is a
      script parameter, so the clamp should be derived from it rather than written as a constant.

## Found while writing the Lua reference

- [ ] **`Terrain.Coords` returns sample indices, not metres.** Every positional script — an island
      falloff, a region mask, a domain warp — therefore describes different terrain at every resolution
      unless it multiplies by `params.resolution` by hand, and nothing reminds it to. This is the same
      class as the erosion units and should be decided with them: either the op returns metres, or the
      name says what it returns.
- [ ] **On an `R2->R2`, world z is `.y`.** The component accessors are positional, so
      `Terrain.Coords{ domain = 2 }.z` is an error about component 2 not existing rather than about the
      axis. The accessor could know that an `R2` value's second component is the z axis and accept `.z`
      for it, which is what every script actually means.
