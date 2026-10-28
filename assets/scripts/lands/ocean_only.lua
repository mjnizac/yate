-- All sea: no land at all, the abyssal plain with a seamount, a guyot (a seamount planed flat at the
-- surface long ago and sunk since) and a smaller cone between them.
--
--   terrain_viewer --script assets/scripts/lands/ocean_only.lua --water --resolution 16
--   terrain_export --script assets/scripts/lands/ocean_only.lua --min -24000,-24000 --max 24000,24000 --resolution 16 --out out/ocean_only
--
-- A 48 km map centred on the origin, the sea 700 m deep (the compressed margin). Parameters: seed.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 8, resolution = params.resolution, height = 100,
        frame = { size = 48000 },
        elements = {
            { id = "seamount", shape = "radial", radius = 7000, at = { -9000, 6000 }, seabed = true,
              harmonics = { { 3, 0.12, 0.4 } }, profile = { "cone", height = 560 } },
            { id = "guyot", shape = "circle", radius = 6000, at = { 9000, -5000 }, seabed = true,
              profile = { "plateau", height = 520, edge = 2200 } },
            { id = "knoll", shape = "circle", radius = 2500, at = { 2000, 12000 }, seabed = true,
              profile = { "cone", height = 300 } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
