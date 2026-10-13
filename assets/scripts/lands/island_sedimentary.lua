-- Island, sedimentary: soft coast, low relief and wide beaches facing the waves.
--
--   terrain_viewer --script assets/scripts/lands/island_sedimentary.lua --water
--
-- Parameters: radius (metres), height (metres), seed, margin ("compressed" or "geologic").

local function main(params)
    local island = Lands.Island{
        seed = params.seed or 5, resolution = params.resolution,
        radius = params.radius or 3000.0, margin = params.margin or "compressed",
        coast = "sedimentary", roughness = 0.5, breakage = 3,
        height = params.height or 220.0, wave_from = 225.0,
    }
    return island:outputs{ "coast_type", "land_mask", "coast_sdf", "shore_mask" }
end

return main
