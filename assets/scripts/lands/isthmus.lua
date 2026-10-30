-- An isthmus, Azgaar's template: hills and troughs across the map, the land joined by narrow necks.
--
--   terrain_viewer --script assets/scripts/lands/isthmus.lua --water --resolution 32
--   terrain_export --script assets/scripts/lands/isthmus.lua --min -24000,-24000 --max 24000,24000 --resolution 32 --out out/isthmus
--
-- A 48 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height, land_fraction. Its land masses are `mass/1` (the largest), `mass/2` and on; `outline = "shapes"`
-- builds the composition of shapes instead, whose elements have ids an `edit` reaches.

local function main(params)
    local land = Lands.Continent{
        preset = "isthmus", seed = params.seed or 7, resolution = params.resolution, height = params.height,
        land_fraction = params.land_fraction,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
