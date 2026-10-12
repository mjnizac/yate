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
message lists the keys the op does accept.

**Defaults, valid ranges, what to set each parameter to and a rendered picture of its effect are in
`docs/lua_reference.md`.** They are deliberately not repeated here: two documents carrying the same
numbers drift, and this one is the index.

| Call | Produces | Notes |
| --- | --- | --- |
| `Noises.fBm`, `Noises.Ridged`, `Noises.Billow` | `Rn -> R1` value, `Rn -> Rn` gradient | fractal noise with its exact analytic gradient, both out of one evaluation; `fBm{ kind = ... }` is the same op |
| `Terrain.Const` | `Rn -> R1` | a constant; usually unnecessary, since a bare number works wherever a field does |
| `Terrain.Coords` | `Rn -> Rn` | the integer **sample coordinate**, not metres, and on an `R2->R2` world z is `.y` |
| `Terrain.Normals` | `R2->R3` | takes a **gradient**, not a height |
| `Terrain.Gradient` | `Rn -> Rn` | central differences, **radius 1**; the only op that is not analytic |
| `Masks.Slope` | `R2->R1` in `[0, 1]` | takes a gradient; feed it a low-octave one or the mask flickers |
| `Combine.Blend` | the mapping of `a` and `b` | linear interpolation by a mask; the mask is not clamped |
| `Combine.Min`, `Combine.Max` | the shared mapping | `Max` unions landforms, `Min` carves |
| `Curves.Clamp`, `Remap`, `Power`, `Smoothstep` | the input's mapping | pointwise; `Remap` assumes a `[-1, 1]` input and `Power` only means what you want on a field in `[-1, 1]` |
| `Filters.Blur` | the input's mapping | radius in **samples**, not metres; its radius becomes halo |
| `Vec.Combine` | `Rn -> Rm` | a positional list of 2 to 4 scalars |
| `Erosion.Thermal` | `Rn -> R1` | material slides past the talus; `iterations` **is** the influence radius |
| `Erosion.Hydraulic` | `Rn -> R1` value, `Rn -> R2` flow | rain, flow, sediment, evaporation; `flow` carries water on `x` and sediment on `y` |

`Filters.Blur`, `Terrain.Gradient` and the two erosions are what put a halo on a graph: their radii
propagate backwards to everything that feeds them, and they add along a chain. The engine refuses a
graph whose halo passes 255, naming the node:

```
terrain.lua:9: [compile] Noise would need a halo of 400, over the limit of 255; every op between here
and an output adds its radius, and an iterative op contributes one per iteration
```

The upper bounds on `Erosion.Thermal`'s `strength` and `Erosion.Hydraulic`'s `flow_rate` are stability
conditions, not style limits, and the error says which one was broken: a thermal `strength` above one
half lets a cell overshoot its neighbour and oscillate instead of settling, and a hydraulic `flow_rate`
above a quarter lets the four outflows of a cell exceed the water it has.

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

Only `base`, `math`, `string` and `table` are opened, so there is no `io`, no `os`, no `package`, no
`require`, no `debug` and no `coroutine`: a script cannot start a process or load a C module.

It **can** read and run other Lua files — `dofile`, `loadfile` and `load` come with `base` and nothing
removes them. That is a gap rather than a decision, it is recorded in `docs/todo.md`, and it reaches
beyond security: the metadata sidecar hashes the script *file*, so a chunk assembled with `load` is not
described by its own hash.

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
