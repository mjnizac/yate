-- Dune regions: dunes only inside polygons, fading to flat ground over `feather` metres at their edges.
--
--   terrain_viewer --script assets/scripts/terrains/dune_regions.lua
--
-- `area` / `areas` take polygons as lists of { x, z } points in metres. The edge wanders by
-- `edge_wander`, so a region reads as a field of sand and not as a drawn outline. Every other key is
-- the dune macro's own (see terrains/dunes.lua).

local function main(params)
    local field = Dunes.Field{
        seed = params.seed,
        wind = params.wind,
        areas = {
            -- A broad, irregular field.
            { { -5200, -2600 }, { -3000, -3600 }, { 400, -3300 }, { 2600, -2200 }, { 3000, 200 },
              { 1200, 2100 }, { -1800, 2600 }, { -4300, 1400 } },
            -- A smaller one beside it, separated by bare ground.
            { { 4300, -600 }, { 6800, -1500 }, { 7600, 1000 }, { 6200, 3000 }, { 4000, 2200 } },
        },
        feather = params.feather or 700.0,
        edge_wander = params.edge_wander,
    }
    return field
end

return main
