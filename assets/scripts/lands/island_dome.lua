-- Island rising to a central summit, with a bay cut into its south side and a peninsula off its east.
--
--   terrain_viewer --script assets/scripts/lands/island_dome.lua --water --resolution 8
--   terrain_export --script assets/scripts/lands/island_dome.lua --min -12000,-12000 --max 12000,12000 --resolution 8 --out out/island_dome
--
-- A 24 km map centred on the origin. Parameters: seed, area (square metres, 150 km^2 by default), height.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 1, resolution = params.resolution, height = params.height or 900,
        frame = { size = 24000 }, profile = "dome",
        elements = {
            { id = "main", shape = "radial", area = params.area or 150e6, rotation = -10,
              harmonics = { { 2, 0.15, 0.3 }, { 5, 0.06, 1.2 } } },
            { id = "peninsula", shape = "capsule", from = { 4500, 1200 }, to = { 9500, 2600 }, width = 2200,
              smooth = 900, profile = { "dome", height = 260 } },
            { id = "bay", shape = "ellipse", water = true, radii = { 2200, 1300 }, at = { -800, -6200 }, smooth = 300 },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
