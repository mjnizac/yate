-- Case: hydraulic_flow_r2
--
-- The water and the sediment hydraulic erosion leaves behind, exported alongside the height. This is the
-- first second channel in the engine that is not a gradient, and the first on an iterative op, so what
-- the golden pins down is the channel mask reaching all the way through an iterative dispatch: the
-- kernel writes channel 1 only when something asked for it, and it writes it only on the last iteration.

local function main()
    local base = Noises.fBm{
        frequency   = 0.02,
        octaves     = 4,
        lacunarity  = 2.0,
        persistence = 0.5,
        amplitude   = 1.0,
    }

    local carved = Erosion.Hydraulic{
        input        = base.value,
        iterations   = 24,
        rain         = 0.02,
        evaporation  = 0.03,
        capacity     = 6.0,
        erosion_rate = 0.35,
        deposition   = 0.25,
        flow_rate    = 0.2,
    }

    return {
        height = { value = carved, range = { -1.5, 1.5 } },
        -- `.flow` is water on x and sediment on y. Written as a 16-bit RGB PNG with blue at zero, which
        -- is what the writer does with any R2->R2 value.
        flow = { value = carved.flow },
    }
end

return main
