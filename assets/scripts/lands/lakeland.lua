-- All land, a low plain scattered with lakes.
--
--   terrain_viewer --script assets/scripts/lands/lakeland.lua --water --resolution 16
--   terrain_export --script assets/scripts/lands/lakeland.lua --min -12000,-12000 --max 12000,12000 --resolution 16 --out out/lakeland
--
-- A 24 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height. Its lakes ("lakes/4") have ids: `tools/dev.sh lands` lists them and `edit` changes one.

local function main(params)
    local land = Lands.Continent{
        preset = "lakeland", seed = params.seed or 10, resolution = params.resolution, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
