-- Alpine: a ridged massif with a hierarchy of summits, joined crests, spurs, glacial valleys and rock
-- walls cut by gullies.
--
--   terrain_viewer --script assets/scripts/terrains/alpine.lua
--
-- The structure comes first and the noise second. A main range runs west to east with a lower range
-- to its south; spurs leave the main crest, three summits stand on it, and a deep valley separates the
-- two ranges. One warped position is shared by every landform and by the detail noise, so crests wander
-- together and the detail follows them instead of cutting across. Parameters: `height` is the main
-- crest in metres above the valley floors, `warp` how far the outlines wander.

local function main(params)
    local height = params.height or 1000.0
    local warp   = params.warp or 240.0
    local seed   = params.seed

    local position = Shapes.Warp{ amplitude = warp, frequency = 0.0006, octaves = 3, seed = seed + 11 }

    -- The crest is not one height: a slow noise lifts it into summits and drops it into passes.
    local swell = Noises.fBm{ frequency = 0.0011, octaves = 3, amplitude = 1.0, seed = seed + 2,
                              position = position }
    local crest = height * 0.85 + swell.value * height * 0.35

    -- The main range, a lower range to the south, and four spurs that leave the main crest.
    local main_range = Landforms.Ridge{
        points = { { -3600, -700 }, { -1900, 250 }, { -300, -150 }, { 1300, 350 }, { 3600, -250 } },
        width = 2300, height = crest, profile = "linear", position = position,
    }
    local south_range = Landforms.Ridge{
        points = { { -3500, -2600 }, { -1500, -2200 }, { 600, -2750 }, { 2600, -2300 }, { 3700, -2500 } },
        width = 2000, height = crest * 0.62, profile = "linear", position = position,
    }
    local spur = function(points, scale, width)
        return Landforms.Ridge{
            points = points, width = width, height = crest * scale, profile = "linear",
            position = position,
        }
    end
    local spurs = Combine.SmoothMax{
        a = Combine.SmoothMax{
            a = spur({ { -1900, 250 }, { -1700, 1300 }, { -1900, 2300 } }, 0.62, 1500),
            b = spur({ { -300, -150 }, { -450, -1100 }, { -250, -1900 } }, 0.55, 1400),
            k = 200.0,
        },
        b = Combine.SmoothMax{
            a = spur({ { 1300, 350 }, { 1700, 1250 }, { 1500, 2200 } }, 0.5, 1400),
            b = spur({ { 1300, 350 }, { 2000, -500 }, { 2300, -1500 } }, 0.45, 1300),
            k = 200.0,
        },
        k = 200.0,
    }

    -- Summits on the main crest, joined to it with a wide smooth maximum so they are the high points of
    -- the chain and not cones stuck on it.
    local summits = Combine.SmoothMax{
        a = Landforms.Cone{ at = { -1900, 250 }, radius = 1000, height = height * 1.45, position = position },
        b = Combine.SmoothMax{
            a = Landforms.Cone{ at = { 1300, 350 }, radius = 900, height = height * 1.3, position = position },
            b = Landforms.Cone{ at = { -300, -150 }, radius = 700, height = height * 1.05, position = position },
            k = 120.0,
        },
        k = 120.0,
    }

    local massif = Combine.SmoothMax{
        a = Combine.SmoothMax{ a = main_range, b = south_range, k = 260.0 },
        b = Combine.SmoothMax{ a = spurs, b = summits, k = 220.0 },
        k = 220.0,
    }

    -- The glacial valley between the two ranges, cut after the massif so it reads as erosion of it.
    local valley = Landforms.Trench{
        points = { { -3600, -1500 }, { -1200, -1250 }, { 700, -1600 }, { 3600, -1350 } },
        width = 1300, depth = height * 0.5, profile = "smooth", position = position,
    }
    -- Under and around the range: a highland that rises toward it, with foothills on top, so the massif
    -- stands on country and not on a table.
    local highland = Landforms.Dome{
        at = { 0, -1100 }, radius = 6500, height = height * 0.3, profile = "smooth", position = position,
    }
    local foothills = Noises.fBm{
        frequency = 0.0008, octaves = 5, persistence = 0.5, amplitude = height * 0.14, seed = seed + 5,
        position = position,
    }
    local structure = massif + valley + highland + foothills.value

    -- Detail follows the structure: sharp ridged relief where the ground is high and steep, almost
    -- none on the valley floors and the gentle shoulders.
    local high = Curves.Smoothstep{ input = structure, min = height * 0.15, max = height * 0.8 }
    local steep = Masks.Slope{
        input = Terrain.Gradient{ input = structure }, min = 0.35, max = 0.9,
    }
    local walls = Noises.Ridged{
        frequency = 0.0028, octaves = 5, persistence = 0.5, amplitude = height * 0.13,
        seed = seed + 21, position = position,
    }
    local texture = Noises.fBm{
        frequency = 0.012, octaves = 3, amplitude = height * 0.03, seed = seed + 31,
    }
    local land = structure + walls.value * high * (0.35 + 0.65 * steep) + texture.value * high

    local carved = Erosion.Hydraulic{
        input = land, iterations = 28, rain = 0.5, capacity = 9.0, evaporation = 0.04,
    }
    -- Hydraulic erosion leaves a one-sample ripple aligned with the grid; a radius-1 blur clears it.
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 14, talus = math.tan(math.rad(38.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -height * 0.6, height * 1.9 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
