# Open tasks

## Next

- [ ] Capture a Tracy session of `test_datasets` and confirm the memory view shows every CPU and VRAM
      pool with correct reserved and used values. The events are emitted; nobody has looked at the
      graphs yet.
- [ ] Route what is still landing in `CPU/Untracked new` at shutdown (about 1.4 MB in 8k allocations,
      mostly spdlog and VMA internals) through named allocators.

## Milestone 5 — Graph and compiler

- [ ] `terrain/graph.cpp`: node storage in a pool allocator, typed handles, channels, source
      locations.
- [ ] `terrain/compiler.cpp`: validation, constant folding, CSE by structural hash, dead-node
      removal, classification, halo propagation.
- [ ] `terrain/kernels.cpp`: op registry, pipeline map keyed by `(op, variant)`, specialization
      constants.
- [ ] `terrain/evaluator.cpp`: topological order, liveness analysis over the section pool, peak
      intermediate VRAM computed before evaluation, one command buffer per section, a Tracy GPU zone
      and a timestamp pair per dispatch so the sidecar can report GPU time per op.
- [ ] Replace the hardcoded fBm-and-normals chain in `RunExport` with the compiled graph.
- [ ] A CPU reference implementation per op kernel, compared on small inputs.
- [ ] A dataset case per op, and one per common mapping (`R2->R1`, `R2->R2`, `R2->R3`, `R3->R1`).
- [ ] Ops to cover: arithmetic, curves and clamps, `Combine.Blend`, `Masks.Slope`, `Noises.Ridged`,
      `Vec.Combine` and component extraction.

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
