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
    area = true, radius = true, lumpiness = true, roughness = true,
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
    if t.area ~= nil then
        check_polygon(t.area)
        polygon = t.area
        extent = 0.0
        for _, p in ipairs(polygon) do extent = math.max(extent, math.sqrt(p[1] * p[1] + p[2] * p[2])) end
    else
        extent = t.radius or 3000.0
        if extent <= 0 then fail("Lands.Island needs radius greater than zero") end
        shape_discs = blob(seed, extent, t.lumpiness or 0.8)
    end
    local size = extent * 2.0

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

    -- 2. Islets, placed outside the outline on the CPU. Room is kept for the warp on both sides.
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
            local sd
            if polygon then
                sd = polygon_distance(polygon, x, z)
            else
                sd = math.huge
                for _, d in ipairs(shape_discs) do
                    sd = math.min(sd, math.sqrt((x - d[1]) ^ 2 + (z - d[2]) ^ 2) - d[3])
                end
            end
            local ok = sd >= channel_min + r + 2.0 * spread
            for _, other in ipairs(islets) do
                local gap = math.sqrt((x - other[1]) ^ 2 + (z - other[2]) ^ 2) - r - other[3]
                if gap < channel_min + 2.0 * spread then ok = false end
            end
            if ok then islets[#islets + 1] = { x, z, r } end
        end
    end

    -- 3. The signed distance. Land is negative.
    local coast_sdf
    if polygon then
        coast_sdf = Shapes.Polygon{ points = polygon, position = position }
    else
        local shapes = {}
        for _, d in ipairs(shape_discs) do
            shapes[#shapes + 1] = Shapes.Circle{ at = { d[1], d[2] }, radius = d[3], position = position }
        end
        coast_sdf = Shapes.Union{ shapes[1], table.unpack(shapes, 2), smooth = extent * 0.1 }
    end
    for _, i in ipairs(islets) do
        coast_sdf = Combine.Min{
            a = coast_sdf, b = Shapes.Circle{ at = { i[1], i[2] }, radius = i[3], position = position },
        }
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

    local reach = extent * 0.7
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
        coast_type = coast_type,
        info = {
            extent = extent, bands = active, islets = islets, margin = {
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
            look    = { palette = "default" },
        }
        for _, name in ipairs(names or {}) do
            if ranges[name] == nil or name == "surface_height" then
                if name ~= "surface_height" then
                    fail("Lands: '" .. tostring(name) .. "' is not a product; they are coast_sdf, land_mask, "
                         .. "land_height, bathymetry, shore_mask and coast_type (height is the surface)")
                end
            else
                result[name] = { value = self[name], range = ranges[name] }
            end
        end
        return result
    end
    return island
end

_G.Lands = Lands
