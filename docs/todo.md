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

- [ ] **CI is red on Windows since milestone 11, and Linux has never built.** `ci.yml` has run on
      GitHub (read from the public API; the logs need admin rights). Run `9dba657` passed on Windows.
      Runs `39092a3` and `ccd62ee` fail in the step "Tests that need no device" on both Windows
      configurations, and every Linux job fails in "Build" in every run. The Windows failure does **not**
      reproduce here: a clean clone of `ccd62ee`, configured and built exactly as the workflow does, with
      CRLF checkout and with the Vulkan drivers disabled, passes all six suites. The failing step's log is
      the missing piece: it names the suite. The same goes for the Linux compile errors.

- [ ] **One GPU, one vendor, one OS.** Everything has been validated on a GTX 1070 on Windows. Queue
      family selection, memory types, readback and the swapchain are exactly the areas where AMD and
      Intel differ. It needs hardware, and the GPU job above is how it would be exercised.

- [ ] **The open sea ends at the ring.** The quad that draws water where no terrain was covers the
      footprint of the resident tiles, so a surface at 0 m above a floor at -50 m shows a plate whose far
      edge floats above the terrain's. Extending it to the far plane would paint sea over a world that
      has not been evaluated, which is the less honest of the two.

- [ ] **No introspection of the API.** An agent can learn a call's keys only from the docs or from the
      error an unknown key produces, which lists them. A generated catalogue would need the bindings to
      declare their parameters in data instead of reading them as they go.

- [ ] **User macros cannot be shared between scripts.** `require`, `dofile` and `load` are removed on
      purpose, and the prelude is a closed list of files. A script can define its own local functions;
      sharing them across scripts needs a loading mechanism and a security decision, neither made.

- [ ] **Slope statistics.** The sidecar reports min, max, mean and the count of non-finite samples. Slope
      percentiles are computed by `test_geometry` from the raw export and are not in the sidecar.

## Not yet decided

- [ ] **The standard's catalog.** The draft `terrain_engine_spec_terrain_standard.md` (on the Desktop, not in the repo) describes a 25-node catalog with macros, file
      inputs and global operations. It would be built on the items above rather than instead of them,
      and its first phase is already mostly done: per-metre erosion, a pixel check and a file-bytes
      check were its "make the current engine honest" list. Whether to take the rest is open.
