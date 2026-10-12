-- Canyon: a high plateau cut by a meandering gorge with stratified walls and a side canyon.
--
-- The gorge is the distance to a spline, offset by half the canyon's width and run through a plateau
-- profile: flat floor, a wall that is steep because `edge` and `falloff` are small against the depth,
-- and the original plateau beyond. The strata are `Curves.Terrace` applied only on the walls, through
-- a blend whose mask is the cut itself, so the plateau top and the canyon floor stay smooth while the
-- walls step. `depth` is a multiple of `step` so that the floor lands exactly on a step.
--
--   terrain_viewer --script assets/scripts/shapes/canyon.lua
--
-- Parameters (metres): rim is the plateau's height, depth the canyon's, width its width at the rim,
-- step the height of one stratum, wall how steep the walls are (smaller is steeper).

local function main(params)
    local rim   = params.rim or 520.0
    local step  = params.step or 40.0
    local depth = params.depth or step * 7.0
    local width = params.width or 680.0
    local wall  = params.wall or 120.0

    local position = Shapes.Warp{ amplitude = 90.0, frequency = 0.0018, octaves = 3, seed = params.seed }

    -- The plateau: nearly flat, with enough noise that the rim is not a ruled line.
    local plateau = Noises.fBm{
        frequency = 0.0014, octaves = 5, persistence = 0.5, amplitude = 26.0, seed = params.seed + 1,
    }

    -- The main gorge, then a side canyon that joins it. `Shapes.Union` of the two centrelines would
    -- give the distance to either; here each is cut separately and the deeper cut wins.
    local gorge = Shapes.Spline{
        points = {
            { -2600, -500 }, { -1700, 250 }, { -800, -300 }, { 100, 420 }, { 1000, -150 },
            { 1900, 380 }, { 2700, -100 },
        },
        subdivisions = 10,
        position = position,
    }
    local side = Shapes.Spline{
        points = { { -300, -2200 }, { -420, -1300 }, { -150, -650 }, { 100, 420 } },
        subdivisions = 8,
        position = position,
    }

    local function cut_of(line, half_width)
        -- Negative on the floor, zero at the rim, positive on the plateau.
        local signed = line - half_width
        return Curves.Profile{ input = signed, kind = "plateau", edge = 6.0, falloff = wall }
    end
    local gorge_cut = cut_of(gorge, width * 0.5)
    local side_cut  = cut_of(side, width * 0.28)
    -- 1 on a canyon floor and 0 on the plateau, whichever canyon it is.
    local cut = Combine.Max{ a = gorge_cut, b = side_cut }

    local deep = rim - cut * depth
    -- The side canyon is shallower: it hangs above the main floor where it joins.
    local top  = rim + plateau.value - gorge_cut * depth - side_cut * depth * 0.55
    local land = Combine.Min{ a = top, b = deep + plateau.value * (1.0 - cut) }

    -- Strata on the walls only: the blend mask is the cut, so it is 0 on the plateau and on the floor
    -- (where the terrace would only add a seam) and the steps appear where the ground is falling.
    local walls = Curves.Smoothstep{ input = cut, min = 0.02, max = 0.2 }
                  * (1.0 - Curves.Smoothstep{ input = cut, min = 0.8, max = 0.98 })
    local stepped = Curves.Terrace{ input = land, step = step, sharpness = 0.65 }
    local strata  = Combine.Blend{ a = land, b = stepped, mask = walls }

    -- Scree: loose material slides from the steps into the floor.
    local settled = Erosion.Thermal{
        input = strata, iterations = 10, talus = math.tan(math.rad(52.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { rim - depth - 80, rim + 120 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
