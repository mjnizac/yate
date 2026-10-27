-- Water showcase: every kind of river and lake on one map, drawn and generated.
--
-- A 24 km map with the sea to the west. Each part of it shows something:
--
--   * north, a range: a glacial river in a U trough (traced down from its source) and a cirque lake;
--   * centre, a plateau at 800 m: a canyon cut through it with stepped walls, a crater lake on top;
--   * south-east, a basin under the sea level, closed: a trench lake at -45 m with cliffs and islands,
--     a gorge falling into it from the plateau;
--   * south, a plain: a meandering lowland river with floodplain, terraces and levees, a shelf lake;
--   * a braided river from the range across the plain to the sea;
--   * and what `Water.Network` found by itself: rivers with tributaries, a lake, and a valley lake
--     dammed on one of its rivers.
--
--   terrain_viewer --script assets/scripts/terrains/water_showcase.lua --water
--
-- Parameters: `rivers` and `lakes` (how many to generate, 0 for none), `stage` (1 the forms only).

local function main(params)
    local seed  = params.seed or 0
    local stage = params.stage or 2
    local c     = Terrain.Coords{}
    local warp  = Shapes.Warp{ amplitude = 450.0, frequency = 1.0 / 2600.0, octaves = 2, seed = seed + 1 }

    -- The land east of a wandering coast, the shelf beyond it.
    local coast = Shapes.Spline{
        points = { { -8000, -14000 }, { -7300, -8500 }, { -8700, -3500 }, { -7100, 1000 }, { -8300, 6000 },
                   { -7500, 14000 }, { 20000, 14000 }, { 20000, -14000 } },
        closed = true, fill = true, subdivisions = 4, position = warp,
    }
    local inland = coast * -1.0
    local land   = Curves.Smoothstep{ input = inland, min = -1500.0, max = 2500.0 }
    local ground = land * 150.0 - 110.0 + Curves.Clamp{ input = c.x + 8000.0, min = 0.0, max = 30000.0 } * 0.004
    ground = ground + Noises.fBm{ frequency = 1.0 / 1800.0, octaves = 5, amplitude = 45.0, seed = seed + 2 }.value * land

    -- The range in the north.
    local north = Curves.Smoothstep{ input = c.z, min = 3500.0, max = 9500.0 }
    local peaks = Noises.fBm{ frequency = 1.0 / 7000.0, octaves = 5, persistence = 0.45, amplitude = 0.5, seed = seed + 3, position = warp }
    local crests = Noises.Ridged{ frequency = 1.0 / 4000.0, octaves = 2, amplitude = 0.5, seed = seed + 5, position = warp }
    local rough = Noises.fBm{ frequency = 1.0 / 1500.0, octaves = 4, persistence = 0.45, amplitude = 30.0, seed = seed + 4 }
    ground = ground + north * (900.0 + 1100.0 * (peaks.value + 0.5) + 450.0 * (crests.value + 0.5) + rough.value) * land

    -- The plateau in the centre.
    local plateau = Shapes.Spline{
        points = { { -1500, -4200 }, { 6500, -4600 }, { 8200, -1000 }, { 6200, 2300 }, { -600, 1900 } },
        closed = true, fill = true, subdivisions = 4, position = warp,
    }
    ground = ground + Curves.Profile{ input = plateau, kind = "plateau", edge = 80.0, falloff = 600.0 } * 650.0

    -- The closed basin in the south-east, its floor at -90 m.
    local basin = Shapes.Ellipse{ at = { 7000, -7600 }, radii = { 3000, 2100 }, rotation = 15, position = warp }
    -- A cone 9 % steep from the floor out to 800 m past the basin's edge, then a wall: it lowers nothing
    -- further away.
    ground = Combine.Min{ a = ground, b = Curves.Clamp{ input = basin + 700.0, min = 0.0, max = 100000.0 } * 0.09 - 90.0
                                          + Curves.Clamp{ input = basin - 800.0, min = 0.0, max = 100000.0 } * 10.0 }

    local net = Water.Network{
        terrain = ground, region = { -12000, -12000, 12000, 12000 }, cell = 100.0, sea_level = 0.0, seed = seed,
        rivers = {
            { points = { { 3600, 11600 }, { 4300, 9000 }, { 4600, 6500 }, { 5200, 4300 } }, kind = "glacial", width = 30, name = "glacial" },
            { points = { { -600, 11600 }, { -1300, 9200 }, { -900, 6800 }, { -1500, 4600 } }, kind = "mountain", width = 16, name = "mountain" },
            { source = { -2500, 9200 }, kind = "braided", width = 22, name = "braided" },
            { points = { { 7600, 1200 }, { 5200, 300 }, { 3000, -1400 }, { 700, -2000 }, { -2600, -2600 }, { -6000, -3200 }, { -9000, -3600 } },
              kind = "canyon", width = 24, name = "canyon" },
            { points = { { 5600, -2600 }, { 6200, -3700 }, { 6600, -4900 }, { 6900, -5700 }, { 7000, -6400 } }, kind = "gorge", width = 12, name = "gorge" },
            { points = { { 6000, -10600 }, { 3000, -10000 }, { 0, -10600 }, { -3500, -9900 }, { -6500, -10400 }, { -9500, -10000 } },
              kind = "lowland", width = 40, roughness = 0.8, name = "lowland" },
        },
        lakes = {
            { at = { 1400, 9300 }, radius = 450, kind = "cirque", depth = 35, name = "cirque" },
            { at = { 7000, -7600 }, radius = 1500, level = -45, depth = 110, kind = "trench", islands = 0.5, name = "trench" },
            { at = { 1800, -3300 }, radius = 520, kind = "crater", depth = 60, name = "crater" },
            { at = { 1500, -7300 }, radius = 950, kind = "shelf", depth = 18, name = "shelf" },
        },
        generate = {
            rivers = { count = params.rivers or 3 }, tributaries = 0.2,
            river = { min_length = 3000, spacing = 3000 },
            lakes = { count = params.lakes or 1, on_rivers = 1, altitude = { 150, 4000 } },
        },
        shapes = params.shapes,
    }
    print(net:summary())

    if stage == 1 then
        return { height = { value = ground, range = { -200, 3000 } } }
    end
    local detail = Noises.fBm{ frequency = 1.0 / 220.0, octaves = 4, amplitude = 5.0, seed = seed + 9 }.value
    local wet = net:apply(ground + detail * land, { detail = detail * land })
    return {
        height  = { value = wet.height, range = { -200, 3000 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = wet.height } } },
        lakes   = { value = wet.lakes, range = { -25000, 3000 } },
        rivers  = { value = wet.rivers, range = { -25000, 3000 } },
        sea     = { value = wet.sea, range = { -25000, 3000 } },
    }
end

return main
