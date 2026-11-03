-- Central lake: a valley map drawn from an illustrated reference (1254 px square, 24 x 24 grid).
--
-- A lake in the middle with a flat island (Fire Serpent Island) and islets, plains round it, an
-- ancient forest of rolling hills to the north-west, a hillier dark forest to the north-east, a swamp
-- between them where the lake reaches north, and a mountain range across the south with a mountain lake
-- (Mountain Haven) in a valley of its west end. The lake, its island and islets and the Haven are masks
-- segmented from the reference; the rivers are its channels, traced. Three rise at the bottom edge and
-- cross the range in valleys opened for them: two into the lake, one into the Haven, whose outflow takes
-- the forest river and leaves by the west edge.
--
-- Every point is in pixels of the reference: `P(px, py)` maps the image to the world at 10.1 m a pixel,
-- so the map is 12.3 km square, image top at z = 0. Lake at 100 m, plains 100-140 m, forests up to
-- ~350 m, the range up to about 2.3 km, Mountain Haven at 400 m in a basin of the range, the rivers in the range
-- falling from 1.1 km at the bottom edge in U and V valleys.
--
--   terrain_viewer --script assets/scripts/terrains/central_lake.lua --bounds -768,-768,13056,13056 --resolution 6.0472441 --water --target 6144,0,6144 --view 30,-35,19000 --max-level 2 --ring 6
--   terrain_export --script assets/scripts/terrains/central_lake.lua --min -768,-768 --max 13056,13056 --resolution 6 --section 512 --out out/central_lake/
--
-- Outputs: height, the water (lakes, rivers) and a biome id (0 plains, 1 ancient
-- forest, 2 dark forest, 3 swamp, 4 mountain, 5 water).

local S, X0, Y0 = 10.1, 38.0, 32.0
local LAKE, HAVEN = 100.0, 400.0
-- The map: the reference's 12.3 km and a ring of mountains 768 m wide (one viewer tile) round it.
local MAP0, MAP1 = -768.0, 13056.0
local SPAN = MAP1 - MAP0

local function P(px, py)
    return { (px - X0) * S, (py - Y0) * S }
end

local function pts(list)
    local out = {}
    for i, p in ipairs(list) do
        out[i] = P(p[1], p[2])
    end
    return out
end

local function region(list, warp)
    return Shapes.Spline{ points = pts(list), closed = true, fill = true, subdivisions = 3, position = warp }
end

-- Masks measured on the reference, filled in at the end of the file.
local DATA = {}
local CELLS, STEP = 627, 2

-- Squared distance along one line of cells to the nearest cell where `f` is 0 (Felzenszwalb and
-- Huttenlocher's lower envelope of parabolas), from `f` at `first`, `first + stride`, ... into `out`.
local function edt_line(f, out, first, stride, n, v, z)
    local k = 1
    v[1], z[1], z[2] = 0, -math.huge, math.huge
    for q = 1, n - 1 do
        local fq = f[first + q * stride]
        local s
        while true do
            local p = v[k]
            s = ((fq + q * q) - (f[first + p * stride] + p * p)) / (2 * (q - p))
            if s > z[k] or k == 1 then break end
            k = k - 1
        end
        if s <= z[k] then
            v[k], z[k], z[k + 1] = q, -math.huge, math.huge
        else
            k = k + 1
            v[k], z[k], z[k + 1] = q, s, math.huge
        end
    end
    k = 1
    for q = 0, n - 1 do
        while z[k + 1] < q do k = k + 1 end
        local p = v[k]
        out[first + q * stride] = (q - p) * (q - p) + f[first + p * stride]
    end
end

-- The signed distance in metres to the edge of a mask from `DATA` (negative inside it), as a grid over
-- the image: the exact Euclidean distance on the cells, inside and outside.
local function mask_distance(rle)
    local n, inside = CELLS * CELLS, {}
    local i = 0
    for row in rle:gmatch("([^;]*);") do
        local on = false
        for run in row:gmatch("%d+") do
            for _ = 1, tonumber(run) do
                i = i + 1
                inside[i] = on
            end
            on = not on
        end
    end
    -- Distance in cells to the nearest cell whose `inside` is `target`: columns, then rows.
    local function edt(target)
        local f, g, v, z = {}, {}, {}, {}
        for k = 1, n do
            f[k] = inside[k] == target and 0.0 or 1e12
        end
        for x = 1, CELLS do
            edt_line(f, g, x, CELLS, CELLS, v, z)
        end
        for y = 0, CELLS - 1 do
            edt_line(g, f, y * CELLS + 1, 1, CELLS, v, z)
        end
        for k = 1, n do
            f[k] = math.sqrt(f[k])
        end
        return f
    end
    local to_in, to_out, cell = edt(true), edt(false), STEP * S
    local values = {}
    for k = 1, n do
        values[k] = (inside[k] and 0.5 - to_out[k] or to_in[k] - 0.5) * cell
    end
    local origin = P(STEP / 2, STEP / 2)
    return Terrain.Grid{ values = values, size = { CELLS, CELLS }, origin = origin, cell = cell }
end

-- The ground a valley allows at a point: the lowest, over the samples { x, z, floor } of the courses, of
-- the floor plus a U (a parabola for 350 m) running into a V at 33 degrees (0.65 m a metre). A minimum of
-- continuous profiles, so where the valleys of two rivers meet there is a ridge and never a step.
local function valley_grid(samples, x0, z0, cell, nx, nz)
    local values = {}
    for jz = 0, nz - 1 do
        local z = z0 + jz * cell
        for ix = 0, nx - 1 do
            local x, best = x0 + ix * cell, 1e9
            for _, s in ipairs(samples) do
                local dx, dz = s[1] - x, s[2] - z
                local d2 = dx * dx + dz * dz
                local u = d2 < 122500.0 and d2 / 700.0 or math.sqrt(d2) - 175.0
                local v = s[3] + u * 0.65
                if v < best then best = v end
            end
            values[jz * nx + ix + 1] = best
        end
    end
    return Terrain.Grid{ values = values, size = { nx, nz }, origin = { x0, z0 }, cell = cell }
end

local function main(params)
    local seed  = params.seed or 0
    local stage = params.stage or 2
    local warp  = Shapes.Warp{ amplitude = 140.0, frequency = 1.0 / 900.0, octaves = 4, seed = seed + 1 }
    local soft  = Shapes.Warp{ amplitude = 250.0, frequency = 1.0 / 3000.0, octaves = 2, seed = seed + 2 }

    -- The central lake and the pond west of it, as the reference draws them. Fire Serpent Island and the
    -- islets are holes in it: flat ground a few metres over the water.
    local lake = mask_distance(DATA.lake)

    -- The ground the lake sits in: 10 % under water down to 30 m; on land 2 m over the first 40 m, so
    -- the islets stand clear of the water, then 0.8 %.
    local ground = LAKE + Curves.Clamp{ input = lake, min = -300.0, max = 0.0 } * 0.1
                        + Curves.Clamp{ input = lake, min = 0.0, max = 40.0 } * 0.05
                        + Curves.Clamp{ input = lake, min = 0.0, max = 20000.0 } * 0.008

    -- The relief runs down to the water as a ramp: faded out from 900 m off the lake to 40 m off it, so
    -- the land leans towards the shore instead of ending over it.
    local shore = Curves.Smoothstep{ input = lake, min = 40.0, max = 900.0 }
    -- Forests: rolling hills, broad in the ancient forest, steeper and taller in the dark one.
    local ancient = Curves.Profile{ input = region({
        { 0, 0 }, { 575, 0 }, { 575, 240 }, { 545, 370 }, { 385, 440 }, { 300, 545 }, { 200, 575 }, { 0, 590 },
    }, soft), kind = "smooth", width = 2200.0 }
    local dark = Curves.Profile{ input = region({
        { 790, 0 }, { 1254, 0 }, { 1254, 640 }, { 1080, 600 }, { 980, 480 }, { 860, 420 }, { 820, 260 },
    }, soft), kind = "smooth", width = 2200.0 }
    -- The swamp's outline (its distance, positive outside) and the swamp itself, full 500 m in.
    local swamp_d = region({
        { 575, 150 }, { 800, 150 }, { 830, 330 }, { 815, 420 }, { 780, 470 }, { 700, 492 }, { 620, 472 },
        { 568, 420 }, { 555, 300 },
    }, soft)
    local swamp = Curves.Profile{ input = swamp_d, kind = "smooth", width = 500.0 }
    local hills = Noises.fBm{ frequency = 1.0 / 1800.0, octaves = 4, persistence = 0.45, amplitude = 1.0, seed = seed + 3, position = soft }.value
    local knolls = Noises.Ridged{ frequency = 1.0 / 2400.0, octaves = 3, amplitude = 1.0, seed = seed + 4, position = soft }.value
    ground = ground + ancient * (45.0 + 55.0 * hills) + dark * (90.0 + 70.0 * hills + 60.0 * knolls)
    -- The plains roll: swells up to 7 m about 1.6 km across and knolls up to 2 m about 450 m across. The
    -- forests: knolls up to 14 m about 700 m apart in the ancient one, rocky crests up to 30 m about 900 m
    -- apart in the dark one. All only raise the ground, so no hollow holds new water, and none within
    -- 30 m of the lake, growing to full by 200 m: the land runs down to the water, ending in places in the
    -- cliffs made below.
    local open = Curves.Clamp{ input = 1.0 - ancient - dark, min = 0.0, max = 1.0 } * (1.0 - swamp)
    local unit = function(s, period) return Curves.Clamp{ input = Noises.fBm{ frequency = 1.0 / period, octaves = 3, seed = seed + s, position = soft }.value * 0.5 + 0.5, min = 0.0, max = 1.0 } end
    local swells = unit(20, 1600.0) * 7.0 + unit(21, 450.0) * 2.0
    local mounds = Noises.Ridged{ frequency = 1.0 / 700.0, octaves = 3, seed = seed + 22, position = soft }.value
    local crests = Noises.Ridged{ frequency = 1.0 / 900.0, octaves = 4, seed = seed + 23, position = warp }.value
    local woods = ancient * mounds * 14.0 + dark * Curves.Power{ input = crests, exponent = 1.5 } * 30.0
    ground = ground + (open * swells + woods) * shore * Curves.Smoothstep{ input = swamp_d, min = 0.0, max = 900.0 }
    -- The swamp: islets on the cell borders, 2.5 m over the lake, between pools 1.5 m under
    -- it in the cells (about half the ground is water).
    local cells = Noises.Cellular{ frequency = 1.0 / 220.0, octaves = 1, seed = seed + 5, position = warp }.value
    local marsh = LAKE + 2.5 - Curves.Smoothstep{ input = cells, min = 0.3, max = 0.6 } * 4.0 + hills * 0.5
    -- Channels 1.2 m deep that wind across the islets and join the pools to each other and to the lake: the
    -- crests of a ridged noise (lines about 350 m apart), so a boat can thread the whole swamp.
    local lines = Noises.Ridged{ frequency = 1.0 / 350.0, octaves = 1, seed = seed + 6, position = warp }.value
    local channels = Curves.Smoothstep{ input = lines, min = 0.86, max = 0.93 }
    marsh = marsh + (LAKE - 1.2 - marsh) * channels
    ground = Combine.Blend{ a = ground, b = Combine.Min{ a = ground, b = marsh }, mask = swamp }

    -- The rivers as the reference draws them. The three in the south rise at the map's bottom edge: the
    -- east one runs up the dwarven valley and over the falls into the lake's east end (and sends a branch
    -- west past the east halfling village to just short of the lake), the centre one crosses the range
    -- into the lake beside the fortress (with the stream from the west village's fields), and the
    -- south-west one fills Mountain Haven, whose outflow runs north, takes the forest river and leaves by
    -- the west edge. `Water.Network` cuts only their channels (its own valleys, scaled by a spur noise,
    -- reach kilometres out and cut the summits into spikes); the valleys are shaped below as U and V.
    local RIVERS = {
        { name = "east", width = { 45, 100 } },
        { name = "centre", width = { 45, 100 } },
        { name = "west", width = { 50, 70 } },
        { name = "haven_in", width = { 30, 70 } },
        { name = "forest", width = { 25, 60 } },
        { name = "brook", width = { 20, 50 } },
        { name = "canal", width = { 25, 60 } },
    }
    local courses, bands = {}, {}
    for i, r in ipairs(RIVERS) do
        courses[i] = pts(DATA.rivers[r.name])
        local band = {}
        for j, p in ipairs(courses[i]) do
            local t = (j - 1) / math.max(1, #courses[i] - 1)
            band[j] = { p[1], p[2], r.width[1] + (r.width[2] - r.width[1]) * t }
        end
        bands[i] = { shape = "band", points = band }
    end
    -- Distance to the nearest river's bank (each band as wide as its river, widening to the mouth).
    local to_river = Shapes.Set{ elements = bands, reach = 3000 }.value

    -- Mountain Haven, its outline from the reference, at 400 m in the range's west end.
    local haven = mask_distance(DATA.haven)

    -- The southern range and its spurs, carved on the CPU: a main crest across the south, a lower front
    -- south of the centre river's bend, a spur either side of the Haven and the eastern ridge. Summits
    -- spread evenly along all of it, so every part rises over the snow line, not only the middle.
    local h = params.height or 2400.0
    -- The references' planar faces and branching ridges are what drainage leaves when hillslopes reach
    -- their critical angle (stream power with a threshold slope: Cordonnier 2016, Tzathas 2024): every
    -- valley cuts a plane each side of it, and the ridges between valleys branch as the rivers do. So
    -- the simulation does the work, on a finer grid (440 cells, about 45 m) with the threshold at 42
    -- degrees, no shoulders, and twice the erodibility and half the diffusion of the alpine default, so
    -- valleys open every few hundred metres and the ridges branch as in the Sierra Nevada reference.
    -- Nothing is added over it but fine gullies.
    local carved, tallest = Ranges.Carve{
        seed = seed, cirques = false, shoulders = false, cells = 440, steps = 600, talus = 0.9,
        k = params.k or 0.03, diffusion = params.diffusion or 0.006, lift = params.lift or 5.0,
        ridges = {
            { points = pts{ { 20, 1160 }, { 250, 1110 }, { 520, 1150 }, { 780, 1130 }, { 960, 1170 } }, height = h, width = 3400 },
            { points = pts{ { 330, 1070 }, { 560, 1060 }, { 760, 1075 }, { 940, 1060 } }, height = h * 0.75, width = 2000 },
            { points = pts{ { 60, 1150 }, { 80, 1000 }, { 95, 880 } }, height = h * 0.85, width = 1600 },
            { points = pts{ { 330, 1150 }, { 360, 1000 }, { 330, 870 } }, height = h * 0.9, width = 1800 },
            { points = pts{ { 1150, 1270 }, { 1170, 1060 }, { 1205, 860 }, { 1270, 720 } }, height = h * 0.95, width = 2400 },
        },
        peaks = {
            { at = P(180, 1150), height = h * 1.3, radius = 2200 },
            { at = P(420, 1185), height = h * 1.25, radius = 2200 },
            { at = P(700, 1185), height = h * 1.35, radius = 2600 },
            { at = P(900, 1150), height = h * 1.25, radius = 2200 },
            { at = P(1210, 1110), height = h * 1.3, radius = 2400 },
            { at = P(70, 960), height = h * 1.0, radius = 1600 },
            { at = P(360, 1010), height = h * 1.05, radius = 1800 },
            { at = P(1200, 880), height = h * 1.0, radius = 1800 },
        },
    }
    -- The simulation settles a range this narrow (3.4 km a side of the crest) below the crest heights
    -- asked for, 1.7 km at most (with twice the uplift): scaled up, the summits reach about 2.3 km and the
    -- planar faces (scaling keeps a plane a plane) stand at 55 to 60 degrees, as the references' walls.
    local structure = carved * (params.relief or 1.35)
    -- The ring: a range all round the reference's square, its crest 1.5 km outside the reference, past the
    -- map's edge (its far side is not on the map), so the forests keep their room; carved like the southern one (same erosion, planar faces,
    -- branching valleys) on a grid as fine (about 60 m), so the two read as one range. Its slopes reach a
    -- little into the reference: 600 m, rising gently over 1.4 km along a wavering line (a straight edge
    -- read as a wall), but not within 250 m of a river, 200 m of the lake or 300 m of the Haven, where the
    -- reference stays as drawn. Where it meets the southern range the higher of the two stands. The west
    -- river ends where the reference ends it, at the foot of these mountains.
    local o0, o1 = -1500.0, 13788.0
    local ring = Ranges.Carve{
        seed = seed + 31, cirques = false, shoulders = false, cells = 400, steps = 600, talus = 0.9,
        k = 0.03, diffusion = 0.006, lift = 5.0,
        ridges = {
            { points = { { o0, o0 }, { 3000, o0 - 300 }, { 9000, o0 + 200 }, { o1, o0 } }, height = h * 0.9, width = 3400 },
            { points = { { o1, o0 }, { o1 + 250, 3000 }, { o1 - 200, 9000 }, { o1, o1 } }, height = h * 0.85, width = 3400 },
            { points = { { o1, o1 }, { 9000, o1 + 300 }, { 3000, o1 - 150 }, { o0, o1 } }, height = h * 0.95, width = 3400 },
            { points = { { o0, o1 }, { o0 - 250, 9000 }, { o0 + 200, 3000 }, { o0, o0 } }, height = h * 0.85, width = 3400 },
        },
    }
    local edge = Shapes.Rect{ at = { 6144, 6144 }, size = { 12288, 12288 },
                              position = Shapes.Warp{ amplitude = 350.0, frequency = 1.0 / 2500.0, octaves = 3, seed = seed + 32 } }
    local clear = Curves.Smoothstep{ input = to_river, min = 250.0, max = 600.0 } * Curves.Smoothstep{ input = lake, min = 100.0, max = 1200.0 }
                  * Curves.Smoothstep{ input = haven, min = 300.0, max = 700.0 }
    local inside = Curves.Smoothstep{ input = edge, min = 0.0, max = 1.0 }
    -- Inside the reference the ring keeps its own inner side, the long spurs and foothills its outer
    -- side had (they were faded out before, which left a steep wall facing the plain), cleared only round
    -- the water, and faded out over 1.1 km round the swamp, a ramp down to it as round the lake.
    local outside = inside + (1.0 - inside) * clear * Curves.Smoothstep{ input = swamp_d, min = 0.0, max = 1100.0 }
    -- The ring is kept apart from the valleys the rivers open: their U and V reach kilometres out and
    -- cut saddles into it. Only within 300 to 900 m of a river does it give way to them.
    local ringed = ring * (params.relief or 1.35) * outside
    local ring_kept = ringed * Curves.Smoothstep{ input = to_river, min = 300.0, max = 900.0 }
    structure = Combine.Max{ a = structure, b = ringed }
    -- The valley floor of every river in the range, as the references draw them: high at the source and
    -- falling to its mouth, steeply near the source (`(1 - u)^p` along the course). The two that rise at
    -- the bottom edge start at 1.1 km, the south-west one ends in Mountain Haven, and the Haven's outflow
    -- drops from the Haven to the plain. Each 60 m cell holds the lowest of the floors of the points of
    -- these courses, sampled every 50 m, plus the U and V of `valley_grid`.
    local FZ0 = MAP0
    local FLOORS = { east = { 1100, LAKE, 2.0 }, centre = { 1100, LAKE, 2.0 },
                     haven_in = { 1100, HAVEN, 1.4 }, west = { HAVEN, LAKE + 10.0, 3.0 } }
    -- Where a drawn course runs along a spur (lower ground either side), a floor designed from the source
    -- would hold the river on a ridge. So each floor is no higher than the lowest unopened ground within
    -- 150 m (sampled on the CPU every 30 m over the range, so every resolution agrees) plus 1.5 m, and
    -- never rises downstream, ending at its lake.
    local NX, NZ, NC, NZ0 = math.floor(SPAN / 30.0) + 1, math.floor((MAP1 - FZ0) / 30.0) + 1, 30.0, FZ0
    local natural = Terrain.Sample{ value = ground + structure, origin = { MAP0, NZ0 }, size = { NX, NZ }, cell = NC, seed = seed }
    local function lowest(x, z)
        local ci, cj, low = math.floor((x - MAP0) / NC + 0.5), math.floor((z - NZ0) / NC + 0.5), math.huge
        for dj = -5, 5 do
            for di = -5, 5 do
                local i, j = ci + di, cj + dj
                if i >= 0 and j >= 0 and i < NX and j < NZ and di * di + dj * dj <= 25 then
                    low = math.min(low, natural[j * NX + i + 1])
                end
            end
        end
        return low
    end
    local samples = {}
    for i, r in ipairs(RIVERS) do
        local f = FLOORS[r.name]
        if f then
            local c, total = courses[i], 0.0
            for j = 2, #c do
                total = total + math.sqrt((c[j][1] - c[j - 1][1]) ^ 2 + (c[j][2] - c[j - 1][2]) ^ 2)
            end
            local run = 0.0
            local last = math.huge
            for j = 2, #c do
                local len = math.sqrt((c[j][1] - c[j - 1][1]) ^ 2 + (c[j][2] - c[j - 1][2]) ^ 2)
                for k = 0, math.max(1, math.floor(len / 50.0)) - 1 do
                    local t = k / math.max(1, math.floor(len / 50.0))
                    local u = (run + len * t) / total
                    local x, z = c[j - 1][1] + (c[j][1] - c[j - 1][1]) * t, c[j - 1][2] + (c[j][2] - c[j - 1][2]) * t
                    local v = math.min(f[2] + (f[1] - f[2]) * (1.0 - u) ^ f[3], lowest(x, z) + 1.5, last)
                    last = math.max(v, f[2])
                    samples[#samples + 1] = { x, z, last}
                end
                run = run + len
            end
        end
    end
    local FX, FZ, FC = MAP0, FZ0, 60.0
    local cap = valley_grid(samples, FX, FZ, FC, math.floor(SPAN / FC) + 1, math.floor((MAP1 - FZ) / FC) + 1)

    -- The range opened round the rivers as a valley: from the river's floor the ground rises as a U (a
    -- parabola) for 350 m and then as a V at 33 degrees (0.65 m a metre), joined to the mountain sides
    -- over 150 m of height, so the slope never breaks into a cliff. It never reaches the lake (faded out
    -- between 300 m and 1.5 km from the shore).
    local walls  = Curves.Clamp{ input = cap - ground, min = 0.0, max = 5000.0 }
    local raised = Combine.Max{ a = Combine.SmoothMin{ a = structure, b = walls, k = 150.0 }, b = ring_kept }
                   * Curves.Smoothstep{ input = lake, min = 300.0, max = 1500.0 }
    local forms  = ground + raised
    -- Mountain Haven lies where the range already holds a basin between its spurs (the ground round it
    -- stands at 380 to 750 m, open to the north): only its bowl is cut, 12 % under the water to 30 m and
    -- the shore raised 2 m over the first 40 m, with the ground round it no steeper than 31 degrees up
    -- from the water within 300 m.
    local bowl = HAVEN + Curves.Clamp{ input = haven, min = -250.0, max = 0.0 } * 0.12
                       + Curves.Clamp{ input = haven, min = 0.0, max = 40.0 } * 0.05
                       + Curves.Clamp{ input = haven, min = 0.0, max = 1500.0 } * 0.6
    forms = Combine.Blend{ a = forms, b = Combine.Min{ a = forms, b = bowl }, mask = 1.0 - Curves.Smoothstep{ input = haven, min = 300.0, max = 700.0 } }
    -- The Haven held in: wherever the basin round it is lower, a ring of ground 3 m over the water, 150 m
    -- wide, then falling at 11 degrees outwards. The river that leaves it to the north follows the rim
    -- down, so its outlet is a notch it cuts in the rim and its course falls at 20 % from there, not a
    -- gap that drops it at once to the valley floor below.
    local keep_rim = Curves.Smoothstep{ input = haven, min = -12.0, max = 0.0 }
    local rim = HAVEN + 3.0 - Curves.Clamp{ input = haven - 150.0, min = 0.0, max = 100000.0 } * 0.2
    forms = Combine.Max{ a = forms, b = rim - (1.0 - keep_rim) * 5000.0 }
    -- Its bed is the bowl wherever the basin under it is deeper (its long north arm lies over the valley
    -- the outlet falls into): a lake 30 m deep at most, not a trench of water.
    forms = Combine.Max{ a = forms, b = bowl - (1.0 - Shapes.Mask{ shape = haven, feather = 0.0 }) * 5000.0 }
    local hollow = 1.0 - Curves.Smoothstep{ input = haven, min = 100.0, max = 400.0 }

    -- Foothills: the plain rises towards the mountains instead of meeting them at a crease. On a 120 m
    -- grid, each cell takes the highest of the mountains near it (their height, up to 400 m) faded with
    -- the distance over 1.8 km, and half of that is added to the ground: a slope up to the mountain
    -- foot, rolling with a broad noise. It stays 80 to 300 m off the rivers, 100 to 400 m off the lake and
    -- clear of the Haven and the swamp, so no water moves.
    local AC = 120.0
    local an = math.floor(SPAN / AC) + 1
    local peaks_at = Terrain.Sample{ value = structure, origin = { MAP0, MAP0 }, size = { an, an }, cell = AC, seed = seed }
    local reach = math.floor(1800.0 / AC)
    local apron_values = {}
    for j = 0, an - 1 do
        for i = 0, an - 1 do
            local best = 0.0
            for dj = -reach, reach do
                local jj = j + dj
                if jj >= 0 and jj < an then
                    for di = -reach, reach do
                        local ii = i + di
                        if ii >= 0 and ii < an then
                            local d = math.sqrt(di * di + dj * dj) / reach
                            if d < 1.0 then
                                local v = math.min(peaks_at[jj * an + ii + 1], 400.0) * (1.0 - d) ^ 1.5
                                if v > best then best = v end
                            end
                        end
                    end
                end
            end
            apron_values[j * an + i + 1] = best
        end
    end
    local apron = Terrain.Grid{ values = apron_values, size = { an, an }, origin = { MAP0, MAP0 }, cell = AC }
    local roll = Noises.fBm{ frequency = 1.0 / 1500.0, octaves = 3, seed = seed + 41, position = soft }.value * 0.3 + 0.7
    local away = Curves.Smoothstep{ input = to_river, min = 80.0, max = 300.0 } * shore
                 * Curves.Smoothstep{ input = haven, min = 300.0, max = 700.0 }
    forms = forms + apron * 0.5 * roll * away * Curves.Smoothstep{ input = swamp_d, min = 0.0, max = 900.0 }
    local mountain = Curves.Smoothstep{ input = raised, min = 60.0, max = 400.0 }

    local rivers = {}
    for i, r in ipairs(RIVERS) do
        rivers[i] = { points = courses[i], kind = "gorge", roughness = 0.0, width = r.width,
                      mouth = "none", name = r.name }
    end
    local net = Water.Network{
        -- The network sees the Haven full to its level, so the rivers through it run at its surface and
        -- the outlet leaves at the lake's level, not from its floor.
        terrain = Combine.Max{ a = forms, b = Shapes.Mask{ shape = haven, feather = 0.0 } * (HAVEN + 5000.0) - 5000.0 },
        region = { MAP0, MAP0, MAP1, MAP1 }, cell = 60.0, seed = seed, rivers = rivers,
        generate = { rivers = { count = 0 }, lakes = { count = 0 } },
    }
    print(net:summary())

    if stage == 1 then
        return { height = { value = forms, range = { 0, 3600 } } }
    end
    -- The faces are left as the simulation makes them, planes (and the gullies kept 250 to 600 m off the
    -- rivers, or the channel is cut through them as a slot): the alpine finish (strata, thermal scree,
    -- hydraulic passes) broke them into spikes. Below the simulation's 45 m cells, the next order of
    -- branching comes from gullies down the slope (World Machine's Flow Restructure): spurs and couloirs
    -- 450 m apart and 35 m deep, then flutes 160 m apart and 12 m deep on their walls, coarse to fine.
    -- With 1 m of grain on the low ground, under the plains' 1 % slope at its 400 m wavelength.
    local spurs = Erosion.Gullies{ input = raised, cell = 450.0, octaves = 3, strength = 35.0, full = 0.7, seed = seed + 53 }
    local finished = Erosion.Gullies{ input = spurs, cell = 160.0, octaves = 3, strength = 12.0, full = 0.6, seed = seed + 57 }
    local grain = Noises.fBm{ frequency = 1.0 / 400.0, octaves = 3, amplitude = 1.0, seed = seed + 9 }.value
    local detail = (finished - raised) * mountain * (1.0 - hollow) * Curves.Smoothstep{ input = to_river, min = 250.0, max = 600.0 } + grain * (1.0 - mountain) * (1.0 - swamp)
    -- The valley floors follow the water: where the ground the network measured has a bump on a river's
    -- way (a rise of the plain, the foot of a spur), its level stays low and a gorge would cut a slot
    -- with sheer walls. So the ground near every river is brought down to its level, 1.5 m over the
    -- water, as the same U and V (`valley_grid` on 80 m cells over the courses sampled every 80 m),
    -- before the channel is cut.
    local levels_at = {}
    for _, r in ipairs(net.rivers) do
        for j = 1, #r.points, math.max(1, math.floor(80.0 / math.max(1.0, (r.points[2] and math.sqrt((r.points[2][1] - r.points[1][1]) ^ 2 + (r.points[2][2] - r.points[1][2]) ^ 2) or 80.0)))) do
            levels_at[#levels_at + 1] = { r.points[j][1], r.points[j][2], r.levels[j] }
        end
    end
    for _, s in ipairs(levels_at) do
        s[3] = s[3] + 1.5
    end
    local valley = valley_grid(levels_at, MAP0, MAP0, 80.0, math.floor(SPAN / 80.0) + 1, math.floor(SPAN / 80.0) + 1)
    -- Near the Haven the valleys never cut under 1.5 m over its water: the rim keeps holding the lake,
    -- and its outlet leaves through a V cut to that height instead of a slot through a 3 m wall.
    valley = Combine.Max{ a = valley, b = HAVEN + 1.5 - Curves.Smoothstep{ input = haven, min = 150.0, max = 250.0 } * 5000.0 }
    forms = Combine.SmoothMin{ a = forms, b = valley, k = 4.0 }
    forms = Combine.Max{ a = forms, b = ground + ring_kept }
    -- Natural channels: the network holds a river's water where the ground it measured lets it, which on
    -- a plain or along a slope can stand a couple of metres over the land beside it. So the ground along
    -- every river is raised to 2 m over its water out to 12 m from the channel, then falls at 40 % to meet
    -- the land, and the network sinks the channel into that: the river runs below its banks and never
    -- over them. Distance and level are those of the courses the network drew (its meanders and widths),
    -- the level of the nearest point on 20 m cells. None in the lakes, where the rivers end.
    local drawn, nearest = {}, {}
    for _, r in ipairs(net.rivers) do
        local band = {}
        for j = 1, #r.points, 2 do
            band[#band + 1] = { r.points[j][1], r.points[j][2], r.widths[j] }
            local key = math.floor(r.points[j][1] / 200.0) .. ":" .. math.floor(r.points[j][2] / 200.0)
            nearest[key] = nearest[key] or {}
            table.insert(nearest[key], { r.points[j][1], r.points[j][2], r.levels[j] })
        end
        if #band >= 2 then drawn[#drawn + 1] = { shape = "band", points = band } end
    end
    local off_channel = Shapes.Set{ elements = drawn, reach = 3000 }.value
    local LN = math.floor(SPAN / 20.0) + 1
    local strip_levels = {}
    for jz = 0, LN - 1 do
        local z = MAP0 + jz * 20.0
        local bz = math.floor(z / 200.0)
        for ix = 0, LN - 1 do
            local x = MAP0 + ix * 20.0
            local bx, best, value = math.floor(x / 200.0), math.huge, -1000.0
            for dz = -1, 1 do
                for dx = -1, 1 do
                    for _, s in ipairs(nearest[(bx + dx) .. ":" .. (bz + dz)] or {}) do
                        local d = (s[1] - x) ^ 2 + (s[2] - z) ^ 2
                        if d < best then best, value = d, s[3] end
                    end
                end
            end
            strip_levels[jz * LN + ix + 1] = value
        end
    end
    -- Every cell no river point was found near takes the value of the nearest cell that has one (a
    -- breadth-first fill), so the grid has no jump between found and empty cells: its cubic
    -- interpolation overshot such a jump by hundreds of metres, digging holes and raising slabs.
    local queue, head = {}, 1
    for k = 1, LN * LN do
        if strip_levels[k] > -500.0 then queue[#queue + 1] = k end
    end
    while head <= #queue do
        local k = queue[head]
        head = head + 1
        local i, j = (k - 1) % LN, (k - 1) // LN
        for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
            local ii, jj = i + d[1], j + d[2]
            if ii >= 0 and jj >= 0 and ii < LN and jj < LN then
                local q = jj * LN + ii + 1
                if strip_levels[q] <= -500.0 then
                    strip_levels[q] = strip_levels[k]
                    queue[#queue + 1] = q
                end
            end
        end
    end
    local strip_level = Terrain.Grid{ values = strip_levels, size = { LN, LN }, origin = { MAP0, MAP0 }, cell = 20.0 }
    local strip = strip_level + 2.0 - Curves.Clamp{ input = off_channel - 12.0, min = 0.0, max = 100000.0 } * 0.4
    local on_land = Curves.Smoothstep{ input = lake, min = 0.0, max = 60.0 } * Curves.Smoothstep{ input = haven, min = 0.0, max = 60.0 }
    -- Within 400 m of a channel: wherever a river runs along a slope its lower side is built up as an
    -- embankment falling at 22 degrees, so the river sits in the slope and not on a ledge over a cliff.
    local near_channel = (1.0 - Curves.Smoothstep{ input = off_channel, min = 300.0, max = 400.0 })
    local at_channel = (1.0 - Curves.Smoothstep{ input = off_channel, min = 150.0, max = 200.0 }) * Curves.Smoothstep{ input = lake, min = 300.0, max = 400.0 }
    -- And no higher than a V at 31 degrees (0.6 m a metre) from 2 m over the water: on a steep reach the
    -- water falls faster than the ground beside it did, and the channel was a slot between walls. Not
    -- within 250 m of the Haven, whose rim it would cut, nor within 400 m of the lake, whose shore ramps
    -- it would sink under the water; out to 200 m from the channel, so a gorge's walls lie back.
    forms = Combine.Blend{ a = forms, b = Combine.Min{ a = forms, b = strip_level + 2.0 + Curves.Clamp{ input = off_channel, min = 0.0, max = 100000.0 } * 0.6 },
                           mask = on_land * at_channel * Curves.Smoothstep{ input = haven, min = 150.0, max = 250.0 } }
    -- Not within 400 m of the lake: its shore keeps the ramps made above.
    forms = Combine.Max{ a = forms, b = strip - (1.0 - on_land * near_channel * Curves.Smoothstep{ input = lake, min = 300.0, max = 400.0 }) * 5000.0 }
    -- Mini cliffs in places along the lake and the rivers on the plain (water under 125-150 m): the land steps up 8 to 20 m about
    -- 20 m from the lake's shore and 12 m from a river's bank, in patches (a broad noise, about half
    -- tenths of the shore), the step fading out over the next few hundred metres. Only land is raised,
    -- the water and its edge stay where they are; none on the mountains.
    local patchy = Curves.Smoothstep{ input = Noises.fBm{ frequency = 1.0 / 900.0, octaves = 2, seed = seed + 43 }.value, min = 0.05, max = 0.2 }
    local rise = 8.0 + 12.0 * Curves.Smoothstep{ input = Noises.fBm{ frequency = 1.0 / 500.0, octaves = 2, seed = seed + 44 }.value, min = -0.3, max = 0.3 }
    local lake_step = Curves.Smoothstep{ input = lake, min = 16.0, max = 24.0 } * (1.0 - Curves.Smoothstep{ input = lake, min = 150.0, max = 450.0 })
    local river_step = Curves.Smoothstep{ input = to_river, min = 10.0, max = 15.0 } * (1.0 - Curves.Smoothstep{ input = to_river, min = 100.0, max = 300.0 })
                       * Curves.Smoothstep{ input = lake, min = 60.0, max = 120.0 }
                       * (1.0 - Curves.Smoothstep{ input = strip_level, min = LAKE + 25.0, max = LAKE + 50.0 })
    forms = forms + Combine.Max{ a = lake_step, b = river_step } * patchy * rise * (1.0 - mountain) * Curves.Smoothstep{ input = swamp_d, min = 0.0, max = 300.0 } * Curves.Smoothstep{ input = haven, min = 200.0, max = 400.0 }
    local wet = net:apply(forms + detail, { detail = detail })

    -- The central lake's water is a plane at its level over the basin; the island stands out of it.
    local basin = Combine.Max{ a = Shapes.Mask{ shape = lake - 300.0, feather = 0.0 },
                               b = Curves.Smoothstep{ input = swamp, min = 0.3, max = 0.31 } }
    local lakes = Combine.Max{ a = wet.lakes, b = basin * (LAKE + 25000.0) - 25000.0 }
    lakes = Combine.Max{ a = lakes, b = Shapes.Mask{ shape = haven, feather = 0.0 } * (HAVEN + 25000.0) - 25000.0 }
    -- A safety net only: no dry sample beside water is left lower than it (where the water would spill
    -- out sideways); each is held 0.3 m over the highest water of its neighbours. The water is never
    -- raised, so a lake still drains by its river alone.
    -- The channel's walls laid back: within 40 m of a river the dry ground is brought down towards its
    -- average over 30 m (only lowered, never raised), so where the network cut a slot into a slope the
    -- walls lean back into banks; not near the Haven, whose rim holds it. The safety below then keeps
    -- every bank over the water.
    local res = params.resolution or 6.0
    local laid = Filters.Blur{ input = wet.height, radius = math.max(1, math.min(32, math.floor(30.0 / res + 0.5))) }
    local near_bank = (1.0 - Curves.Smoothstep{ input = to_river, min = 30.0, max = 40.0 }) * Curves.Smoothstep{ input = haven, min = 200.0, max = 300.0 }
    local water = Combine.Max{ a = lakes, b = wet.rivers }
    local dry = Curves.Smoothstep{ input = wet.height - water, min = 0.0, max = 0.05 }
    local soft_height = Combine.Blend{ a = wet.height, b = Combine.Min{ a = wet.height, b = laid }, mask = near_bank * dry }
    -- No slot along a river: within 120 m of one the dry ground is held under a V at 31 degrees from 2 m
    -- over the highest water near it (the network's own water, at 12, 24, 48 and 96 m), so a channel
    -- cut deep into a slope opens into sloping walls. Not near the lake or the Haven, whose shores are
    -- shaped above.
    local function V(metres)
        local e = Filters.Expand{ input = wet.rivers, radius = math.max(1, math.min(32, math.floor(metres / res + 0.5))) }
        -- A real water level: not the falling-off values the network writes past a channel's end.
        local found = Curves.Smoothstep{ input = e - wet.height, min = -60.0, max = -40.0 }
        return (e + 2.0 + 0.6 * metres) * found + (1.0 - found) * 100000.0
    end
    local vcap = Combine.Min{ a = Combine.Min{ a = V(12.0), b = V(24.0) }, b = Combine.Min{ a = V(48.0), b = V(96.0) } }
    local by_slot = (1.0 - Curves.Smoothstep{ input = to_river, min = 90.0, max = 120.0 }) * Curves.Smoothstep{ input = lake, min = 150.0, max = 250.0 }
                    * Curves.Smoothstep{ input = haven, min = 200.0, max = 300.0 }
    soft_height = Combine.Blend{ a = soft_height, b = Combine.Min{ a = soft_height, b = vcap }, mask = by_slot * dry }
    -- At most 3 m: on a fall the water just upstream is far higher, and a bank built to it was a spike.
    local height = Combine.Blend{ a = soft_height, b = Combine.Max{ a = soft_height, b = Combine.Min{ a = Filters.Expand{ input = water, radius = 1 } + 0.3, b = wet.height + 3.0 } }, mask = dry }

    -- One biome id a sample: 0 plains, 1 ancient forest, 2 dark forest, 3 swamp, 4 mountain, 5 water.
    local biome = Terrain.Const{ value = 0.0 }
    local wetness = water - height
    for id, m in ipairs{ ancient, dark, swamp, mountain, Curves.Smoothstep{ input = wetness, min = 0.0, max = 0.01 } } do
        biome = biome + (id - biome) * Curves.Smoothstep{ input = m, min = 0.49, max = 0.51 }
    end
    return {
        height  = { value = height, range = { 0, 3600 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = height } } },
        lakes   = { value = lakes, range = { -25000, 3600 } },
        rivers  = { value = wet.rivers, range = { -25000, 3600 } },
        biome   = { value = biome, range = { 0, 5 } },
        look    = { snow = 1300 },
    }
end

-- The reference's central lake (with the pond west of it, joined under the road bridge, and the water
-- under the fortress's bridge and piers) and Mountain Haven (with the water under
-- its settlement on piers), segmented by the colour of the water every 2 px (20.2 m): run lengths per
-- row, starting with ground.
DATA.lake = [[
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
281,1,345;
280,3,344;
280,3,344;
279,3,345;
279,2,346;
278,3,346;
278,2,347;
278,2,347;
278,2,347;
278,3,346;
278,3,346;
278,3,112,3,231;
279,2,113,2,231;
280,2,112,3,230;
281,3,111,2,230;
282,3,110,3,229;
283,4,108,3,229;
283,4,108,4,228;
283,4,108,5,227;
283,3,110,4,227;
282,4,111,5,225;
281,5,113,5,223;
279,6,114,6,222;
279,6,116,5,221;
279,6,117,5,220;
279,6,118,4,220;
280,4,120,5,218;
280,4,120,6,217;
280,4,121,5,217;
279,6,120,5,217;
277,10,118,5,217;
275,13,118,4,217;
275,14,117,4,217;
275,14,117,4,217;
275,14,117,4,217;
275,13,116,6,217;
275,12,118,5,217;
275,11,119,4,218;
275,6,123,5,218;
263,5,6,7,123,4,219;
262,7,3,8,122,7,218;
261,19,8,2,2,2,108,7,218;
261,19,8,3,1,3,99,14,219;
261,19,8,7,98,14,220;
264,17,7,7,97,15,220;
264,31,96,15,221;
264,31,95,17,17,1,202;
264,31,95,17,16,3,201;
264,31,13,1,81,17,16,3,201;
264,15,2,14,10,4,81,18,12,6,201;
263,15,3,17,5,7,81,18,9,8,201;
263,14,4,29,81,19,6,12,199;
261,15,7,28,81,36,199;
260,16,7,29,78,39,198;
259,17,6,33,75,39,198;
260,17,5,33,73,41,198;
260,18,3,34,72,42,198;
261,54,72,42,198;
261,54,72,41,27,2,170;
261,1,1,2,1,48,72,41,27,2,171;
266,47,76,38,26,2,172;
216,4,46,48,75,38,25,2,173;
215,5,19,1,27,47,10,2,63,38,24,1,175;
213,7,8,2,5,7,26,32,4,12,6,7,61,36,25,1,175;
212,9,6,4,2,10,26,31,6,11,4,9,60,37,23,2,175;
209,12,4,18,26,31,6,24,60,38,22,1,176;
209,35,25,30,9,22,50,1,8,39,21,1,177;
208,38,23,28,13,20,49,4,5,41,20,1,177;
207,7,2,32,20,29,13,20,23,1,25,51,19,1,177;
206,7,3,32,20,29,14,19,21,3,24,7,3,49,12,1,177;
185,2,6,3,10,6,5,32,18,30,14,20,9,3,7,4,24,6,4,49,12,1,177;
186,3,2,6,7,7,7,31,17,32,12,22,6,6,6,5,23,6,5,49,11,2,176;
187,12,1,10,9,30,11,38,12,23,4,19,4,4,14,6,6,48,11,2,176;
188,21,10,30,10,40,10,49,2,6,13,5,6,49,10,2,176;
190,21,7,34,6,41,9,59,12,6,4,51,9,2,176;
190,22,6,36,2,43,7,61,12,62,8,2,176;
190,23,5,81,7,61,12,62,9,2,175;
190,109,6,62,12,61,11,2,174;
190,110,4,63,11,62,11,3,173;
191,5,3,169,8,63,12,4,172;
200,143,2,24,5,65,12,5,171;
201,142,3,91,15,7,168;
202,140,6,82,2,4,18,6,167;
202,136,11,78,28,5,167;
202,136,12,76,29,5,167;
203,5,2,3,2,39,1,83,13,75,30,9,162;
204,3,9,36,4,82,17,70,31,13,158;
219,32,6,72,2,3,22,25,3,40,32,15,156;
219,32,6,71,4,2,22,25,3,39,33,15,156;
219,32,6,1,1,67,6,1,25,22,4,39,32,17,8,6,141;
219,33,5,69,31,21,7,38,31,21,1,10,141;
219,34,3,67,1,1,32,21,8,38,30,33,140;
219,106,31,21,9,39,28,35,139;
218,102,2,3,31,21,10,38,19,5,3,36,139;
217,102,4,2,30,22,10,39,18,46,137;
214,104,37,22,9,39,17,51,134;
212,105,38,2,2,19,7,39,17,53,133;
211,106,43,64,17,54,132;
211,106,43,20,1,42,18,54,132;
210,107,43,63,18,54,132;
209,108,43,6,2,55,18,54,132;
209,109,51,54,18,31,1,23,131;
208,110,52,54,18,29,3,22,131;
208,112,50,55,17,30,2,22,131;
208,116,46,55,17,55,130;
208,116,46,56,1,4,10,55,131;
208,117,45,62,9,55,131;
208,117,46,1,4,56,7,57,131;
208,115,53,9,2,46,5,57,132;
207,113,56,5,7,107,132;
207,112,57,5,7,107,132;
206,106,1,1,3,2,59,3,7,106,133;
202,111,75,105,134;
202,112,78,96,139;
202,110,81,94,140;
201,110,82,93,141;
201,109,84,92,141;
201,108,86,90,142;
201,97,3,6,88,90,142;
201,96,8,1,88,91,142;
201,2,1,94,95,92,142;
202,95,96,93,141;
205,91,98,92,141;
206,88,102,90,141;
208,86,103,90,140;
209,84,105,90,139;
209,83,106,95,134;
158,6,45,84,105,97,132;
157,8,2,5,37,84,105,98,131;
155,10,2,7,35,85,104,98,131;
155,20,35,84,102,99,132;
155,21,34,85,101,99,132;
155,22,32,86,100,92,3,4,133;
154,23,26,2,2,88,100,92,3,4,133;
154,23,25,24,2,65,103,10,3,78,3,4,133;
153,24,24,24,4,63,105,8,5,83,134;
153,24,23,24,6,63,105,6,7,79,137;
145,2,4,25,23,24,8,62,106,5,7,80,1,2,133;
144,32,21,26,8,61,107,5,6,84,133;
143,33,21,27,6,63,105,6,9,83,131;
143,34,20,28,5,64,104,7,3,2,4,57,1,25,130;
142,36,19,29,3,67,101,9,1,3,4,86,4,4,119;
142,12,2,22,19,101,98,12,6,94,119;
142,11,4,24,15,90,2,11,98,11,5,96,118;
143,9,5,24,14,16,3,71,4,11,98,11,3,98,2,3,112;
145,7,8,50,5,69,6,10,98,117,112;
146,5,12,29,1,16,7,67,8,9,102,113,112;
163,46,8,66,8,8,103,73,5,36,111;
164,45,9,64,9,7,105,70,8,36,110;
164,44,10,64,9,7,105,70,9,36,109;
164,43,11,65,8,6,106,70,9,37,108;
164,43,11,67,4,8,106,47,1,22,10,40,104;
164,43,9,70,3,8,105,47,3,20,11,41,103;
162,46,8,82,104,47,7,11,2,2,12,41,103;
161,13,1,2,1,1,1,29,7,83,101,48,9,9,16,44,101;
160,11,14,25,4,85,101,48,10,8,16,45,100;
161,9,16,24,2,88,6,5,81,1,5,50,10,7,19,1,1,32,3,6,100;
161,9,16,99,2,15,2,7,81,4,1,50,12,5,24,30,3,6,100;
161,9,16,97,4,25,80,54,42,39,100;
162,7,17,96,6,24,79,52,46,38,100;
164,5,17,43,2,50,8,4,3,16,79,52,46,38,100;
165,1,22,38,6,49,10,1,4,16,80,46,1,4,47,37,100;
192,32,8,50,15,15,75,3,2,43,5,3,47,37,100;
193,31,10,50,14,14,75,3,2,42,7,1,49,36,100;
194,29,11,50,14,12,78,2,1,42,56,36,102;
194,28,12,15,2,33,15,10,79,2,1,41,56,37,102;
194,25,16,12,4,34,14,9,80,44,56,17,2,17,103;
195,4,2,17,17,11,6,33,14,9,79,45,56,16,5,14,104;
195,3,3,16,19,6,12,31,14,10,75,44,1,2,59,13,7,12,105;
196,2,4,14,21,3,14,32,12,11,75,44,1,2,60,4,1,6,9,9,107;
202,10,1,3,39,31,12,10,76,48,61,2,2,4,10,7,109;
202,3,3,2,45,32,11,11,75,48,82,2,111;
255,33,10,11,66,1,1,56,194;
254,36,9,11,61,8,1,53,194;
254,37,8,13,5,4,50,65,2,2,187;
252,39,9,3,2,9,2,6,49,12,1,58,185;
250,41,16,14,39,1,2,4,3,72,185;
249,42,16,14,36,5,1,56,4,19,185;
249,41,18,8,2,3,36,7,1,2,1,51,5,17,186;
249,41,20,6,3,2,25,73,5,17,186;
248,42,20,6,2,3,23,4,1,2,1,65,8,16,186;
248,42,20,12,6,1,15,7,1,64,10,15,186;
248,42,20,106,11,15,185;
246,45,18,107,11,15,185;
240,52,16,109,10,16,184;
240,20,1,33,1,3,10,109,9,19,182;
239,16,7,37,9,109,8,21,181;
239,15,9,37,9,110,6,22,180;
238,16,10,36,10,110,5,23,179;
238,15,12,36,10,109,4,25,178;
238,14,13,4,2,30,10,66,1,72,177;
240,11,15,3,4,29,8,65,4,71,177;
240,11,23,31,4,65,6,70,177;
241,9,24,33,1,66,7,69,177;
242,6,27,100,5,70,177;
244,2,29,101,4,70,177;
275,101,3,71,177;
274,175,178;
274,1,1,172,179;
277,171,179;
277,170,180;
277,160,2,7,181;
277,155,8,5,182;
277,154,9,1,186;
277,67,3,83,197;
277,67,3,83,197;
276,50,1,3,1,13,3,82,198;
276,66,5,82,198;
276,47,24,74,2,6,198;
276,47,26,70,5,4,199;
276,47,27,68,209;
278,34,3,7,28,67,210;
282,28,5,5,30,67,210;
283,26,41,67,210;
284,25,41,67,210;
284,25,41,67,210;
285,24,41,67,210;
285,24,41,68,209;
285,24,41,69,208;
285,23,42,69,208;
285,23,48,62,209;
287,18,50,63,209;
287,18,51,62,209;
287,17,51,58,214;
287,17,51,57,215;
288,16,51,56,216;
287,16,52,55,217;
286,17,53,54,217;
286,15,55,54,217;
285,11,60,54,217;
284,11,61,54,217;
284,10,62,54,217;
284,10,62,55,216;
285,9,62,55,216;
285,8,63,55,216;
284,9,63,55,216;
285,8,63,54,217;
285,8,63,54,217;
287,7,62,54,217;
287,8,61,54,217;
287,8,61,53,218;
288,6,63,48,2,2,218;
288,6,64,46,223;
288,6,65,43,225;
291,3,72,34,227;
367,33,227;
366,35,226;
366,34,227;
365,35,227;
365,35,227;
366,34,227;
366,28,3,2,228;
366,25,236;
367,24,236;
368,22,237;
370,19,238;
376,9,242;
376,6,245;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
]]
DATA.haven = [[
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
84,2,541;
83,3,541;
83,4,540;
83,5,539;
82,9,536;
83,8,536;
83,7,537;
83,7,537;
83,9,535;
83,9,535;
82,8,1,2,534;
82,5,1,5,534;
81,7,3,2,534;
78,8,5,3,533;
77,10,4,4,532;
76,11,4,4,532;
76,11,3,5,532;
75,13,2,5,532;
64,2,4,2,1,28,526;
62,6,1,33,4,1,520;
62,46,519;
61,47,519;
60,48,519;
60,48,519;
56,52,519;
55,53,519;
55,53,519;
56,56,2,4,509;
56,63,508;
56,63,508;
56,64,507;
57,65,505;
56,68,503;
57,70,500;
57,65,2,2,501;
57,64,506;
57,62,508;
57,61,509;
58,60,509;
57,61,509;
57,61,509;
57,61,509;
58,60,509;
58,60,509;
58,60,509;
57,62,3,1,504;
57,66,504;
60,55,2,6,504;
57,57,4,5,504;
57,56,5,5,504;
56,56,7,4,504;
56,50,13,4,504;
54,52,521;
52,52,523;
51,53,523;
44,3,1,58,521;
42,62,523;
42,64,521;
41,65,521;
41,65,521;
41,66,520;
48,58,521;
49,56,522;
49,4,1,50,523;
50,1,4,42,1,4,525;
58,36,533;
60,33,534;
60,33,534;
60,33,534;
60,33,534;
60,14,3,15,535;
60,13,6,13,535;
60,2,3,7,8,12,535;
66,4,10,12,535;
67,1,12,1,1,8,537;
83,6,538;
83,1,543;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
627;
]]

-- The rivers' courses in pixels of the reference, from the source to the mouth: snapped every 4 px to the
-- middle of the channel the reference draws (and its width there), kept every 12 px.
DATA.rivers = {
    east = { -- 10 px wide
        { 1068, 1246 }, { 1060, 1239 }, { 1059, 1227 }, { 1064, 1214 }, { 1064, 1202 }, { 1059, 1191 }, { 1055, 1179 }, { 1051, 1168 },
        { 1047, 1156 }, { 1043, 1145 }, { 1040, 1134 }, { 1038, 1122 }, { 1031, 1111 }, { 1022, 1102 }, { 1013, 1093 }, { 1009, 1081 },
        { 1011, 1067 }, { 1006, 1056 }, { 1005, 1044 }, { 1005, 1031 }, { 1004, 1019 }, { 1007, 1008 }, { 1013, 997 }, { 1016, 985 },
        { 1021, 974 }, { 1028, 965 }, { 1031, 960 }, { 1038, 952 }, { 1045, 942 }, { 1050, 932 }, { 1057, 924 }, { 1065, 915 },
        { 1070, 904 }, { 1076, 893 }, { 1081, 881 }, { 1088, 871 }, { 1096, 862 }, { 1104, 853 }, { 1112, 845 }, { 1122, 837 },
        { 1134, 831 }, { 1143, 823 }, { 1152, 813 }, { 1156, 799 }, { 1159, 787 }, { 1166, 776 }, { 1171, 765 }, { 1172, 751 },
        { 1163, 737 }, { 1151, 728 }, { 1142, 718 }, { 1138, 707 }, { 1138, 694 }, { 1136, 679 }, { 1129, 669 }, { 1121, 661 },
        { 1108, 657 }, { 1093, 658 }, { 1082, 655 }, { 1072, 646 }, { 1065, 638 }, { 1062, 634 }, { 1052, 636 }, { 1040, 636 },
    },
    brook = { -- 5 px wide
        { 1038, 952 }, { 1020, 941 }, { 1003, 928 }, { 995, 925 }, { 985, 919 }, { 974, 914 }, { 964, 907 }, { 954, 899 },
        { 944, 892 }, { 933, 887 },
        { 921, 884 }, { 910, 880 }, { 901, 872 }, { 892, 863 }, { 880, 860 }, { 868, 853 }, { 857, 843 }, { 848, 836 },
    },
    centre = { -- 10 px wide
        { 490, 1246 }, { 491, 1238 }, { 486, 1226 }, { 484, 1215 }, { 482, 1203 }, { 484, 1190 }, { 484, 1178 }, { 479, 1167 },
        { 471, 1156 }, { 463, 1145 }, { 463, 1133 }, { 472, 1118 }, { 476, 1104 }, { 478, 1090 }, { 477, 1078 }, { 471, 1067 },
        { 463, 1058 }, { 457, 1047 }, { 450, 1037 }, { 440, 1028 }, { 431, 1018 }, { 428, 1007 }, { 432, 996 }, { 437, 986 },
        { 443, 979 }, { 454, 982 }, { 465, 986 }, { 477, 989 }, { 489, 989 }, { 502, 987 }, { 513, 983 }, { 522, 975 },
        { 533, 969 }, { 543, 963 }, { 553, 956 }, { 561, 948 }, { 569, 939 }, { 577, 931 }, { 585, 922 }, { 592, 914 },
        { 595, 903 }, { 594, 891 }, { 590, 880 }, { 582, 870 }, { 574, 861 }, { 572, 849 }, { 576, 836 }, { 578, 824 },
        { 581, 814 }, { 578, 803 }, { 577, 795 },
    },
    haven_in = { -- 7 px wide
        { 150, 1246 }, { 153, 1238 }, { 151, 1226 }, { 148, 1212 }, { 160, 1203 }, { 173, 1196 }, { 183, 1188 }, { 194, 1181 },
        { 203, 1171 }, { 207, 1159 }, { 213, 1149 }, { 220, 1139 }, { 221, 1125 }, { 222, 1112 }, { 226, 1100 }, { 223, 1087 },
        { 222, 1074 }, { 227, 1063 }, { 236, 1055 }, { 246, 1045 }, { 248, 1032 }, { 246, 1019 }, { 249, 1007 }, { 252, 993 },
        { 251, 981 }, { 249, 973 },
    },
    west = { -- 7 px wide
        { 165, 894 }, { 171, 883 }, { 173, 871 }, { 169, 858 }, { 170, 846 }, { 174, 834 }, { 176, 822 }, { 183, 812 },
        { 194, 802 }, { 195, 790 }, { 190, 783 }, { 181, 782 }, { 171, 776 }, { 160, 768 }, { 149, 760 }, { 137, 758 },
        { 124, 762 }, { 111, 768 }, { 97, 770 }, { 83, 767 }, { 73, 760 }, { 66, 748 }, { 59, 736 }, { 54, 725 },
        { 53, 712 }, { 57, 700 }, { 64, 689 }, { 70, 677 }, { 63, 663 }, { 49, 654 }, { 41, 649 },
    },
    forest = { -- 6 px wide
        { 168, 523 }, { 173, 534 }, { 181, 543 }, { 191, 551 }, { 198, 560 }, { 201, 571 }, { 201, 585 }, { 206, 595 },
        { 214, 605 }, { 217, 618 }, { 219, 630 }, { 228, 638 }, { 235, 648 }, { 235, 660 }, { 235, 672 }, { 240, 684 },
        { 245, 696 }, { 245, 708 }, { 238, 719 }, { 229, 728 }, { 223, 739 }, { 222, 752 }, { 218, 764 }, { 209, 774 },
        { 201, 779 }, { 198, 781 },
    },
    canal = { -- 7 px wide
        { 481, 865 }, { 476, 876 }, { 468, 886 }, { 458, 893 }, { 446, 897 }, { 434, 899 }, { 423, 904 }, { 415, 912 },
        { 411, 923 }, { 414, 934 }, { 422, 944 }, { 429, 953 }, { 434, 964 }, { 437, 973 }, { 438, 978 },
    },
}

return main
