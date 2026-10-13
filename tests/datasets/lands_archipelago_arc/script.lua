-- Archipelago, island arc: islands on an arc of a circle, each cut to fit its neighbours.
--
--   terrain_viewer --script assets/scripts/lands/archipelago_arc.lua --water
--
-- Parameters: seed, radius (half-size of the whole group, metres), count, roughness, margin.
-- `region_id` (exported) says which island each cell is nearest to.

local function main(params)
    local group = Lands.Archipelago{
        seed = params.seed or 3, resolution = params.resolution, layout = "arc",
        radius = params.radius or 12000.0, count = params.count, roughness = params.roughness or 0.6,
        margin = params.margin or "compressed",
    }
    return group:outputs{ "region_id", "coast_type", "land_mask", "coast_sdf" }
end

return main
