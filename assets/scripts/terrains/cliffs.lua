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
    local gullies = params.gullies or 40.0
    local rough6  = params.roughness or 6.0
    local s       = top / 2250.0
    local c       = Terrain.Coords{}

    -- A repeatable jitter in [-1, 1] for the drawn control points.
    local function jitter(i, salt)
        local v = math.sin(i * 127.1 + salt * 311.7 + seed * 74.7) * 43758.5453
        return (v - math.floor(v)) * 2.0 - 1.0
    end
    -- A smooth curve across the map: points from west to east at `z`.
    local function curve(salt, z, spacing, wander)
        local points = {}
        for i = 0, math.floor(18000 / spacing) do
            points[#points + 1] = { -9000 + i * spacing, z(i) + wander * jitter(i, salt) }
        end
        return points
    end
    -- Where a curve crosses `x`, between its control points.
    local function zAt(points, x)
        for i = 2, #points do
            if x <= points[i][1] then
                local a, b = points[i - 1], points[i]
                return a[2] + (b[2] - a[2]) * (x - a[1]) / (b[1] - a[1])
            end
        end
        return points[#points][2]
    end
    -- The land behind a curve, closed far inland.
    local function fill(points, position)
        local closed = {}
        for i, p in ipairs(points) do
            closed[i] = p
        end
        closed[#closed + 1] = { 9000, -20000 }
        closed[#closed + 1] = { -9000, -20000 }
        return Shapes.Spline{ points = closed, closed = true, fill = true, subdivisions = 6, position = position }
    end
    local function land(salt, z, spacing, wander, position)
        return fill(curve(salt, z, spacing, wander), position)
    end

    -- The valleys that reach the sea: the main ones in the bays, minor ones between them.
    local valleys = {}
    for i = 1, math.floor(18000 / 1250) do
        local major = i % 2 == 1
        valleys[#valleys + 1] = { i = i, major = major, x = -9000 + i * 1250 + (major and 200 or 450) * jitter(i, 5) }
    end
    -- 1. Forms. The coast alternates headlands and bays 2.5 km apart, each a different depth; the tier
    -- edges are smoother and set back.
    local coastPoints = curve(1, function(i)
        return 2400 + (i % 2 == 0 and 1 or -1) * 700 * bays * (0.55 + 0.45 * jitter(i, 2))
    end, 1250, 150)
    local coast = fill(coastPoints, c)
    local ramp  = Curves.Profile{ input = coast, kind = "smooth", width = 1400 } * (650 * s)

    -- The lower slope carved by its own drainage, Gaea's first, structural erosion pass (real slopes
    -- follow their river network, which a gully pattern does not): the ramp and the shelf are rebuilt
    -- on a 256 grid in Lua, a shallow trench is seeded down each main valley so the valleys meet the
    -- amphitheatres above them, and `Erosion.StreamPower` cuts it with no uplift. Inland of the ramp the
    -- grid is put back as it was, so only the lower slope changes. The grid covers +-6.4 km.
    local n, cell, origin = 256, 50.0, -6400.0
    local function coastDistance(px, pz)
        local best = math.huge
        for i = 2, #coastPoints do
            local a, b = coastPoints[i - 1], coastPoints[i]
            local dx, dz = b[1] - a[1], b[2] - a[2]
            local u = math.max(0.0, math.min(1.0, ((px - a[1]) * dx + (pz - a[2]) * dz) / (dx * dx + dz * dz)))
            local ex, ez = px - a[1] - u * dx, pz - a[2] - u * dz
            best = math.min(best, math.sqrt(ex * ex + ez * ez))
        end
        return pz < zAt(coastPoints, px) and best or -best
    end
    local function ease(u)
        u = math.max(0.0, math.min(1.0, u))
        return u * u * (3.0 - 2.0 * u)
    end
    local start, inland, shore = {}, {}, {}
    for zi = 0, n - 1 do
        for xi = 0, n - 1 do
            local px, pz = origin + xi * cell, origin + zi * cell
            local d = coastDistance(px, pz)
            shore[#shore + 1] = d
            local h = d >= 0 and ease(d / 1400.0) * 650.0 * s or -160.0 * ease(-d / 1500.0)
            if d > 0 and d < 1400 then
                for _, v in ipairs(valleys) do
                    if v.major then
                        h = h - 30.0 * s * math.exp(-((px - v.x) / 220.0) ^ 2) * math.sin(math.pi * d / 1400.0)
                    end
                end
            end
            start[#start + 1]  = h
            -- Kept as it was inland of the ramp and under the sea, where the drainage rippled the shelf.
            inland[#inland + 1] = math.max(ease((d - 1150.0) / 250.0), ease(-d / 200.0))
        end
    end
    local still = {}
    for i = 1, #start do still[i] = 0.0 end
    local carvedRamp = Erosion.StreamPower{
        size = { n, n }, cell = cell, height = start, uplift = still, iterations = 160, k = 0.016,
        m = 0.5, talus = 0.7, diffusion = 0.012 * cell * cell, refresh = 4,
    }
    -- The shore, after the drainage and with the same distance, so it lands exactly on the coast (World
    -- Machine's Coastal Erosion, Gaea's Sea): the headlands, exposed to the waves, end in a sea cliff of
    -- 100 to 220 m that merges into the slope over 400 m; the bays, sheltered, are flattened to a 4 %
    -- beach over their first 300 m, valley mouths included.
    for i = 1, #carvedRamp do
        local h = carvedRamp[i] + (start[i] - carvedRamp[i]) * inland[i]
        local d = shore[i]
        if d > 0 then
            local px    = origin + ((i - 1) % n) * cell
            local k     = math.floor((px + 9000) / 1250 + 0.5)
            local head  = (0.5 + 0.5 * math.cos(math.pi * (px + 9000) / 1250)) * (0.6 + 0.4 * jitter(k, 7))
            local cliff = (100.0 + 120.0 * math.abs(jitter(k, 8))) * s * head
            h = h + cliff * (1.0 - ease(d / 400.0))
            local beach = (1.0 - head) * (1.0 - ease(d / 300.0))
            h = h + (math.min(h, d * 0.04) - h) * beach
        end
        carvedRamp[i] = h
    end
    -- Read through a broad warp: eight-direction routing on a square grid draws ruled channels.
    local slope = Terrain.Grid{
        values = carvedRamp, size = { n, n }, origin = { origin, origin }, cell = cell,
        position = Shapes.Warp{ amplitude = 120.0, frequency = 1.0 / 1600.0, octaves = 2, seed = seed + 90 },
    }

    -- Buttresses (stage 2) displace where the tier walls stand, at the scale of a column. The breaks are
    -- distorted F3 - F1 cells (World Machine's Mountain Cliffs thread: distorted Voronoi as break
    -- points), whose planar faces move the wall in straight blocks with sharp breaks between them, plus
    -- a little ridged noise so the blocks are not flat-faced. A ridged pattern alone drew even columns.
    local position = c
    if stage >= 2 then
        local cracks = Noises.Cellular{
            order = 3, frequency = 1.0 / 260.0, octaves = 1, amplitude = columns, seed = seed + 3,
            position = Shapes.Warp{ amplitude = 40.0, frequency = 1.0 / 400.0, octaves = 1, seed = seed + 4 },
        }
        local ribs = Noises.Ridged{ frequency = 1.0 / 220.0, octaves = 1, amplitude = columns * 0.35, seed = seed + 5 }
        position = Vec.Combine{ c.x, c.z + cracks.value + ribs.value }
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
    -- Re-entrants: the valleys that reach the sea have cut back into the escarpment as amphitheatres
    -- (headward erosion: World Machine's Valley Cut, Gaea's Canyon walls). Each is a capsule open to the
    -- sea taken out of a tier's land in distance space and rounded into it, a U and not a keyhole: a
    -- circle left a narrow mouth. The main valleys are the bays'; minor ones lie between them and only
    -- reach the lower tiers. Most bite the lowest tier, half the second, a quarter the main wall.
    local reach = { -0.6, 0.0, 0.5 }
    -- The capsules are drawn through a broad warp (an outline breakup, as World Machine's Shapes have),
    -- so the heads are not perfect half circles with parallel sides.
    -- Small against the notch (60 m over 1.2 km): 150 m over 700 m turned them into blobs.
    local breakup = Shapes.Warp{ amplitude = 60.0, frequency = 1.0 / 1200.0, octaves = 1, seed = seed + 70 }
    local notchAt = stage >= 2 and Vec.Combine{ breakup.x, breakup.y + (position - c).y } or breakup
    local form = slope
    local lost = 0.0
    local firstEdge = nil
    local aprons = {}
    for k, tier in ipairs(tiers) do
        local points = curve(10 + k, function() return tier.z end, tier.spacing, tier.wander)
        -- The tier's land with its re-entrants, drawn at `at` for the edge and at `notchesAt` for them.
        local function notched(at, notchesAt)
            local e = fill(points, at)
            for _, v in ipairs(valleys) do
                if jitter(v.i, 30 + k) > reach[k] and (v.major or k < 3) then
                    local width = (v.major and 300 or 170) * (1.0 + 0.6 * math.abs(jitter(v.i, 40 + k))) * s
                    local depth = width * (1.2 + 0.8 * math.abs(jitter(v.i, 50 + k)))
                    local z     = zAt(points, v.x)
                    local trough = Shapes.Polyline{ points = { { v.x, z - depth + width }, { v.x + 150 * jitter(v.i, 60 + k), z + 3000 } }, position = notchesAt }
                    e = Combine.SmoothMax{ a = e, b = width - trough, k = 80.0 }
                end
            end
            return e
        end
        local edge = notched(position, notchAt)
        -- Chutes: narrow gullies down the walls along joints (not fluvial channels, which World Machine's
        -- users keep off cliff faces), each with a larger debris cone below it, through the apron.
        local chuteCones = 0.0
        for j = 0, math.floor(18000 / 700) do
            if jitter(j, 200 + k) > 0.55 then
                local xc    = -9000 + j * 700 + 250 * jitter(j, 210 + k)
                local z     = zAt(points, xc)
                local w     = 25.0 * s
                local deep  = (60.0 + 80.0 * math.abs(jitter(j, 220 + k))) * s
                local chute = Shapes.Polyline{ points = { { xc, z - deep + w }, { xc, z + 3000 } }, position = position }
                edge = Combine.SmoothMax{ a = edge, b = w - chute, k = 15.0 }
                chuteCones = chuteCones + Curves.Profile{
                    input = Shapes.Circle{ at = { xc, z }, radius = 1.0 }, kind = "gaussian", sigma = 90.0,
                }
            end
        end
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
        -- Differential erosion (World Machine's Strata, Gaea's Stratify): a hard cap rock stands nearly
        -- vertical on a softer base that weathers back to about 50 degrees. The cap takes 25 to 45 % of
        -- the tier, varying along the coast; the base runs out seaward of the cap's foot.
        local hard  = Noises.fBm{
            frequency = 1.0 / 2500.0, octaves = 1, amplitude = 0.5, seed = seed + 80 + k, position = alongCoast,
        }
        local share = 0.35 + 0.2 * hard.value
        local run   = tier.rise * s * 0.65 / math.tan(math.rad(50.0))
        local cap   = Curves.Profile{ input = edge, kind = "plateau", edge = 10, falloff = 60 * s }
        local base  = Curves.Profile{ input = edge - run, kind = "linear", width = run }
        local part  = (cap * share + base * (1.0 - share)) * rise
        form = form + part
        -- The talus apron (stage 3): a wedge at 35 degrees leaning on the base's foot, reaching 60 to
        -- 200 m out, over the ground without this tier. Taking the larger of it and the terrain buries
        -- the lower base in scree and never rises above the wall. Analytic, because the thermal pass
        -- relaxes a wall hundreds of metres high too slowly to build one.
        local spread = Noises.fBm{
            frequency = 1.0 / 1800.0, octaves = 2, amplitude = 0.5, seed = seed + 100 + k, position = alongCoast,
        }
        local reach_ = (130.0 + 140.0 * spread.value + 120.0 * chuteCones) * s
        local wedge  = Curves.Clamp{ input = (reach_ - (edge - run)) * math.tan(math.rad(35.0)), min = 0.0, max = 100000.0 }
        -- Capped at a third of the tier's rise: uncapped, the wedge kept rising inland and buried the
        -- plateau; capped by the tier's local height instead, it was nothing in front of the foot.
        aprons[#aprons + 1] = { part = part, wedge = Combine.Min{ a = wedge, b = rise * 0.35 } }
    end
    -- A regional slope with its divide on the main wall's edge (the same curve, without buttresses or
    -- re-entrants): in front of it every tread leans 1.2 % towards the sea, as benches do (flat ones held
    -- water); behind it the plateau is the back of a cuesta and dips 2 % inland. Both measured from one
    -- edge, so they meet in a ridge: measured from z, they met in a trough behind the wall that held a
    -- row of pits. Following the re-entrants changed no measurement; following the cracks too made the
    -- 2 % dip ripple into pits over a sixth of the plateau.
    local wallLine = land(13, function() return -600 end, 2200, 550, c)
    local front    = Curves.Clamp{ input = wallLine, min = 0.0, max = 100000.0 }
    local behind   = Curves.Clamp{ input = -wallLine, min = 0.0, max = 100000.0 }
    -- Not eased in: easing left a nearly flat band behind the edge where the plateau gullies, deeper than
    -- its slope, closed a row of pits. The crease it saves is a divide on top of a wall, out of sight.
    local back     = behind * (-0.02)
    local ground   = form + back - front * 0.012
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
    -- Beds of irregular thickness (Gaea's FractalTerraces against its uniform Terrace): the bed phase is
    -- taken on a monotone remap of the height, the height plus a noise sampled along the height itself,
    -- 15 m over 150 m, which keeps its slope under one so no bed folds back.
    local function remap(input)
        local wobble = Noises.fBm{
            frequency = 1.0 / 150.0, octaves = 2, amplitude = 15.0, seed = seed + 110,
            position = Vec.Combine{ input, Terrain.Const{ value = 0.0 } },
        }
        return input + wobble.value
    end
    local function beds(input, bed, depth)
        local u = remap(input) / bed
        return (u - Curves.Terrace{ input = u, step = 1.0, sharpness = 0.5 }) * steep * depth
    end
    -- The dipping beds tilt 15 to 45 degrees along the coast (World Machine's Strata takes its tilt as
    -- a spatial parameter), not one plane across the whole map.
    local tilt    = Noises.fBm{ frequency = 1.0 / 4000.0, octaves = 1, amplitude = 0.5, seed = seed + 120, position = alongCoast }
    local heading = math.rad(140.0)
    local dip     = (c.x * math.cos(heading) + c.z * math.sin(heading)) * math.tan(math.rad(30.0)) * (1.0 + tilt.value)
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
    -- Talus aprons: coarse debris lies at its angle of repose (about 35 degrees) and only at the foot of
    -- steep ground (Gaea's coarse sediment). The thermal pass is kept only where it deposits, so the
    -- walls lose nothing, but wherever it deposits, the foot of a wall included: kept on gentle ground
    -- only, the debris that settles against the lower wall was thrown away and the aprons never formed.
    -- Iterations are written for 2 m and scaled to the spacing; 100 reaches about 200 m at 8 m.
    local talus   = Erosion.Thermal{ input = kept, iterations = 100, talus = math.tan(math.rad(35.0)), strength = 0.4 }
    local settled = Combine.Max{ a = kept, b = talus }
    for _, apron in ipairs(aprons) do
        settled = Combine.Max{ a = settled, b = ground - apron.part + apron.wedge }
    end
    if stage == 3 then
        return { height = { value = settled, range = { -400, 2400 } },
                 normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } } }
    end

    -- 4. Detail, last and small: blocky rock on the walls by a morphological opening (shrink then expand,
    -- World Machine's Expander in Open mode), which squares off thin spikes into blocks, then 2 m of
    -- finer noise on the walls only, where no water stands.
    local opened = Filters.Expand{ input = Filters.Shrink{ input = settled, radius = 2 }, radius = 2 }
    local blocky = Combine.Blend{ a = settled, b = opened, mask = walls }
    local grain  = Noises.fBm{ frequency = 1.0 / 60.0, octaves = 3, amplitude = 1.0, seed = seed + 11 }
    local final  = blocky + grain.value * walls * 2.0
    return {
        height  = { value = final, range = { -400, 2400 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = final } } },
    }
end

return main
