-- Infinite: land everywhere with lakes scattered over it, four per 16 km world cell. Any two windows over the same world cells agree, so the world can be exported piece by piece.
--
--   terrain_viewer --script assets/scripts/lands/infinite_lakeland.lua --water --resolution 16 --target 40000,0,40000
--   terrain_export --script assets/scripts/lands/infinite_lakeland.lua --min 0,0 --max 64000,64000 --resolution 16 --out out/infinite_lakeland
--
-- The elements are generated for `region` (the window to work in) and the cells round it. Parameters: seed,
-- count (per world cell).

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 13, resolution = params.resolution, height = 120,
        frame = { world_cell = 16000, region = { -32000, -32000, 96000, 96000 } },
        elements = {
            { id = "land", shape = "fill", profile = { "flat", height = 60, edge = 400 } },
            { id = "lakes", shape = "radial", water = true, role = "lake",
              scatter = { count = params.count or 4, area = { 0.3e6, 2e6 }, spacing = 800 } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
