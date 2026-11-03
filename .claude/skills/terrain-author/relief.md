# Relief, structure and erosion: what has worked

Read from `SKILL.md` stage 3 when the terrain has relief to shape (ranges, cliffs, strata, erosion, drainage).

What World Machine and Gaea practice agrees on (official docs and their forums, 2026-10):

* A base form should need little erosion: start from shaped primitives (Gaea's Mountain is a modulated
  Voronoi with distortion; here `Noises.Cellular` under a warp, or `Ranges.*`), not from raw fBm.
* Rivers and lakes come before the general erosion and are kept out of it, or erosion fills them.
  Water is designed, not simulated: `Water.Network` on the forms (no detail), then `net:apply(forms +
  detail, { detail = detail })`; pick a river's `kind` for its valley and a lake's for its basin, and
  `generate` with `count = 0` when the map should have none. A sub-sea basin that touches the region's
  edge is sea.
* Strong erosion on the mountain component, then a mild pass over the combined terrain to blend it.
  Erosion features have a size in metres (Gaea's Feature Scale); keep it fixed across resolutions.
* Detail and erosion go only where masks select them: height, slope (`Masks.Slope`) and convexity
  (`h - Filters.Blur{h}`). Cliffs take noise and erosion on their near-vertical walls, not on the top.
* Strata follow the slope: they are exposed on faces and invisible on flat ground (WM Strata moves
  ground sideways along the normal). Strata from terracing height plus a tilted plane stripe the flats.
* One ramp, not a sawtooth of gradients, when a gradient drives terraces under erosion.
* A heightfield stretches detail above about 45 degrees; a wall needs enough horizontal falloff to
  hold detail, and overhangs are out of reach.
* Detail noise continues the spectrum above the scale of the forms: with lacunarity 2 an octave
  amplitude ratio of 2^((2 - beta) / 2) gives a spectral slope beta (0.5 for beta = 4).
* Erode coarse to fine: the pass with the largest feature size first, finer ones after, or the late
  passes wipe the detail. Several short passes keep a shape; one long pass replaces it.
* Use an erosion as a modifier, not only as the result: the difference between before and after
  isolates debris and broken terraces, and blending the eroded terrain back with the uneroded one
  recovers crags. Terraces (or cells cracked into them) before erosion give it obstacles to break.
* Blocky rock: shrink then expand (`Filters.Shrink` then `Filters.Expand`, a morphological opening);
  sharp creases: shrink. Break points of cliffs come from distorted cells (`Noises.Cellular`).
* Protect cliff faces from fluvial erosion: no channels down a wall, and sediment passes over it
  instead of piling against it. Add talus late or under a mask, or it rounds what should stay sharp.
* Drainage last on the large scale: once the drainage is made valid (every cell with a lower outlet),
  anything that moves the ground afterwards breaks it again; the pit metric shows it.
* Heavy grid erosion leaves straight, axis-aligned channels; warp the result (as `Ranges.Carve` does)
  and check the axis/diagonal power.

Learnt rebuilding `terrains/cliffs.lua` in stages (each rule cost a measured failure):

* A displacement has to span a few samples to be seen: a bed set back 2.5 m on a wall sampled every 8 m
  is invisible. Review fine structure at 2 m in a small window.
* A sharp terrace's residue (a sawtooth) jumps once per bed and aliases into spikes on a steep wall; use a
  soft terrace (sharpness about 0.5), whose residue is continuous.
* Detail on gentle ground holds water when it is steeper than the ground: amplitude A at wavelength L
  makes slopes of about 2 pi A / L. Keep that under the local slope, or use oriented detail
  (`Erosion.Gullies` from the ground's own dip) that closes no basin. Local erosion does not drain it.
* Anything faded out in the downslope direction ends in a dam: a gully guided by a slope that flattens
  at the shore, a mask that fades on land. Guide by a field whose slope never vanishes (a distance).
* Two regional slopes measured from different lines meet in a trough; measure them from one edge so
  they meet in a ridge. Flat treads hold water: give benches a slight outward lean.
* `Erosion.Gullies` spaces its stripes by the gradient's length: give it a gradient scaled to a slope
  like 0.3 when the real one is small, since only its direction matters there.
* A modulation meant to vary along one axis must be sampled on that axis only (`position` with a
  constant coordinate), or it ripples everything it touches.
* Find pits with `terrain_metrics <dir> --pits out.png` before guessing where they come from.
* Iterative ops are written for 2 m and scaled to the spacing: 30 thermal iterations reach 60 m at 8 m.
  A thermal pass also relaxes a wall hundreds of metres high far too slowly to build a talus apron;
  build aprons analytically (a 35 degree wedge from the wall foot, Max with the terrain) and cap the
  wedge by the wall's rise, not by its local height, which is zero where the apron lies.
* A field that a regional slope is measured from must not carry fine displacement: cracks of 30 m in
  the divide rippled a 2 % dip into pits.
* Drainage-scale forms (valleys of a lower slope) come from `Erosion.StreamPower` on a Lua grid;
  rebuild what it needs in Lua, seed shallow trenches where valleys must go, put back the ground it
  should not touch (inland, under the sea), and do shore work on the same grid so it lands on the coast.
* Distance-space subtraction (`Combine.SmoothMax{ a = land, b = width - capsule }`) cuts re-entrants
  and chutes into any edge; a circle gives a keyhole, a capsule open seaward a U.
* The axis/diagonal power measures orientation when the main form runs along an axis (a coast along x
  reads in the thousands); it flags grid artefacts only where the forms are not axis-aligned.
