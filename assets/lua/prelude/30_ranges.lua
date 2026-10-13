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
        steps = true, k = true, diffusion = true, talus = true, lift = true, wander = true,
        cirques = true, wobble = true,
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
    local n        = 336
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
    local peaks = t.peaks or {}
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
            local h = r.height * (1.0 + 0.25 * fbm(s, i * 977.0, 4000.0, 2, 30 + i))
            best = math.max(best, h * smooth(1.0 - d / r.width))
        end
        for _, p in ipairs(peaks) do
            local d = math.sqrt((wx - p.at[1]) ^ 2 + (wz - p.at[2]) ^ 2)
            best = math.max(best, p.height * smooth(1.0 - d / (p.radius or 3000.0)))
        end
        return best * (1.0 + 0.3 * fbm(px, pz, 2600.0, 3, 40))
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

    local lift   = t.lift or 2.6
    local height, uplift = {}, {}
    for z = 0, n - 1 do
        for x = 0, n - 1 do
            local px, pz = originX + x * cell, originZ + z * cell
            local u = rise_at(x, z)
            uplift[#uplift + 1] = u / steps * lift
            -- A rough start at several scales, so the first channels form where the ground happens to
            -- be low, bend around the large bumps and the network is irregular.
            height[#height + 1] = u * 0.02 + 80.0 * fbm(px, pz, 3000.0, 2, 50) + 6.0 * hash(x, z, 70)
        end
    end
    for i = 1, #height do height[i] = math.max(height[i], 0.0) end

    -- 2. Rivers.
    local k         = t.k or 0.016
    local diffusion = t.diffusion or 60.0
    local talus     = t.talus or 1.15
    local carved = Erosion.StreamPower{
        size = { n, n }, cell = cell, height = height, uplift = uplift, iterations = steps, k = k,
        m = 0.5, talus = talus, diffusion = diffusion, refresh = 4,
    }

    -- 3. Cirques under the peaks: bowls with a steep headwall (`u (2 - u)`), each on the side of the
    -- peak away from the next one, then a short second run that keeps the walls (steeper talus) and
    -- lets the rivers reach the floors. `cirques = false` skips this.
    if t.cirques ~= false and #peaks > 0 then
        for z = 0, n - 1 do
            for x = 0, n - 1 do
                local px, pz = originX + x * cell, originZ + z * cell
                local i = z * n + x + 1
                local cut = 0.0
                for j, p in ipairs(peaks) do
                    local radius = (p.radius or 3000.0) * 0.32
                    for c = 0, 2 do
                        local a = (j * 2.1 + c * 2.094) + 0.6 * fbm(j * 31.0, c * 17.0, 1.0, 1, 80)
                        local cx = p.at[1] + math.cos(a) * radius * 1.15
                        local cz = p.at[2] + math.sin(a) * radius * 1.15
                        local d = math.sqrt((px - cx) ^ 2 + (pz - cz) ^ 2)
                        local u = smooth(1.0 - d / radius) ^ 0.5
                        cut = math.max(cut, u * (2.0 - u) * p.height * 0.28)
                    end
                end
                carved[i] = carved[i] - cut
            end
        end
        local still = {}
        for i = 1, #uplift do still[i] = uplift[i] * 0.3 end
        carved = Erosion.StreamPower{
            size = { n, n }, cell = cell, height = carved, uplift = still, iterations = 40, k = k,
            m = 0.5, talus = talus * 1.7, diffusion = diffusion * 0.3, refresh = 2,
        }
    end

    -- 4. On the GPU, read through a broad warp: the simulation routes water in eight directions on a
    -- square grid, and a channel that follows one of them for long reads as a ruled line.
    local wobble = t.wobble or 180.0
    return Terrain.Grid{
        values = carved, size = { n, n }, origin = { originX, originZ }, cell = cell,
        position = Shapes.Warp{ amplitude = wobble, frequency = 0.0006, octaves = 2, seed = seed + 71 },
    }, tallest
end

-- The finish every range gets on the GPU: low rolling country around it, fine gullies on the steep
-- walls of the simulated valleys, and the erosion passes. Returns the script's outputs.
function Ranges.Finish(structure, tallest, seed)
    seed = seed or 0
    local country = Noises.fBm{ frequency = 0.0004, octaves = 3, amplitude = 45.0, seed = seed + 5 }
    local ground  = structure + country.value
    local mountain = Curves.Smoothstep{ input = structure, min = tallest * 0.05, max = tallest * 0.25 }
    -- Few octaves and long cells: streaks running down the walls, not grain.
    local gullied = Erosion.Gullies{
        input = ground, cell = 420.0, octaves = 3, strength = 18.0, full = 0.7, seed = seed + 53,
    }
    local land = Combine.Blend{ a = ground, b = gullied, mask = mountain }
    local carved = Erosion.Hydraulic{
        input = land, iterations = 20, rain = 0.5, capacity = 9.0, evaporation = 0.04,
    }
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 12, talus = math.tan(math.rad(42.0)), strength = 0.3,
    }
    return {
        height  = { value = settled, range = { -tallest * 0.1, tallest * 1.3 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

_G.Ranges = Ranges
