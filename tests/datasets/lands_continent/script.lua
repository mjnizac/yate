-- Continent: several large lobes with ranges along them, cut by rivers into valleys, peninsulas and gulfs.
--
--   terrain_viewer --script assets/scripts/lands/continent.lua --water
--
-- The map is `size` metres across (24 km by default): export or view it over that window, centred on the
-- origin. `land_fraction` of it is land (0.7 by default). Parameters: seed, size, land_fraction, height,
-- roughness, dissection, margin ("compressed" or "geologic") and, for islands, coast.

local function main(params)
    local land = Lands.Continent{
        seed = params.seed or 3, resolution = params.resolution,
        size = params.size, land_fraction = params.land_fraction, height = params.height,
        roughness = params.roughness, dissection = params.dissection, margin = params.margin,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
