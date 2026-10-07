-- Case: blurred_r2
--
-- The only case with a nonzero halo: the blur radius propagates back to the noise node, so the
-- padded sections and the halo-aware input indexing are exercised end to end.

local function main()
    local noise = Noises.fBm{
        frequency = 0.02,
        octaves   = 2,
        amplitude = 500.0,
        offset    = 500.0,
    }

    return {
        height = { value = Filters.Blur{ input = noise.value, radius = 3 },
                   range = { 0, 1000 } },
    }
end

return main
