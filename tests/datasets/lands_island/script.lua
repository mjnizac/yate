-- Island: one elongated mass with a spine, drained by rivers whose valleys become inlets where they meet the sea.
--
--   terrain_viewer --script assets/scripts/lands/island.lua --water
--
-- The map is `size` metres across (24 km by default): export or view it over that window, centred on the
-- origin. `land_fraction` of it is land (0.7 by default). Parameters: seed, size, land_fraction, height,
-- roughness, dissection, margin ("compressed" or "geologic") and, for islands, coast.

local function main(params)
    local land = Lands.Island{
        seed = params.seed or 1, resolution = params.resolution,
        size = params.size, land_fraction = params.land_fraction, height = params.height,
        roughness = params.roughness, dissection = params.dissection, margin = params.margin, coast = params.coast or "mixed",
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
