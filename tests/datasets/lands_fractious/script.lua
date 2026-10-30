-- Fractious land, Azgaar's template: many hills, masked and lowered into a maze of land and water.
--
--   terrain_viewer --script assets/scripts/lands/fractious.lua --water --resolution 64
--   terrain_export --script assets/scripts/lands/fractious.lua --min -48000,-48000 --max 48000,48000 --resolution 64 --out out/fractious
--
-- A 96 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height, land_fraction. Its land masses are `mass/1` (the largest), `mass/2` and on; `outline = "shapes"`
-- builds the composition of shapes instead, whose elements have ids an `edit` reaches.

local function main(params)
    local land = Lands.Continent{
        preset = "fractious", seed = params.seed or 8, resolution = params.resolution, height = params.height,
        land_fraction = params.land_fraction,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
