-- Archipelago, arc: an island arc: volcanic islands on a curve, a trench at the foot of the slope on its outer side.
--
--   terrain_viewer --script assets/scripts/lands/archipelago_arc.lua --water --resolution 32
--   terrain_export --script assets/scripts/lands/archipelago_arc.lua --min -48000,-48000 --max 48000,48000 --resolution 32 --out out/archipelago_arc
--
-- A 96 km map centred on the origin. Parameters: seed, count, land_fraction, height. The islands are
-- elements with ids ("isles/3", ...): `tools/dev.sh lands` lists them and `edit` changes one.

local function main(params)
    local land = Lands.Archipelago{
        preset = "arc", seed = params.seed or 6, resolution = params.resolution, size = 96000,
        count = params.count, land_fraction = params.land_fraction, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
