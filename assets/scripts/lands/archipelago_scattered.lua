-- Archipelago, scattered: islands of very different sizes (a power law), spaced by a channel the warp cannot close.
--
--   terrain_viewer --script assets/scripts/lands/archipelago_scattered.lua --water
--
-- Parameters: seed, radius (half-size of the whole group, metres), count, roughness, margin.
-- `region_id` (exported) says which island each cell is nearest to.

local function main(params)
    local group = Lands.Archipelago{
        seed = params.seed or 1, resolution = params.resolution, layout = "scattered",
        radius = params.radius or 12000.0, count = params.count, roughness = params.roughness or 0.6,
        margin = params.margin or "compressed",
    }
    return group:outputs{ "region_id", "coast_type", "land_mask", "coast_sdf" }
end

return main
