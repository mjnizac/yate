# Open tasks

Only what is still open, grouped by the kind of work. Finished items and the measurements behind them
are in `docs/decisions.md`; the full history is in the git log.

Nothing here is ordered by priority. If one thing comes first it is CI, because every defect recorded
in `docs/decisions.md` was found by hand, on one machine, after it had already shipped.

## Correctness

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

- [ ] **`Terrain.Coords` returns sample indices, not metres.** Every positional script — an island
      falloff, a region mask, a domain warp — therefore describes different terrain at every resolution
      unless it multiplies by `params.resolution` by hand, and nothing reminds it to. This is the same
      class as the erosion units and should be decided with them: either the op returns metres, or the
      name says what it returns.

- [ ] **On an `R2->R2`, world z is `.y`.** The component accessors are positional, so
      `Terrain.Coords{ domain = 2 }.z` is an error about component 2 not existing rather than about the
      axis. The accessor could know that an `R2` value's second component is the z axis and accept `.z`
      for it, which is what every script actually means.

- [ ] **Hydraulic erosion leaves a one-sample ripple aligned with the grid.** Its transfers are pairwise
      over four axis neighbours, so whatever the scheme cannot resolve lands on the axes as a fine
      corduroy that is clearly visible in the viewer on any low-relief terrain. It is not rainfall
      dependent: it is as strong at `rain = 0.1` as at `0.5`. A `Filters.Blur{ radius = 1 }` after the
      op clears it for one unit of halo, which is what the sample terrains do, but that is a workaround
      in every script rather than a fix in the kernel. Including the four diagonals with a 1/sqrt(2)
      weight is the usual remedy and is the same change the thermal kernel's header already weighs.

- [ ] **Thermal erosion terraces a smooth dome into concentric steps.** Correct behaviour — it flattens
      any slope past the talus into facets — but on an analytic falloff it reads as an artefact rather
      than as geology. Worth a note in the Lua reference under `Erosion.Thermal`, and worth considering
      whether a small random perturbation of the talus per cell would break the symmetry without
      costing determinism.

- [ ] **The startup framing ignores the level it is about to pick.** `Frame` is called with
      `sectionSize * resolution * (ring + 1)`, which is the ring's radius at level 0. `IdealLevel` then
      reads the resulting distance and picks a level, and at the defaults that is level 2, where a tile is
      8 m per sample and the ring actually spans 3584 m rather than the 1024 m it was framed for. The
      camera ends up inside a ring four times larger than intended. It is not wrong enough to hide
      anything — the terrain fills the view from 2128 m out — but the number is a lie and the fix is to
      frame, pick the level, then reframe on the radius that level implies.

- [ ] **`basic.lua` declares a range it does not use.** The clamp is at -600 and 800 while the terrain
      measures -173 to 166 at 8 m sampling, so most of the 16-bit depth is spent on heights that never
      occur. Narrowing it is a one-line change, but the right number depends on the amplitude, which is a
      script parameter, so the clamp should be derived from it rather than written as a constant.

## Testing and CI

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

- [ ] **No black-box test of the executables.** The dataset runner is a golden comparison over whole
      scripts, so the review's framing of "tests are white-box" is not quite right — but nothing checks
      the *programs*: exit codes, the text of an error for a bad flag, a missing script, a malformed
      `--param`. The probes written while checking this review are most of such a suite already.

- [ ] **No CI.** Every defect found so far was found by hand, on one machine, with one GPU and one
      compiler. This is the highest-value item the review names: MSVC, GCC and Clang, Debug and Release,
      with ASan and UBSan on the CPU-only suites. The GPU suites need a self-hosted runner and can stay
      nightly.

- [ ] **One GPU, one vendor, one OS.** Everything has been validated on a GTX 1070 on Windows. Queue
      family selection, memory types, readback and the swapchain are exactly the areas where AMD and
      Intel differ. This is real, but it is gated on having CI at all, so it sits behind that item.

### Checked and not accepted

- **"R3 is not end-to-end."** It is, for export: `terrain_export` builds an `R3` graph, evaluates it
  brick by brick and streams it into a raw `f32` volume, and `tests/datasets/volume_r3` is a passing
  golden case. What is missing is `R3` in the *viewer*, which `docs/status.md` already says.
- **"Establish a rule that a new allocator must be justified by benchmark."** The rule already operates:
  `docs/status.md` records three optimisations measured and then rejected — the `frac` reconstruction of
  the simplex cell offset, overlapping sections, and a shared-memory thermal kernel.
- **A `TerrainJob` abstraction.** `ExportJob` already carries script, params, seed, bounds, resolution,
  outputs and device. Renaming it buys nothing until there is a second consumer that needs it.
- **The scoring table.** Ten numeric ratings with no stated methodology are not actionable and are not
  recorded here.

### What the review did not see

Its model is "the architecture is mature, the product is young, so do hardening". That is a fair read of
the repository from the outside, and it is incomplete, because the defects that actually bit this
project in the last week were none of the things on its checklist:

* every 16-bit PNG the engine ever wrote was byte-reversed;
* a reused buffer slot's stale halo made chained ops read at the wrong stride, from milestone 5 to 9;
* the depth buffer was cleared against the reversed-Z convention, so the viewer drew every tile and
  discarded every fragment;
* `PngWriter` dropped a field in its move assignment, so every vector output was written as one
  component in red;
* the viewer handed scripts a hardcoded `resolution` of 1.0.

None of those is a hostile-input problem and none would be caught by a sandbox, a sanitizer or a fuzzer.
They are *correctness* defects, and they survived twelve green suites for one reason: **no check in the
project compared a result against a value the project did not itself produce.** The review's visual
regression item is the only one on its list that touches this, and it is listed as P1. It is the first
thing, together with CI, and the two items at the top of this file's previous section — a check that
looks at a pixel and a check that looks at the bytes of a written file — are the cheap half of it.

## Hardening

- [ ] **The Lua sandbox does not hold, and `docs/lua_api.md` claims it does.** The state opens `base`,
      `math`, `string` and `table` and removes nothing afterwards. Enumerating `_G` from a script
      returns `dofile loadfile load collectgarbage rawset rawget setmetatable getmetatable pcall print
      select next rawlen`. So "a script cannot read files" is false — `dofile` and `loadfile` do exactly
      that — while "cannot start processes or load C modules" is true, since `io`, `os`, `package`,
      `require`, `debug` and `coroutine` really are absent.

      The consequence the review did not draw is the one that matters most here: **`load` breaks
      provenance.** The sidecar records a hash of the script *file*, and a script that builds a chunk at
      runtime and runs it is not described by that hash. For an engine whose headline property is
      reproducibility, that is a correctness hole rather than only a security one.

      The fix is a whitelist rather than a blacklist: clear the globals that are not wanted after
      opening the libraries, and make `docs/lua_api.md` state exactly what remains.

- [ ] **A script can hang the process.** `while true do end` runs until killed; there is no instruction
      budget and no wall-clock limit. Measured: still running after 15 s, exit 124 under `timeout`. A
      `lua_sethook` count hook is a handful of lines and gives a bounded, catchable failure. This is
      worth doing for a *trusted* script too, because the viewer runs the script on a worker thread at
      every hot reload and a typo currently wedges it.

- [ ] **The CLI parser accepts trailing garbage and non-finite numbers.** Measured on this copy:
      `--resolution 123abc` is accepted as 123; `--section 200abc` is accepted as 200 and only then
      rejected for not being a power of two; `--resolution -1` and `--resolution nan` pass the parser
      and are caught downstream *by accident*, by a script that happens to derive a talus from the
      resolution — a script that does not would run with a negative or NaN spacing. `--min 1e400,0` is
      accepted as infinity. The parsers should consume the whole token, reject non-finite values, and
      require a positive resolution at the point of parsing.

- [ ] **Nothing bounds the total work a job asks for.** `--max 1e9,1e9` is accepted and starts an export
      of 1953125 x 1953125 sections. A job whose sample count overflows `u32`, or whose section count is
      absurd, should fail at startup with the number it computed, not begin.

- [ ] **`CopyBounded` truncates silently**, and it is what carries output paths, script paths and output
      names into fixed-size storage. A checked variant that errors when the text does not fit is the
      right default for anything that names a file. Related, and worse: `HashFile` returns sixteen zeros
      when it cannot open the file, so an unreadable script yields a valid-looking `script_hash` of
      `0000000000000000` with no error anywhere.

## Reproducibility and output

- [ ] **Dependencies are pinned by tag, not by commit.** Ten `FetchContent_Declare` calls use
      `GIT_TAG v1.15.1`-style tags, and a tag can be moved. For a project that sells reproducibility
      this is the cheapest gap on the list to close.

- [ ] **Provenance covers the script and nothing else.** The sidecar records engine version and commit,
      script path and hash, seed, bounds, resolution, GPU, driver, timings and memory, which is already
      more than most. What it does not record is a hash of the **shaders** or of the resolved
      parameters, so "what produced this file" is answerable only if the shader sources are assumed to
      follow the engine commit — which they do today and will not once shaders are generated. FNV-1a is
      also a weak choice for an identifier that may be compared across machines; SHA-256 or BLAKE3 costs
      nothing at these sizes.

- [ ] **PNG should probably not be the primary production format.** The measurement the review quotes is
      this repository's own: on a 16384² export the GPU is 2.31 s, readback 3.32 s and the encoder stall
      43.1 s of a 50.0 s total. That is already recorded as the reason not to overlap sections, but the
      conclusion drawn was "do not optimise the GPU" when it should also have been "offer a format that
      is not deflate". A raw `f32` or Zstd-compressed tiled output would move an export from
      encoder-bound to readback-bound. PNG stays as the interchange format.

## Decisions to write down

- [ ] **State the Vulkan 1.4 requirement as a product decision, or lower it.** The engine requires 1.4
      plus synchronization2, maintenance4, timeline semaphores, buffer device address, scalar block
      layout and `shaderInt64`. Nothing in the feature list actually needs 1.4 over 1.3 with those
      extensions; the requirement is there because it was convenient. Either write down that it is
      deliberate, or take the 1.3 path and widen the hardware it runs on.
