-- Archipelago, islands at given places: each centre with its area, the islands' outlines and turns drawn
-- from the seed. What to write when the positions are part of the request ("an island here, 5 km2").
--
--   terrain_viewer --script assets/scripts/lands/archipelago_centers.lua --water --resolution 8
--   terrain_export --script assets/scripts/lands/archipelago_centers.lua --min -12000,-12000 --max 12000,12000 --resolution 8 --out out/archipelago_centers
--
-- A 24 km map centred on the origin. Parameters: seed, height.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 11, resolution = params.resolution, height = params.height or 500,
        frame = { size = 24000 }, profile = "dome",
        elements = {
            { id = "isles", shape = "radial", scatter = { pattern = "centers", centers = {
                { -6000, -4000, 18e6 }, { 3000, -6500, 6e6 }, { 7000, 2000, 10e6 }, { -2000, 6500, 4e6 },
                { 9000, 8500, 1.5e6 }, { -9500, 6000, 2e6 } } } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
