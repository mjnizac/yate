-- Parallel: two ranges side by side, the northern one higher, with a long valley between them that
-- both drain into.
--
--   terrain_viewer --script assets/scripts/ranges/parallel.lua

local function main(params)
    local h = params.height or 2000.0
    local structure, tallest = Ranges.Carve{
        seed = params.seed,
        ridges = {
            { points = { { -10000, 3000 }, { -3000, 4200 }, { 3500, 3200 }, { 10000, 4400 } },
              height = h * 1.1, width = 4200 },
            { points = { { -9500, -3800 }, { -2500, -3000 }, { 4000, -4200 }, { 9500, -3200 } },
              height = h * 0.85, width = 3800 },
        },
        peaks = {
            { at = { -3000, 4200 }, height = h * 1.35, radius = 3000 },
            { at = { 5500, -3800 }, height = h * 1.05, radius = 2600 },
        },
    }
    return Ranges.Finish(structure, tallest, params.seed)
end

return main
