-- Case: gradient_r2
--
-- The analytic gradient exported directly as an R2->R2 value, which the writer stores as a 16-bit
-- RGB PNG with blue at zero.

local function main()
    local noise = Noises.fBm{
        frequency = 0.006,
        octaves   = 3,
        amplitude = 120.0,
    }

    return {
        height   = { value = noise.value, range = { -120, 120 } },
        gradient = { value = noise.gradient },
    }
end

return main
