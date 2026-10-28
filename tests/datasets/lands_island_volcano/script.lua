-- Volcanic island on an active margin: a cone rising straight from a narrow shelf, a smaller parasitic
-- cone on its flank, and a trench at the foot of the slope offshore.
--
--   terrain_viewer --script assets/scripts/lands/island_volcano.lua --water --resolution 8
--   terrain_export --script assets/scripts/lands/island_volcano.lua --min -12000,-12000 --max 12000,12000 --resolution 8 --out out/island_volcano
--
-- A 24 km map centred on the origin. Parameters: seed, area (square metres, 60 km^2 by default), height.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 9, resolution = params.resolution, height = params.height or 1400,
        frame = { size = 24000 }, coast = "rocky", profile = "cone",
        elements = {
            { id = "cone", shape = "radial", area = params.area or 60e6, harmonics = { { 5, 0.06, 0.8 } },
              margin = "active" },
            { id = "parasite", shape = "circle", radius = 1300, at = { 3200, -2600 }, smooth = 700,
              profile = { "cone", height = 520 } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
