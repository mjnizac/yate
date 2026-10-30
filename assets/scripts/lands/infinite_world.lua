-- Infinite: continents and islands from Azgaar's templates over the unbounded world, with the strata, beaches
-- and sea floor of Lands.Template: a continent (170 to 330 km) in some of the 384 km continent cells, an island
-- or an archipelago in some of the 96 km world cells the continents leave open, each at a size and place of
-- its own, and between them the ocean floor as a drowned land in the same strata as the land's. Any
-- two windows over the same cells agree, so the world can be exported piece by piece.
--
--   terrain_viewer --script assets/scripts/lands/infinite_world.lua --seed 1 --water --water-opacity 0.45 --follow --param budget=4 --resolution 128 --max-level 1 --ring 4
--   terrain_export --script assets/scripts/lands/infinite_world.lua --seed 1 --min -144000,-144000 --max 144000,144000 --resolution 128 --out out/infinite_world
--
-- The cells rastered are those under `region`: the `cells` (1 by default) round the cell at the centre of
-- `params.bounds` (the export's window, or the viewer's ring with `--follow`, which re-runs the script as the
-- camera moves), or at `at` (x, z in metres) without them; all of the bounds when they are wider.
-- Parameters: seed, cells, at, vertical, budget.

local function main(params)
    local cell = 96000.0
    local cx, cz, half = 0.0, 0.0, 0.0
    local b = params.bounds
    if b and b.max_x > b.min_x then
        cx, cz = 0.5 * (b.min_x + b.max_x), 0.5 * (b.min_z + b.max_z)
        half = 0.5 * math.max(b.max_x - b.min_x, b.max_z - b.min_z)
    elseif params.at then
        local x, z = tostring(params.at):match("([%-%d%.]+),([%-%d%.]+)")
        cx, cz = (tonumber(x) or 0.0), (tonumber(z) or 0.0)
    end
    -- A viewer's ring (bounds under a cell) and 45 km more each way: the cells a moving camera is about to see
    -- are made before it does. An export makes its window.
    local reach = math.max((tonumber(params.cells) or 1) * cell, (half > 0.0 and half < cell) and half + 45000.0 or half)
    -- Heights and depths times `vertical` (3 by default): at true scale 1500 m over a 96 km cell reads flat.
    local v = tonumber(params.vertical) or 3.0
    local land = Lands.World{
        seed = params.seed or 1, resolution = params.resolution, height = 1500 * v, world_cell = cell,
        shelf_depth = 70 * v, abyssal_depth = 700 * v, cliff_height = 80 * v,
        region = { cx - reach, cz - reach, cx + reach, cz + reach },
        -- At most `budget` new maps a run (the viewer command passes 4): with `--follow` the viewer runs it
        -- again until the world round the camera is all made, nearest first, so no run waits for all of it.
        budget = tonumber(params.budget),
    }
    local outputs = land:outputs{ "land_mask", "beach_mask" }
    -- The snow line scaled with the heights, or the exaggerated land is white from the foothills up.
    outputs.look = { palette = "coast", snow = math.floor(1300 * v) }
    return outputs
end

return main
