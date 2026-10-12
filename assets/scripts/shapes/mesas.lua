-- Mesas and badlands: flat-topped plateaus of several sizes, pyramids and domes around them, strata,
-- and talus at the foot.
--
-- Each mesa is `Landforms.Plateau`: a rectangle, rotated, whose wall is a plateau profile a few metres
-- either side of its outline, so the top is flat and the face is a scarp. The buttes are pyramids and
-- domes. Everything is joined with a smooth maximum so a butte that stands against a mesa is a spur of
-- it and not a crease. Strata come from a terrace over the whole heightfield, and the thermal pass at
-- the end, with a high talus, is what drops scree at the foot of the scarps without rounding the tops.
--
--   terrain_viewer --script assets/scripts/shapes/mesas.lua
--
-- Parameters (metres): height is the main mesa, step the height of one stratum, warp how far the
-- outlines wander.

local function main(params)
    local height = params.height or 520.0
    local step   = params.step or 36.0
    local warp   = params.warp or 70.0

    local position = Shapes.Warp{ amplitude = warp, frequency = 0.0022, octaves = 3, seed = params.seed }

    -- The mesas: three sizes, three heights, three bearings.
    local grand = Landforms.Plateau{
        at = { -300, 150 }, size = { 1500, 900 }, height = height, rotation = 18,
        edge = 6, falloff = 60, rounding = 80, position = position,
    }
    local middle = Landforms.Plateau{
        at = { 1050, -500 }, size = { 760, 620 }, height = height * 0.74, rotation = -32,
        edge = 6, falloff = 55, rounding = 50, position = position,
    }
    local small = Landforms.Plateau{
        at = { 650, 950 }, size = { 380, 300 }, height = height * 0.52, rotation = 55,
        edge = 5, falloff = 40, rounding = 30, position = position,
    }
    local far = Landforms.Plateau{
        at = { -1500, -900 }, size = { 640, 520 }, height = height * 0.62, rotation = 8,
        edge = 6, falloff = 50, rounding = 40, position = position,
    }

    -- The buttes: a pyramid and two domes, standing apart from the mesas.
    local pyramid = Landforms.Pyramid{
        at = { -1050, 720 }, size = { 560, 560 }, height = height * 0.8, rotation = 22,
        position = position,
    }
    local dome_a = Landforms.Dome{
        at = { 1700, 450 }, radius = 380, height = height * 0.55, profile = "dome", position = position,
    }
    local dome_b = Landforms.Dome{
        at = { -700, -1300 }, radius = 300, height = height * 0.45, profile = "dome",
        position = position,
    }

    local function join(a, b) return Combine.SmoothMax{ a = a, b = b, k = 45.0 } end
    local relief = join(join(join(grand, middle), join(small, far)),
                        join(pyramid, join(dome_a, dome_b)))

    -- The floor between them: a gently dissected plain, raised slightly towards the mesas.
    local plain = Noises.fBm{
        frequency = 0.0018, octaves = 5, persistence = 0.5, amplitude = 22.0, seed = params.seed + 1,
    }
    local gullies = Noises.Ridged{
        frequency = 0.0052, octaves = 4, amplitude = 22.0, seed = params.seed + 2,
    }
    local land = relief + plain.value + gullies.value

    -- Strata. A soft terrace so each bed has a face and a short tread, applied to everything, which is
    -- what makes the badlands read as layered rock rather than as noise.
    local stepped = Curves.Terrace{ input = land, step = step, sharpness = 0.55 }

    local carved = Erosion.Hydraulic{
        input = stepped, iterations = 14, rain = 0.35, capacity = 40.0,
    }
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled  = Erosion.Thermal{
        input = smoothed, iterations = 12, talus = math.tan(math.rad(54.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -120, height + 140 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
