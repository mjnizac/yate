-- Island, diagnostic: shows one product of the land instead of the surface, so the signed distance, the
-- mask, the shore band, the coast type and the landmass ids can be read directly.
--
--   terrain_viewer --script assets/scripts/lands/island_debug.lua --param product=coast_type
--
-- product: coast_sdf, land_mask, land_height, bathymetry, shore_mask, coast_type or region_id.

local function main(params)
    local land = Lands.Island{
        seed = params.seed or 1, resolution = params.resolution, size = params.size,
        land_fraction = params.land_fraction, height = params.height,
    }
    local shown = params.product or "land_mask"
    local out = land:outputs{ shown }
    out.height = { value = land[shown], range = out[shown].range }
    return out
end

return main
