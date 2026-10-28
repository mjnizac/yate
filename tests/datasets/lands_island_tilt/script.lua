-- Tilted island (a cuesta): high cliffs on the north-east side, the ground falling across it to beaches
-- on the south-west.
--
--   terrain_viewer --script assets/scripts/lands/island_tilt.lua --water --resolution 8
--   terrain_export --script assets/scripts/lands/island_tilt.lua --min -12000,-12000 --max 12000,12000 --resolution 8 --out out/island_tilt
--
-- A 24 km map centred on the origin. Parameters: seed, area (square metres, 120 km^2 by default), height,
-- direction (degrees the ground rises towards, 45 by default).

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 4, resolution = params.resolution, height = params.height or 450,
        frame = { size = 24000 },
        elements = {
            { id = "cuesta", shape = "ellipse", area = params.area or 120e6, aspect = 1.8, rotation = -40,
              profile = { "tilt", direction = params.direction or 45 } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
