-- Island, rocky coast: hard rock, cliffs on the exposed side.
--
--   terrain_viewer --script assets/scripts/lands/island_rocky.lua --water --resolution 8
--   terrain_export --script assets/scripts/lands/island_rocky.lua --min -12000,-12000 --max 12000,12000 --resolution 8 --out out/island_rocky
--
-- The map is `size` metres across (24 km by default), centred on the origin. Parameters: seed, preset
-- (high_island by default: high_island, low_island, volcano, caldera, rim_island, mesa_island, atoll), size,
-- land_fraction, height, margin ("compressed" or "geologic").

local function main(params)
    local land = Lands.Island{
        preset = params.preset or "high_island", seed = params.seed or 3, resolution = params.resolution,
        size = params.size, land_fraction = params.land_fraction, height = params.height, margin = params.margin,
        coast = "rocky",
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
