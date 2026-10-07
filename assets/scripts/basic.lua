-- Smallest useful terrain: fractal base, ridged peaks blended by slope, then normals.
--
-- Run it with:
--   terrain_export --script assets/scripts/basic.lua --min 0,0 --max 4096,4096 \
--                  --resolution 1.0 --section 512 --out out/
--
-- Every parameter below has a default, so the script works with no --param at all.

local function main(params)
    local amplitude = params.amplitude or 400.0
    local sea       = params.sea_level or 0.0

    local base = Noises.fBm{
        kind        = "simplex",
        frequency   = 0.002,
        octaves     = 6,
        lacunarity  = 2.0,
        persistence = 0.5,
        amplitude   = amplitude,
        offset      = sea,
        seed        = params.seed,
    }

    local ridges = Noises.Ridged{
        frequency   = 0.004,
        octaves     = 4,
        amplitude   = amplitude * 0.6,
        offset      = sea,
        seed        = params.seed + 1,
    }

    -- Ridges only take over where the base is already steep, so peaks sit on slopes rather than
    -- floating over flat ground. The mask reads the base's analytic gradient, which the same
    -- dispatch already produced.
    local height = Combine.Blend{
        a    = base.value,
        b    = ridges.value,
        mask = Masks.Slope{ input = base.gradient, min = 0.2, max = 0.6 },
    }

    return {
        height  = { value = height, range = { sea - amplitude, sea + amplitude * 2.0 } },
        -- Normals come from the base gradient: it is exact, and taking the gradient of the blended
        -- height would mean finite differences.
        normals = { value = Terrain.Normals{ input = base.gradient } },
    }
end

return main
