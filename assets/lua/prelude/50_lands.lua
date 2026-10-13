-- Lands: islands, archipelagos and continents, from one field.
--
-- `Lands.Island` is the core. Everything it makes is a function of `coast_sdf`, the signed distance to
-- the coast in metres, with ONE convention used everywhere here:
--
--     land  d < 0     coast  d = 0     sea  d > 0
--
--   land_mask  = 1 - Smoothstep(-w, w, d)   1 on land, 0 at sea, a few samples of transition
--   land_height                              the ground above the sea, from the distance inland (-d)
--   bathymetry                               the sea floor, from the distance offshore (d), negative
--   surface_height                           the two joined at d = 0 (both are 0 there)
--   shore_mask                               1 on the coast, falling to 0 `shore_band` metres either side
--   coast_type                               0 a beach, 1 a cliff
--
-- The outline is a drawn polygon (`area`) or generated from a seed (`radius`), then bent by a warp in
-- three bands of wavelength (macro, meso, micro) whose amplitude is a fixed fraction of it, so the
-- coast is the same shape at every scale and the warp never folds (its slope stays under 0.6). A band
-- whose wavelength is under ~6 samples is left out, which would only alias: the silhouette (macro and
-- meso) is the same at every level of detail, the micro band appears as the spacing allows.
--
-- The type of coast is an artistic heuristic, not a law: hardness and sediment (slow noise with a bias
-- per `coast` preset), the exposure to the waves (the outward normal against the direction they come
-- from) and the curvature of the coast (a blur of the distance against the distance, convex positive)
-- as a bias. Curvature alone would make every headland a cliff and every bay a beach.
--
-- `Lands.Archipelago` is the same function with a `layout` (scattered, hotspot, arc or atoll): the
-- islands are placed on the CPU with a minimum channel that the warp cannot close, each one is its own
-- outline, and `region_id` says which island a cell is nearest to. `areas` takes several drawn polygons.
--
-- Islets (`breakage`) are primitives, not a side effect of the warp: placed on the CPU outside the main
-- outline with a minimum channel and a minimum distance between them, so they cannot touch the island
-- and cannot leave a hole in it.
--
-- Run by the engine after `40_dunes.lua`.

local engine = ...
local fail   = engine.fail

local Noises, Terrain, Curves, Combine, Filters, Vec, Shapes =
    Noises, Terrain, Curves, Combine, Filters, Vec, Shapes
local type, pairs, ipairs, tostring = type, pairs, ipairs, tostring
local math, table = math, table

local Lands = {}

-- --- Keys -----------------------------------------------------------------------------------------

local ACCEPTED = {
    seed = true, resolution = true,
    -- the outline
    area = true, areas = true, radius = true, lumpiness = true, roughness = true,
    -- an archipelago: `layout` places `count` islands within `radius` of the centre
    layout = true, count = true, island_radius = true, arc_span = true,
    -- islets
    breakage = true, islet_radius = true, channel_min = true,
    -- relief
    height = true, hypsometry = true, beach_width = true, beach_height = true, cliff_height = true,
    cliff_width = true,
    -- coast type
    coast = true, wave_from = true, hardness = true, sediment = true,
    -- the margin
    margin = true, shelf_width = true, shelf_depth = true, slope_width = true, abyssal_depth = true,
    shore_band = true,
}

local MARGINS = {
    -- Legible in the viewer, which shows at most ~28 km: widths are multiples of the island's radius
    -- (`scale = true`), so the same island has the same shelf at any size; depths are metres.
    compressed = { shelf_width = 1.0, shelf_depth = 70.0, slope_width = 1.5, abyssal_depth = 700.0, scale = true },
    -- Proportions of a real margin, for exports of tens of kilometres: a shelf under 1 degree
    -- (130 m over 40 km is 0.19), a slope of about 3 degrees (3400 m over 60 km is 3.2).
    geologic = { shelf_width = 40000.0, shelf_depth = 130.0, slope_width = 60000.0, abyssal_depth = 3500.0 },
}
local LAYOUTS = { scattered = true, hotspot = true, arc = true, atoll = true }
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

local function check_polygon(points)
    if type(points) ~= "table" or #points < 3 then
        fail("Lands.Island expects 'area' as a list of at least 3 { x, z } points")
    end
    for i, p in ipairs(points) do
        if type(p) ~= "table" or type(p[1]) ~= "number" or type(p[2]) ~= "number" then
            fail("Lands.Island expects point " .. i .. " of 'area' to be a pair { x, z }")
        end
    end
end

-- A generated outline: a core and a ring of lobes, as discs { x, z, r }, whose smooth union is a blob.
local function blob(seed, radius, lumpiness)
    local h = hasher(seed + 5000)
    local discs = { { 0.0, 0.0, radius * 0.5 } }
    local count = 5 + math.floor(h(1) * 3)
    for i = 1, count do
        local angle = (i - 1 + 0.7 * h(i, 2)) / count * 2.0 * math.pi
        -- Lobes reach out past the core: their centres sit 0.45 to 0.95 radius out and their own radius
        -- is 0.2 to 0.42, so the outline has bays and capes and not the edge of one disc.
        local rho   = radius * (0.45 + 0.5 * lumpiness * h(i, 3))
        local r     = radius * (0.2 + 0.22 * h(i, 4))
        discs[#discs + 1] = { math.cos(angle) * rho, math.sin(angle) * rho, r }
    end
    return discs
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
        region_id      = { 1.0, math.max(r.regions, 1) },
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
    local relief     = t.height or 420.0
    local margin     = MARGINS[t.margin or "compressed"]
    local coast      = COASTS[t.coast or "mixed"]
    if margin == nil then
        fail("Lands.Island margin must be 'compressed' or 'geologic', got '" .. tostring(t.margin) .. "'")
    end
    if coast == nil then
        fail("Lands.Island coast must be 'mixed', 'rocky', 'sedimentary' or 'cliff', got '" .. tostring(t.coast) .. "'")
    end
    if roughness < 0 or roughness > 1 or resolution <= 0 or relief <= 0 then
        fail("Lands.Island needs roughness in [0, 1], and resolution and height greater than zero")
    end

    local shelf_depth   = t.shelf_depth or margin.shelf_depth
    local abyssal_depth = t.abyssal_depth or margin.abyssal_depth

    -- The outline, and the size of the thing: `extent` is its radius, in metres.
    local shape_discs, polygon
    local extent
    local layout = t.layout
    if layout ~= nil and not LAYOUTS[layout] then
        fail("Lands.Island layout must be 'scattered', 'hotspot', 'arc' or 'atoll', got '" .. tostring(layout) .. "'")
    end
    if layout then
        extent = t.radius or 12000.0
        if extent <= 0 then fail("Lands.Island needs radius greater than zero") end
        shape_discs = {}
    elseif t.areas ~= nil then
        extent = 0.0
    elseif t.area ~= nil then
        check_polygon(t.area)
        polygon = t.area
        extent = 0.0
        for _, p in ipairs(polygon) do extent = math.max(extent, math.sqrt(p[1] * p[1] + p[2] * p[2])) end
    else
        extent = t.radius or 3000.0
        if extent <= 0 then fail("Lands.Island needs radius greater than zero") end
        shape_discs = blob(seed, extent, t.lumpiness or 0.8)
    end
    if t.areas ~= nil then
        if t.area ~= nil or layout or type(t.areas) ~= "table" or #t.areas == 0 then
            fail("Lands.Island takes 'areas' (a list of polygons) alone, without 'area' or 'layout'")
        end
        extent = 0.0
        for _, points in ipairs(t.areas) do
            check_polygon(points)
            for _, p in ipairs(points) do extent = math.max(extent, math.sqrt(p[1] * p[1] + p[2] * p[2])) end
        end
    end
    -- The scale the warp and the noises are tuned to: one island's, not the whole archipelago's.
    local size = extent * 2.0
    if layout then
        size = extent * (layout == "atoll" and 0.2 or 0.3)
    elseif t.areas then
        size = size / math.sqrt(#t.areas)
    end

    -- The margin's widths: multiples of the radius for the compressed preset, metres otherwise.
    local width_scale = margin.scale and extent or 1.0
    local shelf_width = t.shelf_width or margin.shelf_width * width_scale
    local slope_width = t.slope_width or margin.slope_width * width_scale
    if abyssal_depth <= shelf_depth or shelf_width <= 0 or slope_width <= 0 then
        fail("Lands.Island needs abyssal_depth greater than shelf_depth and positive widths")
    end

    local c = Terrain.Coords{}

    -- 1. The warp, in bands. Each is a fixed fraction of its wavelength, so the slope of the offset is
    -- the same at every scale (2 pi ratio, under 0.6 with `roughness` 1), and each is dropped when the
    -- grid cannot show it.
    local ratio = 0.05 + 0.075 * roughness
    local bands = {
        { name = "macro", wavelength = size * 0.50 },
        { name = "meso",  wavelength = size * 0.15 },
        { name = "micro", wavelength = math.max(size * 0.045, 40.0) },
    }
    local active, spread = {}, 0.0
    local ox, oz = c.x, c.z
    for i, band in ipairs(bands) do
        if band.wavelength >= 6.0 * resolution and roughness > 0 then
            local amplitude = ratio * band.wavelength
            local nx = Noises.fBm{ frequency = 1.0 / band.wavelength, octaves = 2, amplitude = amplitude, seed = seed + 11 * i }
            local nz = Noises.fBm{ frequency = 1.0 / band.wavelength, octaves = 2, amplitude = amplitude, seed = seed + 11 * i + 5 }
            ox, oz = ox + nx.value, oz + nz.value
            active[#active + 1] = band.name
            spread = spread + amplitude
        end
    end
    local position = Vec.Combine{ ox, oz }

    -- 2. The outline as groups: each one is a drawn polygon or a list of discs whose smooth union is an
    -- island. An archipelago's islands are placed here, now that the warp's reach is known, so that no
    -- two can be closed into one by it.
    local groups = {}
    local function add_island(x, z, r, k)
        local discs = {}
        for _, d in ipairs(blob(seed + 17 * k, r, t.lumpiness or 0.8)) do
            discs[#discs + 1] = { d[1] + x, d[2] + z, d[3] }
        end
        groups[#groups + 1] = { discs = discs, smooth = r * 0.1, radius = r, x = x, z = z }
    end
    local channel = t.channel_min or math.max(extent * (layout and 0.03 or 0.05), 4.0 * resolution)
    local count   = t.count or (layout == "atoll" and 18 or 8)
    if layout then
        local h = hasher(seed + 7000)
        local range = t.island_radius or { extent * 0.05, extent * 0.2 }
        local reach_of = function(r) return r * 1.4 end -- an island's outline reaches 1.4 of its radius
        local need = channel + 2.0 * spread
        local placed = {}
        local function fits(x, z, r)
            for _, o in ipairs(placed) do
                if math.sqrt((x - o[1]) ^ 2 + (z - o[2]) ^ 2) - reach_of(r) - reach_of(o[3]) < need then return false end
            end
            return true
        end
        if layout == "scattered" then
            local radii = {}
            for i = 1, count do radii[i] = range[1] + (range[2] - range[1]) * h(i, 1) ^ 2.2 end
            table.sort(radii, function(a, b) return a > b end)
            for i, r in ipairs(radii) do
                for try = 1, 200 do
                    local a  = h(i * 1000 + try, 2) * 2.0 * math.pi
                    local rho = extent * 0.85 * math.sqrt(h(i * 1000 + try, 3))
                    local x, z = math.cos(a) * rho, math.sin(a) * rho
                    if fits(x, z, r) then placed[#placed + 1] = { x, z, r }; break end
                end
            end
        elseif layout == "hotspot" then
            -- A chain along a gently bent line, the youngest and largest at one end, shrinking along it.
            local angle, bend = h(1, 4) * 2.0 * math.pi, (h(2, 4) - 0.5) * 0.6
            local top = range[2]
            for _ = 1, 24 do
                local length, r0 = 0.0, {}
                for i = 1, count do
                    local f = (i - 1) / math.max(count - 1, 1)
                    r0[i] = range[1] + (top - range[1]) * (1.0 - f) ^ 1.5
                    length = length + 2.0 * reach_of(r0[i]) + need * (1.0 + 0.5 * h(i, 5))
                end
                if length <= extent * 1.8 then
                    local at = -length / 2.0
                    for i = 1, count do
                        local step = reach_of(r0[i])
                        at = at + step
                        local sway = math.sin(bend * 2.0 * (at / length)) * length * 0.25
                        placed[#placed + 1] = {
                            math.cos(angle) * at - math.sin(angle) * sway,
                            math.sin(angle) * at + math.cos(angle) * sway, r0[i],
                        }
                        at = at + step + need * (1.0 + 0.5 * h(i, 5))
                    end
                    break
                end
                top = top * 0.9
            end
        elseif layout == "arc" then
            -- Islands on an arc of a circle, as along a subduction trench; each is cut to fit its neighbours.
            local rc, span = extent * 0.8, math.rad(t.arc_span or 130.0)
            local a0 = h(1, 6) * 2.0 * math.pi
            for i = 1, count do
                local a = a0 + span * ((i - 1) / math.max(count - 1, 1) - 0.5) + (h(i, 7) - 0.5) * span / count * 0.4
                placed[i] = { math.cos(a) * rc, math.sin(a) * rc, range[1] + (range[2] - range[1]) * h(i, 8) ^ 1.5 }
            end
            for i = 2, count do
                local p, q = placed[i - 1], placed[i]
                local d = math.sqrt((p[1] - q[1]) ^ 2 + (p[2] - q[2]) ^ 2)
                local room = (d - need) / 1.4
                local sum = p[3] + q[3]
                if sum > room then
                    p[3], q[3] = p[3] * room / sum, q[3] * room / sum
                end
            end
        else -- atoll: a ring of islets that touch or leave passes, around a lagoon
            local ring = extent * 0.6
            local chord = 2.0 * ring * math.sin(math.pi / count)
            for i = 1, count do
                local a = (i - 1 + 0.3 * (h(i, 9) - 0.5)) / count * 2.0 * math.pi
                local r = chord * 0.5 * (0.85 + 0.9 * h(i, 10)) / 1.4
                placed[#placed + 1] = { math.cos(a) * ring, math.sin(a) * ring, r }
            end
        end
        for k, o in ipairs(placed) do add_island(o[1], o[2], o[3], k) end
    elseif t.areas then
        for _, points in ipairs(t.areas) do groups[#groups + 1] = { polygon = points } end
    elseif polygon then
        groups[1] = { polygon = polygon }
    else
        local discs = {}
        for i, d in ipairs(shape_discs) do discs[i] = d end
        groups[1] = { discs = discs, smooth = extent * 0.1, radius = extent, x = 0.0, z = 0.0 }
    end
    if #groups == 0 then
        fail("Lands.Island could not place any island: the radius is too small for the channel")
    end
    local function outline_distance(x, z)
        local best = math.huge
        for _, g in ipairs(groups) do
            if g.polygon then
                best = math.min(best, polygon_distance(g.polygon, x, z))
            else
                for _, d in ipairs(g.discs) do
                    best = math.min(best, math.sqrt((x - d[1]) ^ 2 + (z - d[2]) ^ 2) - d[3])
                end
            end
        end
        return best
    end

    -- Islets, placed outside the outline on the CPU. Room is kept for the warp on both sides.
    local islets = {}
    local breakage = t.breakage or 0
    if breakage > 0 then
        local r_range     = t.islet_radius or { extent * 0.025, extent * 0.09 }
        local channel_min = t.channel_min or math.max(extent * 0.05, 4.0 * resolution)
        local h = hasher(seed + 9000)
        local tries = 0
        while #islets < breakage and tries < breakage * 60 do
            tries = tries + 1
            local angle = h(tries, 1) * 2.0 * math.pi
            local rho   = extent * (0.9 + 0.8 * h(tries, 2))
            local r     = r_range[1] + (r_range[2] - r_range[1]) * h(tries, 3) ^ 2
            local x, z  = math.cos(angle) * rho, math.sin(angle) * rho
            local ok = outline_distance(x, z) >= channel_min + r + 2.0 * spread
            for _, other in ipairs(islets) do
                local gap = math.sqrt((x - other[1]) ^ 2 + (z - other[2]) ^ 2) - r - other[3]
                if gap < channel_min + 2.0 * spread then ok = false end
            end
            if ok then islets[#islets + 1] = { x, z, r } end
        end
    end

    -- 3. The signed distance. Land is negative.
    local group_sdfs, coast_sdf = {}, nil
    for gi, g in ipairs(groups) do
        local sdf
        if g.polygon then
            sdf = Shapes.Polygon{ points = g.polygon, position = position }
        else
            local shapes = {}
            for _, d in ipairs(g.discs) do
                shapes[#shapes + 1] = Shapes.Circle{ at = { d[1], d[2] }, radius = d[3], position = position }
            end
            sdf = Shapes.Union{ shapes[1], table.unpack(shapes, 2), smooth = g.smooth }
        end
        group_sdfs[gi] = sdf
        coast_sdf = coast_sdf and Combine.Min{ a = coast_sdf, b = sdf } or sdf
    end
    for _, i in ipairs(islets) do
        coast_sdf = Combine.Min{
            a = coast_sdf, b = Shapes.Circle{ at = { i[1], i[2] }, radius = i[3], position = position },
        }
    end
    -- Which island a cell is nearest to (1 everywhere when there is only one).
    local region_id = coast_sdf * 0.0 + 1.0
    if #groups > 1 then
        local best = group_sdfs[1]
        for gi = 2, #groups do
            local closer = 1.0 - Curves.Smoothstep{
                input = group_sdfs[gi] - best, min = -resolution * 0.25, max = resolution * 0.25,
            }
            region_id = region_id + (gi - region_id) * closer
            best = Combine.Min{ a = best, b = group_sdfs[gi] }
        end
    end

    local transition = math.max(resolution * 1.5, 12.0)
    local land_mask  = 1.0 - Curves.Smoothstep{ input = coast_sdf, min = -transition, max = transition }

    -- 4. The type of coast: 0 a beach, 1 a cliff.
    local wave = math.rad(t.wave_from or 225.0)
    local gradient = Terrain.Gradient{ input = coast_sdf }
    local facing = gradient.x * math.cos(wave) + gradient.z * math.sin(wave)
    local exposure = Curves.Clamp{ input = 0.5 + 0.5 * facing, min = 0.0, max = 1.0 }
    local samples = math.min(math.max(math.floor(size * 0.02 / resolution + 0.5), 2), 24)
    local convexity = Curves.Clamp{
        input = (Filters.Blur{ input = coast_sdf, radius = samples } - coast_sdf) / (size * 0.02),
        min = -1.0, max = 1.0,
    } * 0.5 + 0.5
    local function regional(offset, salt)
        return Curves.Smoothstep{
            input = Noises.fBm{ frequency = 1.0 / (size * 0.4), octaves = 2, amplitude = 0.6, seed = seed + salt }.value
                    + offset,
            min = -0.5, max = 0.5,
        }
    end
    local hard = regional((t.hardness or 0.0) + coast.hardness, 71)
    local sed  = regional((t.sediment or 0.0) + coast.sediment, 73)
    local raw  = 0.50 * hard + 0.22 * exposure + 0.13 * convexity + 0.15 - 0.45 * sed
    local coast_type = Curves.Smoothstep{ input = raw, min = 0.20, max = 0.70 }

    -- 5. The ground. The distance inland is `u`; a beach rises gently over `beach_width`, a cliff in a
    -- tenth of that, and the interior climbs to `height` by a power of the distance (`hypsometry`
    -- above 1: lowland most of the way, then the highs).
    local u = Curves.Clamp{ input = -coast_sdf, min = 0.0, max = 1.0e6 }
    local beach_w = t.beach_width or math.min(math.max(extent * 0.05, 60.0), 400.0)
    local cliff_w = t.cliff_width or math.min(math.max(extent * 0.02, 25.0), 120.0)
    local beach_h = t.beach_height or 6.0
    local cliff_h = t.cliff_height or math.min(relief * 0.22, 90.0)
    local sb = Curves.Clamp{ input = u / beach_w, min = 0.0, max = 1.0 }
    local sc = Curves.Clamp{ input = u / cliff_w, min = 0.0, max = 1.0 }
    local beach = beach_h * (1.0 - (1.0 - sb) * (1.0 - sb))
    local cliff = cliff_h * (1.0 - (1.0 - sc) * (1.0 - sc))
    local shore = Combine.Blend{ a = beach, b = cliff, mask = coast_type }

    -- Where the interior reaches its full height: 0.7 of the radius, and of the largest island in a group.
    local biggest = 0.0
    for _, g in ipairs(groups) do biggest = math.max(biggest, g.radius or 0.0) end
    local reach = (layout and biggest > 0 and biggest or extent) * 0.7
    local si = Curves.Clamp{ input = u / reach, min = 0.0, max = 1.0 }
    local hyps = t.hypsometry or 1.7
    local rise = Curves.Power{ input = si, exponent = hyps } * (si * si * (3.0 - 2.0 * si))
    local relief_noise = Noises.Ridged{
        frequency = 1.0 / (size * 0.18), octaves = 4, persistence = 0.5, amplitude = 0.5, seed = seed + 41,
        position = position,
    }
    local roll = Noises.fBm{
        frequency = 1.0 / (size * 0.35), octaves = 3, persistence = 0.5, amplitude = 0.35, seed = seed + 43,
        position = position,
    }
    -- Everything inland is scaled by `rise`, which is 0 on the coast, so no noise can take the ground
    -- under the sea inside the outline: a hole in the land is a lagoon, and this is not one.
    local interior = relief * rise * (0.8 + relief_noise.value * 0.6 + roll.value)
    local land_height = Curves.Clamp{ input = shore + interior, min = 0.0, max = relief * 2.5 }

    -- 6. The sea floor: a shallow beach face, a shelf under 1 degree in the geologic margin, a break, a
    -- slope and an abyssal plain. Steeper offshore of a cliff.
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
    local bathymetry = -depth

    local surface_height = Combine.Blend{ a = bathymetry, b = land_height, mask = land_mask }

    local band = t.shore_band or math.max(size * 0.02, 4.0 * resolution)
    local shore_mask = 1.0 - Curves.Smoothstep{
        input = Combine.Max{ a = coast_sdf, b = -coast_sdf }, min = 0.0, max = band,
    }

    local island = {
        coast_sdf = coast_sdf, land_mask = land_mask, land_height = land_height,
        bathymetry = bathymetry, surface_height = surface_height, shore_mask = shore_mask,
        coast_type = coast_type, region_id = region_id,
        info = {
            extent = extent, bands = active, islets = islets, regions = #groups,
            islands = (function()
                local list = {}
                for gi, g in ipairs(groups) do list[gi] = { x = g.x, z = g.z, radius = g.radius } end
                return list
            end)(),
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

-- An archipelago: `Lands.Island` with a `layout`. `radius` is then the half-size of the whole group.
function Lands.Archipelago(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Lands.Archipelago expects a table of named arguments, as in Lands.Archipelago{ ... }")
    end
    local copy = {}
    for key, value in pairs(t) do copy[key] = value end
    if copy.areas == nil then
        copy.layout = copy.layout or "scattered"
    end
    if copy.layout == "atoll" then
        -- A lagoon a few tens of metres deep and a steep outer wall.
        local radius = copy.radius or 12000.0
        copy.shelf_width = copy.shelf_width or radius * 0.6
        copy.slope_width = copy.slope_width or radius * 0.4
        copy.height      = copy.height or 14.0
        copy.hypsometry  = copy.hypsometry or 1.0
        copy.beach_width = copy.beach_width or 60.0
    end
    return Lands.Island(copy)
end

-- A continent, as a region and not as a planet: a large lumpy mass (the outline of `Lands.Island` at
-- its most irregular) with mountain ranges raised across its interior by `Ranges.Carve`. The ranges are
-- given as `ranges` (a list of { points, height, width }, as in `Ranges.Carve`) or generated: `ranges_count`
-- crest lines of a few segments, wandering inside the core of the outline. They fade out towards the
-- coast over the first 5 to 20 % of the radius, so no mountain falls straight into the sea. The rivers
-- of the ranges end at the edge of their grid, not at the sea: draining to the coast waits for
-- `Erosion.StreamPower` to take the sea as its base level.
function Lands.Continent(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Lands.Continent expects a table of named arguments, as in Lands.Continent{ ... }")
    end
    local seed   = t.seed or 0
    local radius = t.radius or 14000.0
    local copy = {}
    for key, value in pairs(t) do
        if key ~= "ranges" and key ~= "ranges_count" and key ~= "range_height" then copy[key] = value end
    end
    copy.radius    = radius
    copy.lumpiness = copy.lumpiness or 1.0
    copy.roughness = copy.roughness or 0.7
    copy.height    = copy.height or 380.0
    copy.hypsometry = copy.hypsometry or 1.3
    local continent = Lands.Island(copy)

    local ridges = t.ranges
    local tallest = t.range_height or 2300.0
    if ridges == nil then
        local h = hasher(seed + 8000)
        ridges = {}
        local count = t.ranges_count or 2
        for k = 1, count do
            local angle = h(k, 1) * 2.0 * math.pi
            local points = {}
            -- Crest lines run across the core of the outline, bent and not straight.
            local step = radius * 0.16
            local dir = h(k, 2) * 2.0 * math.pi
            local x0 = math.cos(angle) * radius * 0.2 - math.cos(dir) * step * 2.0
            local z0 = math.sin(angle) * radius * 0.2 - math.sin(dir) * step * 2.0
            for i = 0, 4 do
                dir = dir + (h(k * 10 + i, 3) - 0.5) * 0.7
                x0, z0 = x0 + math.cos(dir) * step, z0 + math.sin(dir) * step
                points[#points + 1] = { x0, z0 }
            end
            ridges[k] = { points = points, height = tallest * (0.8 + 0.4 * h(k, 4)), width = radius * 0.22 }
        end
    end
    local structure, peak, sun = Ranges.Carve{ seed = seed, ridges = ridges }
    local range_height = Ranges.Finish(structure, peak, seed, nil, sun).height.value

    local inland = Curves.Smoothstep{ input = -continent.coast_sdf, min = radius * 0.05, max = radius * 0.2 }
    continent.land_height = continent.land_height + range_height * inland
    continent.surface_height = Combine.Blend{
        a = continent.bathymetry, b = continent.land_height, mask = continent.land_mask,
    }
    continent.info.height_max = continent.info.height_max + peak
    continent.info.ranges = ridges
    return continent
end

_G.Lands = Lands
