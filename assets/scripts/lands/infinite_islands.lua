-- Islands anchored to the world, far from the origin: nothing here depends on the window that is
-- exported or viewed, so the world can be exported piece by piece and the pieces meet exactly.
--
--   terrain_viewer --script assets/scripts/lands/infinite_islands.lua --water --resolution 8 --target 100000,0,-60000
--   terrain_export --script assets/scripts/lands/infinite_islands.lua --min 88000,-72000 --max 112000,-48000 --resolution 8 --out out/infinite_islands
--
-- The world cell is 16 km; the report measures `region`. Parameters: seed, height.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 10, resolution = params.resolution, height = params.height or 700,
        frame = { world_cell = 16000, region = { 80000, -80000, 120000, -40000 } },
        elements = {
            { id = "north", shape = "radial", area = 70e6, at = { 97000, -55000 }, rotation = 30,
              harmonics = { { 3, 0.2, 0.0 }, { 4, 0.08, 1.4 } }, profile = "dome" },
            { id = "south", shape = "superellipse", area = 30e6, aspect = 1.7, exponent = 2.6,
              at = { 106000, -66000 }, rotation = -20, profile = { "plateau", height = 260 } },
            { id = "strait_isle", shape = "circle", area = 2e6, at = { 103500, -59500 }, profile = "cone" },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
