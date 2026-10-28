-- A mainland along one side, a peninsula with a spine of hills, islands off its tip.
--
--   terrain_viewer --script assets/scripts/lands/peninsula.lua --water --resolution 32
--   terrain_export --script assets/scripts/lands/peninsula.lua --min -24000,-24000 --max 24000,24000 --resolution 32 --out out/peninsula
--
-- A 48 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height. Its blocks, ranges ("spine/crest/2", "spine/spur/5") and seas have ids: `tools/dev.sh lands`
-- lists them and `edit` changes one.

local function main(params)
    local land = Lands.Continent{
        preset = "peninsula", seed = params.seed or 6, resolution = params.resolution, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
