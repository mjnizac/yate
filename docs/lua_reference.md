# Lua reference

Every function the engine exposes to a script, with what each parameter does, what to set it to, what
it costs, and a picture of the result.

`docs/lua_api.md` is the index: the shape of the API, its rules and one line per op. This is the long
one — every parameter with its default and valid range, what to set it to, what it costs and a picture
of the result — organised by what you are trying to build rather than by namespace.

## How to read the pictures

Every image on this page is the same 2048 m square at 4 m per sample, exported with the same command and
differing only in the parameter being shown:

```bash
build/Release/bin/terrain_export --script assets/scripts/gallery.lua --param case=<name> --min 0,0 --max 2048,2048 --resolution 4.0 --section 256 --out out/
```

`assets/scripts/gallery.lua` carries one case per image and nothing else, so any of them can be
regenerated or changed. The files committed under `docs/images/lua/` are those exports box-filtered to
half size and 8 bits, which is 2 MB for the set instead of 18.

Grayscale images are heightfields or masks; the caption says the range they are normalised against. The
colour ones are vector outputs, where red, green and blue carry `x`, `y` and `z` remapped from `[-1, 1]`.

Two things to keep in mind when reading a number off this page. **Sample spacing matters**: anything
measured per cell — a talus, a blur radius, a slope — means something different at 1 m than at 8 m, and
the text says so wherever it applies. **Frequency and amplitude are not independent**: a 400 m hill
every 500 m is a spike, and the same 400 m over 5 km is a mountain. §Picking a frequency has the
arithmetic.

---

## 1. The shape of a script

```lua
local function main(params)
    local base = Noises.fBm{ frequency = 0.0002, octaves = 6, amplitude = 400.0, seed = params.seed }
    return {
        height  = { value = base.value, range = { -400, 400 } },
        normals = { value = Terrain.Normals{ input = base.gradient } },
    }
end

return main
```

`main` receives `params` and returns a table of outputs. Building the graph computes nothing: every call
returns a handle immediately, and the engine evaluates once, on the GPU, after the script has run.

| `params` field | Meaning |
| --- | --- |
| `params.seed` | the job seed, from `--seed` |
| `params.resolution` | metres per sample |
| `params.bounds.min`, `.max` | world bounds as `{ x, y, z }`, also as `min_x` … `max_z` |
| `params.<key>` | every `--param key=value`; numeric-looking values arrive as numbers |

Each output is `{ value = <handle>, range = { lo, hi } }`. `range` is required for a scalar output
because that is what the 16-bit file is normalised against; a vector output always remaps `[-1, 1]` and
can be written as a bare handle. Outputs are requested in **name order**, not table order.

### Handles, channels and operators

A handle is one channel of one node.

| Access | Produces |
| --- | --- |
| `h.value` | channel 0, which every op has |
| `h.gradient`, `h.flow` | the op's named second channel, where it has one |
| `h.x`, `h.y`, `h.z`, `h.w` | one component, `Rn -> R1` |
| `a + b`, `a - b`, `a * b`, `a / b`, `-a` | pointwise arithmetic; either side may be a number |

A channel nothing asks for is never computed and never given a buffer, so `Noises.fBm` that is only used
for its value costs exactly one buffer.

### Mappings

| Mapping | What it usually is |
| --- | --- |
| `R2->R1` | heightmap, mask |
| `R2->R2` | gradient, flow field |
| `R2->R3` | normal map |
| `R3->R1` | density field |

Arithmetic is generic when both sides match, and broadcasts an `Rn->R1` over an `Rn->Rm`.

---

## 2. What a graph costs

Three ops read their neighbours: `Filters.Blur`, `Terrain.Gradient` and the two erosions. Their radius
propagates **backwards** to everything that feeds them, so every op upstream computes a border beyond
its own section. That border is the halo, and it is the main thing that decides what a script costs.

| Op | Radius it adds |
| --- | --- |
| `Filters.Blur` | `radius` |
| `Terrain.Gradient` | 1 |
| `Erosion.Thermal` | `iterations`, scaled to the spacing (see §9) |
| `Erosion.Hydraulic` | `iterations`, scaled to the spacing |

Radii **add** along a chain. A graph ending in `Hydraulic{iterations=24} → Thermal{iterations=12} →
Gradient` has a halo of 37 at the noise, so a 512-sample section evaluates 586² samples instead of 512²,
which is 31% more work. At 200 hydraulic iterations it would be 912² — 3.2 times the work — and the
engine refuses a halo above 255 with a message naming the node.

```
terrain.lua:9: [compile] Noise would need a halo of 400, over the limit of 255; every op between here
and an output adds its radius, and an iterative op contributes one per iteration
```

The practical consequence: **iteration counts are a cost, not a quality dial.** Before raising one, ask
whether a lower count at a coarser resolution gives the same shape.

---

## 3. Generation

### `Noises.fBm`, `Noises.Ridged`, `Noises.Billow`

Fractal noise with its exact analytic gradient, out of the same evaluation as the value. `Noises.Ridged`
and `Noises.Billow` are the same op with a different base function; `Noises.fBm{ kind = "ridged" }` is
the same thing written the other way.

| Key | Type | Default | Valid | Meaning |
| --- | --- | --- | --- | --- |
| `kind` | string | `"simplex"` | `simplex`, `ridged`, `billow` | base function; only on `fBm` |
| `domain` | number | `2` | 2 or 3 | `R2` is `(x, z)`, `R3` is `(x, y, z)` |
| `frequency` | number | `0.002` | `> 0` | cycles per metre: wavelength is `1 / frequency` |
| `octaves` | number | `6` | 1 … 32 | how many scales are summed |
| `lacunarity` | number | `2.0` | `> 0` | frequency multiplier per octave |
| `persistence` | number | `0.5` | `> 0` | amplitude multiplier per octave |
| `amplitude` | number | `1.0` | any | output half-range when `normalize` is set |
| `offset` | number | `0.0` | any | added after accumulation |
| `seed` | number | `0` | any | salt, decorrelates nodes sharing the job seed |
| `normalize` | boolean | `true` | | divide by the sum of amplitudes |

Channel 0 is the value, channel 1 is `gradient` (`R2->R2` or `R3->R3`).

**The three base functions**, all at `frequency = 0.0012`, 6 octaves, over `[-1, 1]`:

| `simplex` | `ridged` | `billow` |
| --- | --- | --- |
| ![simplex](images/lua/noise_simplex.png) | ![ridged](images/lua/noise_ridged.png) | ![billow](images/lua/noise_billow.png) |
| Smooth hills and hollows. The default, and what a base terrain should be | Sharp crests. Note it sits high in the band: ridged is `1 - |n|` in spirit, so its mean is well above zero | Rounded lumps. Useful for dunes, clouds and boulder fields |

#### Picking a frequency

This is the parameter that decides whether the result reads as terrain or as spikes, and it cannot be
chosen without the amplitude. A wave of amplitude `A` and wavelength `L` has a maximum slope of
`2πA/L`. With `lacunarity = 2` and `persistence = 0.5` every octave contributes the **same** slope, so
the whole stack is about `octaves` times that.

Measured at 8 m sampling with `amplitude = 400` and 6 octaves:

| `frequency` | Wavelength | Mean slope | Reads as |
| --- | --- | --- | --- |
| `0.002` | 500 m | 62° | a field of spikes |
| `0.0005` | 2 km | 27° | steep alpine |
| `0.0002` | 5 km | 11° | mountain terrain |

Rule of thumb: **wavelength of the base octave ≳ 10 × amplitude**. For 400 m of relief that is a 4 km
wavelength, `frequency = 0.00025`.

| `frequency = 0.0004` | `0.0012` | `0.004` |
| --- | --- | --- |
| ![f 0.0004](images/lua/noise_frequency_4.png) | ![f 0.0012](images/lua/noise_frequency_12.png) | ![f 0.004](images/lua/noise_frequency_40.png) |

#### `octaves`

Each octave adds detail at half the wavelength and half the amplitude. Past the point where an octave's
wavelength is about twice the sample spacing it adds nothing but aliasing.

| 1 | 3 | 8 |
| --- | --- | --- |
| ![1](images/lua/noise_octaves_1.png) | ![3](images/lua/noise_octaves_3.png) | ![8](images/lua/noise_octaves_8.png) |

**Useful ceiling:** `octaves ≤ log2(1 / (frequency × 2 × resolution))`. At `frequency = 0.0012` and 4 m
per sample that is 6.7, so 8 octaves is already past the point of return — compare the last two images.

| Use case | `octaves` |
| --- | --- |
| A mask or a large-scale shape | 2–3 |
| A base terrain for export at 1–4 m | 5–7 |
| Detail for a close-up at sub-metre resolution | 8–10 |

#### `persistence` and `lacunarity`

`persistence` is roughness: how much each successive octave contributes.

| 0.3 | 0.5 | 0.8 |
| --- | --- | --- |
| ![0.3](images/lua/noise_persistence_30.png) | ![0.5](images/lua/noise_persistence_50.png) | ![0.8](images/lua/noise_persistence_80.png) |

| Value | Result |
| --- | --- |
| 0.3–0.4 | smooth, rolling. Good under erosion, which adds its own detail |
| 0.5 | the default, and the point where every octave contributes equal slope |
| 0.7–0.9 | harsh and noisy. Rarely what a heightfield wants; fine for a detail layer multiplied into a mask |

`lacunarity` is the frequency step. 2.0 is the default because it is the value at which octaves tile the
spectrum without gaps or overlap; moving off it changes how "organised" the result looks.

| 1.5 | 2.0 | 3.5 |
| --- | --- | --- |
| ![1.5](images/lua/noise_lacunarity_15.png) | ![2.0](images/lua/noise_lacunarity_20.png) | ![3.5](images/lua/noise_lacunarity_35.png) |

Below 2 the octaves overlap and the result looks blurred and self-similar; above 2.5 they separate and
the result looks like two unrelated layers stacked. Use 1.9–2.1 unless you want that effect; an
irrational-ish value such as 2.137 avoids octaves lining up into visible grid artefacts.

#### `normalize`, `amplitude` and `offset`

With `normalize = true` the octave sum is divided by the sum of the amplitudes, so the output band is
`offset ± amplitude` and you can declare an export range with confidence. With it cleared, `amplitude`
is the **first octave's** amplitude and the total is larger and harder to predict.

Leave it set unless you are reproducing a reference implementation.

#### Combines with

`Terrain.Normals{ input = base.gradient }` for free exact normals · `Masks.Slope` on the gradient to
select by steepness · `Combine.Blend` to layer a second noise · `Curves.*` to shape the distribution ·
the erosions to turn it into landforms.

#### Example

```lua
-- A base terrain with 400 m of relief over a 5 km base wavelength, and its exact normals.
local base = Noises.fBm{
    frequency   = 0.0002,
    octaves     = 6,
    lacunarity  = 2.0,
    persistence = 0.45,
    amplitude   = 400.0,
    seed        = params.seed,
}

return {
    height  = { value = base.value, range = { -400, 400 } },
    normals = { value = Terrain.Normals{ input = base.gradient } },
}
```

### `Terrain.Const`

| Key | Default | Meaning |
| --- | --- | --- |
| `domain` | `2` | 2 or 3 |
| `value` | `0.0` | the constant |

Produces `Rn -> R1`. Mostly you do not need it: a bare number works wherever a field is expected, and
the compiler folds a constant operand into the consuming kernel as an immediate, saving a dispatch and
a buffer. Reach for it when you need a constant as the `a` or `b` of a `Combine.Blend`, where both
sides must be handles.

### `Terrain.Coords`

| Key | Default | Meaning |
| --- | --- | --- |
| `domain` | `2` | 2 or 3 |

The **world position, in metres**, of each sample, as `Rn -> Rn`. This is how a script builds anything
positional — an island falloff, a linear gradient, a region mask.

It is metres at any sample spacing, so a script that places a feature at 1024 m places it there whether it
is exported at 1 m or previewed at 8 m. (It used to return the integer sample *index*, which made every
positional script describe different terrain at every resolution unless it multiplied by
`params.resolution` by hand.)

On an `R2->R2` the two components are the plane's own axes, `x` and `z`: **`.z` is the second component**,
and `.y` is the same one by position, so both spellings work. In `R3` the three components are `.x`, `.y`,
`.z` as you would expect.

![coords](images/lua/terrain_coords.png)
*`Terrain.Coords{}.x` over `[0, 2048]` across a 2048 m square: 0 m on the left, 2048 m on the right.*

#### Example: a circular island falloff

```lua
local p      = Terrain.Coords{ domain = 2 }       -- metres, at any resolution
local centre = 1024.0
local dx     = p.x - centre
local dz     = p.z - centre
local dist2  = dx * dx + dz * dz                   -- squared metres from the centre

-- Squared, because `^` is not one of the overloaded operators and there is no square-root op. Work
-- in squared distance and square the bounds instead: it is exact and it is one dispatch cheaper.
local inner, outer = 300.0, 900.0
local land = Curves.Smoothstep{ input = -dist2, min = -(outer * outer), max = -(inner * inner) }
local base = Noises.fBm{ frequency = 0.002, amplitude = 400.0, offset = 200.0 }

return { height = { value = base.value * land - 200.0, range = { -600, 600 } } }
```

![island](images/lua/recipe_island.png)
*The result: terrain inside the falloff, flat sea outside, over `[-600, 600]`.*

Note `Smoothstep` with a negated input and negated bounds: the curve ramps upward, so to fall off with
distance you feed it the negative.

---

## 4. Analysis

### `Terrain.Normals`

| Key | Default | Meaning |
| --- | --- | --- |
| `input` | required | a gradient, `R2->R2` |
| `vertical_scale` | `1.0` | multiplies the slope before normalising |

Takes a **gradient**, not a height, and produces `R2->R3`. Given a handle whose node exposes a gradient
channel it resolves it, so `Terrain.Normals{ input = base }` works for a noise handle.

![normals](images/lua/terrain_normals.png)
*Normals of a steep base. Green dominates because `y` is up; red and blue carry `x` and `z`.*

`vertical_scale` above 1 exaggerates relief, which is what you want when a renderer lights a heightfield
whose vertical exaggeration differs from the data. Below 1 flattens it.

| Use case | `vertical_scale` |
| --- | --- |
| Export to match the heightmap exactly | `1.0` |
| A game engine that draws the terrain at 2× vertical scale | `2.0` |
| Taking the edge off a noisy field for lighting only | `0.5` |

#### Combines with

Always with a gradient: either a noise node's exact `.gradient`, or `Terrain.Gradient` for a field that
has been through blends, curves or erosion.

```lua
-- Exact, and free: the same dispatch that produced the value produced this.
Terrain.Normals{ input = base.gradient }

-- Measured, radius 1, for a height that no longer has a closed form.
Terrain.Normals{ input = Terrain.Gradient{ input = eroded } }
```

### `Terrain.Gradient`

| Key | Default | Meaning |
| --- | --- | --- |
| `input` | required | `Rn -> R1` |

Central differences, producing `Rn -> Rn`. **Radius 1, so it puts a halo on everything upstream**, and
it is the only op in the engine that is not analytic.

![gradient](images/lua/terrain_gradient.png)
*The gradient of a heightfield: `x` in red, `z` in green, remapped from `[-1, 1]`.*

Use it only when there is no closed form left to differentiate — after erosion, after a blend, after a
curve. For a noise node, `.gradient` is exact and costs nothing extra.

### `Masks.Slope`

| Key | Default | Meaning |
| --- | --- | --- |
| `input` | required | a gradient, `R2->R2` |
| `min` | `0.0` | slope at which the mask starts to rise |
| `max` | `1.0` | slope at which it reaches 1; must exceed `min` |

Produces `R2->R1` in `[0, 1]`: 0 where the terrain is flatter than `min`, 1 where it is steeper than
`max`, smooth in between. Slope is `m/m` — `tan(angle)`, not degrees. `min = 0.577` is 30°, `1.0` is 45°.

**Feed it the gradient of a smooth field, not of your base terrain.** Differentiating an fBm weights each
octave by its frequency, so the gradient of a six-octave field is dominated by the finest octave and the
mask flickers from sample to sample. Measured on the same graph at 8 m sampling, a blend driven by a
six-octave mask has a mean slope of 38°, and by a three-octave mask 13.6°, with the relief unchanged.

| Six-octave gradient | Three-octave gradient | Narrow band, three octaves |
| --- | --- | --- |
| ![noisy](images/lua/mask_slope_noisy.png) | ![shape](images/lua/mask_slope_shape.png) | ![tight](images/lua/mask_slope_tight.png) |
| `min = 0.3, max = 1.1` — flickers | the same thresholds, usable | `min = 0.6, max = 0.75` — a sharper selection |

#### Choosing thresholds

They have to bracket the slope your terrain actually has. Measure it rather than guess: export
`Terrain.Normals{...}.y` and convert, or just try `min`/`max` and look. For the base in these pictures
the slope distribution is p10 0.27, p50 0.69, p90 1.23, which is why `0.3 … 1.1` spans it.

| Use case | `min` | `max` |
| --- | --- | --- |
| Rock on cliffs, grass elsewhere | p50 | p90 |
| Snow only on gentle ground (invert with `1 - mask`) | p10 | p50 |
| A hard selection of the steepest ground | p80 | p85 |

#### Combines with

`Combine.Blend` as the `mask` · multiplied into another mask with `*` · `Curves.Power` to bias it
towards 0 or 1 without moving the thresholds.

---

## 5. Composition

### `Combine.Blend`

| Key | Default | Meaning |
| --- | --- | --- |
| `a`, `b` | required | same mapping |
| `mask` | required | `Rn -> R1` |

Linear interpolation: `a` where the mask is 0, `b` where it is 1. The mask is not clamped, so a mask
outside `[0, 1]` extrapolates — use `Curves.Clamp` if that is a risk.

![blend](images/lua/combine_blend.png)
*Smooth base blended toward ridged peaks by a slope mask, over `[-400, 400]`.*

The canonical use is layering: a smooth base everywhere, something sharper where the terrain is already
steep, so peaks sit on slopes instead of floating over flat ground.

### `Combine.Min`, `Combine.Max`

| Key | Meaning |
| --- | --- |
| `a`, `b` | the two fields |

| `Combine.Min` | `Combine.Max` |
| --- | --- |
| ![min](images/lua/combine_min.png) | ![max](images/lua/combine_max.png) |

`Max` keeps whichever field is higher, which unions two landforms — add a plateau to a mountain range
without cutting into it. `Min` intersects, which is how you carve: a canyon field `Min`-ed into a base
removes material wherever the canyon is lower. Both leave a crease where the two surfaces cross; soften
it with a small `Filters.Blur` if it reads as an edge.

#### Example: a plateau that never sinks below the terrain

```lua
local base    = Noises.fBm{ frequency = 0.0003, amplitude = 300.0 }
local plateau = Terrain.Const{ value = 120.0 }
return { height = { value = Combine.Max{ a = base.value, b = plateau }, range = { -300, 300 } } }
```

---

## 6. Value shaping

All four are pointwise, cost one dispatch and add no halo.

| Key | `Clamp` | `Remap` | `Power` | `Smoothstep` |
| --- | --- | --- | --- | --- |
| `input` | required | required | required | required |
| `min` | `0.0`, the floor | `0.0`, output minimum | — | `0.0`, where the ramp starts |
| `max` | `1.0`, the ceiling | `1.0`, output maximum | — | `1.0`, where it ends; must exceed `min` |
| `exponent` | — | — | `1.0`, in `[0, 64]` | — |

| Input | `Clamp{-0.3, 0.3}` | `Remap{0, 1}` |
| --- | --- | --- |
| ![none](images/lua/curve_none.png) | ![clamp](images/lua/curve_clamp.png) | ![remap](images/lua/curve_remap.png) |

| `Power{0.5}` | `Power{3.0}` | `Smoothstep{-0.2, 0.2}` |
| --- | --- | --- |
| ![p05](images/lua/curve_power_half.png) | ![p3](images/lua/curve_power_three.png) | ![ss](images/lua/curve_smoothstep.png) |

**`Clamp`** flattens everything past its bounds into plateaus and floors. Use it to put a hard sea level
under a terrain, or to stop erosion's overshoot from widening your export range.

**`Remap`** takes the canonical `[-1, 1]` that a normalised noise produces into `[min, max]`. It assumes
that input band — it is `min + (max - min) × (v × 0.5 + 0.5)` — so feeding it something else shifts the
result. For an arbitrary field, scale with arithmetic instead.

**`Power`** biases a distribution while preserving sign, so a signed field stays signed under an even
exponent. Exponent below 1 pushes values towards the extremes (more area at high and low); above 1 pulls
them towards zero, which on a mask means "only the strongest parts survive".

| Use case | `exponent` |
| --- | --- |
| Make a mask more selective | 2–4 |
| Make a mask more generous | 0.3–0.6 |
| Deepen valleys without touching peaks (on a `[0,1]` height) | 1.5–2.5 |

**`Smoothstep`** is the one to reach for when you want a mask out of a scalar: 0 below `min`, 1 above
`max`, with a smooth transition. It is how you turn a height, a distance or a dot product into a weight.

```lua
-- Everything above 600 m is snow, with a 150 m transition.
local snow = Curves.Smoothstep{ input = height, min = 600.0, max = 750.0 }
```

---

## 7. Filtering

### `Filters.Blur`

| Key | Default | Valid | Meaning |
| --- | --- | --- | --- |
| `input` | required | | any mapping |
| `radius` | `1` | 1 … 32 | radius in **samples**, not metres |

| `radius = 1` | `8` | `32` |
| --- | --- | --- |
| ![b1](images/lua/blur_1.png) | ![b8](images/lua/blur_8.png) | ![b32](images/lua/blur_32.png) |

Two things to watch.

**The radius is in samples**, so the same script blurs a different distance at a different resolution.
If you want a blur of `D` metres, write `radius = math.max(1, math.floor(D / params.resolution))`.

**It is the most expensive halo in the engine per unit of effect**: radius 32 adds 32 to the halo of
everything upstream, which on a 512 section is 576² instead of 512², a 27% surcharge for one op. Prefer
lowering the noise's `octaves` or `persistence` over blurring the detail away afterwards — it is free,
and it does not smear the large scales along with the small.

#### Use cases

| Use case | `radius` |
| --- | --- |
| Softening the crease left by `Combine.Min`/`Max` | 1–2 |
| Smoothing a mask so a blend transition is not pixelated | 3–8 |
| Building a large-scale "shape" field to drive a mask | 16–32, or better, a 2-octave noise for free |

---

## 8. Vectors

### `Vec.Combine`

Takes a positional list of 2 to 4 scalars and builds an `Rn -> Rm`.

```lua
local p    = Terrain.Coords{ domain = 2 }
local warp = Vec.Combine{ p.x * 0.5, p.z * 0.5 }
```

Component access is the other direction: `h.x`, `h.y`, `h.z`, `h.w` pull one component out as `Rn -> R1`.

---

## 9. Erosion

Both are iterative, and both charge their iteration count as halo. Both take an `Rn -> R1` height.

> **Units.** Slope is rise over run in both ops, so a script means one thing at any sample spacing:
> `talus = 0.7` is about 35° at 1 m and at 16 m. (Both used to be a height difference *per cell*, which
> made the same value a steeper slope at a finer spacing. Measured on a thermal-only graph at 8 m and
> 16 m over the same world positions, the difference went from 8.0 m on average to 0.02 m.)
>
> **Iterations are written for 2 m and scaled to the spacing in use.** One iteration moves material one
> *cell*, so run unchanged a script eroded by different amounts at different spacings, and a viewer that
> changes level as the camera moves showed terrain that re-eroded on every zoom. The engine now runs the
> count that gives the same amount of erosion where the cells are a different size:
>
> * iterations are multiplied by `(2 m / spacing)^2`: 24 written is 96 at 1 m, 24 at 2 m, 6 at 4 m, 2 at 8 m
>   and 1 from 16 m. A finer spacing than 2 m is capped at 96 (or at what the script wrote, if more),
>   because the count is also the halo;
> * for hydraulic erosion the per-iteration `rain`, `evaporation`, `erosion_rate` and `deposition` are
>   multiplied by `spacing / 2 m` (capped at their limits), so the totals over the run match.
>
> The log says what ran: `Erosion.Hydraulic at 8 m: 24 iteration(s) written for 2 m run as 2`. Measured on
> one 2 km terrain, the same world position at 4, 8 and 16 m differed by 0.06 to 0.24 m on average for
> thermal erosion (0.8 to 2.2 m before) and by 3.4 to 6.4 m for hydraulic (6.7 to 16.0 m before). It is an
> approximation: hydraulic erosion carves channels at the scale of the cell, so a coarser grid cannot show
> the same ones. What it does mean is that a preview at 8 m shows little erosion, because at that spacing
> there is little to show: zoom in and the level, and the erosion, follow.

### `Erosion.Thermal`

Material slides downhill wherever the drop to a neighbour exceeds the talus. Conservative: what a cell
gives away, its neighbours receive.

| Key | Default | Valid | Meaning |
| --- | --- | --- | --- |
| `input` | required | | `Rn -> R1` |
| `iterations` | `16` | ≥ 1 | passes, and the influence radius |
| `talus` | `0.7` | ≥ 0 | slope (rise over run, `tan` of the angle) below which nothing moves |
| `strength` | `0.25` | `(0, 0.5]` | fraction of the excess moved per iteration |

| No erosion | 8 iterations | 32 | 96 |
| --- | --- | --- | --- |
| ![none](images/lua/erosion_none.png) | ![t8](images/lua/erosion_thermal_8.png) | ![t32](images/lua/erosion_thermal_32.png) | ![t96](images/lua/erosion_thermal_96.png) |

All three at `talus = tan(35°) = 0.70` and `strength = 0.3`, at 4 m per sample.

`strength` above 0.5 is rejected, and the reason is stability rather than taste: a cell can overshoot its
neighbour and the field oscillates instead of settling.

| Use case | `iterations` | `talus` | `strength` |
| --- | --- | --- | --- |
| Take the edge off noise, cheaply | 4–8 | `tan(40°)` = 0.84 | 0.3 |
| Talus slopes and scree below cliffs | 24–48 | `tan(33°)` = 0.65 | 0.3 |
| Settle the spoil left by hydraulic erosion | 8–16 | `tan(35°)` = 0.70 | 0.25 |
| A heavily weathered, rounded landscape | 64–128 | `tan(25°)` = 0.47 | 0.4 |

A lower talus erodes more, because more of the terrain is above the threshold. A high iteration count
with a high talus does almost nothing; a low talus with few iterations rounds only the sharpest edges.

### `Erosion.Hydraulic`

Rain, flow, sediment transport, evaporation. Carves channels and deposits fans.

| Key | Default | Valid | Meaning |
| --- | --- | --- | --- |
| `input` | required | | `Rn -> R1` |
| `iterations` | `32` | ≥ 1 | passes, and the influence radius |
| `rain` | `0.02` | ≥ 0 | water added per cell per iteration |
| `evaporation` | `0.05` | `[0, 1)` | fraction of water lost per iteration |
| `capacity` | `4.0` | ≥ 0 | sediment a unit of flow holds per unit of slope (rise over run) |
| `erosion_rate` | `0.3` | `(0, 1]` | how fast a shortfall is taken from the bed |
| `deposition` | `0.3` | `(0, 1]` | how fast an excess is laid back down |
| `flow_rate` | `0.15` | `(0, 0.25]` | fraction of a surface drop that moves per iteration |

| No erosion | 8 iterations | 32 |
| --- | --- | --- |
| ![none](images/lua/erosion_none.png) | ![h8](images/lua/erosion_hydraulic_8.png) | ![h32](images/lua/erosion_hydraulic_32.png) |

**`rain` is the parameter that decides whether anything happens.** The total work the water can do is
proportional to the total rainfall, and the default of 0.02 over 32 iterations is 0.64 m of water across
hundreds of metres of relief — about one metre of bed moved, which is invisible. Measured on a 400 m
terrain, 24 iterations at 8 m sampling:

| `rain` | Relief after | What you see |
| --- | --- | --- |
| 0.02 | 358 m (from 391) | essentially unchanged |
| 0.2 | 350 m | channels appear |
| 0.5 | 338 m | a drainage network |
| 10 | 341 m | the same: the op saturates, because a cell may not cut below its lowest neighbour |

So **0.2–0.5 is the working range**, and raising it further is wasted.

`flow_rate` above 0.25 is rejected: the four outflows of a cell could exceed the water it holds and the
scheme stops being stable.

| Use case | `iterations` | `rain` | `capacity` | Notes |
| --- | --- | --- | --- | --- |
| A hint of drainage, cheap | 8–12 | 0.3 | 16 | halo 12 is affordable on any section |
| Visible valleys and fans | 24–32 | 0.5 | 24–48 | the general-purpose setting |
| Deep, mature channels | 48–64 | 0.5 | 32–64 | halo 64 is 29% more work on a 512 section |
| Arid, little deposition | 24 | 0.3 | 12 | raise `evaporation` to 0.15 |

The capacities are per unit slope now, so they are roughly four to eight times what they were when the
term was a drop per cell and these were tuned at 4–8 m. A given value means the same at any spacing.
Iteration counts are for 2 m and scaled (§9).

#### The `flow` channel

Channel 1 is `flow`: water on `x`, sediment on `y`, as they stood after the last iteration. A wetness map
and a sediment map, which is what a renderer wants for putting rock against silt.

| Water (`flow.x`) | Sediment (`flow.y`) |
| --- | --- |
| ![water](images/lua/erosion_flow_water.png) | ![sediment](images/lua/erosion_flow_sediment.png) |

Asking for it costs a second section buffer and the bandwidth to fill it; a script that only wants a
height pays for neither. You may ask for `flow` **without** asking for the height.

```lua
local base   = Noises.fBm{ frequency = 0.0006, octaves = 6, amplitude = 300.0 }
local carved = Erosion.Hydraulic{ input = base.value, iterations = 32, rain = 0.5, capacity = 6.0 }
return {
    height   = { value = carved, range = { -400, 400 } },
    wetness  = { value = carved.flow.x, range = { 0, 20 } },
    sediment = { value = carved.flow.y, range = { 0, 40 } },
}
```

### Water first, then thermal

The two combine in one order: hydraulic erosion cuts channels and piles the spoil at unnatural angles,
and thermal erosion settles that spoil into talus slopes.

![combined](images/lua/erosion_combined.png)
*`Hydraulic{32, rain = 0.5}` then `Thermal{16, talus = 4 × tan(35°)}`.*

Their radii add: this chain has a halo of 48 before anything else in the graph is counted.

---

## 10. Shapes and landforms

Noise gives texture and shapes give *intent*: a cone here, a gorge there, a mesa with a flat top. A
landform is a **shape** (where the edge is) run through a **profile** (how the ground leaves it) and
multiplied by a height. Everything below is built from ops the engine already evaluates on the GPU; Lua
only wires them together.

Two spaces, and keeping them apart is what makes the rest simple:

| Space | Unit | What it is | Made by |
| --- | --- | --- | --- |
| **Distance** | metres, negative inside | how far a sample is from a shape's edge | `Shapes.*`, `Geometry.*` |
| **Height** | metres | the terrain | `Landforms.*`, `Curves.Profile * height` |

Booleans (union, subtract) are done in distance space, with `Combine.Min`, `Combine.Max` and their
smooth versions; heights are combined as terrain, with `+`, `Combine.Max` or `Combine.SmoothMax`.

**Every shape takes `position`**, the field it is evaluated at. It defaults to `Terrain.Coords{}`, the
position in metres. Pass `Shapes.Warp{}` and the same cone is a hill instead of a drawing of one; pass
`Coords` run through your own noise and you choose how it bends.

### `Terrain.Transform`

Moves, turns and scales a position field. It is what places a shape, so one shape definition serves any
number of places.

| Key | Default | Meaning |
| --- | --- | --- |
| `position` | `Terrain.Coords{}` | the field to transform |
| `at` | `{ 0, 0 }` | where the shape's origin ends up, in metres |
| `rotation` | `0` | degrees; positive turns +x towards +z |
| `scale` | `1` | a number, or `{ sx, sz }` |
| `pivot` | `{ 0, 0 }` | the point of the shape's own space that `at` places and that rotation and scale are about |

The node it builds is the *inverse*: world positions to the shape's local space. Say where you want the
shape and the binding does the arithmetic, in double precision.

A scale of zero is an error. A non-uniform scale stops distances being distances (a circle scaled
`{ 2, 1 }` is an ellipse whose distance is not the Euclidean one), which is fine for a mask and not for a
profile that is meant to be a width in metres; use `Shapes.Ellipse` for that.

```lua
local function main(params)
    -- The same box in two places: placing is moving the positions it is evaluated at.
    local left  = Geometry.Box{ position = Terrain.Transform{ at = { -300, 0 }, rotation = 20 },
                                size = { 400, 120 } }
    local right = Geometry.Box{ position = Terrain.Transform{ at = { 300, 0 }, rotation = -35 },
                                size = { 400, 120 } }
    local both = Combine.Min{ a = left, b = right }
    return { distance = { value = both, range = { -100, 400 } } }
end
return main
```

### `Geometry.*`: signed distance to a primitive

The building blocks. Each is in its own local space (centred on the origin, unrotated) and returns the
signed distance in metres. Most scripts reach for `Shapes.*` below, which adds placement.

| Call | Keys | Distance |
| --- | --- | --- |
| `Geometry.Circle` | `radius` | `length(p) - radius`; radius 0 is a point |
| `Geometry.Ellipse` | `radii = { rx, rz }` | a bound on the distance: exact on the axes, approximate away from the edge |
| `Geometry.Box` | `size = { w, d }`, `rounding = 0` | exact, with rounded corners when `rounding > 0` |
| `Geometry.BoxChebyshev` | `size = { w, d }` | `max(\|x\| - w/2, \|z\| - d/2)`: the distance of a square pyramid's faces |
| `Geometry.Segments` | `points`, `closed = true` | a polygon (signed) or a polyline (unsigned) |

`size` is the full width and depth, which is what "a 400 by 200 box" means. `rounding` must not exceed
the smaller half-size.

**Polygons** are built as a chain of segment nodes, two segments per node, because a kernel has ten
parameter words and no buffers. The cost is real: a 40-gon is 20 passes over every sample. The limit is
128 segments, and the error says so. Inside versus outside is decided by casting a ray towards +x with a
half-open rule on z, so a vertex shared by two edges counts once and a sample exactly on a vertex is not
misclassified. The distance beside a concave corner is the distance to the nearest edge, not the
distance round it.

### `Shapes.*`: placed shapes, booleans and masks

Every constructor takes `at`, `rotation`, `scale`, `pivot` and `position`, as `Terrain.Transform` does.

| Call | Keys | Notes |
| --- | --- | --- |
| `Shapes.Circle` | `radius` | |
| `Shapes.Point` | `at` | a circle of radius 0: the distance to a point |
| `Shapes.Ellipse` | `radii = { rx, rz }` | `rotation` turns it |
| `Shapes.Rect` | `size`, `rounding`, `metric` | `metric = "chebyshev"` gives square faces; no rounding with it |
| `Shapes.Polygon` | `points` | signed; points are in the shape's own space, so `at` moves the whole outline |
| `Shapes.Polyline` | `points`, `closed` | unsigned; `closed` joins the last point to the first |
| `Shapes.Spline` | `points`, `subdivisions = 8`, `closed`, `fill` | a smooth curve through the points |

**`Shapes.Spline`** is a centripetal Catmull-Rom curve through your points, sampled `subdivisions` times
per span. Centripetal because it does not loop or overshoot when hand-placed points are unevenly spaced.
It is unsigned by default (the distance to the curve, which is what a ridge or a river needs); `fill =
true` on a closed spline is the signed distance to the region it encloses. The segment limit applies to
`spans * subdivisions`: ten points open at 8 subdivisions is 72 segments.

| Boolean | Keys | Result |
| --- | --- | --- |
| `Shapes.Union{ a, b, c, ..., smooth = k }` | | where any of them is; `smooth` rounds the seam over `k` metres |
| `Shapes.Intersect{ a, b, ..., smooth = k }` | | where all are |
| `Shapes.Subtract{ a = ..., b = ..., smooth = k }` | | `a` with `b` cut out |
| `Shapes.Mask{ shape, feather = 0 }` | | 1 inside and 0 outside, with a ramp `feather` metres each side; 0 is a hard edge |

`Shapes.Union` takes a positional list; `smooth` is the only named key.

**`Shapes.Warp{ amplitude = 60, frequency = 0.004, octaves = 3, seed = 0 }`** returns a position field
bent by noise: pass it as `position =` to any shape or landform. `amplitude` is the largest shift on each
axis in metres, and a distance is 1-Lipschitz, so a warped shape's edge never moves by more than
`amplitude * sqrt(2)`.

| Use | Recommended |
| --- | --- |
| An outline that reads as natural | `amplitude` 5-10% of the feature's size, `frequency` about 2 over its size |
| A visible meander | `amplitude` 20-30% of the feature, `frequency` 1 over its size |
| Do not | `frequency` higher than the sample spacing resolves: it aliases into a jagged edge |

```lua
local function main(params)
    local position = Shapes.Warp{ amplitude = 60.0, frequency = 0.004, seed = params.seed }

    local lake  = Shapes.Ellipse{ at = { 0, 0 }, radii = { 220, 140 }, rotation = 25, position = position }
    local islet = Shapes.Circle{ at = { 40, 10 }, radius = 40, position = position }
    local water = Shapes.Subtract{ a = lake, b = islet, smooth = 15.0 }

    -- 1 over the lake with a 20 m shore ramp.
    local mask = Shapes.Mask{ shape = water, feather = 20.0 }
    return { lake = { value = mask, range = { 0, 1 } } }
end
return main
```

### `Curves.Profile`

Turns a signed distance into a factor in `[0, 1]`: 0 outside and on the edge, 1 inside. Multiply it by a
height and it is a landform.

| `kind` | Keys | Shape of the cross-section |
| --- | --- | --- |
| `"linear"` | `width` | a straight ramp, 0 at the edge and 1 at `width` metres inside: a cone, a pyramid, a tent ridge |
| `"smooth"` | `width` | the same ramp eased at both ends: a rounded hill |
| `"dome"` | `width` | vertical at the edge and flat at the top: a dome, a crater wall |
| `"gaussian"` | `sigma` | 1 inside and falling as a Gaussian outside, with no edge at all |
| `"plateau"` | `edge = 0`, `falloff` | 1 from `edge` metres inside the outline, falling to 0 `falloff` metres outside it |

Giving a key the kind does not use is an error that names the one it does take.

`plateau` is the scarp: with `edge` and `falloff` small against the height, the wall is steep and the top
is flat. `edge = 6, falloff = 60` on a 300 m mesa is a wall about 66 m across for 300 m of rise, which is
a 78 degree face.

| Want | Kind and values |
| --- | --- |
| A cone | `linear`, `width` = the radius |
| A concave volcano flank | `linear`, then `Curves.Power{ exponent = 1.5 }` |
| A rounded hill | `smooth`, `width` = the radius |
| A crater or a dome | `dome`, `width` = the radius |
| A mesa or a platform | `plateau`, `edge` 2-8 m, `falloff` 20-80 m |
| A soft swell with no defined edge | `gaussian`, `sigma` about a third of the footprint |

### `Curves.Terrace`

Snaps a **height** to steps. Not a profile: it takes metres of altitude.

| Key | Default | Meaning |
| --- | --- | --- |
| `step` | `10` | height of one stratum, metres, greater than zero |
| `sharpness` | `0.5` | `0` eases the whole step, so the ground wavers up in S-curves; `1` is a hard riser between flat treads |

For strata that read as rock, `step` 20-50 m and `sharpness` 0.5-0.7; at 1.0 the treads are flat and the
risers vertical, which looks like a staircase. Apply it through a blend so only the walls step:
`Combine.Blend{ a = land, b = Curves.Terrace{ input = land, ... }, mask = walls }`.

### `Combine.SmoothMin` and `Combine.SmoothMax`

`Combine.Min` and `Combine.Max` with the corner rounded over `k`.

| Key | Default | Meaning |
| --- | --- | --- |
| `a`, `b` | required | the operands |
| `k` | `1` | blend width in the units of the operands, greater than zero |

Where `a` and `b` differ by more than `k` the result is exactly `Min` or `Max`; inside it the corner is
cut by at most `k / 4`. Both are symmetric in `a` and `b`. For heights `k` is metres of altitude, so
`k = 100` rounds a seam between two 500 m ridges over a 100 m band of heights, which is a wide, soft
shoulder. For distances it rounds the join of two shapes.

| Use | `k` |
| --- | --- |
| Joining two landforms so the seam is not a crease | 5-15% of the lower one's height |
| Rounding the corner where two shapes touch | 5-10% of their size |
| A crater lip against a ceiling | 20-40 m |

### `Landforms.*`: heights from shapes

Each returns a height in metres over `R2 -> R1`. `height` and `depth` may be a number or a field, so a
landform can be scaled by a mask. All take `at`, `rotation` and `position`; the round ones have no use for
`rotation`.

| Call | Keys | What it is |
| --- | --- | --- |
| `Landforms.Cone` | `radius`, `height`, `profile = "linear"` | `height` at the centre falling to 0 at `radius` |
| `Landforms.Dome` | `radius`, `height`, `profile = "dome"` | steep at the foot, flat on top |
| `Landforms.Bowl` | `radius`, `depth`, `profile = "smooth"` | a round depression |
| `Landforms.Cylinder` | `radius`, `height`, `edge = 2` | a flat-topped rise with a wall `edge` metres either side of `radius` |
| `Landforms.Plateau` | `size`, `height`, `edge = 2`, `falloff = 20`, `rounding` | a flat-topped, rotated rectangle with a scarp |
| `Landforms.Pyramid` | `size`, `height` | square faces; a rectangle gives a ridge at the top |
| `Landforms.Ridge` | `points`, `width`, `height`, `profile = "smooth"`, `subdivisions = 8`, `closed` | a raised band along a spline |
| `Landforms.Trench` | `points`, `width`, `depth`, `profile = "smooth"`, `subdivisions = 8`, `closed` | the same, cut |

`profile` is one of `linear`, `smooth`, `dome`, `gaussian` for the ones that take it. For a ridge the
cross-section is the profile of the distance to the spline minus half the width, so `width` is the full
width of the band at its base and the crest runs along the spline.

| Want | Call |
| --- | --- |
| A volcano with a crater | `Landforms.Cone` raised to a power, cut with `Shapes.Mask` and `Combine.SmoothMin` (see `volcanic_island.lua`) |
| A mountain range | several `Landforms.Ridge`, joined with `Combine.SmoothMax`, peaks as `Cone`s on the crest |
| A river valley or a gorge | `Landforms.Trench`, or `Curves.Profile{ kind = "plateau" }` on a spline for vertical walls |
| A mesa field | `Landforms.Plateau`, several sizes and bearings, with `Pyramid` and `Dome` buttes |
| A crater | `Landforms.Bowl` with `profile = "dome"`, or `Landforms.Cylinder` with a negative `height` |

**Combines with.** `Combine.SmoothMax` to merge relief; `+` to stack a landform on a base; `Noises.*`
for what shapes cannot give (grain, scatter); `Erosion.Thermal` with a high talus to drop scree at the
foot of a scarp without rounding the top; `Filters.Blur{ radius = 1 }` after hydraulic erosion.

```lua
local function main(params)
    local position = Shapes.Warp{ amplitude = 80.0, frequency = 0.0025, seed = params.seed }

    local hill  = Landforms.Dome{ at = { -150, 0 }, radius = 260, height = 90, position = position }
    local mesa  = Landforms.Plateau{ at = { 160, 40 }, size = { 300, 200 }, height = 140,
                                     rotation = 20, edge = 4, falloff = 40, position = position }
    local ridge = Landforms.Ridge{ points = { { -400, -200 }, { 0, -260 }, { 400, -180 } },
                                   width = 160, height = 100, position = position }
    local gorge = Landforms.Trench{ points = { { -400, 220 }, { 0, 180 }, { 400, 260 } },
                                    width = 90, depth = 60, position = position }

    local land = Combine.SmoothMax{ a = Combine.SmoothMax{ a = hill, b = mesa, k = 20.0 },
                                    b = ridge, k = 20.0 } + gorge
    return { height = { value = land, range = { -100, 250 } } }
end
return main
```

### The reference terrains

Five complete terrains in `assets/scripts/shapes/`. Each runs, exports and previews, has its parameters
at the top as `params.*` with defaults, and is also exercised by `test_geometry`. The pictures are
`terrain_viewer` output.

| Terrain | What it demonstrates | Preview command |
| --- | --- | --- |
| `volcanic_island.lua` | a concave cone sized so the crater rim lands on `peak`; a crater cut with a mask and a smooth minimum; a parasitic cone; gullies kept to the flanks | `--water` |
| `mountain_range.lua` | ridges along splines joined with smooth maxima, peaks on the crest, a valley cut afterwards, relief masked to the massif | `--view 20,-24,4600` |
| `canyon.lua` | a gorge and a side canyon from splines through a plateau profile; strata by `Curves.Terrace` blended in on the walls only | `--view 25,-32,4200` |
| `coast.lua` | a closed spline filled to a signed distance; a beach and a cliff as two profiles of the same distance, blended by a feathered mask; a sea shelf; best with `--water` | `--water --view 60,-18,2300 --target -1250,20,-450` |
| `mesas.lua` | rotated plateaus of several sizes, a pyramid and two domes, strata over everything, scree from a high-talus thermal pass | `--view 35,-24,3300` |

```bash
build/Release/bin/terrain_viewer --script assets/scripts/shapes/volcanic_island.lua --water
build/Release/bin/terrain_viewer --script assets/scripts/shapes/mountain_range.lua --view 20,-24,4600
```

![Volcanic island](images/shapes/volcanic_island.png)

![Mountain range](images/shapes/mountain_range.png)

![Canyon](images/shapes/canyon.png)

![Mesas and badlands](images/shapes/mesas.png)

![Coast with cliffs, a beach and water](images/shapes/coast.png)

**The limit of all of this is the heightfield.** One height per (x, z): no caves, arches, overhangs or
stacked surfaces. A scarp can be vertical, never undercut.

---

## 11. Recipes

### A complete terrain

```lua
local function main(params)
    local amplitude = params.amplitude or 400.0
    local sea       = params.sea_level or 0.0
    local wear      = params.erosion or 24

    -- Base: wavelength ten times the amplitude, so the slopes are terrain and not spikes.
    local base = Noises.fBm{
        frequency = 0.0002, octaves = 6, lacunarity = 2.0, persistence = 0.5,
        amplitude = amplitude, offset = sea, seed = params.seed,
    }

    -- The mask reads a three-octave gradient, not the base's own: the gradient of a six-octave field
    -- is dominated by its finest octave and the mask would flicker sample to sample.
    local shape = Noises.fBm{
        frequency = 0.0002, octaves = 3, amplitude = amplitude, offset = sea, seed = params.seed,
    }
    local ridges = Noises.Ridged{
        frequency = 0.0004, octaves = 4, amplitude = amplitude * 0.6, offset = sea,
        seed = params.seed + 1,
    }
    local blended = Combine.Blend{
        a = base.value, b = ridges.value,
        mask = Masks.Slope{ input = shape.gradient, min = 0.3, max = 1.1 },
    }

    -- Water cuts, then thermal settles the spoil. Both are slopes, so the same script describes the
    -- same terrain at any sample spacing.
    local carved = Erosion.Hydraulic{
        input = blended, iterations = wear, rain = 0.5, capacity = 6.0,
    }
    local settled = Erosion.Thermal{
        input = carved, iterations = wear // 2,
        talus = math.tan(math.rad(35.0)), strength = 0.3,
    }

    local low, high = sea - amplitude * 1.1, sea + amplitude * 1.1
    local height    = Curves.Clamp{ input = settled, min = low, max = high }

    return {
        height  = { value = height, range = { low, high } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = height } } },
    }
end

return main
```

### A rock-versus-grass mask

```lua
local shape = Noises.fBm{ frequency = 0.0002, octaves = 3, amplitude = 400.0 }
local rock  = Masks.Slope{ input = shape.gradient, min = 0.5, max = 0.9 }
-- Sharper selection without moving the thresholds.
local tight = Curves.Power{ input = rock, exponent = 2.0 }
return { rock = { value = tight, range = { 0, 1 } } }
```

### Snow above an altitude, but not on cliffs

```lua
local base  = Noises.fBm{ frequency = 0.0002, amplitude = 400.0 }
local alt   = Curves.Smoothstep{ input = base.value, min = 150.0, max = 300.0 }
local steep = Masks.Slope{ input = base.gradient, min = 0.6, max = 0.9 }
local snow  = alt * (1.0 - steep)
return { snow = { value = snow, range = { 0, 1 } } }
```

### A domain warp

```lua
local p    = Terrain.Coords{ domain = 2 }
local wx   = Noises.fBm{ frequency = 0.0005, amplitude = 300.0, seed = 7 }
-- A position field is bent by feeding displaced coordinates into something that is a function of
-- position. For a ramp that is just arithmetic; for a shape it is `position =`, which `Shapes.Warp`
-- builds for you from two noise fields.
local ramp = (p.x + wx.value) * 0.001
return { warped = { value = ramp, range = { 0, 4 } } }
```

For shapes, pass the warp as the shape's `position`:

```lua
local position = Shapes.Warp{ amplitude = 60.0, frequency = 0.004, seed = 7 }
local outline  = Shapes.Circle{ radius = 200, position = position }
return { mask = { value = Shapes.Mask{ shape = outline, feather = 20 }, range = { 0, 1 } } }
```

---

## 12. Errors

Every binding reads the calling line out of the Lua stack before it touches the graph, so a failure is
reported where it was written.

```
terrain.lua:12: [script] Noises.fBm does not take 'persistance'; it accepts domain, kind, frequency, ...
terrain.lua:14: [validation] Normals expects R2->R2 input, got R2->R1
```

| Stage | Meaning |
| --- | --- |
| `[script]` | a binding rejecting its own arguments: unknown key, wrong type, out of range |
| `[validation]` | a graph builder rejecting a mapping |
| `[compile]` | the compiler: usually a halo over the limit |

An unknown key is an error, never silently ignored, and the message lists the keys the op does accept.

## 13. What a script cannot do

Only `base`, `math`, `string` and `table` are opened, so there is no `io`, no `os`, no `package`, no
`require`, no `debug` and no `coroutine`: a script cannot start a process or load a C module.

`dofile`, `loadfile`, `load`, `collectgarbage`, `pcall` and `xpcall` are removed from `base` after it
opens, which was checked by enumerating `_G` from a script. The loaders would let a script read a file
or run code the sidecar's script hash does not describe, and `pcall` would let it swallow the time
budget's error and keep looping. The budget is 10 s of wall clock for building the graph; exceeding it
is an error with a line number.

What a script genuinely cannot do is branch on terrain data, because none exists while it runs: a
handle is a promise, not a value.
