-- Island, diagnostic: shows one product of the island instead of the surface, so the signed distance,
-- the mask, the shore band and the coast type can be read directly.
--
--   terrain_viewer --script assets/scripts/lands/island_mask_debug.lua --param product=coast_type
--
-- product: coast_sdf, land_mask, land_height, bathymetry, shore_mask or coast_type.

local function main(params)
    local island = Lands.Island{
        seed = params.seed or 1, resolution = params.resolution,
        radius = params.radius or 3000.0, roughness = params.roughness or 0.6,
        breakage = params.breakage or 4, coast = params.coast or "mixed",
    }
    local out = island:outputs{ params.product or "land_mask" }
    local shown = params.product or "land_mask"
    out.height = { value = island[shown], range = out.height.range }
    return out
end

return main
