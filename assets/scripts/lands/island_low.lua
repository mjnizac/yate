-- Low island (a cay): a few metres of sand over the sea, a long narrow body with a hook at one end.
--
--   terrain_viewer --script assets/scripts/lands/island_low.lua --water --resolution 2
--   terrain_export --script assets/scripts/lands/island_low.lua --min -2000,-2000 --max 2000,2000 --resolution 2 --out out/island_low
--
-- A 4 km map centred on the origin. Parameters: seed, height (6 m by default).

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 5, resolution = params.resolution, height = params.height or 6,
        frame = { size = 4000 }, coast = "sedimentary", profile = "flat",
        beach_width = 40, beach_height = 2,
        elements = {
            { id = "cay", shape = "band", points = { { -1300, -200, 260 }, { -300, 100, 340 }, { 700, 0, 260 },
                                                      { 1150, 350, 160 }, { 1200, 700, 90 } } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
