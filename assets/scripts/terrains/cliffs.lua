-- Cliffs: a coastal escarpment, a snowy plateau over three tiers of walls, bedded ledges on the faces and
-- a gullied lower slope stepping down to the sea through spurs and bays.
--
--   tools/dev.sh review assets/scripts/terrains/cliffs.lua --bounds -6144,-6144,6144,6144 --res 8 --level 0 --sun 180,35 --target 0,800,-1500 --shot 195,-14,6500 --shot 160,-30,9000
--
-- Built in stages, each one reviewable on its own with `--param stage=N`:
--
--   1. Forms, no noise: the coast and three tier edges are drawn curves; each is a polygon's signed
--      distance through a plateau profile. Dimensions are the World Machine graph this example first
--      followed: a plateau at `top` (2250 m), a lower slope of 650 m over 1.4 km, tiers of 500, 500 and
--      600 m with walls about 70 degrees, spurs and bays 2.5 km apart, a shelf to -160 m.
--   2. Structure: buttresses move the tier edges sideways at the scale of a column, so the pattern runs
--      down the face; strata move ground along the slope (World Machine's Strata), so they show on the
--      faces and not on the plateau: flat beds every 80 m and beds dipping 30 degrees every 380 m.
--   3. Erosion, coarse to fine, with the walls protected: 6 m of detail on gentle ground first, so the
--      erosion drains it, then gullies on the lower slope, water erosion on
--      gentle ground with its deposit capped, talus at the foot of the walls.
--   4. Detail: 2 m of fine noise on the walls; the gentle ground got its 6 m before the erosion.
--
-- The land rises towards -z so the faces look at the viewer's sun. Parameters: `top` (metres), `bays`
-- (0 to 1, how deep the bays are), `columns` (metres the buttresses stand out), `gullies` and
-- `roughness` (metres of gully cut and of lower-slope detail), `stage` (1 to 4).

local function main(params)
    local seed    = params.seed or 0
    local top     = params.top or 2250.0
    local bays    = params.bays or 0.6
    local columns = params.columns or 30.0
    local stage   = params.stage or 4
    local gullies = params.gullies or 120.0
    local rough6  = params.roughness or 6.0
    local s       = top / 2250.0
    local c       = Terrain.Coords{}

    -- A repeatable jitter in [-1, 1] for the drawn control points.
    local function jitter(i, salt)
        local v = math.sin(i * 127.1 + salt * 311.7 + seed * 74.7) * 43758.5453
        return (v - math.floor(v)) * 2.0 - 1.0
    end
    -- The land behind a smooth curve across the map: points from west to east at `z`, closed far inland.
    local function land(salt, z, spacing, wander, position)
        local points = {}
        for i = 0, math.floor(18000 / spacing) do
            local x = -9000 + i * spacing
            points[#points + 1] = { x, z(i) + wander * jitter(i, salt) }
        end
        points[#points + 1] = { 9000, -20000 }
        points[#points + 1] = { -9000, -20000 }
        return Shapes.Spline{ points = points, closed = true, fill = true, subdivisions = 6, position = position }
    end

    -- 1. Forms. The coast alternates headlands and bays 2.5 km apart, each a different depth; the tier
    -- edges are smoother and set back.
    local coast = land(1, function(i)
        return 2400 + (i % 2 == 0 and 1 or -1) * 700 * bays * (0.55 + 0.45 * jitter(i, 2))
    end, 1250, 150)
    local ramp  = Curves.Profile{ input = coast, kind = "smooth", width = 1400 } * (650 * s)

    -- Buttresses (stage 2) displace where the tier walls stand, at the scale of a column.
    local position = c
    if stage >= 2 then
        local ribs = Noises.Ridged{ frequency = 1.0 / 220.0, octaves = 1, amplitude = columns, seed = seed + 3 }
        position = Vec.Combine{ c.x, c.z + ribs.value }
    end
    -- Each tier edge wanders on its own, and the two lower tiers thin out and swell along the coast (one
    -- smooth octave, 3.5 km), so the benches pinch out and merge instead of running parallel end to end.
    local tiers = {
        { z = 900, spacing = 1300, wander = 450, rise = 500, vary = 0.6 },
        { z = 200, spacing = 1700, wander = 500, rise = 500, vary = 0.5 },
        { z = -600, spacing = 2200, wander = 550, rise = 600, vary = 0.0 },
    }
    -- The swell varies along the coast only (sampled at z = 0): varying across it too, it rippled the whole
    -- plateau into basins up to 170 m deep.
    -- What the lower tiers lose the main wall gains, so the plateau stays at `top` and does not tilt with
    -- them (it did, by 5 degrees).
    local alongCoast = Vec.Combine{ c.x, Terrain.Const{ value = 0.0 } }
    local form = ramp
    local lost = 0.0
    local firstEdge = nil
    for k, tier in ipairs(tiers) do
        local edge  = land(10 + k, function() return tier.z end, tier.spacing, tier.wander, position)
        firstEdge   = firstEdge or edge
        local rise  = tier.rise * s
        if tier.vary == 0.0 then
            rise = rise + lost
        else
            local swell = Noises.fBm{
                frequency = 1.0 / 3500.0, octaves = 1, amplitude = 0.5, seed = seed + 20 + k, position = alongCoast,
            }
            local cut = tier.vary * (0.5 - swell.value) * (tier.rise * s)
            rise = rise - cut
            lost = lost + cut
        end
        form = form + Curves.Profile{ input = edge, kind = "plateau", edge = 20, falloff = 220 * s } * rise
    end
    local shelf  = Curves.Smoothstep{ input = coast, min = 0.0, max = 1500.0 } * (-160.0)
    -- A regional slope with its divide on the main wall's edge (the same curve, without buttresses): in
    -- front of it every tread leans 1.2 % towards the sea, as benches do (flat ones held water); behind it
    -- the plateau is the back of a cuesta and dips 2 % inland. Both measured from one edge, so they meet
    -- in a ridge: measured from z, they met in a trough behind the wall that held a row of pits.
    local wallLine = land(13, function() return -600 end, 2200, 550, c)
    local front    = Curves.Clamp{ input = wallLine, min = 0.0, max = 100000.0 }
    local behind   = Curves.Clamp{ input = -wallLine, min = 0.0, max = 100000.0 }
    -- Not eased in: easing left a nearly flat band behind the edge where the plateau gullies, deeper than
    -- its slope, closed a row of pits. The crease it saves is a divide on top of a wall, out of sight.
    local back     = behind * (-0.02)
    local ground   = form + shelf + back - front * 0.012
    -- Steepness of the form alone: what later stages use to tell walls from gentle ground.
    local walls  = Masks.Slope{ input = Terrain.Gradient{ input = Filters.Blur{ input = ground, radius = 2 } }, min = 0.6, max = 1.5 }
    if stage == 1 then
        return { height = { value = ground, range = { -400, 2400 } },
                 normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = ground } } } }
    end

    -- 2. Strata along the slope: a bed's phase from its height (plus a dipping plane), the residue of a
    -- soft terrace of it, times how steep the wall is, so a flat plateau moves nowhere and a wall gets
    -- ledges. `depth` is how far a bed steps back horizontally, in metres: it has to span a few samples
    -- to be seen. The residue is continuous (a sharp terrace's sawtooth jumps once per bed, which on a
    -- 70 degree wall at 8 m aliases into spikes), and only the walls get beds: on the lower slope two bed
    -- sets drew a grid of contour lines.
    local steep = Curves.Clamp{ input = Masks.Slope{ input = Terrain.Gradient{ input = ground }, min = 0.0, max = 3.0 }, min = 0.0, max = 1.0 } * 3.0 * walls
    local function beds(input, bed, depth)
        local u = input / bed
        return (u - Curves.Terrace{ input = u, step = 1.0, sharpness = 0.5 }) * steep * depth
    end
    local heading = math.rad(140.0)
    local dip     = (c.x * math.cos(heading) + c.z * math.sin(heading)) * math.tan(math.rad(30.0))
    local layered = ground + beds(ground, 80.0, 24.0) + beds(ground + dip, 380.0, 40.0)
    if stage == 2 then
        return { height = { value = layered, range = { -400, 2400 } },
                 normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = layered } } } }
    end

    -- 3. Erosion, coarse to fine. Gullies from the smooth ramp's slope, so they run straight to the sea;
    -- water erosion only where the ground is gentle, its deposit capped a few tens of metres above the
    -- ground so nothing piles against a face; talus at the foot of the walls.
    -- Detail on gentle ground must stay shallower than the ground it lies on, or it holds water: noise of
    -- amplitude A and wavelength L makes slopes of about 2 pi A / L. Isotropic noise of 6 m over 300 m
    -- (0.12) left closed hollows over a sixth of the land on the 2 % plateau, which no local erosion
    -- drains. So the noise goes on the lower slope only (25 %), and the plateau gets gullies drawn from
    -- its own dip, which run downhill and close no basin.
    local gentle  = 1.0 - walls
    local sloping = Masks.Slope{ input = Terrain.Gradient{ input = ramp }, min = 0.05, max = 0.15 }
    local detail  = Noises.fBm{ frequency = 1.0 / 300.0, octaves = 4, amplitude = 1.0, seed = seed + 9 }
    -- Gullies space their stripes by the length of the gradient they get, so the 2 % dip is given scaled
    -- up to a slope like the lower slope's; only its direction matters here.
    local plateau = Erosion.Gullies{
        input = layered, gradient = Terrain.Gradient{ input = back * 20.0 }, cell = 700.0, octaves = 3,
        strength = 12.0 * s, full = 0.3, seed = seed + 13,
    }
    local rough   = Combine.Blend{ a = layered, b = plateau, mask = gentle * (1.0 - sloping) }
        + detail.value * gentle * sloping * rough6
    -- Guided by the distance to the coast, whose slope never fades: guided by the eased ramp, which
    -- flattens at the shore, every gully died out just above the sea and closed itself with a dam. They
    -- reach the sea and stop 1.3 to 1.7 km inland, under the first tier.
    local gullied = Erosion.Gullies{
        input = rough, gradient = Terrain.Gradient{ input = coast * -0.3 }, cell = 900.0, octaves = 3,
        strength = gullies * s, full = 0.3, seed = seed + 7,
    }
    -- Only in front of the first tier: on the narrow benches between tiers they closed wedge-shaped basins.
    local below   = Curves.Smoothstep{ input = firstEdge, min = 0.0, max = 200.0 }
    local cut     = Combine.Blend{ a = rough, b = gullied, mask = gentle * below * Curves.Smoothstep{ input = coast, min = -1700.0, max = -1300.0 } }
    local eroded  = Erosion.Hydraulic{ input = cut, iterations = 24, rain = 0.5, capacity = 16.0 }
    local kept    = Combine.Min{ a = Combine.Blend{ a = cut, b = eroded, mask = gentle }, b = cut + 25.0 }
    local talus   = Erosion.Thermal{ input = kept, iterations = 30, talus = math.tan(math.rad(36.0)), strength = 0.4 }
    local settled = Combine.Max{ a = kept, b = Combine.Blend{ a = kept, b = talus, mask = gentle } }
    if stage == 3 then
        return { height = { value = settled, range = { -400, 2400 } },
                 normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } } }
    end

    -- 4. Detail, last and small: 2 m of finer noise on the walls only, where no water stands.
    local grain = Noises.fBm{ frequency = 1.0 / 60.0, octaves = 3, amplitude = 1.0, seed = seed + 11 }
    local final = settled + grain.value * walls * 2.0
    return {
        height  = { value = final, range = { -400, 2400 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = final } } },
    }
end

return main
