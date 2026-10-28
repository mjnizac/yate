-- Atoll: a ring of low reef islets round a lagoon, broken by passes where the sea comes in.
--
--   terrain_viewer --script assets/scripts/lands/atoll.lua --water --resolution 2
--   terrain_export --script assets/scripts/lands/atoll.lua --min -2000,-2000 --max 2000,2000 --resolution 2 --out out/atoll
--
-- A 4 km map centred on the origin. Parameters: seed, radius (of the ring, 1300 m by default), height.

local function main(params)
    local radius = params.radius or 1300
    local land = Lands.Compose{
        seed = params.seed or 6, resolution = params.resolution, height = params.height or 5,
        frame = { size = 4000 }, coast = "sedimentary", profile = "flat",
        beach_width = 30, beach_height = 1.5, lagoon_depth = 25,
        elements = {
            { id = "reef", shape = "ring", radius = radius, width = 160 },
            { id = "lagoon", shape = "circle", water = true, role = "lagoon", radius = radius - 80 },
            { id = "pass_n", shape = "capsule", water = true, at = { 0, radius }, length = 1, width = 260, smooth = 30 },
            { id = "pass_w", shape = "capsule", water = true, at = { -radius, 0 }, length = 1, width = 200, smooth = 30 },
            { id = "pass_se", shape = "capsule", water = true, at = { radius * 0.70, -radius * 0.71 }, length = 1, width = 230,
              smooth = 30 },
            { id = "motu", shape = "ellipse", radii = { 260, 120 }, at = { radius * 0.5, radius * 0.86 }, rotation = -30, smooth = 60 },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
