-- Archipelago, Azgaar's archipelago template: ranges and hills cut into islands by troughs and two straits.
--
--   terrain_viewer --script assets/scripts/lands/archipelago_scattered.lua --water --resolution 16
--   terrain_export --script assets/scripts/lands/archipelago_scattered.lua --min -24000,-24000 --max 24000,24000 --resolution 16 --out out/archipelago_scattered
--
-- A 48 km map centred on the origin. Parameters: seed, land_fraction, height. Its land masses are
-- `mass/1` (the largest), `mass/2` and on; `outline = "shapes"` builds the scatter of shapes instead, whose
-- islands have ids (`isles/3`) an `edit` reaches and whose `count` it takes.

local function main(params)
    local land = Lands.Archipelago{
        preset = "scattered", seed = params.seed or 3, resolution = params.resolution, size = 48000,
        land_fraction = params.land_fraction, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
