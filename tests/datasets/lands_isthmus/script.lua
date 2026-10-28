-- Two landmasses joined by a narrow, low isthmus.
--
--   terrain_viewer --script assets/scripts/lands/isthmus.lua --water --resolution 32
--   terrain_export --script assets/scripts/lands/isthmus.lua --min -24000,-24000 --max 24000,24000 --resolution 32 --out out/isthmus
--
-- A 48 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height. Its blocks, ranges ("west_range/crest/2", "west_range/spur/5") and seas have ids: `tools/dev.sh lands`
-- lists them and `edit` changes one.

local function main(params)
    local land = Lands.Continent{
        preset = "isthmus", seed = params.seed or 7, resolution = params.resolution, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
