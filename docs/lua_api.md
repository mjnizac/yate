# Lua API

Lua 5.4 through Sol2. The whole runtime lives in `src/lua.cpp`, the only translation unit that
includes Sol2, so no Sol2 type reaches a header.

A script is loaded, its `main(params)` is called once, and what it built is compiled into a sequence
of compute dispatches. Script functions do no math: each call appends a node to a graph and returns a
handle.

## Entry point

```lua
local function main(params)
    ...
    return { name = { value = <handle>, range = { min, max } }, ... }
end

return main
```

`main` may also be left as a global instead of returned.

`params` carries:

| Field | Meaning |
| --- | --- |
| `params.seed` | job seed |
| `params.resolution` | metres per sample |
| `params.bounds.min`, `.max` | world-space bounds as `{ x, y, z }` |
| `params.bounds.min_x` … `max_z` | the same values as named fields |
| `params.<key>` | every `--param key=value`; a value that reads as a number is pushed as a number, anything else stays a string |

The return value maps output names to descriptions. `range` is required for a scalar output, because
that is what it is normalized against for 16 bits; a vector output always remaps `[-1, 1]` and may be
written as a bare handle. Outputs are requested in **name order**, not table order, because a Lua
table has no iteration order and the request order reaches the buffer planner.

The **domain is whatever the script built**. Every output must agree on it: one job evaluates one
domain, and an `R3` graph also needs a y component on `--min` and `--max`.

## Handles

A handle is one channel of one node. It carries the mapping, nothing else.

| Access | Produces |
| --- | --- |
| `h.value` | channel 0, which every op has |
| `h.<channel>` | a named second channel, where the op has one |
| `h.x`, `h.y`, `h.z`, `h.w` | one component, `Rn -> R1` |
| `a + b`, `a - b`, `a * b`, `a / b`, `-a` | pointwise arithmetic; either side may be a number |

Channel names belong to the op, not to the handle:

| Op | Channel 1 | Mapping |
| --- | --- | --- |
| `Noises.fBm`, `Ridged`, `Billow` | `gradient`, the analytic gradient | `Rn -> Rn` |
| `Erosion.Hydraulic` | `flow`, the water on `x` and the sediment on `y` | `Rn -> R2` |

A wrong name is an error that names the op and lists what it does offer:

```
terrain.lua:7: [script] a Noise value has no 'magnitude'; it offers value, gradient and the components
.x, .y, .z, .w
```

A channel no consumer asks for is never computed and is never given a buffer: the kernel receives a
channel mask in its push constants and skips writing what nothing wants.

## Ops

Every op takes a single named table. An unknown key is an error, not a silently ignored one, and the
message lists the keys the op does accept. Every default is documented here.

### Noises

`Noises.fBm`, `Noises.Ridged`, `Noises.Billow`. Ridged and billow are base functions in their own
right rather than a second axis, each carrying its own exact derivative.

| Key | Default | Notes |
| --- | --- | --- |
| `kind` | `"simplex"` | `simplex`, `ridged` or `billow`. Only on `fBm`. |
| `domain` | `2` | 2 or 3 |
| `frequency` | `0.002` | strictly positive |
| `octaves` | `6` | whole, 1 to 32 |
| `lacunarity` | `2.0` | strictly positive |
| `persistence` | `0.5` | strictly positive |
| `amplitude` | `1.0` | output half-range when normalized |
| `offset` | `0.0` | added after accumulation |
| `seed` | `0` | salt, decorrelates nodes that share the job seed |
| `normalize` | `true` | divide by the sum of the amplitudes, so `amplitude` is the half-range |

Channel 0 is the value, channel 1 the analytic gradient, both out of one evaluation.

### Terrain

| Call | Mapping | Keys |
| --- | --- | --- |
| `Terrain.Normals` | `R2->R2` in, `R2->R3` out | `input`, `vertical_scale` (1.0) |
| `Terrain.Gradient` | `Rn->R1` in, `Rn->Rn` out | `input` |
| `Terrain.Coords` | `Rn->Rn` | `domain` (2) |
| `Terrain.Const` | `Rn->R1` | `domain` (2), `value` (0.0) |

`Terrain.Normals` takes a gradient. Given an `R2->R1` whose node exposes one, it resolves the gradient
channel, so `Terrain.Normals{ input = base }` works for a noise handle.

`Terrain.Gradient` measures a gradient by central differences, and it is the only op in the engine that
is not analytic. Use it for a field that came out of blends, curves or erosion, where there is no closed
form left to differentiate. For anything that has an analytic derivative, use that instead: a noise
node's `.gradient` is exact and comes out of the same evaluation as its value.

```lua
-- exact, same dispatch as the value
Terrain.Normals{ input = base.gradient }
-- measured, radius 1, for terrain that has been through erosion
Terrain.Normals{ input = Terrain.Gradient{ input = eroded } }
```

### Masks, Combine, Curves, Filters, Vec

| Call | Keys |
| --- | --- |
| `Masks.Slope` | `input` (a gradient), `min` (0.0), `max` (1.0), `min < max` |
| `Combine.Blend` | `a`, `b` (same mapping), `mask` (`Rn->R1`) |
| `Combine.Min`, `Combine.Max` | `a`, `b` |
| `Curves.Clamp` | `input`, `min` (0.0), `max` (1.0) |
| `Curves.Remap` | `input`, `min` (0.0), `max` (1.0) |
| `Curves.Power` | `input`, `exponent` (1.0, in [0, 64]) |
| `Curves.Smoothstep` | `input`, `min` (0.0), `max` (1.0), `min < max` |
| `Filters.Blur` | `input`, `radius` (1, in [1, 32]) |
| `Vec.Combine` | a positional list of 2 to 4 scalars |

### Erosion

Iterative ops. The iteration count is not only a quality knob: material moves one cell per iteration, so
the count **is** the influence radius, and therefore the halo every section computes beyond its own
interior. 24 iterations on a 512 section means computing 560x560 samples instead of 512x512; 200 would
mean 912x912. Chaining two iterative ops adds their radii, and the engine refuses a graph whose halo
passes 255, which is what the kernel interface can carry:

```
terrain.lua:9: [compile] Noise would need a halo of 400, over the limit of 255; every op between here
and an output adds its radius, and an iterative op contributes one per iteration
```

| Call | Keys |
| --- | --- |
| `Erosion.Thermal` | `input`, `iterations` (16), `talus` (0.02), `strength` (0.25, in (0, 0.5]) |
| `Erosion.Hydraulic` | `input`, `iterations` (32), `rain` (0.02), `evaporation` (0.05), `capacity` (4.0), `erosion_rate` (0.3), `deposition` (0.3), `flow_rate` (0.15, in (0, 0.25]) |

Both take an `Rn -> R1` height. Thermal erosion slides material downhill wherever the slope passes the
talus angle. Hydraulic erosion rains, routes water, carries sediment and evaporates.

Hydraulic erosion has a second channel, `flow`, holding the water and the sediment as they stood after
the last iteration — a wetness map and a sediment map, which is what a renderer wants for putting rock
against silt:

```lua
local carved = Erosion.Hydraulic{ input = base.value, iterations = 32 }
return {
  height = { value = carved, range = { -200, 1800 } },
  wet    = { value = carved.flow },   -- water on x, sediment on y
}
```

Asking for it costs a second section buffer and the bandwidth to fill it, so a script that only wants a
height pays for neither.

The upper bounds are stability conditions, not style limits, and the error says which one was broken: a
thermal `strength` above one half lets a cell overshoot its neighbour and oscillate instead of settling,
and a hydraulic `flow_rate` above a quarter lets the four outflows of a cell exceed the water it has.

`Filters.Blur`, `Terrain.Gradient` and the erosion ops are what put a halo on a graph: their radii
propagate backwards to everything that feeds them.

## Mappings

Every value has a mapping type `Rn -> Rm`: `n` is the domain dimension, `m` the number of components,
1 to 4. Domain `R2` is `(x, z)`; domain `R3` is `(x, y, z)`.

| Mapping | Example |
| --- | --- |
| `R2->R1` | heightmap, mask |
| `R2->R2` | gradient of a heightmap, flow field |
| `R2->R3` | normal map |
| `R3->R1` | density field |
| `R3->R3` | 3D vector field, for example a domain warp |

Pointwise arithmetic is generic over `Rn -> Rm` when both operands match, and broadcasts an `Rn -> R1`
operand over `Rn -> Rm`. The mapping also decides how large the value's buffer is in VRAM, halo
included.

## What a script cannot do

Only `base`, `math`, `string` and `table` are opened. There is no `io`, no `os`, no `package` and no
`require`, so a script cannot read files, start processes or load C modules.

## Errors

Every binding reads the calling line out of the Lua stack before it touches the graph, so a failure is
reported where it was written:

```
terrain.lua:12: [script] Noises.fBm does not take 'persistance'; it accepts domain, kind, frequency, ...
terrain.lua:14: [validation] Normals expects R2->R2 input, got R2->R1
```

Two stages appear. `[script]` is a binding rejecting its own arguments; `[validation]` is a graph
builder rejecting a mapping. Both carry the script line. `tests/unit/test_lua_errors.cpp` pins 126
checks' worth of these: the stage, the line and the words that have to be in the message.
