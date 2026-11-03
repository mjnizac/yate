---
name: terrain-author
description: Build or improve a YATE terrain script from a reference (photo, video frame, heightmap, World Machine or Gaea graph) or from a description. Use when writing or tuning anything under assets/scripts/, when a terrain "looks like noise", or when validating an example as reusable.
---

# Authoring a terrain in YATE

The goal is a script that reproduces the *forms* of the reference at its real dimensions, measured and
reproducible. Texture comes last and is cheap; forms are what make it look right.

## 1. Measure before writing code

Write a short table from the reference, in metres, before touching Lua:

| Form | Where | Dimensions | Profile |
| --- | --- | --- | --- |
| main wall | north edge | 600 m high, 4 km long | vertical top third, scree below |

Estimate scale from anything known in the frame (trees, roads, a stated summit height). If a dimension
cannot be estimated, ask the user instead of covering it with noise. Note what in the reference is light,
shadow or material rather than relief: it is not geometry and does not go into the script.

## 2. Start from the closest example

`docs/lua_reference.md` "Start here" maps a kind of terrain to an example. Prefer a prelude macro
(`Ranges.*`, `Lands.*`, `Dunes.*`) when one covers the family. Check a call's keys with
`tools/dev.sh api <name>` (the engine's own list, and the line of the reference that describes it), then read
only that section of the reference.

## 3. Build in stages, checking each one

1. Forms only: `Shapes.*`, `Geometry.*`, `Landforms.*`, `Curves.Profile`, `Curves.Terrace`. **No
   `Noises.*` in this stage.** Compare against the reference blurred to the same scale.
2. Structure: strata, terraces, faults; buttresses as a sideways displacement of the form.
3. Erosion: `Erosion.StreamPower` for valleys and drainage, then the GPU erosions.
4. Detail noise with its amplitude in metres declared in a comment, small against the relief (under
   about 5 %).

World Machine and Gaea practice, and the rules learnt rebuilding `terrains/cliffs.lua`: `relief.md`, next to
this file. Read it before stage 2 on any terrain with relief.

Change one variable per run. If a stage does not get closer, find out why before adding anything: do not
stack more noise or parameters.

## 4. Run and look

One command per iteration:

```bash
tools/dev.sh review <script> --bounds minx,minz,maxx,maxz --res <m> [--level L | --no-sea] --sun az,el --target x,y,z --shot yaw,pitch,dist --shot ...
```

It leaves `out/review/<script>/` with `metrics.json`, `hillshade.png`, `view_N.png` and prints the relief
summary; read the images with the Read tool. A reference heightmap is measured directly:
`build/Release/bin/terrain_metrics <ref.png> --level L`, after checking its `refs.json` says
`heightmap`.

Use the same three views every run (oblique, grazing, top-down) and keep them in the script's header
comment. The viewer has no cast shadows: judge geometry on the hillshade, with the sun in front of the
faces that matter, not on the viewer. Separate geometric defects (wrong forms, spikes, grid patterns)
from lighting, material or camera problems. Read the relief numbers against the family's case in
`tests/library/`: closed depressions mean undrained ground, a slope p99.9 near 90 means spikes, an
axis/diagonal power over about 1.5 means a grid artefact, and a spectral slope above the references'
means too little fine relief (below it, too much noise). Compare a reference only on scale-free numbers
unless its size and height are known, and check it is a heightmap and not shaded relief.

Land and sea (islands, archipelagos, continents, coasts) are built with `Lands.Compose`: the request-to-element
table and the checks are in `lands.md`, next to this file.

## 5. When an example counts as validated

* It runs from a clean build with the declared seed, bounds and resolution, and section sizes do not
  change its bytes.
* Its metrics fall in ranges taken from at least two references, or the difference is stated.
* No artefacts: no slope spikes two samples wide, no grid-aligned ripple, no closed depressions where
  water should drain.
* The user has reviewed the views. Until its case in `tests/library/` checks every range (no entry left
  as `"target": true`), it is an example, not a validated one.

## 6. What to keep

A technique that worked becomes an example script or a prelude change; a recurring mistake becomes a
line in this skill. Failed experiments are not written down in `docs/`.
