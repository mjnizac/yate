-- A closed basin: mountains on four sides round a sunken desert floor, no sea.
--
--   terrain_viewer --script assets/scripts/lands/taklamakan.lua --water --resolution 64
--   terrain_export --script assets/scripts/lands/taklamakan.lua --min -48000,-48000 --max 48000,48000 --resolution 64 --out out/taklamakan
--
-- A 96 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height. Its blocks, ranges ("north/crest/2", "north/spur/5") and seas have ids: `tools/dev.sh lands`
-- lists them and `edit` changes one.

local function main(params)
    local land = Lands.Continent{
        preset = "taklamakan", seed = params.seed or 9, resolution = params.resolution, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
