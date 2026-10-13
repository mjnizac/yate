-- Arc: a range curved like a bow, with short spurs on its outer side and a basin inside the curve.
--
--   terrain_viewer --script assets/scripts/ranges/arc.lua

local function main(params)
    local h = params.height or 2100.0
    local arc = {}
    for i = 0, 8 do
        local a = math.rad(200 + i * 17.5)
        arc[#arc + 1] = { math.cos(a) * 8000.0, math.sin(a) * 8000.0 + 5000.0 }
    end
    local structure, tallest = Ranges.Carve{
        seed = params.seed,
        ridges = {
            { points = arc, height = h, width = 4200 },
            { points = { arc[3], { arc[3][1] * 1.35, arc[3][2] * 1.35 - 1500 } }, height = h * 0.6, width = 2800 },
            { points = { arc[6], { arc[6][1] * 1.3, arc[6][2] * 1.3 - 2600 } }, height = h * 0.65, width = 2800 },
        },
        peaks = {
            { at = arc[4], height = h * 1.3, radius = 3200 },
            { at = arc[7], height = h * 1.15, radius = 2800 },
        },
    }
    return Ranges.Finish(structure, tallest, params.seed)
end

return main
