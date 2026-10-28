-- Archipelago, shattered: one land broken into pieces by channels, Azgaar's 'shattered' template from shapes.
--
--   terrain_viewer --script assets/scripts/lands/archipelago_shattered.lua --water --resolution 16
--   terrain_export --script assets/scripts/lands/archipelago_shattered.lua --min -24000,-24000 --max 24000,24000 --resolution 16 --out out/archipelago_shattered
--
-- A 48 km map centred on the origin. Parameters: seed, count, land_fraction, height. The islands are
-- elements with ids ("isles/3", ...): `tools/dev.sh lands` lists them and `edit` changes one.

local function main(params)
    local land = Lands.Archipelago{
        preset = "shattered", seed = params.seed or 8, resolution = params.resolution, size = 48000,
        count = params.count, land_fraction = params.land_fraction, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
