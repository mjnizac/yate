-- Mountain range: ridges along splines, peaks on the crest, a valley cut between, and relief on top.
--
-- The massif is landforms joined with a smooth maximum, so the seams where a spur leaves the main ridge
-- are rounded instead of creased. One warped position is shared by every landform, which bends the whole
-- range together; without it the ridges are exactly the splines they were drawn from, and the eye reads
-- a diagram.
--
--   terrain_viewer --script assets/scripts/shapes/mountain_range.lua
--
-- Parameters (metres): height is the main ridge, width its width, valley how deep the cut between the
-- ridges is, warp how far the outlines wander.

local function main(params)
    local height = params.height or 1050.0
    local width  = params.width or 1500.0
    local valley = params.valley or 220.0
    local warp   = params.warp or 130.0

    local position = Shapes.Warp{ amplitude = warp, frequency = 0.0012, octaves = 3, seed = params.seed }

    -- A broad swell under the whole range, so the ridges stand on a highland and not on a plain.
    local highland = Landforms.Dome{
        at = { 0, -200 }, radius = 2600, height = 300, profile = "smooth", position = position,
    }

    -- The main ridge and a spur that leaves it to the north.
    local main_ridge = Landforms.Ridge{
        points   = { { -1900, -300 }, { -950, 250 }, { 0, -150 }, { 950, 320 }, { 1900, -40 } },
        width    = width, height = height, profile = "linear", position = position,
    }
    local spur = Landforms.Ridge{
        points   = { { -150, 0 }, { 250, 900 }, { 420, 1800 } },
        width    = width * 0.7, height = height * 0.72, profile = "linear", position = position,
    }
    -- A second, lower range to the south, parallel to the first.
    local south = Landforms.Ridge{
        points   = { { -1800, -1500 }, { -600, -1200 }, { 700, -1650 }, { 1800, -1300 } },
        width    = width * 0.8, height = height * 0.6, profile = "linear", position = position,
    }

    -- Peaks on the crest: cones that stand above the ridge they sit on.
    local peaks = Combine.SmoothMax{
        a = Landforms.Cone{ at = { -950, 250 }, radius = 520, height = height * 1.35,
                            position = position },
        b = Landforms.Cone{ at = { 950, 320 }, radius = 460, height = height * 1.2,
                            position = position },
        k = 90.0,
    }
    local lower_peak = Landforms.Cone{
        at = { 250, 900 }, radius = 380, height = height * 0.95, position = position,
    }

    local massif = Combine.SmoothMax{
        a = Combine.SmoothMax{ a = Combine.SmoothMax{ a = main_ridge, b = spur, k = 140.0 },
                               b = south, k = 140.0 },
        b = Combine.SmoothMax{ a = peaks, b = lower_peak, k = 90.0 },
        k = 110.0,
    }
    local ranges = Combine.SmoothMax{ a = massif, b = highland, k = 400.0 }

    -- The valley between the two ranges, cut after the massif so it reads as erosion of it.
    local cut = Landforms.Trench{
        points = { { -1700, -750 }, { -400, -620 }, { 900, -800 }, { 1800, -700 } },
        width  = 520, depth = valley, profile = "smooth", position = position,
    }

    -- Relief: broad hills everywhere and sharp ridged detail on the massif only, found by reading how
    -- high the massif already is.
    local hills = Noises.fBm{
        frequency = 0.0016, octaves = 5, persistence = 0.5, amplitude = 60.0, seed = params.seed + 3,
    }
    local detail = Noises.Ridged{
        frequency = 0.0042, octaves = 5, amplitude = 150.0, seed = params.seed + 4,
    }
    local on_massif = Curves.Smoothstep{ input = massif, min = 120.0, max = height * 0.75 }

    local land = ranges + cut + hills.value + detail.value * on_massif

    local carved = Erosion.Hydraulic{
        input = land, iterations = 24, rain = 0.4, capacity = 40.0,
    }
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled  = Erosion.Thermal{
        input = smoothed, iterations = 10, talus = math.tan(math.rad(36.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -300, height * 1.7 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
