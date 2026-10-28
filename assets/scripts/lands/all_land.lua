-- All land, no ocean: a low plain under the whole map with an upland block, a ridge of hills and two
-- lakes cut into it. What `Lands.Compose` builds when the sea is not wanted at all.
--
--   terrain_viewer --script assets/scripts/lands/all_land.lua --water --resolution 8
--   terrain_export --script assets/scripts/lands/all_land.lua --min -12000,-12000 --max 12000,12000 --resolution 8 --out out/all_land
--
-- A 24 km map centred on the origin. Parameters: seed, height.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 7, resolution = params.resolution, height = params.height or 500,
        frame = { size = 24000 },
        elements = {
            { id = "plain", shape = "fill", profile = { "flat", height = 40, edge = 400 } },
            { id = "upland", shape = "superellipse", radii = { 6000, 4000 }, exponent = 3, at = { -4000, 3000 },
              rotation = 15, smooth = 2500, profile = { "plateau", height = 280, edge = 1500 } },
            { id = "hills", shape = "band", points = { { 1000, -9000, 2400 }, { 4000, -3000, 3200 }, { 8500, 2000, 2200 } },
              smooth = 2000, profile = { "dome", height = 420, width = 1400 } },
            { id = "lake_a", shape = "ellipse", water = true, radii = { 1400, 800 }, at = { -6500, -5500 }, rotation = 30,
              role = "lake" },
            { id = "lake_b", shape = "circle", water = true, radius = 600, at = { 6000, 8000 }, role = "lake" },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
