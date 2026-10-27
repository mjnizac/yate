-- Water.Network: rivers, lakes and the sea as analytic elements that shape the terrain around them.
--
--   local net = Water.Network{ terrain = base, region = { -8000, -8000, 8000, 8000 }, sea_level = 0,
--                              generate = { rivers = { count = 4 }, lakes = { count = 2 } },
--                              rivers = { ... }, lakes = { ... }, shapes = params.shapes, seed = params.seed }
--   local wet = net:apply(base + detail)
--   return { height = { value = wet.height }, lakes = { value = wet.lakes }, rivers = { value = wet.rivers },
--            sea = { value = wet.sea } }
--
-- Nothing is simulated. The terrain is sampled once on a coarse grid (`Terrain.Sample`) to decide where
-- water goes: where rivers rise and run down, where lakes lie and at what level, which low ground is the
-- sea. Then every element is a curve or an outline with a level, and the terrain is made to follow it:
--
--   * a river is a curve with a water level that never rises downstream. Its channel is cut under the
--     level and its valley is an envelope over the channel (`Min` of the terrain and the level plus a
--     profile of the distance), so the valley shape is the river's kind: a V in the mountains, stepped
--     walls in a canyon, a U under a glacier, a floodplain with terraces and levees in the lowlands, a
--     wide gravel bed when braided, sheer walls in a gorge;
--   * a lake is an outline with a level at any height, under the sea level too. Its floor is shaped by
--     its kind (a bowl, a trench with cliffs, a shelf and a drop-off, a crater, a cirque), it can hold
--     islands, a low rim keeps the water in, and the ground around can fall towards it as a basin;
--   * the sea is the low ground connected to the edge of the region under `sea_level`; a depression
--     under that level that does not reach the edge stays dry or holds a lake.
--
-- Rivers and lakes come from three places, combined: `rivers` and `lakes` given here, the `river` and
-- `lake` features of `--shapes`, and `generate`, which looks for the sites itself (`count` or `density`
-- per 100 km2, 0 for none). A river given by its `source` alone is traced down the terrain.
--
-- Run by the engine after `45_water.lua`.

local engine = ...
local fail   = engine.fail

local Hydrology, Terrain, Curves, Combine, Noises, Water = Hydrology, Terrain, Curves, Combine, Noises, Water
local type, pairs, ipairs, table, math, tostring, setmetatable = type, pairs, ipairs, table, math, tostring, setmetatable

local function check(name, t, allowed)
    if type(t) ~= "table" then
        fail(name .. " expects a table of named arguments")
    end
    local known, list = {}, {}
    for _, key in ipairs(allowed) do
        known[key] = true
        list[#list + 1] = key
    end
    for key in pairs(t) do
        if type(key) == "string" and not known[key] then
            fail(name .. " does not take '" .. key .. "'; it accepts " .. table.concat(list, ", "))
        end
    end
end

-- --- Kinds ------------------------------------------------------------------------------------------

-- How each kind of river runs and what valley it cuts. `omega` is how far its meanders swing, in radians at
-- a roughness of 0.5 (Langbein and Leopold; a lowland river loops, a gorge barely bends); `valley` is the
-- half-width in metres the valley may reach; the rest are the kind's own profile.
local RIVER_KINDS = {
    mountain = { omega = 0.45, valley = 1400, slope = 30, grade = 1.2, falls = true },
    canyon   = { omega = 0.9, valley = 1600, slope = 62, step = 70, grade = 3.0 },
    gorge    = { omega = 0.25, valley = 400, slope = 84, grade = 1.6, falls = true },
    glacial  = { omega = 0.15, valley = 1800, slope = 62, floor = 0.3, grade = 1.0, falls = true },
    lowland  = { omega = 1.7, valley = 2200, slope = 12, plain = 0.5, terrace = 4.0, levee = 1.2, grade = 1.8 },
    braided  = { omega = 0.25, valley = 1800, slope = 18, plain = 0.55, widen = 5.0, grade = 1.3 },
}
local LAKE_KINDS = {
    bowl   = { wall = 0.45, basin = 14 },
    trench = { wall = 0.06, shelf = 0.18, shelf_depth = 0.12, basin = 70 },
    shelf  = { wall = 0.08, shelf = 0.45, shelf_depth = 0.3, basin = 10 },
    crater = { wall = 0.25, rim = 40, basin = 35 },
    cirque = { wall = 0.15, basin = 55, headwall = 1.5 },
    -- A lake behind a terminal moraine, as a glacier leaves them: a ridge across its downhill side.
    moraine = { wall = 0.3, basin = 18, moraine = 18 },
    -- A delta's fan: not water but land just over it, built where a river spreads at its mouth.
    fan    = { wall = 0.2, basin = 0 },
}

-- The codes `water.comp.glsl` knows the kinds by.
local RIVER_CODES = { mountain = 0, canyon = 1, gorge = 2, glacial = 3, lowland = 4, braided = 5 }
local LAKE_CODES  = { bowl = 0, trench = 1, shelf = 2, crater = 3, cirque = 4, fan = 5, moraine = 0 }
-- How steep the ground around a lake falls to it, by its shore, in degrees: a sandy beach, rock, a cliff.
local SHORES = { beach = 4, rock = 30, cliff = 72 }

local function kind_list(kinds)
    local list = {}
    for name in pairs(kinds) do
        list[#list + 1] = name
    end
    table.sort(list)
    return table.concat(list, ", ")
end

-- --- Small geometry on point lists -----------------------------------------------------------------

local function length_of(points)
    local total = 0.0
    for i = 2, #points do
        total = total + math.sqrt((points[i][1] - points[i - 1][1]) ^ 2 + (points[i][2] - points[i - 1][2]) ^ 2)
    end
    return total
end

-- Chaikin corner cutting, the ends kept: a cell path becomes a curve.
local function smooth(points, passes)
    for _ = 1, passes do
        if #points < 3 then
            return points
        end
        local out = { points[1] }
        for i = 1, #points - 1 do
            local a, b = points[i], points[i + 1]
            out[#out + 1] = { a[1] * 0.75 + b[1] * 0.25, a[2] * 0.75 + b[2] * 0.25 }
            out[#out + 1] = { a[1] * 0.25 + b[1] * 0.75, a[2] * 0.25 + b[2] * 0.75 }
        end
        out[#out + 1] = points[#points]
        points = out
    end
    return points
end

-- The same corner cutting on a closed outline: a polygon of drawn points becomes a smooth shore.
local function smooth_closed(points, passes)
    for _ = 1, passes do
        local out = {}
        for i = 1, #points do
            local a, b = points[i], points[i % #points + 1]
            out[#out + 1] = { a[1] * 0.75 + b[1] * 0.25, a[2] * 0.75 + b[2] * 0.25 }
            out[#out + 1] = { a[1] * 0.25 + b[1] * 0.75, a[2] * 0.25 + b[2] * 0.75 }
        end
        points = out
    end
    return points
end

-- Points every `step` metres along the curve.
local function resample(points, step)
    local out, carry = { points[1] }, 0.0
    for i = 2, #points do
        local a, b = points[i - 1], points[i]
        local d = math.sqrt((b[1] - a[1]) ^ 2 + (b[2] - a[2]) ^ 2)
        local s = step - carry
        while s <= d do
            local t = s / d
            out[#out + 1] = { a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t }
            s = s + step
        end
        carry = d - (s - step)
    end
    local last = points[#points]
    if out[#out][1] ~= last[1] or out[#out][2] ~= last[2] then
        out[#out + 1] = last
    end
    return out
end

-- A repeatable value in [-1, 1] from an integer and a salt.
local function jitter(i, salt)
    local v = math.sin(i * 127.1 + salt * 311.7) * 43758.5453
    return (v - math.floor(v)) * 2.0 - 1.0
end

-- The same, continuous in a real `x`: neighbouring integers eased into each other, so whatever it
-- modulates varies without a step.
local function wobble(x, salt)
    local i = math.floor(x)
    local t = x - i
    t = t * t * (3.0 - 2.0 * t)
    return jitter(i, salt) * (1.0 - t) + jitter(i + 1, salt) * t
end

-- Meanders as a sine-generated curve (Langbein and Leopold): the direction of the channel swings about
-- the valley's as `omega * sin(phase)`, with Kinoshita's skew and flattening in the third harmonic, so the
-- bends are loops that lean downstream and not the corners a sideways sine turns into. The curve is built
-- along a straight axis and laid onto `points`, the course, by its distance along it; the swing fades in
-- and out over half a wavelength so source and mouth stay where they are. `wavelength` is measured along
-- the channel and wanders by a third along the river.
local function meander(points, wavelength, omega, salt)
    local total = length_of(points)
    if omega <= 0.01 or #points < 3 or total < wavelength then
        return points
    end
    -- The course by distance along it, and its direction there averaged over a quarter wavelength.
    local runs = { 0.0 }
    for i = 2, #points do
        runs[i] = runs[i - 1] + math.sqrt((points[i][1] - points[i - 1][1]) ^ 2 + (points[i][2] - points[i - 1][2]) ^ 2)
    end
    local function at(u)
        u = math.max(0.0, math.min(total, u))
        local lo, hi = 1, #points
        while hi - lo > 1 do
            local mid = (lo + hi) // 2
            if runs[mid] <= u then lo = mid else hi = mid end
        end
        local t = (u - runs[lo]) / math.max(1e-9, runs[hi] - runs[lo])
        return points[lo][1] + (points[hi][1] - points[lo][1]) * t, points[lo][2] + (points[hi][2] - points[lo][2]) * t
    end
    local reach = wavelength * 0.25
    local axial, lateral = {}, {}
    local step = wavelength / 48.0
    local s, x, y, phase = 0.0, 0.0, 0.0, 0.0
    while x < total and #axial < 200000 do
        local fade  = math.max(0.0, math.min(1.0, x / (wavelength * 0.5), (total - x) / (wavelength * 0.5)))
        local swing = omega * fade * (0.8 + 0.2 * wobble(s / wavelength, salt))
        local theta = swing * (math.sin(phase) + 0.12 * math.cos(3.0 * phase) - 0.05 * math.sin(3.0 * phase))
        axial[#axial + 1], lateral[#lateral + 1] = x, y
        x = x + step * math.cos(theta)
        y = y + step * math.sin(theta)
        s = s + step
        phase = phase + 2.0 * math.pi * step / (wavelength * (1.0 + 0.33 * wobble(s / wavelength, salt + 7)))
    end
    -- Whatever sideways offset is left at the mouth is taken out evenly, so the end lands on the course.
    local last = lateral[#lateral]
    local out = {}
    for i = 1, #axial do
        local u = axial[i] * total / math.max(1.0, axial[#axial])
        local px, pz = at(u)
        local ax, az = at(u - reach)
        local bx, bz = at(u + reach)
        local tx, tz = bx - ax, bz - az
        local tl = math.max(1e-6, math.sqrt(tx * tx + tz * tz))
        local off = lateral[i] - last * axial[i] / math.max(1.0, axial[#axial])
        out[i] = { px - tz / tl * off, pz + tx / tl * off }
    end
    out[1], out[#out] = points[1], points[#points]
    return out
end

-- --- The analysis grid -------------------------------------------------------------------------------

-- The terrain sampled on a coarse grid over the region, drained, with what the elements need to ask it.
local function analyse(t)
    local region = t.region
    local cell   = t.cell
    local n      = math.floor((region[3] - region[1]) / cell) + 1
    local m      = math.floor((region[4] - region[2]) / cell) + 1
    local h      = Terrain.Sample{ value = t.terrain, origin = { region[1], region[2] }, size = { n, m },
                                   cell = cell, seed = t.seed }
    local d      = Hydrology.Drain{
        size = { n, m }, cell = cell, height = h, sea_level = t.sea_level, breach = 4.0,
        lake_area = t.lake_area, lake_depth = 3.0, channel_area = 1e12,
    }
    local g = { n = n, m = m, cell = cell, x0 = region[1], z0 = region[2], h = h, d = d }
    function g.index(x, z)
        local i = math.floor((x - g.x0) / cell + 0.5)
        local j = math.floor((z - g.z0) / cell + 0.5)
        i = math.max(0, math.min(n - 1, i))
        j = math.max(0, math.min(m - 1, j))
        return j * n + i + 1
    end
    function g.at(i)
        return { g.x0 + ((i - 1) % n) * cell, g.z0 + ((i - 1) // n) * cell }
    end
    -- Bilinear height of the terrain as sampled.
    function g.height(x, z)
        local fx = math.max(0.0, math.min(n - 1.001, (x - g.x0) / cell))
        local fz = math.max(0.0, math.min(m - 1.001, (z - g.z0) / cell))
        local i, j = math.floor(fx), math.floor(fz)
        local u, v = fx - i, fz - j
        local k = j * n + i + 1
        local a = h[k] + (h[k + 1] - h[k]) * u
        local b = h[k + n] + (h[k + n + 1] - h[k + n]) * u
        return a + (b - a) * v
    end
    return g
end

-- Cells under a lake surface, grouped into lakes: { cells, level, area, depth, centre }.
local function natural_lakes(g)
    local d, h, n, m = g.d, g.h, g.n, g.m
    local label, lakes = {}, {}
    for start = 1, n * m do
        if not label[start] and d.lake[start] > d.height[start] + 0.5 then
            local lake = { cells = {}, level = -1e30, depth = 0.0, cx = 0.0, cz = 0.0 }
            local stack = { start }
            label[start] = lake
            while #stack > 0 do
                local c = table.remove(stack)
                lake.cells[#lake.cells + 1] = c
                lake.level = math.max(lake.level, d.lake[c])
                lake.depth = math.max(lake.depth, d.lake[c] - h[c])
                local p = g.at(c)
                lake.cx, lake.cz = lake.cx + p[1], lake.cz + p[2]
                local x, z = (c - 1) % n, (c - 1) // n
                for dz = -1, 1 do
                    for dx = -1, 1 do
                        local nx, nz = x + dx, z + dz
                        if nx >= 0 and nz >= 0 and nx < n and nz < m then
                            local q = nz * n + nx + 1
                            if not label[q] and d.lake[q] > d.height[q] + 0.5 then
                                label[q] = lake
                                stack[#stack + 1] = q
                            end
                        end
                    end
                end
            end
            lake.cx, lake.cz = lake.cx / #lake.cells, lake.cz / #lake.cells
            lake.area = #lake.cells * g.cell * g.cell
            lakes[#lakes + 1] = lake
        end
    end
    return lakes, label
end

-- Douglas-Peucker on a closed outline: the points that matter to within `tolerance` metres.
local function simplify(points, tolerance)
    if #points <= 8 then
        return points
    end
    local keep = { [1] = true, [#points] = true }
    local function walk(a, b)
        local ax, az, bx, bz = points[a][1], points[a][2], points[b][1], points[b][2]
        local dx, dz = bx - ax, bz - az
        local l2 = math.max(1e-9, dx * dx + dz * dz)
        local far, at = 0.0, nil
        for i = a + 1, b - 1 do
            local t = math.max(0.0, math.min(1.0, ((points[i][1] - ax) * dx + (points[i][2] - az) * dz) / l2))
            local ex, ez = points[i][1] - ax - dx * t, points[i][2] - az - dz * t
            local d = ex * ex + ez * ez
            if d > far then
                far, at = d, i
            end
        end
        if at and far > tolerance * tolerance then
            keep[at] = true
            walk(a, at)
            walk(at, b)
        end
    end
    walk(1, #points)
    local out = {}
    for i = 1, #points do
        if keep[i] then
            out[#out + 1] = points[i]
        end
    end
    return out
end

-- A lake's shore as the contour of its level over the sampled ground (marching squares over its cells,
-- the crossing placed where the water meets the ground between two of them), the longest loop, simplified
-- to under 120 points.
local function outline_of(g, lake, label)
    local n = g.n
    local minx, maxx, minz, maxz = math.huge, -math.huge, math.huge, -math.huge
    for _, c in ipairs(lake.cells) do
        local x, z = (c - 1) % n, (c - 1) // n
        minx, maxx = math.min(minx, x), math.max(maxx, x)
        minz, maxz = math.min(minz, z), math.max(maxz, z)
    end
    minx, maxx, minz, maxz = minx - 1, maxx + 1, minz - 1, maxz + 1
    -- Positive under the water, negative above it or outside this lake.
    local function v(x, z)
        if x < 0 or z < 0 or x >= n or z >= g.m then
            return -1.0
        end
        local c = z * n + x + 1
        if label[c] ~= lake then
            return math.min(-0.25, lake.level - g.h[c])
        end
        return math.max(0.25, lake.level - g.h[c])
    end
    local function pos(x, z)
        return g.x0 + x * g.cell, g.z0 + z * g.cell
    end
    local function cross(x1, z1, a, x2, z2, b)
        local t = a / (a - b)
        local px, pz = pos(x1 + (x2 - x1) * t, z1 + (z2 - z1) * t)
        return { px, pz }
    end
    -- Segments, each from one edge crossing to another, chained by their ends.
    local ends = {}
    local function key(p)
        return math.floor(p[1] * 8.0 + 0.5) .. ":" .. math.floor(p[2] * 8.0 + 0.5)
    end
    local segments = {}
    for z = minz, maxz - 1 do
        for x = minx, maxx - 1 do
            local a, b, c, d = v(x, z), v(x + 1, z), v(x + 1, z + 1), v(x, z + 1)
            local edges = {}
            if (a > 0) ~= (b > 0) then edges[#edges + 1] = cross(x, z, a, x + 1, z, b) end
            if (b > 0) ~= (c > 0) then edges[#edges + 1] = cross(x + 1, z, b, x + 1, z + 1, c) end
            if (c > 0) ~= (d > 0) then edges[#edges + 1] = cross(x + 1, z + 1, c, x, z + 1, d) end
            if (d > 0) ~= (a > 0) then edges[#edges + 1] = cross(x, z + 1, d, x, z, a) end
            for k = 1, #edges - 1, 2 do
                segments[#segments + 1] = { edges[k], edges[k + 1] }
            end
        end
    end
    for i, s in ipairs(segments) do
        for side = 1, 2 do
            local kk = key(s[side])
            ends[kk] = ends[kk] or {}
            table.insert(ends[kk], { i, side })
        end
    end
    local used, best = {}, {}
    for i = 1, #segments do
        if not used[i] then
            local loop = { segments[i][1] }
            local at, side = i, 2
            while true do
                used[at] = true
                local p = segments[at][side]
                loop[#loop + 1] = p
                local next_at = nil
                for _, e in ipairs(ends[key(p)] or {}) do
                    if not used[e[1]] then
                        next_at = e
                        break
                    end
                end
                if not next_at then
                    break
                end
                at, side = next_at[1], 3 - next_at[2]
            end
            if #loop > #best then
                best = loop
            end
        end
    end
    if #best < 4 then
        -- Too small to trace: a square round its cells.
        local x0, z0 = pos(minx + 0.5, minz + 0.5)
        local x1, z1 = pos(maxx - 0.5, maxz - 0.5)
        return { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } }
    end
    local tolerance = g.cell * 0.35
    local out = simplify(best, tolerance)
    while #out > 120 do
        tolerance = tolerance * 1.5
        out = simplify(best, tolerance)
    end
    if #out > 2 and out[1][1] == out[#out][1] and out[1][2] == out[#out][2] then
        out[#out] = nil
    end
    return out
end

-- --- Routing ----------------------------------------------------------------------------------------

-- The longest way up to every cell (`up`) and the donor it comes through (`via`), in metres.
local function upstream(g)
    local d, count = g.d, g.n * g.m
    local order = {}
    for i = 1, count do
        order[i] = i
    end
    table.sort(order, function(a, b) return d.area[a] < d.area[b] end)
    local up, via = {}, {}
    for i = 1, count do
        up[i] = 0.0
    end
    for _, i in ipairs(order) do
        local r = d.receiver[i]
        if r > 0 then
            local step = (math.abs((r - i) % g.n) == 0 or math.abs(r - i) == 1) and g.cell or g.cell * 1.41421356
            if up[i] + step > up[r] then
                up[r] = up[i] + step
                via[r] = i
            end
        end
    end
    return up, via
end

-- The cells from `start` down to an outlet, the sea or a lake.
local function trace_down(g, start, in_lake)
    local path, c, seen = { start }, start, {}
    while true do
        seen[c] = true
        local r = g.d.receiver[c]
        if r == 0 or seen[r] then
            break
        end
        path[#path + 1] = r
        if g.d.sea[r] or in_lake(r) then
            break
        end
        c = r
    end
    return path
end

-- The cells from `mouth` up its longest branch, stopping where the drainage falls under `head`.
local function trace_up(g, mouth, up, via, head)
    local path, c = { mouth }, mouth
    while via[c] and g.d.area[via[c]] >= head do
        c = via[c]
        table.insert(path, 1, c)
    end
    return path
end

-- --- Elements ---------------------------------------------------------------------------------------

local RIVER_KEYS = { "points", "source", "kind", "width", "depth", "roughness", "valley", "slope", "grade", "mouth", "name" }
local LAKE_KEYS  = { "points", "at", "radius", "level", "depth", "kind", "islands", "basin", "shore", "outlet", "name" }

-- The kind a river takes from the ground it runs through, when it is not given.
local function river_kind_auto(g, points, levels)
    local fall  = levels[1] - levels[#levels]
    local slope = fall / math.max(1.0, length_of(points))
    local mid   = points[math.max(1, #points // 2)]
    local level = levels[math.max(1, #levels // 2)]
    -- How high the ground stands either side of the middle of the river, 600 m out.
    local relief = 0.0
    for k = 0, 7 do
        local a = k * math.pi / 4.0
        relief = math.max(relief, g.height(mid[1] + math.cos(a) * 600.0, mid[2] + math.sin(a) * 600.0) - level)
    end
    if levels[1] > 1800.0 and relief > 250.0 then
        return "glacial"
    elseif relief > 350.0 and slope < 0.03 then
        return "canyon"
    elseif slope > 0.025 then
        return "mountain"
    elseif slope > 0.006 then
        return "braided"
    end
    return "lowland"
end

local function lake_kind_auto(lake)
    local size = math.sqrt(lake.area)
    if lake.level > 1600.0 then
        return "cirque"
    elseif lake.depth > size * 0.08 then
        return "trench"
    elseif lake.depth < size * 0.02 then
        return "shelf"
    end
    return "bowl"
end

-- A level along the river that never rises, falling at least `slope` per metre, held over `floor`.
local function profile(g, points, slope, floor)
    local levels, s = {}, 0.0
    for i, p in ipairs(points) do
        local ground = g.height(p[1], p[2])
        if i == 1 then
            levels[i] = ground
        else
            local ds = math.sqrt((p[1] - points[i - 1][1]) ^ 2 + (p[2] - points[i - 1][2]) ^ 2)
            levels[i] = math.min(ground, levels[i - 1] - slope * ds)
        end
        if floor then
            levels[i] = math.max(levels[i], floor)
        end
    end
    return levels
end

-- --- The network ------------------------------------------------------------------------------------

local Network = {}
Network.__index = Network

local NETWORK_KEYS = {
    "terrain", "region", "cell", "sea_level", "rivers", "lakes", "generate", "shapes", "seed", "field_cell",
}

function Water.Network(t)
    check("Water.Network", t, NETWORK_KEYS)
    if t.terrain == nil then
        fail("Water.Network needs 'terrain', the forms to look for water in and to carve")
    end
    local region = t.region or { -8000, -8000, 8000, 8000 }
    if type(region) ~= "table" or #region ~= 4 or region[3] <= region[1] or region[4] <= region[2] then
        fail("Water.Network expects region as { min_x, min_z, max_x, max_z }")
    end
    local gen   = t.generate or {}
    local cell  = t.cell or math.max(40.0, math.max(region[3] - region[1], region[4] - region[2]) / 256.0)
    local lake_area = (gen.lakes and gen.lakes.area and gen.lakes.area[1]) or 2e5
    local g = analyse{ terrain = t.terrain, region = region, cell = cell, seed = t.seed or 0,
                       sea_level = t.sea_level, lake_area = lake_area }
    local net = setmetatable({ g = g, sea_level = t.sea_level, rivers = {}, lakes = {}, seed = t.seed or 0,
                               field_cell = t.field_cell }, Network)

    -- Lakes first: which depressions hold water decides where rivers end and begin.
    local natural, label = natural_lakes(g)
    local chosen = {}
    local function add_lake(l)
        l.kind = l.kind or "bowl"
        if not LAKE_KINDS[l.kind] then
            fail("Water.Network: lake kind '" .. tostring(l.kind) .. "' is not one of " .. kind_list(LAKE_KINDS))
        end
        net.lakes[#net.lakes + 1] = l
    end
    local glakes = gen.lakes or {}
    local want = glakes.count
    if want == nil and glakes.density then
        want = math.floor(glakes.density * (region[3] - region[1]) * (region[4] - region[2]) / 1e8 + 0.5)
    end
    if (want or 0) > 0 then
        local area = glakes.area or { 2e5, 1e9 }
        local alt  = glakes.altitude or { -1e9, 1e9 }
        local pool = {}
        for _, l in ipairs(natural) do
            if l.area >= area[1] and l.area <= area[2] and l.level >= alt[1] and l.level <= alt[2] then
                pool[#pool + 1] = l
            end
        end
        table.sort(pool, function(a, b) return a.area * a.depth > b.area * b.depth end)
        for k = 1, math.min(want, #pool) do
            local l = pool[k]
            chosen[l] = true
            local kind = glakes.kinds
            if kind == nil or kind == "auto" then
                kind = lake_kind_auto(l)
            elseif type(kind) == "table" then
                kind = kind[(k - 1) % #kind + 1]
            end
            add_lake{ points = outline_of(g, l, label), level = l.level, depth = math.max(l.depth, 8.0) * 1.6,
                      kind = kind, islands = glakes.islands, generated = true }
        end
    end
    local function in_lake(c)
        return label[c] ~= nil and chosen[label[c]] == true
    end

    -- Drawn lakes: an outline, or a centre and a radius with an irregular shore.
    local function drawn_lake(l, where)
        check(where, l, LAKE_KEYS)
        local points = l.points
        if points == nil then
            if type(l.at) ~= "table" or type(l.radius) ~= "number" then
                fail(where .. " needs 'points', or 'at' and 'radius'")
            end
            points = {}
            for k = 1, 24 do
                local a = (k - 1) / 24 * 2.0 * math.pi
                local r = l.radius * (1.0 + 0.18 * jitter(k, 3) + 0.1 * math.sin(3.0 * a + l.radius))
                points[k] = { l.at[1] + math.cos(a) * r, l.at[2] + math.sin(a) * r }
            end
        end
        local level = l.level
        if level == nil or level == "auto" then
            -- The lowest ground on the shore, so the water reaches the outline.
            level = 1e30
            for _, p in ipairs(points) do
                level = math.min(level, g.height(p[1], p[2]))
            end
        end
        if l.shore ~= nil and SHORES[l.shore] == nil then
            fail(where .. ": shore '" .. tostring(l.shore) .. "' is not one of beach, rock, cliff")
        end
        add_lake{ points = points, level = level, depth = l.depth or 25.0, kind = l.kind or "bowl",
                  islands = l.islands, basin = l.basin, shore = l.shore, outlet = l.outlet, name = l.name }
    end
    for i, l in ipairs(t.lakes or {}) do
        drawn_lake(l, "Water.Network lake " .. i)
    end
    for i, s in ipairs(t.shapes or {}) do
        if s.kind == "lake" and s.geometry == "polygon" then
            drawn_lake({ points = s.points, level = s.level, depth = s.depth, kind = s.lake_kind,
                         islands = s.islands, shore = s.shore, outlet = s.outlet, name = s.name }, "shape " .. i)
        end
    end

    -- The rivers made so far, their points in buckets of 200 m, so a new river can find the first one it
    -- touches without testing every point of every river.
    local buckets = {}
    local BUCKET = 200.0
    local function key(x, z)
        return math.floor(x / BUCKET) * 100003 + math.floor(z / BUCKET)
    end
    local function half_at(river, i)
        return river.widths[math.max(1, math.min(#river.widths, i))] * 0.5
    end
    local widest = 0.0
    local function register(river)
        for _, wd in ipairs(river.widths) do
            widest = math.max(widest, wd * 0.5)
        end
        for i, p in ipairs(river.points) do
            local kk = key(p[1], p[2])
            buckets[kk] = buckets[kk] or {}
            table.insert(buckets[kk], { river = river, index = i })
        end
    end
    -- The first point of `path` (past its first few widths, so a river may rise beside another) whose
    -- channel touches a river already made: { at, river, index, run }.
    local function contact(path, w, ignore, ignore_run)
        local run = 0.0
        for i, p in ipairs(path) do
            if i > 1 then
                run = run + math.sqrt((p[1] - path[i - 1][1]) ^ 2 + (p[2] - path[i - 1][2]) ^ 2)
            end
            if run > w * 3.0 then
                local bx, bz = math.floor(p[1] / BUCKET), math.floor(p[2] / BUCKET)
                local best, found = 1e30, nil
                -- As many buckets out as the widest river made so far can reach.
                local around = math.ceil((w * 0.5 + widest + 4.0) / BUCKET)
                for dx = -around, around do
                    for dz = -around, around do
                        for _, e in ipairs(buckets[(bx + dx) * 100003 + bz + dz] or {}) do
                            if e.river == ignore and run < (ignore_run or math.huge) then
                                goto next
                            end
                            local q = e.river.points[e.index]
                            local d = math.sqrt((q[1] - p[1]) ^ 2 + (q[2] - p[2]) ^ 2)
                            if d < w * 0.5 + half_at(e.river, e.index) + 4.0 and d < best then
                                best, found = d, e
                            end
                            ::next::
                        end
                    end
                end
                if found then
                    return { at = i, river = found.river, index = found.index, run = run }
                end
            end
        end
        return nil
    end
    -- A river cut down to `level` from point `from` to its mouth, its axis with it, the cut eased in over
    -- a few hundred metres upstream: a knickpoint that has started to retreat, not a step.
    local lower
    function lower(river, from, level)
        local ease = math.max(1, math.floor(300.0 / math.max(1.0, length_of(river.points) / #river.points)))
        local top = river.levels[math.max(1, from - ease)]
        for i = math.max(1, from - ease), from - 1 do
            local t = (from - i) / ease
            river.levels[i] = math.min(river.levels[i], level + (top - level) * t)
        end
        for i = from, #river.levels do
            river.levels[i] = math.min(river.levels[i], level)
        end
        local start = math.max(1, math.floor(#river.axis_levels * (from - 1) / #river.levels))
        for i = start, #river.axis_levels do
            river.axis_levels[i] = math.min(river.axis_levels[i], level)
        end
        -- Its mouth is now lower, so the river it flows into is cut down too, and so on to the sea.
        if river.receiver and river.levels[#river.levels] < river.receiver.levels[river.join] then
            lower(river.receiver, river.join, river.levels[#river.levels])
        end
    end

    -- Rivers. Each is a list of points with a level, plus its look.
    local function add_river(r, points, where)
        if #points < 2 then
            return
        end
        local width = r.width or 30.0
        if type(width) == "number" then
            width = { width * 0.35, width }
        end
        local floor = net.sea_level
        local ends_in = r.ends_in_lake
        if ends_in then
            floor = ends_in.level
        end
        local slope = r.slope or 0.0008
        local coarse = profile(g, points, slope, floor)
        local kind = r.kind
        if kind == nil or kind == "auto" then
            kind = river_kind_auto(g, points, coarse)
        end
        local k = RIVER_KINDS[kind]
        if k == nil then
            fail(where .. ": river kind '" .. tostring(kind) .. "' is not one of " .. kind_list(RIVER_KINDS))
        end
        local widen = k.widen or 1.0
        local w = (width[1] + width[2]) * 0.5 * widen
        local roughness = r.roughness or 0.5
        local path = resample(smooth(points, 3), math.max(12.0, w * 0.6))
        -- The meanders swing further the rougher the river, and their wavelength, along the channel, is
        -- about eleven widths stretched by the swing (Leopold and Wolman).
        -- Capped under 1.55 rad: past about two radians the loops of a sine-generated curve cross.
        local omega = math.min(1.55, k.omega * (0.4 + 1.2 * roughness))
        path = meander(path, w * 11.0 * (1.0 + 0.3 * omega * omega), omega, #net.rivers + 1)
        path = resample(path, math.max(10.0, w * 0.5))
        local n = #path
        local runs = { 0.0 }
        for i = 2, n do
            runs[i] = runs[i - 1] + math.sqrt((path[i][1] - path[i - 1][1]) ^ 2 + (path[i][2] - path[i - 1][2]) ^ 2)
        end
        local total = runs[n]
        local ds = total / math.max(1, n - 1)
        local levels = profile(g, path, slope, floor)

        -- Falls: where the ground the river follows drops by 25 m or more within a cell and a half of the
        -- sampled terrain, at a slope of 45 % and three times the river's own, a cliff it falls over. Only
        -- the kinds that keep their steps have them (`falls`): a canyon cuts back through its cliff instead. Each fall keeps its step: the grading and the smoothing below work on the stretches
        -- between falls, which would otherwise spread a waterfall into a long rapid.
        local falls = {}
        if k.falls then
            local span = math.max(1, math.ceil(g.cell * 1.5 / ds))
            local steep = math.max(0.45, 3.0 * (levels[1] - levels[n]) / math.max(1.0, total))
            local function falls_at(j)
                local drop = levels[j] - levels[j + span]
                return drop > math.max(25.0, steep * span * ds), drop
            end
            local i = 1
            while i + span <= n do
                local is_fall, drop = falls_at(i)
                if is_fall then
                    -- The step goes where the drop over the window is largest.
                    local best, at = drop, i
                    local j = i + 1
                    while j + span <= n and falls_at(j) do
                        if levels[j] - levels[j + span] > best then
                            best, at = levels[j] - levels[j + span], j
                        end
                        j = j + 1
                    end
                    local bottom = levels[at + span]
                    for m = at + 1, at + span do
                        levels[m] = bottom
                    end
                    falls[#falls + 1] = at
                    i = at + span + 1
                else
                    i = i + 1
                end
            end
        end
        -- The stretches between falls, each graded and smoothed on its own. The graded profile, concave
        -- between the stretch's ends (`grade` its power), holds the river under the ground it crosses,
        -- which is what makes a canyon of a plateau and a valley of a hillside. The smoothing, over about
        -- 300 m and twice, takes out the kink the 100 m cells of the sampled terrain leave at every cell;
        -- a moving average of a sequence that never rises never rises either.
        local reach = math.max(1, math.floor(300.0 / math.max(1.0, ds)))
        local stretches, from = {}, 1
        for _, f in ipairs(falls) do
            stretches[#stretches + 1] = { from, f }
            from = f + 1
        end
        stretches[#stretches + 1] = { from, n }
        for _, st in ipairs(stretches) do
            local a, b = st[1], st[2]
            if b > a then
                local top, bottom = levels[a], levels[b]
                local length = runs[b] - runs[a]
                for i = a + 1, b do
                    local graded = bottom + (top - bottom) * (1.0 - (runs[i] - runs[a]) / math.max(1.0, length)) ^ (r.grade or k.grade)
                    levels[i] = math.min(levels[i], graded)
                end
                for _ = 1, 2 do
                    local sums = { [a - 1] = 0.0 }
                    for i = a, b do
                        sums[i] = sums[i - 1] + levels[i]
                    end
                    local smoothed = {}
                    for i = a, b do
                        local half_window = math.min(reach, i - a, b - i)
                        smoothed[i] = (sums[i + half_window] - sums[i - half_window - 1]) / (2 * half_window + 1)
                    end
                    for i = a, b do
                        levels[i] = smoothed[i]
                    end
                end
            end
        end
        if r.start_level then
            for i = 1, n do
                levels[i] = math.min(levels[i], r.start_level)
            end
        end

        -- Width at every point: from the drainage area where the river was found by its drainage (ten
        -- metres at 1 km2, as the square root of the area), otherwise from source to mouth as given.
        local base = {}
        local areas = r.areas
        for i = 1, n do
            local share = runs[i] / math.max(1.0, total)
            if areas and #areas > 1 then
                local at = math.max(1, math.min(#areas, math.floor(share * (#areas - 1) + 1.5)))
                local range = r.width_range or { 4.0, 120.0 }
                base[i] = math.max(range[1], math.min(range[2], 10.0 * math.sqrt(areas[at] / 1e6)))
            else
                base[i] = width[1] + (width[2] - width[1]) * share
            end
        end
        -- How deep the bed is, as a share of what the width gives: deeper in the plunge pool below a fall
        -- (two and a half times, fading over a few widths), shallower over the rapids above one.
        local deep = {}
        for i = 1, n do
            deep[i] = 1.0
        end
        for _, f in ipairs(falls) do
            for i = f + 1, n do
                local d = runs[i] - runs[f]
                if d > base[f] * widen * 6.0 then
                    break
                end
                deep[i] = math.max(deep[i], 1.0 + 1.5 * math.exp(-d / (base[f] * widen * 2.0)))
            end
            for i = f, 1, -1 do
                local d = runs[f] - runs[i]
                if d > 250.0 then
                    break
                end
                deep[i] = math.min(deep[i], 0.6 + 0.4 * d / 250.0)
            end
        end

        -- Where it meets a river already in the network it ends, on that river's axis: the network is a
        -- tree, and a confluence has one level. No level is raised to get there. A river arriving lower
        -- than the one it joins cuts that one down from the confluence on (its level falls there anyway,
        -- so nothing downstream rises); one arriving higher drops to it over its last stretch. Below the
        -- confluence the river it joins carries both, and is wider: widths add as the square root of the
        -- areas they drain.
        local hit = contact(path, w, r.parent, r.parent_run)
        local receiver = nil
        if hit then
            if r.generated and hit.run < (r.min_keep or 1500.0) then
                return nil -- it would only duplicate the river it joins so soon
            end
            if r.parent and hit.river == r.parent then
                return nil -- a distributary that would rejoin its own river is not one
            end
            local B, q = hit.river, hit.index
            for i = n, hit.at + 1, -1 do
                path[i], levels[i], base[i], deep[i] = nil, nil, nil, nil
            end
            n = #path
            local here = levels[n]
            if here < B.levels[q] then
                -- Fully along the stretch the tributary enters by, which for a wide river reaches well
                -- upstream of its nearest point: eased in there, it would meet water still higher.
                local step = length_of(B.points) / math.max(1, #B.points - 1)
                lower(B, math.max(1, q - math.ceil((half_at(B, q) * 2.0 + w) / math.max(1.0, step))), here)
            else
                local m = math.max(1, n - 8)
                for i = m, n do
                    local t = (i - m) / math.max(1, n - m)
                    levels[i] = math.min(levels[i], levels[m] + (B.levels[q] - levels[m]) * t)
                end
            end
            for i = q, #B.base do
                B.base[i] = math.sqrt(B.base[i] ^ 2 + base[n] ^ 2)
                B.widths[i] = B.base[i] * B.widen
            end
            path[n + 1] = { B.points[q][1], B.points[q][2] }
            levels[n + 1] = math.min(levels[n], B.levels[q])
            base[n + 1], deep[n + 1] = base[n], deep[n]
            n = n + 1
            receiver = B
            total = length_of(path)
        end

        -- The bank asymmetry: in a bend the deepest line runs against the outer bank, which is cut steep,
        -- and the bed rises gently to a point bar on the inner one. Read from the curvature, averaged over
        -- a width; `skew` is where across the channel the deepest line runs, on the side the shader calls
        -- negative when the river turns towards the positive one.
        local turn = { 0.0 }
        for i = 2, n - 1 do
            local ax, az = path[i][1] - path[i - 1][1], path[i][2] - path[i - 1][2]
            local bx, bz = path[i + 1][1] - path[i][1], path[i + 1][2] - path[i][2]
            local la = math.max(1e-6, math.sqrt(ax * ax + az * az))
            local lb = math.max(1e-6, math.sqrt(bx * bx + bz * bz))
            local cross = (ax * bz - az * bx) / (la * lb)
            turn[i] = math.asin(math.max(-1.0, math.min(1.0, cross))) / ((la + lb) * 0.5)
        end
        turn[n] = 0.0
        local skews = {}
        local across = math.max(1, math.floor(w / math.max(1.0, ds)))
        for i = 1, n do
            local sum, count = 0.0, 0
            for j = math.max(1, i - across), math.min(n, i + across) do
                sum, count = sum + turn[j], count + 1
            end
            skews[i] = math.max(-0.6, math.min(0.6, -(sum / count) * w * 3.0))
        end

        -- The valley's axis: the course without its meanders, averaged over a few hundred metres, with the
        -- levels carried over by the share of the length run. The valley floor takes its level from here.
        local axis = resample(path, 50.0)
        local radius = math.max(1, math.floor(math.max((r.valley or k.valley) * 0.4, w * 11.0) / 50.0))
        local averaged = {}
        for i = 1, #axis do
            local a, b = math.max(1, i - radius), math.min(#axis, i + radius)
            local span = math.min(i - a, b - i)
            local sx, sz = 0.0, 0.0
            for j = i - span, i + span do
                sx, sz = sx + axis[j][1], sz + axis[j][2]
            end
            averaged[i] = { sx / (2 * span + 1), sz / (2 * span + 1) }
        end
        runs = { 0.0 }
        for i = 2, n do
            runs[i] = runs[i - 1] + math.sqrt((path[i][1] - path[i - 1][1]) ^ 2 + (path[i][2] - path[i - 1][2]) ^ 2)
        end
        local run2 = runs[n]
        local axis_levels, at, total_axis, run_axis = {}, 1, length_of(averaged), 0.0
        for i = 1, #averaged do
            if i > 1 then
                run_axis = run_axis + math.sqrt((averaged[i][1] - averaged[i - 1][1]) ^ 2 + (averaged[i][2] - averaged[i - 1][2]) ^ 2)
            end
            local want = run_axis / math.max(1.0, total_axis) * run2
            while at < n and runs[at + 1] < want do
                at = at + 1
            end
            local nxt = math.min(n, at + 1)
            local t = nxt > at and (want - runs[at]) / math.max(1e-6, runs[nxt] - runs[at]) or 0.0
            axis_levels[i] = levels[at] + (levels[nxt] - levels[at]) * math.max(0.0, math.min(1.0, t))
        end
        local widths = {}
        for i = 1, n do
            widths[i] = base[i] * widen
        end
        net.rivers[#net.rivers + 1] = {
            axis = averaged, axis_levels = axis_levels,
            points = path, levels = levels, kind = kind, base = base, widths = widths, widen = widen,
            width = { widths[1], widths[n] }, deep = deep, skews = skews, falls = #falls,
            depth = r.depth, roughness = roughness, valley = r.valley or k.valley,
            name = r.name, generated = r.generated, receiver = receiver, join = hit and hit.index or nil,
            mouth = r.mouth, distributary = r.distributary, lakes = r.lakes,
        }
        register(net.rivers[#net.rivers])
        return net.rivers[#net.rivers]
    end

    -- A run of cells becomes rivers split at the lakes it crosses: the part above a lake ends in it, the
    -- part below starts at its level.
    -- A generated river carries the drainage area of every cell, which its width follows.
    local function rivers_from_cells(r, cells, where)
        local run, entered, areas = {}, nil, {}
        local made = {}
        for _, c in ipairs(cells) do
            local lake = in_lake(c) and label[c] or nil
            run[#run + 1] = g.at(c)
            areas[#areas + 1] = g.d.area[c]
            if lake then
                if #run >= 3 then
                    local copy = {}
                    for key, v in pairs(r) do
                        copy[key] = v
                    end
                    copy.ends_in_lake = lake
                    copy.start_level = entered and entered.level or r.start_level
                    copy.areas = r.generated and areas or nil
                    made[#made + 1] = add_river(copy, run, where)
                end
                run, entered, areas = { g.at(c) }, lake, { g.d.area[c] }
            end
        end
        if #run >= 3 then
            local copy = {}
            for key, v in pairs(r) do
                copy[key] = v
            end
            copy.start_level = entered and entered.level or r.start_level
            copy.areas = r.generated and areas or nil
            made[#made + 1] = add_river(copy, run, where)
        end
        return made
    end

    local function drawn_river(r, where)
        check(where, r, RIVER_KEYS)
        if r.points and #r.points >= 2 then
            add_river(r, r.points, where)
        elseif r.source or (r.points and #r.points == 1) then
            local s = r.source or r.points[1]
            local cells = trace_down(g, g.index(s[1], s[2]), in_lake)
            local copy = {}
            for key, v in pairs(r) do
                copy[key] = v
            end
            copy.points, copy.source = nil, nil
            rivers_from_cells(copy, cells, where)
        else
            fail(where .. " needs 'points' (the river drawn) or 'source' (where it rises)")
        end
    end
    for i, r in ipairs(t.rivers or {}) do
        drawn_river(r, "Water.Network river " .. i)
    end
    for i, s in ipairs(t.shapes or {}) do
        if s.kind == "river" and (s.geometry == "line" or s.geometry == "point") then
            drawn_river({ points = s.points, kind = s.river_kind, width = s.width, depth = s.depth,
                          roughness = s.roughness, name = s.name }, "shape " .. i)
        end
    end
    -- Outlets: a lake with `outlet = true` (or a width in metres) spills where its shore is lowest, and a
    -- river leaves it there at its level, traced down the terrain.
    for _, l in ipairs(net.lakes) do
        if l.outlet then
            local lowest, at = math.huge, nil
            local cx, cz = 0.0, 0.0
            for _, p in ipairs(l.points) do
                cx, cz = cx + p[1] / #l.points, cz + p[2] / #l.points
            end
            for _, p in ipairs(l.points) do
                local h = g.height(p[1], p[2])
                if h < lowest then
                    lowest, at = h, p
                end
            end
            local dx, dz = at[1] - cx, at[2] - cz
            local dl = math.max(1.0, math.sqrt(dx * dx + dz * dz))
            drawn_river({ source = { at[1] + dx / dl * 80.0, at[2] + dz / dl * 80.0 },
                          width = type(l.outlet) == "number" and l.outlet or 12.0 },
                        "the outlet of a lake")
            local r = net.rivers[#net.rivers]
            if r then
                for i = 1, #r.levels do
                    r.levels[i] = math.min(r.levels[i], l.level)
                end
                for i = 1, #r.axis_levels do
                    r.axis_levels[i] = math.min(r.axis_levels[i], l.level)
                end
            end
        end
    end

    -- Generated rivers: the longest courses to distinct mouths, then tributaries along them.
    local griv = gen.rivers or {}
    local count = griv.count
    if count == nil and griv.density then
        count = math.floor(griv.density * (region[3] - region[1]) * (region[4] - region[2]) / 1e8 + 0.5)
    end
    if (count or 0) > 0 then
        local opts  = gen.river or {}
        local up, via = upstream(g)
        local head  = opts.head_area or 1.5e6
        local min_length = opts.min_length or 2500.0
        local spacing = opts.spacing or 2000.0
        local mouths = {}
        for i = 1, g.n * g.m do
            local r = g.d.receiver[i]
            local ends = r == 0 or g.d.sea[r] or in_lake(r)
            if ends and not g.d.sea[i] and not in_lake(i) and g.d.area[i] >= head * 4.0 and up[i] >= min_length then
                mouths[#mouths + 1] = i
            end
        end
        table.sort(mouths, function(a, b) return up[a] * math.sqrt(g.d.area[a]) > up[b] * math.sqrt(g.d.area[b]) end)
        local picked = {}
        local on_stem = {}
        for _, mth in ipairs(mouths) do
            if #picked >= count then
                break
            end
            local p, far = g.at(mth), true
            for _, q in ipairs(picked) do
                local o = g.at(q)
                if (p[1] - o[1]) ^ 2 + (p[2] - o[2]) ^ 2 < spacing * spacing then
                    far = false
                end
            end
            if far then
                picked[#picked + 1] = mth
            end
        end
        local width = opts.width or { 6.0, 80.0 }
        local function width_for(area)
            local w = 10.0 * math.sqrt(area / 1e6)
            return math.max(width[1], math.min(width[2], w))
        end
        local kinds = opts.kinds
        for k, mth in ipairs(picked) do
            local cells = trace_up(g, mth, up, via, head)
            for _, c in ipairs(cells) do
                on_stem[c] = k
            end
            local kind = kinds
            if type(kinds) == "table" then
                kind = kinds[(k - 1) % #kinds + 1]
            end
            rivers_from_cells({ kind = kind, width = width_for(g.d.area[mth]), width_range = width, depth = opts.depth,
                                             roughness = opts.roughness, generated = true }, cells, "generated river")
            -- Tributaries: the largest side branches long enough, spread along the stem.
            local tributaries = gen.tributaries or 0.0
            if tributaries > 0.0 then
                local side = {}
                for i = 1, g.n * g.m do
                    local r = g.d.receiver[i]
                    if r > 0 and on_stem[r] == k and on_stem[i] == nil and g.d.area[i] >= head * 2.0
                        and up[i] >= min_length * 0.5 then
                        side[#side + 1] = i
                    end
                end
                table.sort(side, function(a, b) return g.d.area[a] > g.d.area[b] end)
                local want_t = math.floor(tributaries * #cells * g.cell / 3000.0 + 0.5)
                local taken = {}
                for _, i in ipairs(side) do
                    if #taken >= want_t then
                        break
                    end
                    local p, far = g.at(i), true
                    for _, q in ipairs(taken) do
                        local o = g.at(q)
                        if (p[1] - o[1]) ^ 2 + (p[2] - o[2]) ^ 2 < (spacing * 0.6) ^ 2 then
                            far = false
                        end
                    end
                    if far and not in_lake(i) then
                        taken[#taken + 1] = i
                        local tcells = trace_up(g, i, up, via, head)
                        tcells[#tcells + 1] = g.d.receiver[i]
                        -- It ends where it meets the stem (see `add_river`), at the stem's level there.
                        local pts, tareas = {}, {}
                        for _, c in ipairs(tcells) do
                            pts[#pts + 1], tareas[#tareas + 1] = g.at(c), g.d.area[c]
                        end
                        add_river({ kind = kind, width = width_for(g.d.area[i]) * 0.8, depth = opts.depth,
                                    roughness = opts.roughness, generated = true, min_keep = 400.0,
                                    areas = tareas, width_range = width }, pts, "generated tributary")
                    end
                end
            end
        end
    end

    -- A valley dammed behind a sill: a lake on river `r` whose downstream end is at `share` of its course,
    -- `size` metres long, its water at the river's level there plus a few metres and the river held at that
    -- level through it. A glacial river's are dammed by moraines.
    local function dam(r, share, size, kind)
        local i_end = math.max(3, math.floor(#r.points * share))
        local i_mid = i_end
        local s = 0.0
        while i_mid > 2 and s < size * 0.5 do
            s = s + math.sqrt((r.points[i_mid][1] - r.points[i_mid - 1][1]) ^ 2 + (r.points[i_mid][2] - r.points[i_mid - 1][2]) ^ 2)
            i_mid = i_mid - 1
        end
        local a, b = r.points[math.max(1, i_mid - 3)], r.points[math.min(#r.points, i_mid + 3)]
        local ax, az = b[1] - a[1], b[2] - a[2]
        local al = math.max(1e-6, math.sqrt(ax * ax + az * az))
        ax, az = ax / al, az / al
        local c = r.points[i_mid]
        local level = r.levels[i_end] + 6.0
        local outline = {}
        for k = 1, 20 do
            local t = (k - 1) / 20 * 2.0 * math.pi
            local along = math.cos(t) * size * 0.5
            local across = math.sin(t) * size * 0.2 * (1.0 + 0.25 * jitter(k, 9 + #net.lakes))
            outline[k] = { c[1] + ax * along - az * across, c[2] + az * along + ax * across }
        end
        add_lake{ points = outline, level = level, depth = 30.0, kind = kind or "bowl", generated = true, dam = true,
                  downhill = { ax, az } }
        for i = 1, math.min(#r.levels, i_end + 2) do
            r.levels[i] = math.max(r.levels[i], level)
        end
        for i = 1, math.floor(#r.axis_levels * (i_end + 2) / #r.levels) do
            r.axis_levels[i] = math.max(r.axis_levels[i], level)
        end
    end
    local on_rivers = (gen.lakes and gen.lakes.on_rivers) or 0
    local placed = 0
    for _, r in ipairs(net.rivers) do
        if placed >= on_rivers then
            break
        end
        local length = length_of(r.points)
        if r.generated and r.kind ~= "glacial" and length > 4000.0 then
            dam(r, 0.66, math.max(500.0, math.min(2500.0, length * 0.14)))
            placed = placed + 1
        end
    end
    -- Paternoster lakes: a glacial valley's string of rock basins and moraine-dammed lakes, two by default,
    -- `lakes` on a drawn river for another number. From the mouth up, so each lake holds the river over the
    -- next one down.
    for _, r in ipairs(net.rivers) do
        local count = r.lakes or (r.kind == "glacial" and 2 or 0)
        local length = length_of(r.points)
        if count > 0 and length > 4000.0 then
            for k = 1, count do
                dam(r, 0.85 - 0.6 * (k - 1) / math.max(1, count), math.max(400.0, math.min(1800.0, length * 0.09)), "moraine")
            end
        end
    end
    -- Mouths. A river that ends in the sea or a lake, not in another river, spreads there: into a delta
    -- (two or three distributaries fanning out from about 25 widths up, over a fan of land a metre and a half
    -- over the water) where the water is calm, a lake, or a lowland or braided river at the sea; into an
    -- estuary (its last stretch widening as a funnel to four times and deepening) where it meets the sea
    -- otherwise. `mouth = "delta" | "estuary" | "none"` on a drawn river chooses.
    local function inside(points, x, z)
        local hit = false
        for i = 1, #points do
            local a, b = points[i], points[i % #points + 1]
            if (a[2] > z) ~= (b[2] > z) and x < a[1] + (z - a[2]) * (b[1] - a[1]) / (b[2] - a[2]) then
                hit = not hit
            end
        end
        return hit
    end
    local mouths = {}
    for _, r in ipairs(net.rivers) do
        if r.receiver == nil and not r.distributary then
            mouths[#mouths + 1] = r
        end
    end
    -- A river cut back to its first `keep` points, taken out of the buckets past them.
    local function cut(river, keep)
        local old = #river.points
        for i = old, keep + 1, -1 do
            local p = river.points[i]
            local list = buckets[key(p[1], p[2])]
            if list then
                for j = #list, 1, -1 do
                    if list[j].river == river and list[j].index > keep then
                        table.remove(list, j)
                    end
                end
            end
            river.points[i], river.levels[i], river.base[i], river.widths[i] = nil, nil, nil, nil
            river.deep[i], river.skews[i] = nil, nil
        end
        local axes = math.max(2, math.ceil(#river.axis * keep / old))
        for i = #river.axis, axes + 1, -1 do
            river.axis[i], river.axis_levels[i] = nil, nil
        end
        river.width = { river.widths[1], river.widths[keep] }
    end
    for _, r in ipairs(mouths) do
        -- The mouth is where the river first reaches the sea or a lake, not where its course was drawn to.
        local n = #r.points
        local into_lake, at_sea = nil, false
        for i = 1, n do
            local p = r.points[i]
            for _, l in ipairs(net.lakes) do
                if l.kind ~= "fan" and inside(l.points, p[1], p[2]) then
                    into_lake = l
                end
            end
            at_sea = net.sea_level ~= nil and g.d.sea[g.index(p[1], p[2])]
            if into_lake or at_sea then
                n = i
                break
            end
        end
        local e = r.points[n]
        local mouth = r.mouth
        if mouth == nil or mouth == "auto" then
            if into_lake or (at_sea and (r.kind == "lowland" or r.kind == "braided")) then
                mouth = "delta"
            elseif at_sea then
                mouth = "estuary"
            else
                mouth = "none"
            end
        end
        if not (into_lake or at_sea) then
            mouth = "none"
        end
        local runs = { [n] = 0.0 }
        for i = n - 1, 1, -1 do
            runs[i] = runs[i + 1] + math.sqrt((r.points[i + 1][1] - r.points[i][1]) ^ 2 + (r.points[i + 1][2] - r.points[i][2]) ^ 2)
        end
        local w = r.widths[n]
        local water = into_lake and into_lake.level or net.sea_level or r.levels[n]
        if mouth == "estuary" then
            local reach = math.min(3000.0, runs[1] * 0.3)
            for i = 1, #r.points do
                local d = i <= n and runs[i] or 0.0
                if d < reach then
                    local t = 1.0 - d / reach
                    r.base[i] = r.base[i] * (1.0 + 3.0 * t * t)
                    r.widths[i] = r.base[i] * r.widen
                    r.deep[i] = r.deep[i] * (1.0 + 0.5 * t)
                end
            end
        elseif mouth == "delta" and runs[1] > 1500.0 then
            local reach = math.max(600.0, math.min(2500.0, w * 25.0))
            if into_lake then
                -- A delta in a lake stays in the lake: no wider than half of it.
                local minx, maxx, minz, maxz = 1e30, -1e30, 1e30, -1e30
                for _, p in ipairs(into_lake.points) do
                    minx, maxx = math.min(minx, p[1]), math.max(maxx, p[1])
                    minz, maxz = math.min(minz, p[2]), math.max(maxz, p[2])
                end
                reach = math.min(reach, 0.5 * math.min(maxx - minx, maxz - minz))
            end
            -- Where it splits: `reach` up from the mouth. The river ends there and all its water goes on in
            -- its distributaries, one ahead and two fanning out.
            local i0 = 1
            for i = n, 1, -1 do
                if runs[i] >= reach then
                    i0 = i
                    break
                end
            end
            local p0 = { r.points[i0][1], r.points[i0][2] }
            local dx, dz = e[1] - p0[1], e[2] - p0[2]
            local dl = math.max(1.0, math.sqrt(dx * dx + dz * dz))
            dx, dz = dx / dl, dz / dl
            local level0, base0 = r.levels[i0], r.base[i0]
            cut(r, i0)
            -- The fan: land round the mouth from its apex at the split, a lobe pushed out by each distributary
            -- (ahead and 32 degrees either side), as a delta builds where its channels carry the sediment.
            local fan = { { p0[1], p0[2] } }
            for k = 0, 24 do
                local a = (k / 24 - 0.5) * math.rad(130.0)
                local cx, cz = dx * math.cos(a) - dz * math.sin(a), dx * math.sin(a) + dz * math.cos(a)
                local lobes = 0.0
                for _, lobe in ipairs({ 0.0, 32.0, -32.0 }) do
                    lobes = lobes + math.exp(-((a - math.rad(lobe)) / 0.22) ^ 2)
                end
                local rr = reach * (0.8 + 0.6 * lobes + 0.08 * jitter(k, #net.lakes))
                fan[#fan + 1] = { p0[1] + cx * rr, p0[2] + cz * rr }
            end
            add_lake{ points = fan, level = water, depth = 1.0, kind = "fan", generated = true }
            for b, angle in ipairs({ 0.0, 32.0, -32.0 }) do
                local a = math.rad(angle + 6.0 * jitter(b, n))
                local cx, cz = dx * math.cos(a) - dz * math.sin(a), dx * math.sin(a) + dz * math.cos(a)
                local side = angle >= 0.0 and 1.0 or -1.0
                local pts = {}
                for k = 0, 8 do
                    local t = k / 8
                    -- A slight bend away from the middle as it goes.
                    local bend = angle == 0.0 and 0.0 or side * 0.2 * t * t
                    local bx, bz = cx * math.cos(bend) - cz * math.sin(bend), cx * math.sin(bend) + cz * math.cos(bend)
                    pts[#pts + 1] = { p0[1] + bx * reach * 1.3 * t, p0[2] + bz * reach * 1.3 * t }
                end
                local made = add_river({ kind = "lowland", width = { base0 * (b == 1 and 0.55 or 0.4), base0 * (b == 1 and 0.5 or 0.35) },
                                         roughness = 0.35, start_level = level0, distributary = true, parent = r,
                                         parent_run = reach * 0.25, ends_in_lake = into_lake }, pts, "a distributary")
                if made then
                    made.distributary = true
                end
            end
        end
    end

    -- Each river costs the graph three nodes (five with a floodplain) and each lake five, out of 1024 for
    -- the whole script; past what fits, say so here rather than with the graph's own error halfway through.
    local cost = 0
    for _, r in ipairs(net.rivers) do
        cost = cost + ((r.kind == "lowland" or r.kind == "braided") and 5 or 3)
    end
    cost = cost + 5 * #net.lakes
    if cost > 820 then
        fail("Water.Network has " .. #net.rivers .. " rivers and " .. #net.lakes .. " lakes, about " .. cost
            .. " graph nodes of the 1024 a script has; generate fewer (count, density, tributaries)")
    end
    return net
end

-- --- Shaping the terrain ----------------------------------------------------------------------------

function Network:apply(height, opts)
    if height == nil then
        fail("net:apply needs the terrain to shape")
    end
    opts = opts or {}
    local position = Terrain.Coords{}
    local ground = height
    local lake_water, river_water = nil, nil
    local function max_of(a, b)
        if a == nil or b == nil then
            return a or b
        end
        return Combine.Max{ a = a, b = b }
    end

    -- Lakes: the floor under the level, a low rim around it, and the ground around falling to it. Each
    -- is one block and two `Hydrology.Carve` nodes (see water.comp.glsl, where the shapes are).
    -- The grid buffer holds 16 M values for the whole script: lakes get 2.5 M of it, rivers 10 M.
    local lake_budget = 2.5e6
    for i, l in ipairs(self.lakes) do
        local k = LAKE_KINDS[l.kind]
        local xs, zs = {}, {}
        local shore = #l.points <= 64 and smooth_closed(l.points, 2) or l.points
        for j, p in ipairs(shore) do
            xs[j], zs[j] = p[1], p[2]
        end
        local size = math.sqrt(math.abs(length_of(l.points) ^ 2 / (4.0 * math.pi)))
        local basin_reach = (l.basin or 1.0) * math.max(300.0, size * 0.8)
        local cell = math.max(8.0, math.min(40.0, size / 60.0))
        -- Inside the lakes' share of the grid buffer: four values a cell over the outline grown by the basin.
        local minx, minz, maxx, maxz = 1e30, 1e30, -1e30, -1e30
        for j = 1, #xs do
            minx, maxx = math.min(minx, xs[j]), math.max(maxx, xs[j])
            minz, maxz = math.min(minz, zs[j]), math.max(maxz, zs[j])
        end
        local span = function(c) return ((maxx - minx + 2 * (basin_reach + 4 * c)) / c) * ((maxz - minz + 2 * (basin_reach + 4 * c)) / c) * 4.0 end
        local share = lake_budget / math.max(1, #self.lakes - i + 1)
        while span(cell) > share do
            cell = cell * 1.25
        end
        lake_budget = lake_budget - span(cell)
        -- Which way is uphill: along a dammed valley, against its flow; otherwise from the ground around.
        local cx, cz = 0.0, 0.0
        for _, p in ipairs(l.points) do
            cx, cz = cx + p[1] / #l.points, cz + p[2] / #l.points
        end
        local ux, uz
        if l.downhill then
            ux, uz = -l.downhill[1], -l.downhill[2]
        else
            local g, e = self.g, math.max(200.0, size * 0.5)
            ux = g.height(cx + e, cz) - g.height(cx - e, cz)
            uz = g.height(cx, cz + e) - g.height(cx, cz - e)
            local ul = math.max(1e-6, math.sqrt(ux * ux + uz * uz))
            ux, uz = ux / ul, uz / ul
        end
        local basin = math.tan(math.rad(l.shore and SHORES[l.shore] or k.basin))
        local block = Hydrology.LakeBlock{
            xs = xs, zs = zs, cell = cell, margin = basin_reach + 4.0 * cell,
            params = { LAKE_CODES[l.kind], l.level, l.depth, size, k.wall, k.shelf or 0.0, k.shelf_depth or 0.0,
                       basin, basin_reach, l.islands or 0.0, k.rim or 0.0 },
            basin = basin, headwall = k.headwall or 0.0, moraine = k.moraine or 0.0,
            uphill = { ux, uz }, centre = { cx, cz },
        }
        -- The bed is not smooth: ridges and hollows under the water, and islands where they rise above it.
        local bed = Noises.Ridged{ frequency = 1.0 / math.max(150.0, size * 0.25), octaves = 3,
                                   amplitude = 0.5, seed = self.seed + 400 + i }.value
        if l.kind ~= "fan" then
            local cap = Hydrology.Carve{ block = block, mode = "lake", aux = bed, position = position }
            ground = Combine.Min{ a = ground, b = cap }
            lake_water = max_of(lake_water, cap.water)
        end
        ground = Combine.Max{ a = ground, b = Hydrology.Carve{ block = block, mode = "rim", aux = bed, position = position } }
    end

    -- Rivers: the channel under the level, the valley as an envelope over it, one block each.
    local budget = 10e6
    -- One field of spurs for every valley side, shared: one node, not one per river.
    local spurs = Noises.fBm{ frequency = 1.0 / 600.0, octaves = 4, amplitude = 0.35, seed = self.seed + 700 }.value + 1.0
    -- Floodplains raise ground and channels lower it; all the raising first, then all the lowering, so a
    -- floodplain never fills another river's channel that runs within it.
    local before, fills, caps = ground, {}, {}
    for i, r in ipairs(self.rivers) do
        local w1 = 0.0
        for _, wd in ipairs(r.widths) do
            w1 = math.max(w1, wd)
        end
        local valley = r.valley
        local k = RIVER_KINDS[r.kind]
        -- Cells at the valley's scale: the channel itself is measured exactly on the course (see
        -- water.comp.glsl), so the cells only carry the valley and say which segment is near.
        local cell = self.field_cell or math.max(15.0, math.min(80.0, valley / 25.0))
        -- Keep every river's block inside the grid budget: a long river gets coarser cells.
        local xs, zs = {}, {}
        local minx, minz, maxx, maxz = 1e30, 1e30, -1e30, -1e30
        for j, p in ipairs(r.points) do
            xs[j], zs[j] = p[1], p[2]
            minx, maxx = math.min(minx, p[1]), math.max(maxx, p[1])
            minz, maxz = math.min(minz, p[2]), math.max(maxz, p[2])
        end
        local margin = valley + 6.0 * cell
        local cells = ((maxx - minx + 2 * margin) / cell) * ((maxz - minz + 2 * margin) / cell) * 4.0 -- four values a cell
        local share = budget / math.max(1, #self.rivers - i + 1)
        if cells > share then
            cell = cell * math.sqrt(cells / share)
            margin = valley + 6.0 * cell
        end
        budget = budget - math.min(cells, share)
        -- At each point: the half-width, its banks wandering by up to a third with the roughness every few
        -- widths; the depth, from the width (or as given, scaled with it) and deeper or shallower at falls.
        local length, run, halves, depths = math.max(1.0, length_of(r.points)), 0.0, {}, {}
        local deepest = 0.0
        local last = r.base[#r.base]
        for j, p in ipairs(r.points) do
            if j > 1 then
                run = run + math.sqrt((p[1] - r.points[j - 1][1]) ^ 2 + (p[2] - r.points[j - 1][2]) ^ 2)
            end
            local w = r.widths[j]
            local wander = math.sin(run / (w * 2.5) + i) * 0.6 + wobble(run / (w * 4.0), i) * 0.4
            halves[j] = w * 0.5 * (1.0 + 0.66 * r.roughness * wander * 0.5) + 0.5
            local depth = r.depth and r.depth * math.sqrt(r.base[j] / last) or math.max(2.0, r.base[j] * 0.08)
            depths[j] = depth * r.deep[j]
            deepest = math.max(deepest, depths[j])
        end
        local axs, azs = {}, {}
        for j, p in ipairs(r.axis) do
            axs[j], azs[j] = p[1], p[2]
        end
        local plain = k.plain and math.min(valley * k.plain, w1 * 15.0) or 0.0
        r.block = Hydrology.RiverBlock{
            xs = xs, zs = zs, levels = r.levels, halves = halves, depths = depths, skews = r.skews, axis_xs = axs,
            axis_zs = azs, axis_levels = r.axis_levels, cell = cell, margin = margin,
            params = { RIVER_CODES[r.kind], deepest, valley, math.tan(math.rad(k.slope)), k.step or 1.0, k.floor or 0.0,
                       plain, k.terrace or 3.0, k.levee or 0.0, w1,
                       -- Segments searched either side of a cell's nearest one: enough to span a cell and a half.
                       math.min(64, math.ceil(1.5 * cell / math.max(1.0, length / math.max(1, #r.points - 1))) + 2) },
        }
        -- A tributary's water gives way where its river is nearer (the receiver's block is read for it).
        local carve = Hydrology.Carve{ block = r.block, receiver = r.receiver and r.receiver.block or -1, mode = "cap",
                                       aux = spurs, position = position }
        caps[#caps + 1] = { cap = carve, k = math.max(4.0, deepest * 2.0) }
        if k.plain then
            -- A floodplain fills what lies under it by up to three metres, and builds a levee on each bank.
            fills[#fills + 1] = Hydrology.Carve{ block = r.block, mode = "fill", aux = before, position = position }
        end
        river_water = max_of(river_water, carve.water)
    end
    for _, fill in ipairs(fills) do
        ground = Combine.Max{ a = ground, b = fill }
    end
    for _, c in ipairs(caps) do
        ground = Combine.SmoothMin{ a = ground, b = c.cap, k = c.k }
    end

    -- The sea: the sampled sea cells and a few around them at the level, the rest falling away linearly,
    -- which Catmull-Rom carries without the overshoot a jump would give.
    local sea = nil
    if self.sea_level then
        local g = self.g
        local dist, frontier = {}, {}
        for i = 1, g.n * g.m do
            if g.d.sea[i] then
                dist[i] = 0
                frontier[#frontier + 1] = i
            end
        end
        local k = 1
        while k <= #frontier do
            local c = frontier[k]
            local x, z = (c - 1) % g.n, (c - 1) // g.n
            for dz = -1, 1 do
                for dx = -1, 1 do
                    local nx, nz = x + dx, z + dz
                    if nx >= 0 and nz >= 0 and nx < g.n and nz < g.m then
                        local q = nz * g.n + nx + 1
                        if dist[q] == nil then
                            dist[q] = dist[c] + 1
                            frontier[#frontier + 1] = q
                        end
                    end
                end
            end
            k = k + 1
        end
        local values = {}
        for i = 1, g.n * g.m do
            local cells = dist[i] or 1e6
            values[i] = self.sea_level - math.max(0.0, math.min(cells, 40) - 3.0) * 30.0
        end
        sea = Terrain.Grid{ values = values, size = { g.n, g.m }, origin = { g.x0, g.z0 }, cell = g.cell }
    end

    -- The detail the cuts removed, given back once for all of them: where the ground was lowered most of
    -- it returns, so a valley side keeps the texture of the slope it was cut from.
    -- Never under water, though: detail put back on a bed lifts it through a shallow river, which then
    -- runs dry in patches.
    if opts.detail then
        local back = Curves.Smoothstep{ input = height - ground, min = 0.0, max = 4.0 }
        local top = max_of(max_of(lake_water, river_water), sea)
        if top then
            back = back * Curves.Smoothstep{ input = ground - top, min = 0.5, max = 3.0 }
        end
        ground = ground + back * opts.detail * 0.6
    end
    local low = ground - 500.0
    return { height = ground, lakes = lake_water or low, rivers = river_water or low, sea = sea or low }
end

-- The water as it is drawn, walked down every river's channel: how often it rises (by more than
-- `tolerance`, 0.3 m), by how much at worst, and how often the ground stands over it. `wet` is what
-- `apply` returned; `seed` the job seed. Points inside lakes are left out, their water is the lake's.
function Network:audit(wet, seed, tolerance)
    tolerance = tolerance or 0.3
    local report = { rises = 0, dry = 0, points = 0, worst = 0.0, rivers = {} }
    for k, r in ipairs(self.rivers) do
        local xs, zs = {}, {}
        local step = math.max(1, #r.points // 400)
        for i = 1, #r.points - 1, step do
            xs[#xs + 1], zs[#zs + 1] = r.points[i][1], r.points[i][2]
        end
        local water  = Terrain.Sample{ value = wet.rivers, xs = xs, zs = zs, cell = 4.0, seed = seed or 0 }
        local ground = Terrain.Sample{ value = wet.height, xs = xs, zs = zs, cell = 4.0, seed = seed or 0 }
        local lake   = Terrain.Sample{ value = wet.lakes, xs = xs, zs = zs, cell = 4.0, seed = seed or 0 }
        local entry  = { kind = r.kind, generated = r.generated, rises = 0, dry = 0, worst = 0.0, points = 0 }
        local previous = nil
        for i = 1, #xs do
            if lake[i] < ground[i] then
                entry.points = entry.points + 1
                if water[i] < ground[i] then
                    entry.dry = entry.dry + 1
                end
                if previous and water[i] > previous + tolerance then
                    entry.rises = entry.rises + 1
                    entry.first = entry.first or { x = xs[i], z = zs[i], before = previous, after = water[i], ground = ground[i] }
                    entry.worst = math.max(entry.worst, water[i] - previous)
                end
                previous = water[i]
            else
                previous = nil
            end
        end
        report.rivers[k] = entry
        report.rises  = report.rises + entry.rises
        report.dry    = report.dry + entry.dry
        report.points = report.points + entry.points
        report.worst  = math.max(report.worst, entry.worst)
    end
    return report
end

-- What was found and built, for a script to print or test: counts and each element's kind.
function Network:summary()
    local rivers, lakes = {}, {}
    for _, r in ipairs(self.rivers) do
        rivers[#rivers + 1] = r.kind .. (r.generated and "*" or "")
    end
    for _, l in ipairs(self.lakes) do
        lakes[#lakes + 1] = l.kind .. (l.generated and "*" or "")
    end
    return #self.rivers .. " river(s): " .. table.concat(rivers, " ") .. "; " .. #self.lakes .. " lake(s): "
        .. table.concat(lakes, " ")
end

Water.RIVER_KINDS = RIVER_KINDS
Water.LAKE_KINDS  = LAKE_KINDS
