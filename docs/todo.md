# Open tasks

Only what is still open. Finished work and the measurements behind it are in `docs/decisions.md`; the
full history is in the git log.

## Correctness

- [ ] **Erosion iterations are per cell, so the *amount* of erosion still depends on the sample
      spacing.** The slope units are fixed (`talus` and the hydraulic capacity are rise over run, and a
      thermal-only graph agrees between 8 m and 16 m to 0.02 m on average). What is left is that one
      iteration moves material one *cell*: 24 iterations reach 192 m at 8 m and 384 m at 16 m. On
      `basic.lua`, with hydraulic erosion on top, 8 m against 16 m over the same world positions still
      differs by 5.45 m on average and 44.3 m at worst (it was 8.0 and 94.7 before the units were
      fixed). The standard's answer is to take the propagation distance in metres and derive the
      iteration count from the spacing; it changes what `iterations` means, so it is a decision, not a
      patch.

- [ ] **Hydraulic erosion leaves a high-frequency ripple.** A `Filters.Blur{ radius = 1 }` after the op
      clears it, and every sample terrain does that. Adding the four diagonal neighbours (weight
      1/sqrt(2), `flow_rate` bound lowered to 0.146) was tried and **rejected on measurement**: the
      mean |second difference| went from 5.3 to 6.4 along the axes and 7.8 along the diagonals, and the
      axis-to-diagonal ratio flipped from 1.36 to 0.83. The diagonals move where the residue lands
      rather than removing it, so the cause is in the per-cell erosion and deposition step
      (`taken`/`dropped` in `hydraulic_erosion.comp.glsl`), not in the transfer stencil.

## Testing

- [ ] **The CI workflow has never run.** `.github/workflows/ci.yml` builds with MSVC (Debug, Release) and
      runs the six suites that need no device on every push; Linux with GCC and Clang under ASan and
      UBSan is there too and is allowed to fail, because the engine has only ever been built with MSVC
      on Windows; and a GPU job runs the whole suite plus the 16k section-size acceptance on a
      self-hosted runner when the repository variable `GPU_RUNNER` is `true`. None of that is verified
      until it has run once on GitHub. Expect the first Linux run to fail, and the action's Vulkan SDK
      version to need adjusting.

- [ ] **One GPU, one vendor, one OS.** Everything has been validated on a GTX 1070 on Windows. Queue
      family selection, memory types, readback and the swapchain are exactly the areas where AMD and
      Intel differ. It needs hardware, and the GPU job above is how it would be exercised.

## Not yet decided

- [ ] **The standard's catalog.** The draft `terrain_engine_spec_terrain_standard.md` (on the Desktop, not in the repo) describes a 25-node catalog with macros, file
      inputs and global operations. It would be built on the items above rather than instead of them,
      and its first phase is already mostly done: per-metre erosion, a pixel check and a file-bytes
      check were its "make the current engine honest" list. Whether to take the rest is open.
