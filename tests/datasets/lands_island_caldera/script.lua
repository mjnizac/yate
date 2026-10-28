-- Volcanic island with a caldera: a ring of high ground round a floor below the sea, with a sea breach
-- (a sector of water) where the wall has fallen in.
--
--   terrain_viewer --script assets/scripts/lands/island_caldera.lua --water --resolution 4
--   terrain_export --script assets/scripts/lands/island_caldera.lua --min -6000,-6000 --max 6000,6000 --resolution 4 --out out/island_caldera
--
-- A 12 km map centred on the origin. Parameters: seed, area (square metres, 30 km^2 by default), height.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 2, resolution = params.resolution, height = params.height or 650,
        frame = { size = 12000 }, coast = "rocky",
        elements = {
            { id = "cone", shape = "radial", area = params.area or 30e6, harmonics = { { 4, 0.08, 1.0 } },
              profile = "caldera" },
            { id = "breach", shape = "sector", water = true, reach = 3400, angle = 18, rotation = 300, smooth = 120 },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
