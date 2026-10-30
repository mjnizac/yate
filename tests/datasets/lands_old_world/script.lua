-- Old world, Azgaar's template: large masses parted by a strait, ranges and troughs across them.
--
--   terrain_viewer --script assets/scripts/lands/old_world.lua --water --resolution 128
--   terrain_export --script assets/scripts/lands/old_world.lua --min -96000,-96000 --max 96000,96000 --resolution 128 --out out/old_world
--
-- A 192 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height, land_fraction. Its land masses are `mass/1` (the largest), `mass/2` and on; `outline = "shapes"`
-- builds the composition of shapes instead, whose elements have ids an `edit` reaches.

local function main(params)
    local land = Lands.Continent{
        preset = "old_world", seed = params.seed or 3, resolution = params.resolution, height = params.height,
        land_fraction = params.land_fraction,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
