-- Mediterranean, Azgaar's template: land along two shores round a central sea, with islands in it.
--
--   terrain_viewer --script assets/scripts/lands/mediterranean.lua --water --resolution 64
--   terrain_export --script assets/scripts/lands/mediterranean.lua --min -48000,-48000 --max 48000,48000 --resolution 64 --out out/mediterranean
--
-- A 96 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height, land_fraction. Its land masses are `mass/1` (the largest), `mass/2` and on; `outline = "shapes"`
-- builds the composition of shapes instead, whose elements have ids an `edit` reaches.

local function main(params)
    local land = Lands.Continent{
        preset = "mediterranean", seed = params.seed or 4, resolution = params.resolution, height = params.height,
        land_fraction = params.land_fraction,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
