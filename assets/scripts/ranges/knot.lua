-- Knot: three ranges meeting at a high massif, the way mountain systems join at a node.
--
--   terrain_viewer --script assets/scripts/ranges/knot.lua

local function main(params)
    local h = params.height or 2100.0
    local structure, tallest, sun = Ranges.Carve{
        seed = params.seed,
        ridges = {
            { points = { { 0, 0 }, { -3500, 2500 }, { -7500, 3200 }, { -10500, 5500 } }, height = h, width = 4200 },
            { points = { { 0, 0 }, { 4200, 1500 }, { 8500, 600 }, { 11000, 2500 } }, height = h * 0.9, width = 4000 },
            { points = { { 0, 0 }, { 800, -4000 }, { -500, -8000 }, { 1200, -10500 } }, height = h * 0.8, width = 3800 },
        },
        peaks = {
            { at = { 0, 0 }, height = h * 1.45, radius = 4000 },
            { at = { -7500, 3200 }, height = h * 1.05, radius = 2600 },
        },
    }
    return Ranges.Finish(structure, tallest, params.seed, nil, sun)
end

return main
