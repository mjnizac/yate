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
| `h.value` | channel 0 |
| `h.gradient` | channel 1, the analytic gradient, where the node has one |
| `h.x`, `h.y`, `h.z`, `h.w` | one component, `Rn -> R1` |
| `a + b`, `a - b`, `a * b`, `a / b`, `-a` | pointwise arithmetic; either side may be a number |

A channel no consumer asks for is never computed: the kernel receives a channel mask in its push
constants.

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
| `Terrain.Coords` | `Rn->Rn` | `domain` (2) |
| `Terrain.Const` | `Rn->R1` | `domain` (2), `value` (0.0) |

`Terrain.Normals` takes a gradient. Given an `R2->R1` whose node exposes one, it resolves the
gradient channel, so `Terrain.Normals{ input = base }` works for a noise handle. For anything else it
says so and names `.gradient`: there is no gradient op for an arbitrary field, because computing one
would mean finite differences.

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

`Filters.Blur` is the only neighbourhood op so far, so it is what puts a halo on the graph: its
radius propagates backwards to everything that feeds it.

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
