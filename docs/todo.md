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

- [ ] `Terrain.Normals` and `Masks.Slope` take a gradient, so a height that is not a noise channel
      cannot be turned into normals. The spec's own example writes `Terrain.Normals{ input =
      eroded.value }`, which needs a `Gradient` neighbourhood op over central differences. It is the
      first op in the engine that would not be analytic, which is why it is a decision and not a
      chore. **Being decided in milestone 8**, where erosion produces exactly such a field.
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
