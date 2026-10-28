-- An infinite archipelago: clusters of islets over the unbounded world, one cluster of seven per 24 km world cell. Any two
-- windows over the same world cells have the same islands, so the world can be exported piece by piece.
--
--   terrain_viewer --script assets/scripts/lands/infinite_cluster.lua --water --resolution 16 --target 40000,0,40000
--   terrain_export --script assets/scripts/lands/infinite_cluster.lua --min 0,0 --max 64000,64000 --resolution 16 --out out/infinite_cluster
--
-- The islands are generated for `region` (the window to work in, 128 km here) and the cells round it.
-- Parameters: seed, count (per world cell).

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 12, resolution = params.resolution, height = 600, profile = "dome",
        frame = { world_cell = 24000, region = { -32000, -32000, 96000, 96000 } },
        elements = {
            { id = "isles", shape = "radial", scatter = { pattern = "cluster", count = params.count or 7, area = { 0.4e6, 3e6 },
              clusters = 1, spread = 2500, spacing = 400 } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
