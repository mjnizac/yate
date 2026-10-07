# Open tasks

## Findings to act on

- [ ] **f32 coordinate precision far from the origin.** `test_noise` measures it: about 5e5 metres
      out, `d0 = p - cellOrigin` is a difference of two numbers around 5e3 in noise space where the
      f32 step is 5e-4, so the cell offset keeps roughly three good digits and the noise value is
      accurate to about 1e-2 relative. Near the origin the same comparison agrees to 1e-6. Every op
      downstream of noise inherits that budget. The usual fix is to wrap the lattice coordinate by a
      large power of two before converting to float, which makes the noise periodic at a scale far
      beyond any map while keeping the float coordinates small. Decide before milestone 7, which is
      where large worlds arrive.
- [ ] **Collecting GPU timestamps still costs more than the GPU work.** The extra submission is gone:
      `Queue::CollectGpuZones` now records into the section's own command buffer and `RecordSection`
      went from 6.92 ms to 3.22 ms over 16 sections. What remains is Tracy's own readback inside
      `TracyVkCollect`, 107 microseconds of CPU per section against dispatches of 2 to 11
      microseconds. Collecting every N sections would amortise it; measure whether the query pool
      tolerates the backlog first. Profiling-only overhead either way.
- [ ] Total export time is about 18% above the pre-milestone-5 baseline and the cause is not
      identified. Pipeline creation was one part and is fixed; the rest is unexplained. PNG encoding
      dominates the wall time, so measure before optimising.

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
- [ ] PNG encoding is about 99% of export wall time. Before optimising, measure whether a lower zlib
      level or a different filter choice is enough; the data is high-entropy noise, so compression has
      little to work with.
- [ ] `ENGINE_SIMPLEX2_SCALE` was chosen to put a unit-amplitude fBm inside roughly [-1, 1] and is
      checked loosely by `test_noise`. Measure the true bound once there are more noise kinds.
