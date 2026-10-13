-- Sierra: one long range, gently bent, its crest rising and falling through a line of summits.
--
--   terrain_viewer --script assets/scripts/ranges/sierra.lua

local function main(params)
    local h = params.height or 2000.0
    local structure, tallest, extras = Ranges.Carve{
        seed = params.seed,
        ridges = {
            { points = { { -11000, -1500 }, { -5000, 400 }, { 1000, -300 }, { 6500, 900 }, { 11500, -600 } },
              height = h, width = 4800 },
        },
        peaks = {
            { at = { -5200, 400 }, height = h * 1.25, radius = 3200 },
            { at = { 1200, -300 }, height = h * 1.35, radius = 3400 },
            { at = { 7000, 900 }, height = h * 1.15, radius = 2800 },
        },
    }
    return Ranges.Finish(structure, tallest, params.seed, { resolution = params.resolution }, extras)
end

return main
