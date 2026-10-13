-- Lands: islands, archipelagos and continents, from one field.
--
-- The shape is not drawn. A coarse grid is built on the CPU the way the references are built by nature:
--
--   1. Bodies: elongated, rotated masses (an island, the islands of a chain, the lobes of a continent),
--      each with a spine of higher ground along its long axis, seen through a domain warp so their
--      outlines are not ellipses.
--   2. Rivers: `Erosion.StreamPower` cuts that into a dendritic drainage. Valleys that reach low ground
--      become inlets and rias once the sea is placed; ridges become peninsulas.
--   3. Sea level: placed by quantile, so `land_fraction` of the map (0.7 by default) is land whatever the
--      noise did. `size` is the side of the map in metres: a script that is exported or viewed over
--      `size` metres centred on the origin has that share of land.
--   4. The signed distance to the coast is taken on the grid, and everything else on the GPU is a
--      function of it, with ONE sign convention:
--
--          land d < 0     coast d = 0     sea d > 0
--
--      land_mask = 1 - Smoothstep(-w, w, d)   1 on land, 0 at sea, a few samples of transition
--      land_height                             the eroded ground, with gullies, shaped at the shore
--      bathymetry                              the sea floor, negative, from the distance offshore
--      surface_height                          the two joined at d = 0 (both are 0 there)
--      shore_mask                              1 on the coast, falling to 0 `shore_band` metres either side
--      coast_type                              0 a beach, 1 a cliff
--      region_id                               which landmass a cell is nearest to (1 is the largest)
--
-- The coast is then bent by a warp in three bands of wavelength (macro, meso, micro) whose amplitude is a
-- fixed fraction of it, so its fine detail is the same shape at every scale. A band whose wavelength is
-- under ~6 samples is left out.
--
-- The type of coast is an artistic heuristic, not a law: hardness and sediment (slow noise with a bias per
-- `coast` preset), the exposure to the waves (the outward normal against the direction they come from) and
-- the curvature of the coast as a bias.
--
-- A drawn outline is still available (`area`, `areas`) for when a shape must be exactly that; it goes
-- through the same rivers and the same sea level.
--
-- Run by the engine after `40_dunes.lua`.

local engine = ...
local fail   = engine.fail

local Noises, Terrain, Curves, Combine, Filters, Vec, Erosion =
    Noises, Terrain, Curves, Combine, Filters, Vec, Erosion
local type, pairs, ipairs, tostring = type, pairs, ipairs, tostring
local math, table = math, table

local Lands = {}

-- --- Keys -----------------------------------------------------------------------------------------

local ACCEPTED = {
    seed = true, resolution = true,
    -- the map and the shape
    size = true, land_fraction = true, layout = true, count = true, aspect = true, area = true, areas = true,
    roughness = true, dissection = true, cells = true,
    -- relief
    height = true, beach_width = true, beach_height = true, cliff_height = true, cliff_width = true,
    -- coast type
    coast = true, wave_from = true, hardness = true, sediment = true,
    -- the margin
    margin = true, lagoon_depth = true, shelf_width = true, shelf_depth = true, slope_width = true, abyssal_depth = true,
    shore_band = true,
}

local MARGINS = {
    -- Legible in the viewer, which shows at most ~28 km: widths are multiples of the map's half size
    -- (`scale = true`), so the same map has the same shelf at any size; depths are metres.
    compressed = { shelf_width = 0.12, shelf_depth = 70.0, slope_width = 0.2, abyssal_depth = 700.0, scale = true },
    -- Proportions of a real margin, for exports of tens of kilometres: a shelf under 1 degree
    -- (130 m over 40 km is 0.19), a slope of about 3 degrees (3400 m over 60 km is 3.2).
    geologic = { shelf_width = 40000.0, shelf_depth = 130.0, slope_width = 60000.0, abyssal_depth = 3500.0 },
}
local LAYOUTS = {
    single = true, scattered = true, hotspot = true, arc = true, atoll = true, continent = true,
}
local COASTS = {
    mixed       = { hardness = 0.0, sediment = 0.0 },
    rocky       = { hardness = 0.25, sediment = -0.15 },
    sedimentary = { hardness = -0.25, sediment = 0.3 },
    cliff       = { hardness = 0.4, sediment = -0.2 },
}

-- --- Plain Lua helpers ----------------------------------------------------------------------------

local function hasher(seed)
    return function(i, salt)
        local v = (i * 374761393 + (salt or 0) * 668265263 + seed * 2246822519) & 0xffffffff
        v = ((v ~ (v >> 13)) * 1274126177) & 0xffffffff
        return ((v ~ (v >> 16)) & 0xffff) / 65535.0
    end
end

local function smooth(x)
    if x <= 0.0 then return 0.0 end
    if x >= 1.0 then return 1.0 end
    return x * x * (3.0 - 2.0 * x)
end

-- Hashed value noise: the same on every machine. `noise(x, z, wavelength, octaves, salt)` is in [-1, 1].
local function make_noise(seed)
    local floor = math.floor
    local function corner(ix, iz, salt)
        local v = (ix * 374761393 + iz * 668265263 + salt * 2246822519 + seed * 3266489917) & 0xffffffff
        v = ((v ~ (v >> 13)) * 1274126177) & 0xffffffff
        return ((v ~ (v >> 16)) & 0xffff) / 65535.0
    end
    local function value(x, z, salt)
        local ix, iz = floor(x), floor(z)
        local fx, fz = x - ix, z - iz
        fx, fz = fx * fx * (3.0 - 2.0 * fx), fz * fz * (3.0 - 2.0 * fz)
        local a = corner(ix, iz, salt)
        local b = corner(ix + 1, iz, salt)
        local c = corner(ix, iz + 1, salt)
        local d = corner(ix + 1, iz + 1, salt)
        local low = a + (b - a) * fx
        return low + (c + (d - c) * fx - low) * fz
    end
    return function(x, z, wavelength, octaves, salt)
        local sum, amp, norm, f = 0.0, 1.0, 0.0, 1.0 / wavelength
        for o = 1, octaves do
            sum  = sum + amp * (value(x * f, z * f, salt + o * 131) * 2.0 - 1.0)
            norm = norm + amp
            amp  = amp * 0.5
            f    = f * 2.03
        end
        return sum / norm
    end
end

-- Signed distance from (x, z) to a closed polygon, negative inside.
local function polygon_distance(points, x, z)
    local best, inside = math.huge, false
    local n = #points
    for i = 1, n do
        local a, b = points[i], points[i % n + 1]
        local bx, bz = b[1] - a[1], b[2] - a[2]
        local l2 = bx * bx + bz * bz
        local u = l2 > 0 and math.min(math.max(((x - a[1]) * bx + (z - a[2]) * bz) / l2, 0.0), 1.0) or 0.0
        local dx, dz = x - a[1] - bx * u, z - a[2] - bz * u
        best = math.min(best, dx * dx + dz * dz)
        if (a[2] > z) ~= (b[2] > z) and x < a[1] + (z - a[2]) * bx / bz then
            inside = not inside
        end
    end
    best = math.sqrt(best)
    return inside and -best or best
end

local function check_polygon(points, what)
    if type(points) ~= "table" or #points < 3 then
        fail("Lands expects " .. what .. " as a list of at least 3 { x, z } points")
    end
    for i, p in ipairs(points) do
        if type(p) ~= "table" or type(p[1]) ~= "number" or type(p[2]) ~= "number" then
            fail("Lands expects point " .. i .. " of " .. what .. " to be a pair { x, z }")
        end
    end
end

-- Distance of (x, z) to a polyline.
local function line_distance(points, x, z)
    local best = math.huge
    for i = 2, #points do
        local a, b = points[i - 1], points[i]
        local bx, bz = b[1] - a[1], b[2] - a[2]
        local l2 = bx * bx + bz * bz
        local u = l2 > 0 and math.min(math.max(((x - a[1]) * bx + (z - a[2]) * bz) / l2, 0.0), 1.0) or 0.0
        local dx, dz = x - a[1] - bx * u, z - a[2] - bz * u
        best = math.min(best, dx * dx + dz * dz)
    end
    return math.sqrt(best)
end

-- --- The bodies -----------------------------------------------------------------------------------

local function body(x, z, rx, rz, rotation, strength)
    return { x = x, z = z, rx = rx, rz = rz, cos = math.cos(rotation), sin = math.sin(rotation),
             rotation = rotation, strength = strength or 1.0 }
end

-- A spine of higher ground along the body's long axis, bent and not straight.
local function add_spine(b, h, salt, wander)
    local points = {}
    local n = 5
    for i = 0, n - 1 do
        local s = (i / (n - 1) - 0.5) * 1.5 * b.rx
        local w = (h(i, salt) - 0.5) * 2.0 * wander * b.rz
        points[#points + 1] = {
            b.x + b.cos * s - b.sin * w,
            b.z + b.sin * s + b.cos * w,
        }
    end
    b.spine = points
    b.spine_width = math.max(b.rz * 0.32, 1.0)
end

-- The bodies of a layout, in metres, for a map of `size`. `target` is the land area wanted.
local function layout_bodies(layout, seed, size, count, aspect_in, target)
    local h = hasher(seed + 4000)
    local bodies = {}
    local function aspect(i, lo, hi)
        return aspect_in or (lo + (hi - lo) * h(i, 40))
    end
    if layout == "single" then
        -- One main mass nearly the width of the map, elongated, with a couple of lobes and a
        -- spine; the satellites become islets near the sea edge.
        local rot = h(1, 1) * math.pi
        local asp = aspect(1, 1.15, 1.6)
        local main = body(0.0, 0.0, size * 0.30, size * 0.30 / asp, rot)
        add_spine(main, h, 2, 0.35)
        bodies[1] = main
        for i = 1, 3 do
            local a = rot + (h(i, 3) - 0.5) * 2.4 + (i % 2) * math.pi
            local d = size * (0.16 + 0.10 * h(i, 4))
            local r = size * (0.07 + 0.06 * h(i, 5))
            local lobe = body(math.cos(a) * d, math.sin(a) * d, r * 1.3, r * 0.8, a + (h(i, 6) - 0.5))
            add_spine(lobe, h, 10 + i, 0.3)
            bodies[#bodies + 1] = lobe
        end
    elseif layout == "continent" then
        local n = math.max(count or 5, 2)
        for i = 1, n do
            local a = (i - 1) / n * 2.0 * math.pi + h(i, 1) * 0.6
            local d = size * (0.08 + 0.12 * h(i, 2))
            local r = size * (0.20 + 0.10 * h(i, 3))
            local b = body(math.cos(a) * d, math.sin(a) * d, r, r / aspect(i, 1.1, 1.7), a + (h(i, 4) - 0.5) * 1.6)
            add_spine(b, h, 20 + i, 0.4)
            bodies[#bodies + 1] = b
        end
    else
        local n = count or (layout == "atoll" and 16 or 7)
        local radii, total = {}, 0.0
        for i = 1, n do
            radii[i] = 0.05 + 0.2 * h(i, 7) ^ 2.0
            total = total + math.pi * radii[i] * radii[i]
        end
        table.sort(radii, function(p, q) return p > q end)
        -- Scaled so the bodies together are about the land wanted (their cores are ~0.6 of a full
        -- ellipse); at most 0.3 of the map for one.
        local scale = math.sqrt(target / (size * size) / (total * 0.6))
        for i = 1, n do radii[i] = math.min(radii[i] * scale, 0.24) * size end

        local rot0 = h(1, 8) * math.pi
        if layout == "scattered" then
            for i, r in ipairs(radii) do
                local asp = aspect(i, 1.1, 2.0)
                for try = 1, 80 do
                    local a  = h(i * 100 + try, 9) * 2.0 * math.pi
                    local d  = size * 0.40 * math.sqrt(h(i * 100 + try, 10))
                    local x, z = math.cos(a) * d, math.sin(a) * d
                    local ok = true
                    for _, o in ipairs(bodies) do
                        local gap = math.sqrt((x - o.x) ^ 2 + (z - o.z) ^ 2) - 1.05 * (r + o.rx)
                        if gap < 0 then ok = false; break end
                    end
                    if ok or try == 80 then
                        local b = body(x, z, r, r / asp, rot0 + (h(i, 11) - 0.5) * 1.0)
                        add_spine(b, h, 30 + i, 0.3)
                        bodies[#bodies + 1] = b
                        break
                    end
                end
            end
        elseif layout == "hotspot" or layout == "arc" then
            -- Along a line (hotspot) or an arc (island arc), end to end with a strait between each:
            -- the radii are scaled so the chain fits the map.
            local gap_ratio = 0.04
            local length = 0.0
            for _, r in ipairs(radii) do length = length + 2.0 * r + gap_ratio * size end
            local limit = size * (layout == "arc" and 0.85 or 0.82)
            local shrink = math.min(1.0, limit / length)
            for i = 1, n do radii[i] = radii[i] * shrink end
            -- Large at one end and shrinking along it (a chain ages), not sorted by size on an arc.
            if layout == "arc" then
                local mixed = {}
                for i = 1, n do mixed[i] = radii[(i * 5) % n + 1] end
                for i = 1, n do radii[i] = mixed[i] end
            end
            local total = 0.0
            for _, r in ipairs(radii) do total = total + 2.0 * r + gap_ratio * size * shrink end
            local bend = (h(2, 12) - 0.5) * 1.2
            local at = -total / 2.0
            local arc_radius = size * 0.34
            for i, r in ipairs(radii) do
                at = at + r
                local x, z, rot
                if layout == "hotspot" then
                    local f = (at / total) + 0.5
                    local sway = math.sin(f * math.pi * 1.2 + bend) * size * 0.10
                    x = math.cos(rot0) * at - math.sin(rot0) * sway
                    z = math.sin(rot0) * at + math.cos(rot0) * sway
                    rot = rot0 + bend * (f - 0.5)
                else
                    local a = rot0 + at / arc_radius
                    x, z = math.cos(a) * arc_radius, math.sin(a) * arc_radius
                    rot = a + math.pi * 0.5
                end
                local b = body(x, z, r, r / aspect(i, 1.2, 1.9), rot)
                add_spine(b, h, 40 + i, 0.3)
                bodies[#bodies + 1] = b
                at = at + r + gap_ratio * size * shrink
            end
        else -- atoll: a ring of low, tangentially elongated islets around a lagoon
            local ring = size * 0.30
            for i = 1, n do
                local a = (i - 1 + 0.4 * (h(i, 13) - 0.5)) / n * 2.0 * math.pi
                local tang = ring * 2.0 * math.pi / n * (0.65 + 0.4 * h(i, 14))
                local b = body(math.cos(a) * ring, math.sin(a) * ring, tang, tang * 0.38 * (0.8 + 0.5 * h(i, 15)),
                               a + math.pi * 0.5)
                bodies[#bodies + 1] = b
            end
        end
    end
    return bodies
end

-- --- The macro grid -------------------------------------------------------------------------------

-- Everything on the CPU: returns the grid as { n, cell, origin, relief, sdf, region, regions, level }.
local function macro(o)
    local noise = make_noise(o.seed)
    local n, size = o.cells, o.size
    local cell    = size / n
    local origin  = -size * 0.5

    local bodies = {}
    if not o.polygons then
        bodies = layout_bodies(o.layout, o.seed, size, o.count, o.aspect, o.land_fraction * size * size)
    end

    -- 1. The uplift: bodies seen through a domain warp, lumpy, with spines on top.
    local warp_amp = size * 0.06
    local lumps    = size * 0.22
    local macro_h = {}
    local border = size * 0.5
    for iz = 0, n - 1 do
        local pz = origin + iz * cell
        for ix = 0, n - 1 do
            local px = origin + ix * cell
            local wx = px + warp_amp * noise(px, pz, size * 0.30, 3, 11)
            local wz = pz + warp_amp * noise(px, pz, size * 0.30, 3, 12)
            local v = 0.0
            local ridge = 0.0
            if o.polygons then
                for _, poly in ipairs(o.polygons) do
                    v = math.max(v, smooth(-polygon_distance(poly, wx, wz) / (size * 0.05)))
                end
            else
                local keep, tail = 1.0, 0.0
                for _, b in ipairs(bodies) do
                    local dx, dz = wx - b.x, wz - b.z
                    local u = (dx * b.cos + dz * b.sin) / b.rx
                    local w = (-dx * b.sin + dz * b.cos) / b.rz
                    local d = math.sqrt(u * u + w * w)
                    -- A low skirt round every body, so the sea level (a quantile) always has ground
                    -- to choose a contour in: where it lands, valleys become inlets.
                    tail = math.max(tail, o.tail * (1.0 - smooth(d / o.reach)))
                    if d < 1.0 then
                        keep = keep * (1.0 - smooth(1.0 - d) * b.strength)
                        if b.spine then
                            local sd = line_distance(b.spine, wx, wz)
                            ridge = math.max(ridge, smooth(1.0 - sd / b.spine_width) * smooth((1.0 - d) * 2.0))
                        end
                    end
                end
                v = math.max(1.0 - keep, tail)
            end
            if o.layout == "atoll" then
                -- Only a ring of ground: the lagoon in the middle and the open sea outside are low.
                v = v * smooth(1.0 - math.abs(math.sqrt(px * px + pz * pz) - size * 0.30) / (size * 0.14))
            end
            -- Lumps at two scales: peninsulas and bays, and the small irregularity of a coast.
            local lump = 0.5 + 0.5 * noise(px, pz, lumps, 4, 21)
            local fine = noise(px, pz, lumps * 0.25, 3, 22)
            local value = v * (0.55 + 0.75 * lump) + 0.18 * fine * v + 0.55 * ridge * v
            -- The map edge is sea.
            local edge = 1.0 - math.max(math.abs(px), math.abs(pz)) / border
            value = value * smooth(edge / 0.12)
            macro_h[iz * n + ix + 1] = value
        end
    end

    -- 2. Rivers. Uplift follows the macro height, so valleys cut towards the sea.
    local relief  = o.height
    local steps   = 300
    local height, uplift = {}, {}
    for i = 1, n * n do
        local u = math.max(macro_h[i], 0.0) * relief
        uplift[i] = u / steps * o.lift
        local rough = smooth(u / (relief * 0.05))
        local px = origin + ((i - 1) % n) * cell
        local pz = origin + ((i - 1) // n) * cell
        height[i] = u * 0.02 + (relief * 0.08 * noise(px, pz, size * 0.12, 2, 31)) * rough
        if height[i] < 0.0 then height[i] = 0.0 end
    end
    local carved = Erosion.StreamPower{
        size = { n, n }, cell = cell, height = height, uplift = uplift, iterations = steps,
        k = o.k, m = 0.5, talus = 0.6, diffusion = 0.012 * cell * cell, refresh = 4,
    }

    -- 3. Sea level by quantile: `land_fraction` of the cells are above it.
    local sorted = {}
    for i = 1, n * n do sorted[i] = carved[i] end
    table.sort(sorted)
    local index = math.min(math.max(math.floor((1.0 - o.land_fraction) * n * n) + 1, 1), n * n)
    -- Flat ground at the base level is sea, so a layout that cannot reach `land_fraction` (a thin chain)
    -- gets what it has and not an arbitrary patch of plain.
    local level = math.max(sorted[index], math.max(relief * 0.02, 3.0))
    local peak = 0.0
    local relief_cells = {}
    for i = 1, n * n do
        local r = carved[i] - level
        if r < 0.0 then r = 0.0 end
        relief_cells[i] = r
        if r > peak then peak = r end
    end
    -- The height asked for is the highest point.
    local factor = peak > 0.0 and (relief / peak) or 1.0
    for i = 1, n * n do relief_cells[i] = relief_cells[i] * factor end

    -- 4. Signed distance and landmass labels on the grid.
    local land = {}
    for i = 1, n * n do land[i] = carved[i] > level end

    -- Connected components of land (4-connected), largest first.
    local label, areas = {}, {}
    local count = 0
    for i = 1, n * n do
        if land[i] and not label[i] then
            count = count + 1
            local stack, members = { i }, 0
            label[i] = count
            while #stack > 0 do
                local c = stack[#stack]
                stack[#stack] = nil
                members = members + 1
                local cx, cz = (c - 1) % n, (c - 1) // n
                local function visit(nx, nz)
                    if nx >= 0 and nz >= 0 and nx < n and nz < n then
                        local j = nz * n + nx + 1
                        if land[j] and not label[j] then
                            label[j] = count
                            stack[#stack + 1] = j
                        end
                    end
                end
                visit(cx - 1, cz); visit(cx + 1, cz); visit(cx, cz - 1); visit(cx, cz + 1)
            end
            areas[count] = members
        end
    end
    local order = {}
    for k = 1, count do order[k] = k end
    table.sort(order, function(p, q) return areas[p] > areas[q] end)
    local rank = {}
    for r, k in ipairs(order) do rank[k] = r end

    -- A chamfer distance, in cells, to the nearest cell where `source(i)` is true, carrying that
    -- cell's rank.
    local D1, D2 = 1.0, 1.41421356
    local function chamfer(source)
        local dist, lab = {}, {}
        local inf = 1e9
        for i = 1, n * n do
            if source(i) then dist[i] = 0.0; lab[i] = label[i] and rank[label[i]] or 0
            else dist[i] = inf; lab[i] = 0 end
        end
        local function relax(i, j, w)
            local d = dist[j] + w
            if d < dist[i] then dist[i] = d; lab[i] = lab[j] end
        end
        for z = 0, n - 1 do
            for x = 0, n - 1 do
                local i = z * n + x + 1
                if x > 0 then relax(i, i - 1, D1) end
                if z > 0 then
                    relax(i, i - n, D1)
                    if x > 0 then relax(i, i - n - 1, D2) end
                    if x < n - 1 then relax(i, i - n + 1, D2) end
                end
            end
        end
        for z = n - 1, 0, -1 do
            for x = n - 1, 0, -1 do
                local i = z * n + x + 1
                if x < n - 1 then relax(i, i + 1, D1) end
                if z < n - 1 then
                    relax(i, i + n, D1)
                    if x < n - 1 then relax(i, i + n + 1, D2) end
                    if x > 0 then relax(i, i + n - 1, D2) end
                end
            end
        end
        return dist, lab
    end
    local to_land, nearest = chamfer(function(i) return land[i] end)
    local to_sea = chamfer(function(i) return not land[i] end)
    local sdf = {}
    for i = 1, n * n do
        -- Cell centres are a half cell either side of the boundary.
        if land[i] then
            sdf[i] = -(math.min(to_sea[i], 1e6) - 0.5) * cell
        else
            sdf[i] = (math.min(to_land[i], 1e6) - 0.5) * cell
        end
        if count == 0 then sdf[i] = 1e5 end
    end
    -- One blur takes the chamfer's octagonal contours off.
    local smoothed = {}
    for z = 0, n - 1 do
        for x = 0, n - 1 do
            local sum, w = 0.0, 0.0
            for dz = -1, 1 do
                for dx = -1, 1 do
                    local xx, zz = math.min(math.max(x + dx, 0), n - 1), math.min(math.max(z + dz, 0), n - 1)
                    local k = (dx == 0 and dz == 0) and 4.0 or ((dx == 0 or dz == 0) and 2.0 or 1.0)
                    sum = sum + sdf[zz * n + xx + 1] * k
                    w = w + k
                end
            end
            smoothed[z * n + x + 1] = sum / w
        end
    end
    local region = {}
    for i = 1, n * n do
        region[i] = math.max(land[i] and (rank[label[i]] or 1) or nearest[i], 1)
    end
    -- The lagoon of an atoll: sea inside the ring, shallow whatever its distance from the coast.
    local lagoon = {}
    for i = 1, n * n do
        local px = origin + ((i - 1) % n) * cell
        local pz = origin + ((i - 1) // n) * cell
        lagoon[i] = (o.layout == "atoll" and not land[i] and math.sqrt(px * px + pz * pz) < size * 0.30) and 1.0 or 0.0
    end
    return {
        n = n, cell = cell, origin = origin, relief = relief_cells, sdf = smoothed, region = region, lagoon = lagoon,
        regions = math.max(count, 1), level = level, masses = count,
    }
end

-- --- Lands.Island ---------------------------------------------------------------------------------

local function product_ranges(r)
    return {
        coast_sdf      = { -r.depth_max * 0.3, r.depth_max },
        land_mask      = { 0.0, 1.0 },
        land_height    = { 0.0, r.height_max },
        bathymetry     = { -r.depth_max, 0.0 },
        surface_height = { -r.depth_max, r.height_max },
        shore_mask     = { 0.0, 1.0 },
        coast_type     = { 0.0, 1.0 },
        region_id      = { 1.0, math.max(r.regions, 2) },
    }
end

function Lands.Island(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Lands.Island expects a table of named arguments, as in Lands.Island{ ... }")
    end
    for key in pairs(t) do
        if not ACCEPTED[key] then
            local list = {}
            for k in pairs(ACCEPTED) do list[#list + 1] = k end
            table.sort(list)
            fail("Lands.Island does not take '" .. tostring(key) .. "'; it accepts " .. table.concat(list, ", "))
        end
    end

    local seed       = t.seed or 0
    local resolution = t.resolution or 8.0
    local roughness  = t.roughness or 0.6
    local relief     = t.height or 600.0
    local size       = t.size or 24000.0
    local layout     = t.layout or "single"
    local margin     = MARGINS[t.margin or "compressed"]
    local coast      = COASTS[t.coast or "mixed"]
    if not LAYOUTS[layout] then
        fail("Lands.Island layout must be 'single', 'scattered', 'hotspot', 'arc', 'atoll' or 'continent', got '"
             .. tostring(layout) .. "'")
    end
    if margin == nil then
        fail("Lands.Island margin must be 'compressed' or 'geologic', got '" .. tostring(t.margin) .. "'")
    end
    if coast == nil then
        fail("Lands.Island coast must be 'mixed', 'rocky', 'sedimentary' or 'cliff', got '" .. tostring(t.coast) .. "'")
    end
    if roughness < 0 or roughness > 1 or resolution <= 0 or relief <= 0 or size <= 0 then
        fail("Lands.Island needs roughness in [0, 1], and resolution, height and size greater than zero")
    end
    -- 70 % of a map, unless the layout cannot: islands kept apart by straits fill about 55 % of a square, a
    -- ring round a lagoon or a chain of islands much less. Asking for more merges them into one mass.
    local land_fraction = t.land_fraction
        or ((layout == "atoll" and 0.4) or (layout == "scattered" and 0.55)
            or ((layout == "hotspot" or layout == "arc") and 0.45) or 0.7)
    if land_fraction <= 0.02 or land_fraction >= 0.95 then
        fail("Lands.Island needs land_fraction between 0.02 and 0.95")
    end
    local dissection = t.dissection or 1.0
    if dissection < 0 or dissection > 3 then
        fail("Lands.Island needs dissection between 0 and 3")
    end

    local polygons
    if t.area ~= nil or t.areas ~= nil then
        polygons = {}
        if t.area ~= nil then
            check_polygon(t.area, "'area'")
            polygons[1] = t.area
        end
        for i, points in ipairs(t.areas or {}) do
            check_polygon(points, "polygon " .. i .. " of 'areas'")
            polygons[#polygons + 1] = points
        end
    end

    local extent = size * 0.5
    local shelf_depth   = t.shelf_depth or margin.shelf_depth
    local abyssal_depth = t.abyssal_depth or margin.abyssal_depth
    local width_scale   = margin.scale and extent or 1.0
    local shelf_width   = t.shelf_width or margin.shelf_width * width_scale
    local slope_width   = t.slope_width or margin.slope_width * width_scale
    if abyssal_depth <= shelf_depth or shelf_width <= 0 or slope_width <= 0 then
        fail("Lands.Island needs abyssal_depth greater than shelf_depth and positive widths")
    end

    -- 1. The macro grid, on the CPU.
    local cells = math.min(math.max(t.cells or 256, 64), 512)
    local grid = macro({
        seed = seed, size = size, cells = cells, layout = layout, count = t.count, aspect = t.aspect,
        land_fraction = land_fraction, height = relief, polygons = polygons,
        lift = 2.4, k = 0.016 * dissection,
        tail = (layout == "single" or layout == "continent") and 0.35 or (layout == "scattered" and 0.09 or 0.1),
        reach = (layout == "single" or layout == "continent") and 2.2 or (layout == "scattered" and 1.5 or 1.5),
    })

    local c = Terrain.Coords{}

    -- 2. The coast's own warp, in bands. Each is a fixed fraction of its wavelength (the slope of the
    -- offset is the same at every scale, under 0.6 with `roughness` 1) and each is dropped when the
    -- grid cannot show it.
    local ratio = 0.05 + 0.075 * roughness
    local bands = {
        { name = "macro", wavelength = size * 0.06 },
        { name = "meso",  wavelength = size * 0.02 },
        { name = "micro", wavelength = math.max(size * 0.006, 40.0) },
    }
    local active = {}
    local ox, oz = c.x, c.z
    for i, band in ipairs(bands) do
        if band.wavelength >= 6.0 * resolution and roughness > 0 then
            local amplitude = ratio * band.wavelength
            local nx = Noises.fBm{ frequency = 1.0 / band.wavelength, octaves = 2, amplitude = amplitude, seed = seed + 11 * i }
            local nz = Noises.fBm{ frequency = 1.0 / band.wavelength, octaves = 2, amplitude = amplitude, seed = seed + 11 * i + 5 }
            ox, oz = ox + nx.value, oz + nz.value
            active[#active + 1] = band.name
        end
    end
    local position = Vec.Combine{ ox, oz }

    -- 3. The signed distance and the ground, read from the grid at the warped position. Land is negative.
    local function sample(values)
        return Terrain.Grid{
            values = values, size = { grid.n, grid.n }, origin = { grid.origin, grid.origin },
            cell = grid.cell, position = position,
        }
    end
    local coast_sdf = sample(grid.sdf)
    local transition = math.max(resolution * 1.5, 12.0)
    local land_mask  = 1.0 - Curves.Smoothstep{ input = coast_sdf, min = -transition, max = transition }
    local region_id  = Curves.Terrace{ input = sample(grid.region), step = 1.0, sharpness = 1.0 }

    -- 4. The type of coast: 0 a beach, 1 a cliff.
    local wave = math.rad(t.wave_from or 225.0)
    local gradient = Terrain.Gradient{ input = coast_sdf }
    local facing = gradient.x * math.cos(wave) + gradient.z * math.sin(wave)
    local exposure = Curves.Clamp{ input = 0.5 + 0.5 * facing, min = 0.0, max = 1.0 }
    local blur_radius = math.min(math.max(math.floor(size * 0.01 / resolution + 0.5), 2), 24)
    local convexity = Curves.Clamp{
        input = (Filters.Blur{ input = coast_sdf, radius = blur_radius } - coast_sdf) / (size * 0.01),
        min = -1.0, max = 1.0,
    } * 0.5 + 0.5
    local function regional(offset, salt)
        return Curves.Smoothstep{
            input = Noises.fBm{ frequency = 1.0 / (size * 0.25), octaves = 2, amplitude = 0.6, seed = seed + salt }.value
                    + offset,
            min = -0.5, max = 0.5,
        }
    end
    local hard = regional((t.hardness or 0.0) + coast.hardness, 71)
    local sed  = regional((t.sediment or 0.0) + coast.sediment, 73)
    local raw  = 0.50 * hard + 0.22 * exposure + 0.13 * convexity + 0.15 - 0.45 * sed
    local coast_type = Curves.Smoothstep{ input = raw, min = 0.20, max = 0.70 }

    -- 5. The ground: the grid's eroded relief plus gullies at the scale the grid cannot show, over a beach
    -- or cliff profile at the shore.
    local u = Curves.Clamp{ input = -coast_sdf, min = 0.0, max = 1.0e6 }
    local beach_w = t.beach_width or math.min(math.max(size * 0.0025, 20.0), 300.0)
    local cliff_w = t.cliff_width or math.min(math.max(size * 0.001, 10.0), 100.0)
    local beach_h = t.beach_height or 6.0
    local cliff_h = t.cliff_height or math.min(relief * 0.15, 80.0)
    local sb = Curves.Clamp{ input = u / beach_w, min = 0.0, max = 1.0 }
    local sc = Curves.Clamp{ input = u / cliff_w, min = 0.0, max = 1.0 }
    local beach = beach_h * (1.0 - (1.0 - sb) * (1.0 - sb))
    local cliff = cliff_h * (1.0 - (1.0 - sc) * (1.0 - sc))
    local shore = Combine.Blend{ a = beach, b = cliff, mask = coast_type }

    local eroded = sample(grid.relief)
    local gullied = Erosion.Gullies{
        input = eroded, cell = math.max(size * 0.012, 60.0), octaves = 3, strength = math.min(relief * 0.04, 30.0),
        full = 0.5, seed = seed + 53,
    }
    -- The shore profile rises to the grid's ground over a few hundred metres, so a high coast is a cliff
    -- and a low one a beach.
    local inland = Curves.Smoothstep{ input = u, min = 0.0, max = math.max(size * 0.01, 60.0) }
    local land_height = Curves.Clamp{
        input = Combine.Blend{ a = shore, b = Combine.Max{ a = gullied, b = shore }, mask = inland },
        min = 0.0, max = relief * 2.5,
    }

    -- 6. The sea floor: a shallow beach face, a shelf, a break, a slope and an abyssal plain. Steeper
    -- offshore of a cliff.
    local off = Curves.Clamp{ input = coast_sdf, min = 0.0, max = 1.0e7 }
    local sf = Curves.Clamp{ input = off / 160.0, min = 0.0, max = 1.0 }
    local face = (3.0 + 22.0 * coast_type) * (1.0 - (1.0 - sf) * (1.0 - sf))
    local on_shelf = shelf_depth * Curves.Clamp{ input = off / shelf_width, min = 0.0, max = 1.0 }
    local sl = Curves.Clamp{ input = (off - shelf_width) / slope_width, min = 0.0, max = 1.0 }
    local on_slope = (abyssal_depth - shelf_depth) * (sl * sl * (3.0 - 2.0 * sl))
    local floor_noise = Noises.fBm{
        frequency = 1.0 / (size * 0.3), octaves = 3, amplitude = 0.03, seed = seed + 51,
    }
    local depth = (face + on_shelf + on_slope) * (1.0 + floor_noise.value)
    if layout == "atoll" then
        local inside = Curves.Smoothstep{ input = sample(grid.lagoon), min = 0.3, max = 0.7 }
        depth = Combine.Blend{
            a = depth, b = Curves.Clamp{ input = depth, min = 0.0, max = t.lagoon_depth or 45.0 }, mask = inside,
        }
    end
    local bathymetry = -depth

    local surface_height = Combine.Blend{ a = bathymetry, b = land_height, mask = land_mask }

    local band = t.shore_band or math.max(size * 0.01, 4.0 * resolution)
    local shore_mask = 1.0 - Curves.Smoothstep{
        input = Combine.Max{ a = coast_sdf, b = -coast_sdf }, min = 0.0, max = band,
    }

    local island = {
        coast_sdf = coast_sdf, land_mask = land_mask, land_height = land_height,
        bathymetry = bathymetry, surface_height = surface_height, shore_mask = shore_mask,
        coast_type = coast_type, region_id = region_id,
        info = {
            size = size, extent = extent, layout = layout, bands = active, land_fraction = land_fraction,
            regions = grid.regions, masses = grid.masses, sea_level_in_grid = grid.level,
            margin = {
                shelf_width = shelf_width, shelf_depth = shelf_depth, slope_width = slope_width,
                abyssal_depth = abyssal_depth,
            },
            height_max = relief * 1.6, depth_max = abyssal_depth * 1.05,
        },
    }

    -- The script's outputs: `height` and `normals` always, then whatever is asked for by name (a script
    -- holds at most eight outputs, so the rest are on demand).
    function island:outputs(names)
        local ranges = product_ranges(self.info)
        local result = {
            height  = { value = self.surface_height, range = ranges.surface_height },
            normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = self.surface_height } } },
            look    = { palette = "coast" },
        }
        for _, name in ipairs(names or {}) do
            if ranges[name] == nil or name == "surface_height" then
                if name ~= "surface_height" then
                    fail("Lands: '" .. tostring(name) .. "' is not a product; they are coast_sdf, land_mask, "
                         .. "land_height, bathymetry, shore_mask, coast_type and region_id (height is the surface)")
                end
            else
                result[name] = { value = self[name], range = ranges[name] }
            end
        end
        return result
    end
    return island
end

-- An archipelago: `Lands.Island` with a `layout` of scattered, hotspot, arc or atoll.
function Lands.Archipelago(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Lands.Archipelago expects a table of named arguments, as in Lands.Archipelago{ ... }")
    end
    local copy = {}
    for key, value in pairs(t) do copy[key] = value end
    copy.layout = copy.layout or "scattered"
    if copy.layout == "single" or copy.layout == "continent" then
        fail("Lands.Archipelago layout must be 'scattered', 'hotspot', 'arc' or 'atoll'")
    end
    if copy.layout == "atoll" then
        -- Low ground, a lagoon of a few tens of metres and a steep outer wall.
        local size = copy.size or 24000.0
        copy.height       = copy.height or 25.0
        copy.shelf_width  = copy.shelf_width or size * 0.05
        copy.slope_width  = copy.slope_width or size * 0.08
        copy.dissection   = copy.dissection or 0.2
        copy.beach_width  = copy.beach_width or 60.0
        copy.cliff_height = copy.cliff_height or 6.0
    end
    return Lands.Island(copy)
end

-- A continent, as a region and not as a planet: several large lobes with ranges along them, drained by
-- rivers that cut it into valleys, peninsulas and gulfs.
function Lands.Continent(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Lands.Continent expects a table of named arguments, as in Lands.Continent{ ... }")
    end
    local copy = {}
    for key, value in pairs(t) do copy[key] = value end
    copy.layout = "continent"
    copy.height = copy.height or 1800.0
    return Lands.Island(copy)
end

_G.Lands = Lands
