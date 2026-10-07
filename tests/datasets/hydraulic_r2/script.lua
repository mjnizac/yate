-- Case: hydraulic_r2
--
-- Hydraulic erosion, which is the one op whose intermediate state is a different shape from what it
-- produces: it takes a height, carries height, water and sediment through 40 iterations in a
-- three-component buffer the rest of the graph never sees, and hands back a height. The golden covers
-- the indexing of that first-and-last-iteration asymmetry, which the seam test alone would not pin
-- down to particular values.
--
-- Then thermal erosion on top, so the case also covers two iterative ops in series: their radii add,
-- so the noise feeding them is computed over a halo of 40 + 12.

local function main()
    local base = Noises.fBm{
        frequency   = 0.025,
        octaves     = 4,
        lacunarity  = 2.0,
        persistence = 0.55,
        amplitude   = 1.0,
    }

    local carved = Erosion.Hydraulic{
        input        = base.value,
        iterations   = 40,
        rain         = 0.015,
        evaporation  = 0.04,
        capacity     = 6.0,
        erosion_rate = 0.35,
        deposition   = 0.25,
        flow_rate    = 0.2,
    }

    local settled = Erosion.Thermal{
        input      = carved,
        iterations = 12,
        talus      = 0.02,
        strength   = 0.35,
    }

    return {
        height = { value = settled, range = { -1.5, 1.5 } },
    }
end

return main
