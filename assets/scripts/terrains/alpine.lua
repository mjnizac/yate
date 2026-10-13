-- Alpine: a massif on a main crest with spurs, a dominant summit with cliffs under it, carved by rivers.
--
--   terrain_viewer --script assets/scripts/terrains/alpine.lua
--
-- `Ranges.Carve` (prelude) raises the ground along the crest lines and lets a stream-power simulation
-- cut the valleys; cirques are dug under the peaks. `Ranges.Finish` adds the country, gullies and the
-- erosion passes. Parameters: `height` (metres) the main crest, `k` the erodibility (higher: more,
-- deeper valleys), `diffusion` how far apart they are.

local function main(params)
    local h = params.height or 2200.0
    local structure, tallest, extras = Ranges.Carve{
        seed = params.seed,
        ridges = {
            -- The main crest, bent, and its spurs.
            { points = { { -6200, -900 }, { -2600, 300 }, { 400, -200 }, { 3200, 700 }, { 6000, 100 } },
              height = h, width = 5200 },
            { points = { { -1200, 200 }, { -600, 2600 }, { -1400, 5200 } }, height = h * 0.7, width = 3400 },
            { points = { { 2000, 300 }, { 2900, -2400 }, { 4200, -4600 } }, height = h * 0.65, width = 3200 },
            { points = { { -3600, -200 }, { -4600, -2800 } }, height = h * 0.6, width = 3000 },
        },
        peaks = {
            { at = { -400, 0 }, height = h * 1.3, radius = 3600 },
            { at = { 3400, 600 }, height = h * 1.1, radius = 2800 },
        },
        k = params.k, diffusion = params.diffusion,
    }
    return Ranges.Finish(structure, tallest, params.seed, { resolution = params.resolution }, extras)
end

return main
