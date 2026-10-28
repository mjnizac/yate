-- A supercontinent: four blocks round a central band, two inland seas, ranges where the blocks meet.
--
--   terrain_viewer --script assets/scripts/lands/pangea.lua --water --resolution 128
--   terrain_export --script assets/scripts/lands/pangea.lua --min -96000,-96000 --max 96000,96000 --resolution 128 --out out/pangea
--
-- A 192 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height. Its blocks, ranges ("range1/crest/2", "range1/spur/5") and seas have ids: `tools/dev.sh lands`
-- lists them and `edit` changes one.

local function main(params)
    local land = Lands.Continent{
        preset = "pangea", seed = params.seed or 2, resolution = params.resolution, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
