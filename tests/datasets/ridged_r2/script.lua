-- Case: ridged_r2
--
-- Ridged noise with normalization off, so both the base-function variant and the unnormalized
-- accumulation path are covered.

local function main()
    local noise = Noises.Ridged{
        frequency   = 0.008,
        octaves     = 4,
        persistence = 0.6,
        amplitude   = 300.0,
        normalize   = false,
    }

    return {
        height = { value = noise.value, range = { 0, 900 } },
    }
end

return main
