-- Continents, Azgaar's template: two large masses parted by straits, ranges along their coasts, troughs and pits.
--
--   terrain_viewer --script assets/scripts/lands/continent.lua --water --resolution 128
--   terrain_export --script assets/scripts/lands/continent.lua --min -96000,-96000 --max 96000,96000 --resolution 128 --out out/continent
--
-- A 192 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height, land_fraction. Its land masses are `mass/1` (the largest), `mass/2` and on; `outline = "shapes"`
-- builds the composition of shapes instead, whose elements have ids an `edit` reaches.

local function main(params)
    local land = Lands.Continent{
        preset = "continent", seed = params.seed or 1, resolution = params.resolution, height = params.height,
        land_fraction = params.land_fraction,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
