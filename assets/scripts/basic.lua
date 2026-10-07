-- Smallest useful terrain: fractal base, ridged peaks blended by slope, eroded, then normals.
--
-- Run it with:
--   terrain_export --script assets/scripts/basic.lua --min 0,0 --max 4096,4096 \
--                  --resolution 1.0 --section 512 --out out/
--
-- Every parameter below has a default, so the script works with no --param at all.

local function main(params)
    local amplitude = params.amplitude or 400.0
    local sea       = params.sea_level or 0.0
    -- Iterations are not just a quality knob: they are the influence radius, so they are also the
    -- padding every section computes beyond its own interior. 24 is cheap; 200 would cost a section
    -- (512 + 400)^2 samples instead of 512^2.
    local wear      = params.erosion or 24

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
    local blended = Combine.Blend{
        a    = base.value,
        b    = ridges.value,
        mask = Masks.Slope{ input = base.gradient, min = 0.2, max = 0.6 },
    }

    -- Water first, cutting channels, then thermal erosion to settle the spoil into talus slopes.
    local carved = Erosion.Hydraulic{
        input      = blended,
        iterations = wear,
        rain       = 0.02,
        capacity   = 6.0,
    }

    local settled = Erosion.Thermal{
        input      = carved,
        iterations = wear // 2,
        talus      = amplitude * 0.004,
        strength   = 0.35,
    }

    -- Erosion deposits above the original peaks and cuts below the original valleys, and by how much
    -- depends on the terrain rather than on the parameters. Clamping to the declared range here means
    -- the script decides the band instead of leaving the writer to warn that a few samples fell outside
    -- it, and the 16-bit output gets the full range it was given.
    local low    = sea - amplitude * 1.5
    local high   = sea + amplitude * 2.0
    local height = Curves.Clamp{ input = settled, min = low, max = high }

    return {
        height = { value = height, range = { low, high } },
        -- The base gradient is exact but describes the terrain before erosion, so the normals are
        -- measured from the final height instead. Terrain.Gradient is the one op in the engine that
        -- is not analytic, and this is what it is for.
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = height } } },
    }
end

return main
