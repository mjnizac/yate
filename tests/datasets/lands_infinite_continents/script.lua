-- Infinite: continents over the unbounded world: up to one large lobed landmass per 128 km world cell, of
-- 3000 to 6000 km2, rising to a broad upland. Any two windows over the same world cells agree, so the world can
-- be exported piece by piece.
--
--   terrain_viewer --script assets/scripts/lands/infinite_continents.lua --seed 1 --water --resolution 128 --target 29000,0,-8000
--   terrain_export --script assets/scripts/lands/infinite_continents.lua --seed 1 --min -35000,-72000 --max 93000,56000 --resolution 128 --out out/infinite_continents
--
-- The elements are generated for `region` (the window to work in) and the cells round it. Parameters: seed,
-- count (per world cell).

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 13, resolution = params.resolution, height = 1800,
        frame = { world_cell = 128000, region = { -128000, -128000, 256000, 256000 } },
        elements = {
            { id = "lands", shape = "radial", profile = { "dome", height = 900 },
              scatter = { count = params.count or 1, area = { 3e9, 6e9 }, spacing = 12000 } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
