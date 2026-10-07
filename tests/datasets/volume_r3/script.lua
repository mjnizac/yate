-- Case: volume_r3
--
-- An R3->R1 density field written as a raw little-endian f32 volume. The domain comes from the
-- noise node, and the exporter lays the grid out from what the script built.

local function main()
    local noise = Noises.fBm{
        domain    = 3,
        frequency = 0.01,
        octaves   = 3,
        amplitude = 1.0,
    }

    return {
        height = { value = noise.value, range = { -1, 1 } },
    }
end

return main
