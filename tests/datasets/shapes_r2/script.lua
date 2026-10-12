-- Case: shapes_r2
--
-- Everything the shape layer adds, in one graph, over a warped position so that no part of it is
-- axis-aligned. The golden is what notices a change to a distance formula, to the parity rule of a
-- polygon, to a profile or to the prelude; test_geometry says what each of those *should* be, and this
-- says that they have not moved.
--
-- The mask output is a polygon's own mask, which is where a vertex counted twice would show.

local function main(params)
    local position = Shapes.Warp{ amplitude = 35.0, frequency = 0.006, octaves = 3, seed = params.seed }

    local cone = Landforms.Cone{
        at = { -150, -120 }, radius = 260, height = 120.0, position = position,
    }
    local ridge = Landforms.Ridge{
        points = { { -480, 300 }, { -150, 180 }, { 200, 330 }, { 480, 150 } },
        width = 140.0, height = 70.0, position = position,
    }
    local mesa = Landforms.Plateau{
        at = { 230, -190 }, size = { 260, 170 }, height = 90.0, rotation = 24,
        edge = 4, falloff = 28, position = position,
    }
    local land = Combine.SmoothMax{
        a = Combine.SmoothMax{ a = cone, b = ridge, k = 40.0 }, b = mesa, k = 30.0,
    }
    local stepped = Curves.Terrace{ input = land, step = 20.0, sharpness = 0.5 }

    local outline = Shapes.Polygon{
        points = { { -300, -300 }, { 100, -380 }, { 360, -60 }, { 120, 120 }, { -60, -40 }, { -330, 60 } },
        position = position,
    }
    local mask = Shapes.Mask{ shape = outline, feather = 24.0 }

    return {
        height = { value = stepped, range = { -50, 200 } },
        mask   = { value = mask, range = { 0, 1 } },
    }
end

return main
