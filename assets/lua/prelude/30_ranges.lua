-- Ranges: mountain ranges raised along crest lines and carved by rivers.
--
-- `Ranges.Carve` is the whole recipe the alpine terrain arrived at, so a new range is a list of crest
-- lines and not two hundred lines of script:
--
--   1. Uplift, in plain Lua on a coarse grid: every crest line lifts the ground around it, most on the
--      line and falling to nothing `width` metres away, its height wandering along the line. Peaks lift
--      more where they are.
--   2. `Erosion.StreamPower` lets rivers cut into that until uplift and erosion balance: valleys that
--      join into larger ones, sharp ridges between them. Rivers leave a crest across it, so the network
--      follows the range instead of radiating from a centre.
--   3. Cirques are dug under the peaks into the result, and a short second run with a steeper talus
--      lets the rivers reach their floors and keeps their headwalls tall: the cliffs under a summit.
--   4. `Terrain.Grid` carries the grid to the sections, read through a broad warp so no channel runs
--      straight along the grid.
--
-- Run by the engine after `20_landforms.lua`. The grid is simulated once per script run, which takes
-- seconds, not milliseconds.

local engine = ...
local fail   = engine.fail

local Erosion, Terrain, Shapes, Noises, Curves, Combine, Filters =
    Erosion, Terrain, Shapes, Noises, Curves, Combine, Filters
local type, ipairs, pairs, tostring = type, ipairs, pairs, tostring
local math = math

local Ranges = {}

local function check(t)
    if type(t) ~= "table" then
        fail("Ranges.Carve expects a table of named arguments, as in Ranges.Carve{ ... }")
    end
    local allowed = {
        ridges = true, peaks = true, seed = true, center = true, extent = true, cell = true,
        steps = true, k = true, diffusion = true, talus = true, lift = true, wander = true, cells = true,
        cirques = true, wobble = true, fill = true, fill_radius = true, repose = true, talus_cones = true, shoulders = true, glacial = true,
        glacier_area = true,
    }
    for key in pairs(t) do
        if not allowed[key] then
            fail("Ranges.Carve does not take '" .. tostring(key) .. "'; it accepts ridges, peaks, seed, "
                 .. "center, extent, cell, steps, k, diffusion, talus, lift, wander, cirques, wobble")
        end
    end
    if type(t.ridges) ~= "table" or #t.ridges == 0 then
        fail("Ranges.Carve needs 'ridges', a list of { points = { { x, z }, ... }, height = m, width = m }")
    end
    for i, r in ipairs(t.ridges) do
        if type(r.points) ~= "table" or #r.points < 2 or type(r.height) ~= "number"
            or type(r.width) ~= "number" or r.width <= 0 then
            fail("Ranges.Carve ridge " .. i .. " needs at least 2 points, a height and a width in metres")
        end
    end
end

-- Hashed value noise, the same on every machine: integers and floats only.
local function make_noise(seed)
    local function hash(ix, iz, salt)
        local v = (ix * 374761393 + iz * 668265263 + salt * 2246822519 + seed * 3266489917) & 0xffffffff
        v = ((v ~ (v >> 13)) * 1274126177) & 0xffffffff
        return ((v ~ (v >> 16)) & 0xffff) / 65535.0
    end
    local function value(px, pz, wavelength, salt)
        local fx, fz = px / wavelength, pz / wavelength
        local ix, iz = math.floor(fx), math.floor(fz)
        local tx, tz = fx - ix, fz - iz
        tx, tz = tx * tx * (3 - 2 * tx), tz * tz * (3 - 2 * tz)
        local a = hash(ix, iz, salt) + (hash(ix + 1, iz, salt) - hash(ix, iz, salt)) * tx
        local b = hash(ix, iz + 1, salt) + (hash(ix + 1, iz + 1, salt) - hash(ix, iz + 1, salt)) * tx
        return (a + (b - a) * tz) * 2.0 - 1.0
    end
    local function fbm(px, pz, wavelength, octaves, salt)
        local sum, amplitude, norm = 0.0, 1.0, 0.0
        for o = 1, octaves do
            sum = sum + value(px, pz, wavelength, salt + o) * amplitude
            norm = norm + amplitude
            wavelength, amplitude = wavelength * 0.5, amplitude * 0.5
        end
        return sum / norm
    end
    return hash, fbm
end

local function smooth(x)
    x = math.min(math.max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)
end

-- Distance from (px, pz) to a polyline, and how far along it the nearest point is, in metres.
local function along(px, pz, points, lengths)
    local best, at = math.huge, 0.0
    for i = 1, #points - 1 do
        local a, b = points[i], points[i + 1]
        local bx, bz = b[1] - a[1], b[2] - a[2]
        local l2 = bx * bx + bz * bz
        local u = l2 > 0 and math.min(math.max(((px - a[1]) * bx + (pz - a[2]) * bz) / l2, 0.0), 1.0) or 0
        local dx, dz = px - a[1] - bx * u, pz - a[2] - bz * u
        local d = dx * dx + dz * dz
        if d < best then
            best, at = d, lengths[i] + u * math.sqrt(l2)
        end
    end
    return math.sqrt(best), at
end

-- The carved range, a field in metres (`R2 -> R1`) whose base level is 0.
function Ranges.Carve(t)
    check(t)
    local seed     = t.seed or 0
    -- The grid covers every crest and its flanks with room to spare, because its edge is the base
    -- level: a range that reached it would end in a wall. Its size in cells is fixed, so a larger range
    -- gets coarser cells and costs the same.
    local minX, minZ, maxX, maxZ = math.huge, math.huge, -math.huge, -math.huge
    for _, r in ipairs(t.ridges) do
        for _, p in ipairs(r.points) do
            minX, maxX = math.min(minX, p[1] - r.width), math.max(maxX, p[1] + r.width)
            minZ, maxZ = math.min(minZ, p[2] - r.width), math.max(maxZ, p[2] + r.width)
        end
    end
    local margin   = (t.wander or 1300.0) + 2500.0
    local center   = t.center or { (minX + maxX) * 0.5, (minZ + maxZ) * 0.5 }
    local extent   = t.extent or (math.max(maxX - minX, maxZ - minZ) + 2.0 * margin)
    local n        = t.cells or 230
    local cell     = t.cell or extent / n
    n              = math.floor(extent / cell + 0.5)
    local originX  = center[1] - n * cell * 0.5
    local originZ  = center[2] - n * cell * 0.5
    local steps    = t.steps or 380
    local wander   = t.wander or 1300.0
    local hash, fbm = make_noise(seed)

    local ridges = {}
    for _, r in ipairs(t.ridges) do
        local lengths, total = { 0.0 }, 0.0
        for i = 2, #r.points do
            local a, b = r.points[i - 1], r.points[i]
            total = total + math.sqrt((b[1] - a[1]) ^ 2 + (b[2] - a[2]) ^ 2)
            lengths[i] = total
        end
        ridges[#ridges + 1] = { points = r.points, lengths = lengths, height = r.height, width = r.width }
    end
    -- Summits along every crest, so a range is not one height end to end: one every 5 km or so, at a
    -- hashed place and height, the first of each crest the tallest. Explicit `peaks` are added to them.
    local function point_at(r, s)
        for i = 2, #r.points do
            if r.lengths[i] >= s or i == #r.points then
                local a, b = r.points[i - 1], r.points[i]
                local span = math.max(r.lengths[i] - r.lengths[i - 1], 1e-6)
                local u = math.min(math.max((s - r.lengths[i - 1]) / span, 0.0), 1.0)
                return { a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u }
            end
        end
    end
    local peaks = {}
    for i, r in ipairs(ridges) do
        local total = r.lengths[#r.lengths]
        local count = math.max(1, math.floor(total / 5000.0 + 0.5))
        local first = math.floor(hash(i, 0, 90) * count)
        for j = 0, count - 1 do
            local s = total * (j + 0.5 + 0.5 * (hash(i, j, 91) - 0.5)) / count
            local boost = j == first and 1.45 or 1.12 + 0.2 * hash(i, j, 92)
            peaks[#peaks + 1] = { at = point_at(r, s), height = r.height * boost, radius = r.width * 0.75 }
        end
    end
    for _, p in ipairs(t.peaks or {}) do peaks[#peaks + 1] = p end
    local tallest = 0.0
    for _, r in ipairs(ridges) do tallest = math.max(tallest, r.height) end
    for _, p in ipairs(peaks) do tallest = math.max(tallest, p.height) end

    -- 1. Uplift. A crest's height wanders along it (a 4 km wave of a quarter of its height), so the
    -- range has high and low stretches; the plan is bent by `wander` metres so no crest is a ruler.
    local function rise(px, pz)
        local wx = px + wander * fbm(px, pz, 6000.0, 3, 10)
        local wz = pz + wander * fbm(px, pz, 6000.0, 3, 20)
        local best = 0.0
        for i, r in ipairs(ridges) do
            local d, s = along(wx, wz, r.points, r.lengths)
            local h = r.height * (0.85 + 0.25 * fbm(s, i * 977.0, 4000.0, 2, 30 + i))
            best = math.max(best, h * smooth(1.0 - d / r.width))
        end
        for _, p in ipairs(peaks) do
            local d = math.sqrt((wx - p.at[1]) ^ 2 + (wz - p.at[2]) ^ 2)
            best = math.max(best, p.height * smooth(1.0 - d / (p.radius or 3000.0)))
        end
        return best * (1.0 + 0.5 * fbm(px, pz, 4000.0, 3, 40))
    end

    -- The uplift is smooth, so it is evaluated on every other cell and interpolated: a quarter of the
    -- noise, which is most of the time spent in Lua.
    local half = math.floor(n / 2) + 2
    local coarse = {}
    for z = 0, half - 1 do
        for x = 0, half - 1 do
            coarse[z * half + x] = rise(originX + 2 * x * cell, originZ + 2 * z * cell)
        end
    end
    local function rise_at(x, z)
        local hx, hz = x * 0.5, z * 0.5
        local ix, iz = math.floor(hx), math.floor(hz)
        local fx, fz = hx - ix, hz - iz
        local a = coarse[iz * half + ix] + (coarse[iz * half + ix + 1] - coarse[iz * half + ix]) * fx
        local b = coarse[(iz + 1) * half + ix]
                  + (coarse[(iz + 1) * half + ix + 1] - coarse[(iz + 1) * half + ix]) * fx
        return a + (b - a) * fz
    end

    local lift   = t.lift or 2.4
    -- Shoulders: the uplift is gently stepped, so the eroded range keeps broad benches at a few levels
    -- (alpine pastures, cirque floors) between its steeper walls instead of one slope from crest to
    -- valley. `shoulders` is the strength, 0 to 1.
    local shoulders = t.shoulders or 0.6
    local bench     = tallest * 0.24
    local function stepped(u)
        if shoulders <= 0 then return u end
        local f = u / bench
        local whole = math.floor(f)
        local frac = f - whole
        local s_ = frac < 0.35 and 0.0 or (frac > 0.85 and 1.0 or smooth((frac - 0.35) / 0.5))
        return u + ((whole + s_) * bench - u) * shoulders
    end
    local height, uplift = {}, {}
    for z = 0, n - 1 do
        for x = 0, n - 1 do
            local px, pz = originX + x * cell, originZ + z * cell
            local u = stepped(rise_at(x, z))
            uplift[#uplift + 1] = u / steps * lift
            -- A rough start at several scales, so the first channels form where the ground happens to
            -- be low, bend around the large bumps and the network is irregular.
            height[#height + 1] = u * 0.02 + 80.0 * fbm(px, pz, 3000.0, 2, 50) + 6.0 * hash(x, z, 70)
        end
    end
    for i = 1, #height do height[i] = math.max(height[i], 0.0) end

    -- 2. Rivers.
    local k         = t.k or 0.016
    -- Relative to the cell, so the spacing of valleys is the same whatever size the grid has.
    local diffusion = t.diffusion or 0.012 * cell * cell
    local talus     = t.talus or 0.6
    local carved = Erosion.StreamPower{
        size = { n, n }, cell = cell, height = height, uplift = uplift, iterations = steps, k = k,
        m = 0.5, talus = talus, diffusion = diffusion, refresh = 4,
    }

    -- 3. Cirques under the peaks: bowls with a steep headwall (`u (2 - u)`), each on the side of the
    -- peak away from the next one, then a short second run that keeps the walls (steeper talus) and
    -- lets the rivers reach the floors. `cirques = false` skips this.
    local cuts_ = {}
    if t.cirques ~= false and #peaks > 0 then
        -- Each cirque touches only the cells inside its radius, so it is visited there and nowhere
        -- else: walking every cell for every cirque was most of the time this function took.
        for j, p in ipairs(peaks) do
            local radius = (p.radius or 3000.0) * 0.32
            for c = 0, 2 do
                local a  = (j * 2.1 + c * 2.094) + 0.6 * fbm(j * 31.0, c * 17.0, 1.0, 1, 80)
                local cx = p.at[1] + math.cos(a) * radius * 1.15
                local cz = p.at[2] + math.sin(a) * radius * 1.15
                local depth = p.height * 0.28
                local x0 = math.max(0, math.floor((cx - radius - originX) / cell))
                local x1 = math.min(n - 1, math.ceil((cx + radius - originX) / cell))
                local z0 = math.max(0, math.floor((cz - radius - originZ) / cell))
                local z1 = math.min(n - 1, math.ceil((cz + radius - originZ) / cell))
                for z = z0, z1 do
                    for x = x0, x1 do
                        local d = math.sqrt((originX + x * cell - cx) ^ 2 + (originZ + z * cell - cz) ^ 2)
                        if d < radius then
                            local u = smooth(1.0 - d / radius) ^ 0.5
                            local i = z * n + x + 1
                            cuts_[i] = math.max(cuts_[i] or 0.0, u * (2.0 - u) * depth)
                        end
                    end
                end
            end
        end
        for i, cut in pairs(cuts_) do carved[i] = carved[i] - cut end
        local still = {}
        for i = 1, #uplift do still[i] = uplift[i] * 0.3 end
        carved = Erosion.StreamPower{
            size = { n, n }, cell = cell, height = carved, uplift = still, iterations = 40, k = k,
            m = 0.5, talus = talus * 2.0, diffusion = diffusion * 0.3, refresh = 2,
        }
    end

    -- Deposition: what the slopes shed collects in the low ground. Every cell is raised towards a broad
    -- average of its surroundings, less a little, but only low on the range: valley floors fill flat and
    -- widen, and the foot of every slope gets a concave apron of scree, while the high ground keeps its
    -- edges. Radius in metres, so it does not depend on the grid.
    local fill = t.fill or 1.0
    if fill > 0 then
        local radius = math.max(1, math.floor((t.fill_radius or 500.0) / cell + 0.5))
        local function box(src)
            local tmp, out = {}, {}
            for z = 0, n - 1 do
                local sum, row = 0.0, z * n
                for x = -radius, radius do sum = sum + src[row + math.min(math.max(x, 0), n - 1) + 1] end
                for x = 0, n - 1 do
                    tmp[row + x + 1] = sum / (2 * radius + 1)
                    sum = sum + src[row + math.min(x + radius + 1, n - 1) + 1]
                              - src[row + math.max(x - radius, 0) + 1]
                end
            end
            for x = 0, n - 1 do
                local sum = 0.0
                for z = -radius, radius do sum = sum + tmp[math.min(math.max(z, 0), n - 1) * n + x + 1] end
                for z = 0, n - 1 do
                    out[z * n + x + 1] = sum / (2 * radius + 1)
                    sum = sum + tmp[math.min(z + radius + 1, n - 1) * n + x + 1]
                              - tmp[math.max(z - radius, 0) * n + x + 1]
                end
            end
            return out
        end
        local broad = box(box(carved))
        for i = 1, #carved do
            local h = carved[i]
            local low = smooth(1.0 - h / (tallest * 0.3))
            local floor = broad[i] - tallest * 0.04
            if floor > h then
                carved[i] = h + math.min(floor - h, tallest * 0.08) * low * fill
            end
        end
    end

    -- Glacial valleys: the main valleys are widened into a U. The drainage of the carved grid is traced
    -- (steepest descent, area accumulated from the top), every cell where enough ground drains is a
    -- valley floor, and the ground around it is lowered to no more than a parabola rising from that
    -- floor: flat near the river, steepening outwards, until it meets the V the river cut and leaves
    -- it. The parabola's width grows with the square root of the area, so trunk valleys are broad and
    -- tributaries narrow; tributaries end higher than the trunk they join, as hanging valleys do.
    if t.glacial ~= false then
        local order = {}
        for i = 1, #carved do order[i] = i end
        table.sort(order, function(a_, b_) return carved[a_] > carved[b_] end)
        local area, receiver = {}, {}
        for i = 1, #carved do area[i] = cell * cell end
        for _, i in ipairs(order) do
            local x, z = (i - 1) % n, (i - 1) // n
            local best, to = 0.0, nil
            for dz = -1, 1 do
                for dx = -1, 1 do
                    local xx, zz = x + dx, z + dz
                    if (dx ~= 0 or dz ~= 0) and xx >= 0 and zz >= 0 and xx < n and zz < n then
                        local q = zz * n + xx + 1
                        local drop = (carved[i] - carved[q]) / ((dx ~= 0 and dz ~= 0) and 1.4142 or 1.0)
                        if drop > best then best, to = drop, q end
                    end
                end
            end
            receiver[i] = to
            if to then area[to] = area[to] + area[i] end
        end
        local threshold = (t.glacier_area or 2.5e6)
        local nearest, floorAt, radius = {}, {}, {}
        for i = 1, #carved do
            if area[i] >= threshold and carved[i] < tallest * 0.7 and carved[i] > tallest * 0.02 then
                nearest[i] = i
                floorAt[i] = carved[i]
                radius[i] = math.min(math.max(0.22 * math.sqrt(area[i]), 120.0), 700.0)
            end
        end
        local function cx(i) return ((i - 1) % n) * cell end
        local function cz(i) return ((i - 1) // n) * cell end
        local function consider(i, q)
            local c = nearest[q]
            if c then
                local d = (cx(i) - cx(c)) ^ 2 + (cz(i) - cz(c)) ^ 2
                local mine = nearest[i]
                if not mine or d < (cx(i) - cx(mine)) ^ 2 + (cz(i) - cz(mine)) ^ 2 then
                    nearest[i] = c
                end
            end
        end
        for z = 0, n - 1 do
            for x = 0, n - 1 do
                local i = z * n + x + 1
                if x > 0 then consider(i, i - 1) end
                if z > 0 then consider(i, i - n) end
                if x > 0 and z > 0 then consider(i, i - n - 1) end
                if x < n - 1 and z > 0 then consider(i, i - n + 1) end
            end
        end
        for z = n - 1, 0, -1 do
            for x = n - 1, 0, -1 do
                local i = z * n + x + 1
                if x < n - 1 then consider(i, i + 1) end
                if z < n - 1 then consider(i, i + n) end
                if x < n - 1 and z < n - 1 then consider(i, i + n + 1) end
                if x > 0 and z < n - 1 then consider(i, i + n - 1) end
            end
        end
        for i = 1, #carved do
            local c = nearest[i]
            if c then
                local d2 = (cx(i) - cx(c)) ^ 2 + (cz(i) - cz(c)) ^ 2
                local u = floorAt[c] + d2 / (2.0 * radius[c])
                if u < carved[i] then carved[i] = u end
            end
        end
    end

    -- Talus: scree cones under the walls. A surface is grown from every cell, lowered by `drop`, down at
    -- the angle of repose; where the ground falls faster than that (a wall) the surface stands above
    -- it at the wall's foot, and the difference is the cone. Thickness is capped, so a cone is a skirt
    -- and not a burial. Two chamfer passes over the grid compute it exactly enough for straight cones.
    local repose = math.tan(math.rad(t.repose or 33.0))
    if repose > 0 and (t.talus_cones ~= false) then
        local drop  = tallest * 0.1
        local cap   = tallest * 0.12
        local D = {}
        for i = 1, #carved do D[i] = carved[i] - drop end
        local straight, diagonal = repose * cell, repose * cell * math.sqrt(2.0)
        local function relax(x, z, dx, dz, step)
            local xx, zz = x + dx, z + dz
            if xx >= 0 and zz >= 0 and xx < n and zz < n then
                local v = D[zz * n + xx + 1] - step
                local i = z * n + x + 1
                if v > D[i] then D[i] = v end
            end
        end
        for z = 0, n - 1 do
            for x = 0, n - 1 do
                relax(x, z, -1, 0, straight); relax(x, z, 0, -1, straight)
                relax(x, z, -1, -1, diagonal); relax(x, z, 1, -1, diagonal)
            end
        end
        for z = n - 1, 0, -1 do
            for x = n - 1, 0, -1 do
                relax(x, z, 1, 0, straight); relax(x, z, 0, 1, straight)
                relax(x, z, 1, 1, diagonal); relax(x, z, -1, 1, diagonal)
            end
        end
        for i = 1, #carved do
            local h = carved[i]
            if D[i] > h then
                carved[i] = h + math.min(D[i] - h, cap)
            end
        end
    end

    -- Shadow: whether the sun reaches each cell, for the viewer (see `Ranges.Finish`). The sun is the
    -- viewer's, `kSunDirection` in viewer/terrain.frag.glsl; the march goes towards it in steps that
    -- grow with distance, and the horizon it finds is compared with the sun's elevation over a soft
    -- band, so the edge of a shadow is a penumbra and not a stair.
    local sx, sy, sz = 0.55, 0.42, 0.40
    local flat = math.sqrt(sx * sx + sz * sz)
    local ux, uz, elevation = sx / flat, sz / flat, sy / flat
    local shadow = {}
    for z = 0, n - 1 do
        for x = 0, n - 1 do
            local i = z * n + x + 1
            local h = carved[i]
            local horizon = -1.0
            local d = cell
            while d < 9000.0 do
                local qx = math.floor(x + ux * d / cell + 0.5)
                local qz = math.floor(z + uz * d / cell + 0.5)
                if qx < 0 or qz < 0 or qx >= n or qz >= n then break end
                local slope = (carved[qz * n + qx + 1] - h) / d
                if slope > horizon then horizon = slope end
                d = d * 1.18 + cell * 0.5
            end
            shadow[i] = smooth((elevation - horizon) / 0.12 + 0.5)
        end
    end

    -- 4. On the GPU, read through a broad warp: the simulation routes water in eight directions on a
    -- square grid, and a channel that follows one of them for long reads as a ruled line.
    local wobble = t.wobble or 180.0
    local position = Shapes.Warp{ amplitude = wobble, frequency = 0.0006, octaves = 2, seed = seed + 71 }
    local sun = Terrain.Grid{
        values = shadow, size = { n, n }, origin = { originX, originZ }, cell = cell, position = position,
    }
    return Terrain.Grid{
        values = carved, size = { n, n }, origin = { originX, originZ }, cell = cell, position = position,
    }, tallest, sun
end

-- The finish every range gets on the GPU: low rolling country around it, fine gullies on the steep
-- walls of the simulated valleys, and the erosion passes. Returns the script's outputs.
function Ranges.Finish(structure, tallest, seed, options, sun)
    seed    = seed or 0
    options = options or {}
    local country = Noises.fBm{ frequency = 0.0004, octaves = 3, amplitude = 45.0, seed = seed + 5 }
    local ground  = structure + country.value
    local mountain = Curves.Smoothstep{ input = structure, min = tallest * 0.05, max = tallest * 0.25 }

    -- Strata: rock laid down in beds that were then tilted. The height is cut into steps along planes
    -- dipping `dip` degrees, displaced by a broad noise so the beds bend: each step is a ledge (a flat
    -- at its own inclination, where grass or snow lies) under a cliff. Seen from afar the ledges line up
    -- into slanting bands across the whole mountain. Only on the middle and upper slopes, and only in
    -- patches, so some faces are bedded rock and others are not.
    local dip    = math.rad(options.dip or 15.0)
    local strike = math.rad(options.strike or 25.0 + 80.0 * ((seed * 2654435761) % 1000) / 1000.0)
    local c      = Terrain.Coords{}
    local tilt   = (c.x * math.cos(strike) + c.z * math.sin(strike)) * math.tan(dip)
    local bend   = Noises.fBm{ frequency = 0.0005, octaves = 3, amplitude = 220.0, seed = seed + 81 }
    local plane  = tilt + bend.value
    local step   = options.bed or 220.0
    local bedded = Curves.Terrace{ input = ground + plane, step = step, sharpness = 0.5 } - plane
    local patches = Curves.Smoothstep{
        input = Noises.fBm{ frequency = 0.00035, octaves = 2, amplitude = 1.0, seed = seed + 83 }.value,
        min = 0.0, max = 0.35,
    }
    local band = Curves.Smoothstep{ input = structure, min = tallest * 0.25, max = tallest * 0.5 }
    local strata = Combine.Blend{ a = ground, b = bedded, mask = band * patches * (options.strata or 1.0) }
    -- Scree under the cliffs: what thermal erosion would shed off the steps, kept only where it lies
    -- above the ground (a maximum), so the cliffs keep their edges and their feet get cones.
    local shed = Erosion.Thermal{
        input = strata, iterations = 48, talus = math.tan(math.rad(35.0)), strength = 0.5,
    }
    strata = Combine.Max{ a = strata, b = shed }

    -- Few octaves and long cells: streaks running down the walls, not grain.
    local gullied = Erosion.Gullies{
        input = strata, cell = 420.0, octaves = 3, strength = 18.0, full = 0.7, seed = seed + 53,
    }
    -- A second, finer generation on the walls of the first: what reads as erosion at close range.
    local fine = Erosion.Gullies{
        input = gullied, cell = 140.0, octaves = 3, strength = 9.0, full = 0.6, seed = seed + 57,
    }
    local land = Combine.Blend{ a = ground, b = fine, mask = mountain }
    local carved = Erosion.Hydraulic{
        input = land, iterations = 20, rain = 0.5, capacity = 9.0, evaporation = 0.04,
    }
    -- The blur that clears the grid ripple of the hydraulic pass is kept off the crests: where the
    -- ground stands above its surroundings it is a ridge, and a ridge should stay an edge.
    local blurred = Filters.Blur{ input = carved, radius = 1 }
    local crest = Curves.Smoothstep{
        input = carved - Filters.Blur{ input = carved, radius = 3 }, min = 0.5, max = 4.0,
    }
    local smoothed = Combine.Blend{ a = blurred, b = carved, mask = crest }
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 12, talus = math.tan(math.rad(42.0)), strength = 0.3,
    }
    local outputs = {
        height  = { value = settled, range = { -tallest * 0.1, tallest * 1.3 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
    -- The viewer darkens what this says is out of the sun; an export writes it as its own map.
    if sun then outputs.shadow = { value = sun, range = { 0, 1 } } end
    return outputs
end

_G.Ranges = Ranges
