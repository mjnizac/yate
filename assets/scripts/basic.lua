-- Smallest useful terrain: fractal base, ridged peaks blended by slope, then normals.
-- The Lua runtime arrives in milestone 6; until then this file documents the target API and is
-- the script the first dataset case will use.

local function main(params)
    local base = Noises.fBm{
        kind       = "simplex",
        frequency  = 0.002,
        octaves    = 6,
        lacunarity = 2.0,
        gain       = 0.5,
        seed       = params.seed,
    }

    local ridges = Noises.Ridged{
        frequency = 0.004,
        octaves   = 4,
        seed      = params.seed + 1,
    }

    -- Ridges only take over where the base is already steep, so peaks sit on slopes.
    local height = Combine.Blend{
        a    = base.value,
        b    = ridges.value,
        mask = Masks.Slope{ input = base, min = 0.2, max = 0.6 },
    }

    return {
        height  = { value = height, range = { params.minHeight, params.maxHeight } },
        normals = { value = Terrain.Normals{ input = height } },
    }
end

return main
