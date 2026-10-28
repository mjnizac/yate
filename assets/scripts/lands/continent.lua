-- A continent: a block with lobes, two gulfs, a main range with saddles and spurs and a lower one.
--
--   terrain_viewer --script assets/scripts/lands/continent.lua --water --resolution 128
--   terrain_export --script assets/scripts/lands/continent.lua --min -96000,-96000 --max 96000,96000 --resolution 128 --out out/continent
--
-- A 192 km map centred on the origin (the viewer shows about 28 km of it around --target). Parameters:
-- seed, height. Its blocks, ranges ("range/crest/2", "range/spur/5") and seas have ids: `tools/dev.sh lands`
-- lists them and `edit` changes one.

local function main(params)
    local land = Lands.Continent{
        preset = "continent", seed = params.seed or 1, resolution = params.resolution, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
