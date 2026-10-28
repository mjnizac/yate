-- Archipelago, cluster: islands in a few clusters (a Neyman-Scott process), small ones crowded round each centre.
--
--   terrain_viewer --script assets/scripts/lands/archipelago_cluster.lua --water --resolution 16
--   terrain_export --script assets/scripts/lands/archipelago_cluster.lua --min -24000,-24000 --max 24000,24000 --resolution 16 --out out/archipelago_cluster
--
-- A 48 km map centred on the origin. Parameters: seed, count, land_fraction, height. The islands are
-- elements with ids ("isles/3", ...): `tools/dev.sh lands` lists them and `edit` changes one.

local function main(params)
    local land = Lands.Archipelago{
        preset = "cluster", seed = params.seed or 4, resolution = params.resolution, size = 48000,
        count = params.count, land_fraction = params.land_fraction, height = params.height,
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
