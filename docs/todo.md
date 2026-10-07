# Open tasks

## Findings to act on

- [ ] **f32 coordinate precision far from the origin: measured, mitigation deferred.** A derivative
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
        metres, so K = 256 repeats every 4 km. **Deferred to milestone 7**, where the actual world
        size is known and the trade can be made against it rather than guessed.

## Next

- [ ] Confirm the Tracy memory view shows every CPU and VRAM pool with correct reserved and used
      values. The GPU zones now reach the profiler and the CPU zones were already there; the memory
      graphs are the part nobody has read.
- [x] **`CPU/Untracked new` is third-party static state, not a leak.** Measured rather than assumed:
      a 4-section export and a 256-section one both end with exactly 1321536 bytes in 92 allocations,
      so it does not scale with work. It is spdlog's async queue, VMA's internals and Sol2's type
      registry, none of which take an allocator. Routing them would mean forking a dependency for
      1.3 MB of process-lifetime memory, so this stays reported as its own pool and is not pursued.

## Milestone 6 — follow-ups

- [ ] **A scalar operand should be an immediate, not a buffer.** The `blended_r2` trace shows it:
      `blended * amplitude + 200.0` compiles to two `Const` dispatches that each fill a whole section
      buffer with one repeated number, plus two `Arith` dispatches that read them. That is 0.16 ms of
      the case's 1.6 ms of GPU time and two section buffers, for two floats. `Arith` already has spare
      push-constant words and a variant byte, so a "second operand is an immediate" variant is cheap;
      the compiler would fold a `Const` producer into its consumer during canonicalization. Measure
      afterwards: the dispatches are tiny, and the win may be mostly VRAM rather than time.

- [ ] `Terrain.Normals` and `Masks.Slope` take a gradient, so a height that is not a noise channel
      cannot be turned into normals. The spec's own example writes `Terrain.Normals{ input =
      eroded.value }`, which needs a `Gradient` neighbourhood op over central differences. It is the
      first op in the engine that would not be analytic, which is why it is a decision and not a
      chore: decide alongside milestone 8, where erosion produces exactly such a field.
- [ ] A script runs on whichever thread asked for it and the error slot is thread-local, but two
      scripts on two threads would still share the interned chunk-name table. Fine today, since the
      exporter runs one script; revisit when the viewer reloads on a worker thread (milestone 9).

## Carried over

- [ ] `SectionPool::Trim` asserts rather than remapping block indices. Decide whether slots should
      hold a block handle instead of an index before the evaluator reuses buffers across sections.
- [ ] `GeneralAllocator` cannot report real reserved bytes. Revisit if mimalloc gains per-heap segment
      callbacks, or switch to a private `mi_heap_t` per allocator with explicit locking.
- [ ] `ENGINE_ASSERT` compiles away in Release, so a condition with side effects would be dropped.
      Consider a compile-time check that forbids that.
- [x] `ENGINE_SIMPLEX2_SCALE` and its 3D counterpart are now measured across all six base-and-domain
      combinations by `test_noise`, which prints the band each one produces and fails on a gross
      break. Both constants were recalibrated from that measurement rather than guessed.
