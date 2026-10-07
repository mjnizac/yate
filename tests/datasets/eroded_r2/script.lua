-- Case: eroded_r2
--
-- The first iterative op end to end. Thermal erosion runs 24 iterations over a ping-ponged pair of
-- buffers, which means a halo of 24: every section computes 24 samples of padding on each side so that
-- what its interior reads after the last iteration is exact. The golden is the only thing that would
-- notice an off-by-one in that arithmetic outside the seam test.
--
-- The mask output is the slope after erosion, which is what makes the effect visible in the data
-- rather than only in the heights.

local function main()
    local base = Noises.fBm{
        frequency   = 0.02,
        octaves     = 4,
        lacunarity  = 2.0,
        persistence = 0.5,
        amplitude   = 1.0,
    }

    local eroded = Erosion.Thermal{
        input      = base.value,
        iterations = 24,
        talus      = 0.015,
        strength   = 0.4,
    }

    return {
        height = { value = eroded, range = { -1, 1 } },
        -- The pre-erosion gradient, so the golden also covers the case where an iterative op and a
        -- pointwise op read the same producer with different halos.
        slope  = { value = Masks.Slope{ input = base.gradient, min = 0.0, max = 0.1 },
                   range = { 0, 1 } },
    }
end

return main
