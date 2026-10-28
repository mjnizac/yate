-- Archipelago, hotspot: a hotspot chain: the youngest island large and high at one end, older ones smaller and lower along it.
--
--   terrain_viewer --script assets/scripts/lands/archipelago_hotspot.lua --water --resolution 16
--   terrain_export --script assets/scripts/lands/archipelago_hotspot.lua --min -24000,-24000 --max 24000,24000 --resolution 16 --out out/archipelago_hotspot
--
-- A 48 km map centred on the origin. Parameters: seed, count, land_fraction, height. The islands are
-- elements with ids ("isles/3", ...): `tools/dev.sh lands` lists them and `edit` changes one.

local function main(params)
    local land = Lands.Archipelago{
        preset = "hotspot", seed = params.seed or 5, resolution = params.resolution, size = 48000,
        count = params.count, land_fraction = params.land_fraction, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
