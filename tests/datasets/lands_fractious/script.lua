-- Many pieces of land merging into each other, cut by long channels (Azgaar's fractious).
--
--   terrain_viewer --script assets/scripts/lands/fractious.lua --water --resolution 64
--   terrain_export --script assets/scripts/lands/fractious.lua --min -48000,-48000 --max 48000,48000 --resolution 64 --out out/fractious
--
-- A 96 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height. Its pieces ("pieces/3") and channels ("channels/2") have ids: `tools/dev.sh lands` lists them and
-- `edit` changes one.

local function main(params)
    local land = Lands.Continent{
        preset = "fractious", seed = params.seed or 8, resolution = params.resolution, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
