-- Alpine: a raised block carved by cirques and valleys, around one dominant summit on a main arête,
-- with branching gullies on every face. The summits, arêtes and spurs are what the carving leaves
-- standing, not forms added on top.
--
--   terrain_viewer --script assets/scripts/terrains/alpine.lua
--
-- The block is a long roof, highest under the main summit and falling along its axis to two lower
-- summits. Cirques bite into its two flanks, never across the axis, so the main arête runs unbroken to
-- the summit and the walls between neighbouring cirques are the spurs that leave it. Each cirque drains
-- through a valley floored at the country around. The cuts are drawn in their own wandering frame, so
-- their outlines are lobed and not the circles and ellipses they started as. `Erosion.Gullies` then cuts
-- the drainage pattern across every slope, which the erosion simulations are too local to draw.
--
-- Parameters: `height` (metres) is the main summit, `scale` enlarges the plan, `warp` (metres) how far it
-- wanders, `concave` how much the flanks ease into footslopes, `tributaries` and `gullies` (metres) how
-- deep the two generations of drainage cut and `cell` (metres) how far apart the tributaries are.

local function main(params)
    local height = params.height or 2800.0
    local warp   = params.warp or 220.0
    local scale  = params.scale or 1.2
    local seed   = params.seed

    local world = Shapes.Warp{ amplitude = warp, frequency = 0.0005, octaves = 3, seed = seed + 11 }
    local position = Vec.Combine{ world.x / scale, world.z / scale }

    -- Plan coordinates along the axis of the massif (`t`, west to east) and across it (`s`, north
    -- positive). The axis is turned 12 degrees, like the block.
    local angle = math.rad(12.0)
    local function at(t, s)
        return { t * math.cos(angle) - s * math.sin(angle), t * math.sin(angle) + s * math.cos(angle) }
    end

    -- 1. The block. A roof along the axis (linear across it, so the flanks are planes that meet in an
    -- edge), times a crown that peaks at the main summit and steps down to two lower ones on the axis.
    -- The distance to the axis, not to an ellipse: the kernel's distance inside an ellipse is a bound,
    -- not exact, and a roof built on it had flat patches and a step on the crest. A segment's distance
    -- is exact and its ends are round, which is the outline the massif wants anyway.
    local axis = Geometry.Segments{ points = { at(-6200, 0), at(6200, 0) }, closed = false,
                                    position = position }
    local roof = Curves.Profile{ input = axis - 4800, kind = "linear", width = 4800 }
    local function cone(t, radius)
        return Curves.Profile{
            input = Shapes.Point{ at = at(t, 0), position = position } - radius, kind = "linear",
            width = radius,
        }
    end
    local crown = 0.34 + 0.66 * cone(-400, 4200) + 0.2 * cone(2700, 2200) + 0.14 * cone(-3000, 2000)
    local block = roof * crown * height

    -- 2. The cuts, in a frame of their own that wanders, so a rim is lobed rather than a circle that
    -- has been moved. The bend is kept below about 0.6 in slope (amplitude x 2 pi x frequency): past 1 the
    -- frame folds over itself and a rim turns into a tangle of contours.
    local bend_x = Noises.fBm{ frequency = 0.0007, octaves = 2, amplitude = 1.0, seed = seed + 41,
                               position = position }
    local bend_z = Noises.fBm{ frequency = 0.0007, octaves = 2, amplitude = 1.0, seed = seed + 43,
                               position = position }
    local carve = Vec.Combine{ position.x + bend_x.value * 120.0, position.z + bend_z.value * 120.0 }

    -- A cirque is an ellipse elongated down its drainage. Its depth is `u (2 - u)` of the rise `u`
    -- from the rim: steepest at the rim and flattening to a point at the centre, which is a concave
    -- headwall over a spoon-shaped floor, never a flat one. `abrupt` is a smoothstep over the outer
    -- part over a little of the spoon: a steep wall and a broad floor that still drains.
    local function cirque(t, s, across, down, depth, style)
        local turn = s > 0 and 90 or -90
        local rim = Shapes.Ellipse{
            at = at(t, s), radii = { down, across }, rotation = 12 + turn, position = carve,
        }
        local u = Curves.Profile{ input = rim, kind = "linear", width = math.min(across, down) }
        local metres = u * (2.0 - u) * (height * depth)
        if style == "abrupt" then
            -- Mostly a smoothstep over the outer part, but a quarter of the spoon underneath, so the
            -- floor still falls towards the outlet instead of lying flat.
            local wall = Curves.Profile{ input = rim, kind = "smooth", width = math.min(across, down) * 0.6 }
            metres = (wall * 0.75 + u * (2.0 - u) * 0.25) * (height * depth)
        end
        return -metres
    end
    -- A valley from a cirque straight out across the flank, a trough about 1.9 times as wide as deep
    -- at half width, so its walls are near 40 degrees.
    local function valley(t, s, length, depth)
        local side = s > 0 and 1 or -1
        return Landforms.Trench{
            points = { at(t, s), at(t + 150 * side, s + side * length * 0.5), at(t, s + side * length) },
            width = depth * height * 3.8 / scale, depth = height * depth, profile = "smooth",
            position = carve,
        }
    end

    -- Cirque centres sit 1.3 to 1.9 km off the axis, so their headwalls stop short of it and the arête
    -- survives; sizes, depths and walls differ on purpose.
    local cuts = {
        -- Depths are kept so a cirque floor stays well above the country: deeper, and the floor is
        -- clamped flat to it. The valleys below are shallower than the block falls, so they reach the
        -- country only on its outer third.
        -- The main summit's two faces: the north one the largest and deepest, the south one abrupt.
        cirque(-500, 1800, 1300, 1700, 0.33, "concave"), cirque(-200, -1700, 1150, 1500, 0.27, "abrupt"),
        -- Between the main summit and the eastern one: a pair whose headwalls nearly meet, the col.
        cirque(1400, 1500, 900, 1200, 0.23, "abrupt"), cirque(1300, -1500, 950, 1200, 0.25, "concave"),
        -- The eastern summit.
        cirque(3300, 1500, 1000, 1300, 0.2, "concave"), cirque(3500, -1400, 850, 1100, 0.18, "abrupt"),
        -- The west, lower and smaller.
        cirque(-2300, 1500, 1000, 1300, 0.23, "concave"), cirque(-2500, -1500, 1050, 1300, 0.22, "concave"),
        cirque(-4300, 900, 800, 1000, 0.16, "concave"),
        valley(-500, 1800, 4200, 0.35), valley(-200, -1700, 4000, 0.32),
        valley(1400, 1500, 3800, 0.28), valley(1300, -1500, 3800, 0.28),
        valley(3300, 1500, 3500, 0.24), valley(3500, -1400, 3500, 0.22),
        valley(-2300, 1500, 3800, 0.28), valley(-2500, -1500, 3800, 0.27),
        valley(-4300, 900, 3000, 0.21),
    }
    local cut = cuts[1]
    for i = 2, #cuts do
        cut = Combine.SmoothMin{ a = cut, b = cuts[i], k = 40.0 }
    end

    -- 3. The country the valleys open onto: nothing is cut below it.
    local country = Noises.fBm{
        frequency = 0.0005, octaves = 3, amplitude = height * 0.02, seed = seed + 5, position = world,
    }
    -- Concave flanks: a mountain's slope is steepest under its crest and eases into long footslopes, not
    -- the straight faces of a roof. Raising the normalised height to a power above 1 does exactly that,
    -- and sharpens the summits on the way.
    local mountain = Curves.Power{
        input = Curves.Clamp{ input = (block + cut) / height, min = 0.0, max = 2.0 },
        exponent = params.concave or 1.5,
    } * height
    -- Added, not joined with a maximum: where the country dipped below the clamped foot of the mountain
    -- a maximum left flat patches at zero and a crease along every valley mouth.
    local structure = mountain + country.value

    -- Drainage in two generations, as in the references: tributary valleys a kilometre apart cut into
    -- every flank, then gullies cut into the walls of those tributaries. The second pass reads the slope
    -- the first left, so its gullies run down the tributaries' sides and join them, which is the nested,
    -- branching pattern of real relief; one pass at one scale gave combed faces. Drawn on the world
    -- position, so the scales are metres and not the plan's.
    local tributaries = Erosion.Gullies{
        input = structure, position = world, cell = params.cell or 1300.0, octaves = 3,
        strength = params.tributaries or 170.0, full = 0.35, seed = seed + 51,
    }
    local gullied = Erosion.Gullies{
        input = tributaries, position = world, cell = 360.0, octaves = 4,
        strength = params.gullies or 45.0, full = 0.5, seed = seed + 53,
    }
    -- On the mountain only: on the country around, the same cut at a gentle slope is a scribble.
    local land = Combine.Blend{
        a = structure, b = gullied,
        mask = Curves.Smoothstep{ input = mountain, min = height * 0.04, max = height * 0.2 },
    }

    local carved = Erosion.Hydraulic{
        input = land, iterations = 24, rain = 0.5, capacity = 9.0, evaporation = 0.04,
    }
    -- Hydraulic erosion leaves a one-sample ripple aligned with the grid; a radius-1 blur clears it.
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 14, talus = math.tan(math.rad(40.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -height * 0.2, height * 1.3 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
