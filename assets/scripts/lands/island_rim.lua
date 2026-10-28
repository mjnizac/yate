-- Island with cliffs higher than its interior: a lobed outline whose crest runs a few hundred metres
-- inland, over a lower plateau. Nothing in the composition assumes the middle is the highest ground.
--
--   terrain_viewer --script assets/scripts/lands/island_rim.lua --water --resolution 4
--   terrain_export --script assets/scripts/lands/island_rim.lua --min -6000,-6000 --max 6000,6000 --resolution 4 --out out/island_rim
--
-- A 12 km map centred on the origin. Parameters: seed, area (square metres, 40 km^2 by default), height.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 1, resolution = params.resolution, height = params.height or 380,
        frame = { size = 12000 }, coast = "cliff",
        elements = {
            { id = "main", shape = "radial", area = params.area or 40e6, rotation = 20,
              harmonics = { { 3, 0.18, 0.5 }, { 5, 0.07, 2.0 } },
              profile = { "rim", inner = 140 } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
