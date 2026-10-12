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
| `Terrain.Coords` | `Rn -> Rn` | the world position **in metres**; on an `R2->R2` world z is `.z` (the second component) |
| `Terrain.Normals` | `R2->R3` | takes a **gradient**, not a height |
| `Terrain.Gradient` | `Rn -> Rn` | central differences, **radius 1**; the only op that is not analytic |
| `Masks.Slope` | `R2->R1` in `[0, 1]` | takes a gradient; feed it a low-octave one or the mask flickers |
| `Combine.Blend` | the mapping of `a` and `b` | linear interpolation by a mask; the mask is not clamped |
| `Combine.Min`, `Combine.Max` | the shared mapping | `Max` unions landforms, `Min` carves |
| `Curves.Clamp`, `Remap`, `Power`, `Smoothstep` | the input's mapping | pointwise; `Remap` assumes a `[-1, 1]` input and `Power` only means what you want on a field in `[-1, 1]` |
| `Filters.Blur` | the input's mapping | radius in **samples**, not metres; its radius becomes halo |
| `Vec.Combine` | `Rn -> Rm` | a positional list of 2 to 4 scalars |
| `Terrain.Transform` | `R2->R2` | moves, turns and scales a position field; what places a shape |
| `Geometry.Circle`, `Ellipse`, `Box`, `BoxChebyshev`, `Segments` | `R2->R1` | signed distance in metres, negative inside, to a primitive in its own local space |
| `Curves.Profile` | the input's mapping | a signed distance to a factor in `[0, 1]`: `linear`, `smooth`, `dome`, `gaussian`, `plateau` |
| `Curves.Terrace` | the input's mapping | snaps a height to steps; `step` in metres, `sharpness` in `[0, 1]` |
| `Combine.SmoothMin`, `Combine.SmoothMax` | the shared mapping | `Min` and `Max` with the corner rounded over `k` |
| `Shapes.*`, `Landforms.*` | `R2->R1` | the prelude: placed shapes, booleans, masks, warps and heights built from them (see below) |
| `Erosion.Thermal` | `Rn -> R1` | material slides past the talus, a **slope** (rise over run); `iterations` **is** the influence radius |
| `Erosion.Hydraulic` | `Rn -> R1` value, `Rn -> R2` flow | rain, flow, sediment, evaporation; `capacity` is per unit slope; `flow` carries water on `x` and sediment on `y` |

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

## Discovering what a call takes

There is no listing of the API in the engine. Two things stand in for one:

* `docs/lua_reference.md` has every call with its keys, defaults and valid ranges.
* A call with an unknown key fails with the keys it does accept, from the C++ bindings and the prelude
  alike:

  ```
  terrain.lua:4: [script] Landforms.Cone does not take 'slope'; it accepts at, rotation, position, radius, height, profile
  ```

  A missing required key says which one, and a wrong type says what was found.

Reproducibility is `--seed` and `--param key=value`, both recorded in the sidecar with SHA-256 of the
script, the shaders the job ran, the parameters and the prelude. `outputs[].statistics` and
`timings_ms` in the same sidecar are what a generation reports about itself.

## The prelude

`Shapes.*` and `Landforms.*` are not ops. They are Lua the engine runs before the script
(`assets/lua/prelude/`), and every call in them is one of the ops above, so they add no kernel and no
node type: a landform is a shape, a profile and a multiply.

* They are Lua because that is what a script author reads and edits, and a landform is five calls.
* Errors from inside them report the line of **your** script that called them, never a line of the
  prelude, and an unknown key lists what is accepted, as in the C++ bindings.
* The prelude is hashed into the sidecar as `prelude_hash`: the script's own hash does not describe the
  terrain by itself, since `Landforms.Cone` is code the engine supplies.
* It runs inside the same sandbox and time budget as the script. It captures `Geometry`, `Terrain`,
  `Curves` and the rest into locals when it loads, so a script that reassigns one of those globals does
  not change what `Shapes.Circle` means.

The prelude is a closed list of files next to the executable, not a scan of a directory.

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

`dofile`, `loadfile`, `load`, `collectgarbage`, `pcall` and `xpcall` are removed from `base` after it
opens. The loaders are how a script would read a file or run code the metadata sidecar's script hash
does not describe; `pcall` goes because it defeats the time budget, which can only *raise* an error and
which a script could otherwise catch and ignore.

A script also has a wall-clock budget, 10 s by default (`ScriptEnvironment::timeoutSeconds`). Building
a graph is bookkeeping, so exceeding it means a loop, and the result is an error with a line number
rather than a process that never returns.

## Errors

Every binding reads the calling line out of the Lua stack before it touches the graph, so a failure is
reported where it was written:

```
terrain.lua:12: [script] Noises.fBm does not take 'persistance'; it accepts domain, kind, frequency, ...
terrain.lua:14: [validation] Normals expects R2->R2 input, got R2->R1
```

Two stages appear. `[script]` is a binding rejecting its own arguments; `[validation]` is a graph
builder rejecting a mapping. Both carry the script line. `tests/unit/test_lua_errors.cpp` pins 202
checks' worth of these: the stage, the line and the words that have to be in the message.
