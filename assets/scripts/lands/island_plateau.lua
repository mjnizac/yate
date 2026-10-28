-- Mesa island: a flat top a few hundred metres up, edged by a steep escarpment, built from a
-- superellipse and a smaller block joined to it.
--
--   terrain_viewer --script assets/scripts/lands/island_plateau.lua --water --resolution 4
--   terrain_export --script assets/scripts/lands/island_plateau.lua --min -6000,-6000 --max 6000,6000 --resolution 4 --out out/island_plateau
--
-- A 12 km map centred on the origin. Parameters: seed, area (square metres, 28 km^2 by default), height.

local function main(params)
    local land = Lands.Compose{
        seed = params.seed or 3, resolution = params.resolution, height = params.height or 320,
        frame = { size = 12000 }, coast = "cliff",
        profile = { "plateau", edge = 260 },
        elements = {
            { id = "mesa", shape = "superellipse", area = params.area or 28e6, aspect = 1.6, exponent = 3,
              rotation = 25 },
            { id = "butte", shape = "superellipse", radii = { 900, 650 }, exponent = 2.5, at = { -3600, 2400 },
              smooth = 500, profile = { "plateau", height = 210, edge = 200 } },
        },
    }
    return land:outputs{ "coast_type", "land_mask", "coast_sdf", "region_id" }
end

return main
