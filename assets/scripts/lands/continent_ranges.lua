-- Continent, with ranges: a large irregular mass with mountain ranges across its interior, plains
-- between them, and a coast that is rocky where the ranges meet it and sedimentary elsewhere.
--
--   terrain_viewer --script assets/scripts/lands/continent_ranges.lua --water
--
-- Parameters: seed, radius (metres), ranges_count, range_height (metres), roughness.

local function main(params)
    local continent = Lands.Continent{
        seed = params.seed or 3, resolution = params.resolution,
        radius = params.radius or 14000.0, ranges_count = params.ranges_count or 2,
        range_height = params.range_height, roughness = params.roughness,
        margin = params.margin or "compressed",
    }
    return continent:outputs{ "coast_type", "land_mask", "coast_sdf", "shore_mask" }
end

return main
