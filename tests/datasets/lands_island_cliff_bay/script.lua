-- Island, cliffs and bays: a hard coast cut by sediment, so cliffs and beaches share one outline.
--
--   terrain_viewer --script assets/scripts/lands/island_cliff_bay.lua --water
--
-- Parameters: radius (metres), height (metres), seed, margin ("compressed" or "geologic").

local function main(params)
    local island = Lands.Island{
        seed = params.seed or 7, resolution = params.resolution,
        radius = params.radius or 3000.0, height = params.height or 420.0,
        margin = params.margin or "compressed",
        coast = "cliff", roughness = 0.7, breakage = 4,
        sediment = 0.25,
    }
    return island:outputs{ "coast_type", "land_mask", "coast_sdf", "shore_mask" }
end

return main
