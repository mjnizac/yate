# Lua API

Not implemented yet. The Lua runtime and its bindings are milestone 6; see `docs/status.md`.

This file records the contract the bindings will implement, because the mapping system, the kernel
interface and the error format it depends on already exist.

## Mappings

Every value in the graph has a mapping type `Rn -> Rm`: `n` is the domain dimension, `m` the number
of components, 1 to 4. Domain `R2` is `(x, z)`; domain `R3` is `(x, y, z)`.

| Mapping | Example |
| --- | --- |
| `R2->R1` | heightmap, mask |
| `R2->R2` | gradient of a heightmap, flow field |
| `R2->R3` | normal map |
| `R3->R1` | density field |
| `R3->R3` | 3D vector field, for example a domain warp |

Pointwise arithmetic is generic over `Rn -> Rm` when both operands match, and broadcasts an
`Rn -> R1` operand over `Rn -> Rm`. Components are extracted with `.x`, `.y`, `.z`, `.w`, producing
`Rn -> R1`, and assembled with `Vec.Combine{ ... }`. A mapping mismatch is a compile error, reported
at the script line where the node was created.

The mapping also decides how large the value's buffer is in VRAM. `engine/terrain/mapping.hpp`
already implements that computation, including the halo, and `tests/unit/test_mapping.cpp` pins it
down.

## Lazy evaluation

Script functions do no math. Each call appends a node to a typed graph and returns a lightweight
handle. A node can expose several channels, each with its own mapping: a 2D `Noises.fBm` exposes
`value` (`R2->R1`) and an analytic `gradient` (`R2->R2`). Channels are picked explicitly, and a
channel no consumer requests is never computed — the kernel receives a channel mask in its push
constants.

## Entry point

A script defines `main(params)`. `params` carries `seed`, `bounds` (world-space minimum and
maximum), `resolution` in meters per pixel, and the user-defined parameters the viewer exposes in
its UI.

`main` returns a table mapping output names to output descriptions: the graph handle and, for scalar
outputs, the `range` used to normalize to 16 bits.

```lua
local function main(params)
  local base   = Noises.fBm{ kind = "simplex", frequency = 0.002, octaves = 6,
                             lacunarity = 2.0, gain = 0.5 }
  local ridges = Noises.Ridged{ frequency = 0.004, octaves = 4 }
  local h      = Combine.Blend{ a = base, b = ridges,
                                mask = Masks.Slope{ input = base, min = 0.2, max = 0.6 } }
  local eroded = Erosion.Hydraulic{ input = h, iterations = 200, rain = 0.01,
                                    evaporation = 0.02 }
  local normals = Terrain.Normals{ input = eroded.value }   -- R2->R1 in, R2->R3 out
  return {
    height  = { value = eroded.value, range = { -200, 1800 } },
    normals = { value = normals },
  }
end
```

## Errors

Every binding captures the calling script line with `debug.getinfo` and stores it in the node, so a
validation or compilation failure reports:

```
terrain.lua:42: [validation] Erosion.Hydraulic expects R2->R1 input, got R3->R1
```

That format is already implemented by `engine::Error::Format` and covered by
`tests/unit/test_errors.cpp`. Messages must be specific enough to fix the script without reading
engine code.
