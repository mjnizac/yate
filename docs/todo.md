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
- [ ] Route what is still landing in `CPU/Untracked new` at shutdown (about 1.4 MB in 8k allocations,
      mostly spdlog and VMA internals) through named allocators.

## Carried over

- [ ] `SectionPool::Trim` asserts rather than remapping block indices. Decide whether slots should
      hold a block handle instead of an index before the evaluator reuses buffers across sections.
- [ ] `GeneralAllocator` cannot report real reserved bytes. Revisit if mimalloc gains per-heap segment
      callbacks, or switch to a private `mi_heap_t` per allocator with explicit locking.
- [ ] `ENGINE_ASSERT` compiles away in Release, so a condition with side effects would be dropped.
      Consider a compile-time check that forbids that.
- [ ] `ENGINE_SIMPLEX2_SCALE` was chosen to put a unit-amplitude fBm inside roughly [-1, 1] and is
      checked loosely by `test_noise`. Measure the true bound once there are more noise kinds.
