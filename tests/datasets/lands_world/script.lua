-- Lands.World over one 24 km world cell and no continents: an island from Azgaar's templates with its fjords,
-- beaches and strata, and the ocean floor of the unbounded world (stairs, drowned land, guyots, ridges,
-- trenches and canyons) round it, small enough for the script's budget.

local function main(params)
    local land = Lands.World{
        seed = params.seed or 1, resolution = params.resolution, world_cell = 24000.0, continents = 0.0,
        height = 1500.0, region = { -12000.0, -12000.0, 12000.0, 12000.0 },
    }
    return land:outputs{ "land_mask", "region_id" }
end

return main
