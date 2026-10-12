-- Alpine: one massif with a dominant summit, secondary summits on a connected crest, spurs separated by
-- continuous valleys that start in cirques, and detail kept subordinate to all of it.
--
--   terrain_viewer --script assets/scripts/terrains/alpine.lua
--
-- The structure is drawn, not generated. Reading it from the top: a main crest that runs west to east
-- and a lower range to the south; four summits of decreasing height on the crest; four spurs; a glacial
-- valley along the foot of the main crest and three that leave it between the spurs, each opening in a
-- cirque. One warped position is shared by every landform and by the ridged detail, so the spines
-- wander together. The detail is small: it breaks the regularity of the structure, it does not make it.
--
-- Parameters (metres): `height` is the height of the main crest above the valley floors, `warp` how far
-- the outlines wander.

local function main(params)
    local height = params.height or 1000.0
    local warp   = params.warp or 260.0
    local seed   = params.seed

    local position = Shapes.Warp{ amplitude = warp, frequency = 0.0005, octaves = 3, seed = seed + 11 }

    -- The crest barely varies: its rises and notches are the summits and passes below, not noise.
    local slow = Noises.fBm{ frequency = 0.0005, octaves = 2, amplitude = 1.0, seed = seed + 2,
                             position = position }
    local crest = height * 0.92 + slow.value * height * 0.08

    local function ridge(points, scale, width)
        return Landforms.Ridge{
            points = points, width = width, height = crest * scale, profile = "linear",
            position = position,
        }
    end
    local function summit(x, z, radius, scale)
        return Landforms.Cone{
            at = { x, z }, radius = radius, height = height * scale, position = position,
        }
    end
    local function join(a, b, k)
        return Combine.SmoothMax{ a = a, b = b, k = k }
    end

    -- The main crest and the lower range behind it.
    local main_crest = ridge(
        { { -3600, -600 }, { -1900, 250 }, { -300, -100 }, { 1300, 350 }, { 3600, -300 } }, 1.0, 2500)
    local south = ridge(
        { { -3500, -2700 }, { -1500, -2300 }, { 600, -2800 }, { 2600, -2400 }, { 3700, -2600 } },
        0.62, 2200)

    -- Four spurs, alternating north and south of the crest, each lower than the one it leaves.
    local spurs = join(
        join(ridge({ { -1900, 250 }, { -1750, 1250 }, { -2000, 2300 } }, 0.62, 1500),
             ridge({ { -300, -100 }, { -500, -1000 }, { -300, -1900 } }, 0.55, 1400), 220.0),
        join(ridge({ { 1300, 350 }, { 1800, 1250 }, { 1600, 2250 } }, 0.5, 1400),
             ridge({ { 1300, 350 }, { 2100, -500 }, { 2400, -1500 } }, 0.45, 1300), 220.0),
        220.0)

    -- Summits: one dominant, two secondary, one minor. Wide cones joined with a wide smooth maximum, so
    -- each is the culmination of the crest under it and the crest runs from one to the next.
    local summits = join(
        join(summit(-300, -100, 1800, 1.5), summit(-1900, 250, 1250, 1.25), 300.0),
        join(summit(1300, 350, 1150, 1.2), summit(3000, -200, 950, 1.05), 260.0),
        260.0)

    local massif = join(join(main_crest, south, 300.0), join(spurs, summits, 260.0), 260.0)

    -- Highland under and around the range, so it stands on country and not on a table.
    local highland = Landforms.Dome{
        at = { 0, -1100 }, radius = 6500, height = height * 0.3, profile = "smooth", position = position,
    }
    local foothills = Noises.fBm{
        frequency = 0.0008, octaves = 4, persistence = 0.5, amplitude = height * 0.1, seed = seed + 5,
        position = position,
    }

    -- Valleys, cut after the massif so they read as erosion of it. The long one runs along the foot of
    -- the main crest; the others leave it between the spurs and each begins in a cirque.
    local function valley(points, width, depth)
        return Landforms.Trench{
            points = points, width = width, depth = height * depth, profile = "smooth",
            position = position,
        }
    end
    local function cirque(x, z, radius, depth)
        return Landforms.Bowl{
            at = { x, z }, radius = radius, depth = height * depth, profile = "smooth",
            position = position,
        }
    end
    local cuts =
        valley({ { -3600, -1500 }, { -1200, -1300 }, { 700, -1600 }, { 3600, -1350 } }, 1300, 0.5)
        + valley({ { -1000, 350 }, { -900, 1300 }, { -1000, 2700 } }, 900, 0.5)
        + valley({ { 450, -250 }, { 600, -900 }, { 700, -1500 } }, 800, 0.5)
        + valley({ { 2500, 250 }, { 2800, 1200 }, { 2700, 2700 } }, 900, 0.45)
        + cirque(-1000, 350, 700, 0.45)
        + cirque(450, -250, 600, 0.42)
        + cirque(2500, 250, 650, 0.4)

    -- The cuts act where the massif stands, fading to nothing at its foot, so each valley opens out onto
    -- the country below instead of ending in a pit.
    local where = Curves.Smoothstep{ input = massif, min = height * 0.05, max = height * 0.55 }
    local structure = massif + cuts * where + highland + foothills.value

    -- Detail follows the structure: ridged relief where the ground is high and steep, almost none on
    -- the valley floors and the gentle shoulders. Small on purpose.
    local high = Curves.Smoothstep{ input = structure, min = height * 0.2, max = height * 0.9 }
    local steep = Masks.Slope{
        input = Terrain.Gradient{ input = structure }, min = 0.4, max = 0.95,
    }
    local walls = Noises.Ridged{
        frequency = 0.0026, octaves = 4, persistence = 0.5, amplitude = height * 0.07,
        seed = seed + 21, position = position,
    }
    local land = structure + walls.value * high * (0.3 + 0.7 * steep)

    local carved = Erosion.Hydraulic{
        input = land, iterations = 24, rain = 0.5, capacity = 9.0, evaporation = 0.04,
    }
    -- Hydraulic erosion leaves a one-sample ripple aligned with the grid; a radius-1 blur clears it.
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 14, talus = math.tan(math.rad(38.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -height * 0.7, height * 2.0 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
