-- A continent with a closed sea inside it.
--
--   terrain_viewer --script assets/scripts/lands/inland_sea.lua --water --resolution 64
--   terrain_export --script assets/scripts/lands/inland_sea.lua --min -48000,-48000 --max 48000,48000 --resolution 64 --out out/inland_sea
--
-- A 96 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height. Its blocks, ranges ("range/crest/2", "range/spur/5") and seas have ids: `tools/dev.sh lands`
-- lists them and `edit` changes one.

local function main(params)
    local land = Lands.Continent{
        preset = "inland_sea", seed = params.seed or 5, resolution = params.resolution, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
