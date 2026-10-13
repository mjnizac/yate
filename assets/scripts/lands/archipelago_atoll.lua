-- Atoll: a ring of low islets around a lagoon a few tens of metres deep, with a steep outer wall.
--
--   terrain_viewer --script assets/scripts/lands/archipelago_atoll.lua --water
--
-- Parameters: seed, radius (half-size of the whole group, metres), count, roughness, margin.
-- `region_id` (exported) says which island each cell is nearest to.

local function main(params)
    local group = Lands.Archipelago{
        seed = params.seed or 4, resolution = params.resolution, layout = "atoll",
        radius = params.radius or 12000.0, count = params.count, roughness = params.roughness or 0.6,
        margin = params.margin or "compressed",
    }
    return group:outputs{ "region_id", "coast_type", "land_mask", "coast_sdf" }
end

return main
