-- Island, sedimentary coast: soft rock, low relief and wide beaches.
--
--   terrain_viewer --script assets/scripts/lands/island_sedimentary.lua --water --resolution 8
--   terrain_export --script assets/scripts/lands/island_sedimentary.lua --min -12000,-12000 --max 12000,12000 --resolution 8 --out out/island_sedimentary
--
-- The map is `size` metres across (24 km by default), centred on the origin. Parameters: seed, preset
-- (low_island by default: high_island, low_island, volcano, caldera, rim_island, mesa_island, atoll), size,
-- land_fraction, height, margin ("compressed" or "geologic").

local function main(params)
    local land = Lands.Island{
        preset = params.preset or "low_island", seed = params.seed or 2, resolution = params.resolution,
        size = params.size, land_fraction = params.land_fraction, height = params.height, margin = params.margin,
        coast = "sedimentary",
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
