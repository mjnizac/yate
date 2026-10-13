-- Island on a geologic margin: a shelf under one degree out to 40 km, a break and a slope. Needs a
-- window of 100 km or more to read; in the viewer's 28 km it is nearly all shelf.
--
--   terrain_export --script assets/scripts/lands/island_margin_geologic.lua --min -60000,-60000 --max 60000,60000 --resolution 120 ...

local function main(params)
    local island = Lands.Island{
        seed = params.seed or 2, resolution = params.resolution,
        radius = params.radius or 6000.0, height = params.height or 600.0,
        margin = "geologic", roughness = 0.6, breakage = 4,
    }
    return island:outputs{ "bathymetry", "coast_sdf" }
end

return main
