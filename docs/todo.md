# Open tasks

## Blocking

- [ ] Install the Vulkan SDK 1.4.x (see `docs/commands.md`), then configure and build.
- [ ] Fix whatever the first compile reports, one `fix:` commit per cause.
- [ ] Run `ctest --output-on-failure` and confirm the four suites pass.
- [ ] Capture a Tracy session of `test_compute_roundtrip` and confirm the memory view shows every
      CPU and VRAM pool with correct reserved and used values. That is the acceptance criterion of
      milestones 2 and 3.

## Milestone 4 — First export

- [ ] Hand-written simplex kernel in `assets/shaders/ops/`, with its analytic gradient as a second
      output channel.
- [ ] `terrain/output_writer.cpp`: 16-bit PNG through libspng, rows streamed progressively, plus the
      JSON metadata sidecar (timings, peak memory per category, environment).
- [ ] Add libspng to `cmake/engine_dependencies.cmake`, pinned to a release tag.
- [ ] Replace the `ExportLayer` body in `src/terrain/export_main.cpp` with real evaluation, and move
      it to `terrain/export.cpp` behind `terrain/export.hpp`.
- [ ] Normalization from the declared `range`, with a clamped-pixel count in the log.
- [ ] `tests/runner/test_datasets`: golden comparison on decoded data plus baseline recording.
- [ ] First dataset case under `tests/datasets/`.

## Milestone 5 — Graph and compiler

- [ ] `terrain/graph.cpp`: node storage in a pool allocator, typed handles, channels, source
      locations.
- [ ] `terrain/compiler.cpp`: validation, constant folding, CSE by structural hash, dead-node
      removal, classification, halo propagation.
- [ ] `terrain/kernels.cpp`: op registry, pipeline map keyed by `(op, variant)`, specialization
      constants, persisted pipeline cache.
- [ ] `terrain/evaluator.cpp`: topological order, liveness analysis over the section pool, peak
      intermediate VRAM computed before evaluation, one command buffer per section.
- [ ] A CPU reference implementation per op kernel, compared on small inputs.

## Carried over

- [ ] `SectionPool::Trim` currently asserts rather than remapping block indices. Decide whether
      slots should hold a block handle instead of an index before the evaluator starts reusing
      buffers across sections.
- [ ] `GeneralAllocator` cannot report real reserved bytes. Revisit if mimalloc gains per-heap
      segment callbacks, or switch to a private `mi_heap_t` per allocator with explicit locking.
- [ ] `ENGINE_ASSERT` compiles away in Release, so a condition with side effects would be dropped.
      Consider a compile-time check that forbids that.
