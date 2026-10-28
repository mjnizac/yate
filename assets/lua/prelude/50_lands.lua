-- Lands: islands, archipelagos, continents and any other land and sea, built from simple shapes first and
-- detail after.
--
-- `Lands.Compose` takes a list of elements: shapes of land, water (bays, lakes, seas, channels), forms of
-- the sea floor (seamounts, guyots, trenches) and relief (ranges, massifs, basins), each with an id. They
-- combine in order in one node (`Shapes.Set`, the `Lands` op). Each land element rises by its own profile,
-- with nothing assumed about how; relief elements add to it. A `scatter` element stands for many (an
-- archipelago), a `ridge` element for a range with saddles and spurs. Everything else is a function of the
-- one signed distance to the coast, with ONE sign convention:
--
--          land d < 0     coast d = 0     sea d > 0
--
--      land_mask = 1 - Smoothstep(-w, w, d)   1 on land, 0 at sea, a few samples of transition
--      land_height                             the profiles and the relief, shaped at the shore
--      bathymetry                              the sea floor, negative, from the distance offshore
--      surface_height                          the two joined at d = 0 (both are 0 there)
--      shore_mask                              1 on the coast, falling to 0 `shore_band` metres either side
--      coast_type                              0 a beach, 1 a cliff
--      region_id                               the land element a sample belongs to
--
-- The type of coast is an artistic heuristic, not a law: hardness and sediment (slow noise with a bias per
-- `coast` preset), the exposure to the waves (the outward normal against the direction they come from) and
-- the curvature of the coast as a bias.
--
-- `Lands.Island`, `Lands.Archipelago` and `Lands.Continent` build a preset (`Lands.Preset` returns its
-- composition, to change before building). The resolved elements go to the export's metadata
-- (`engine.report`). The plan this follows is `docs/lands_plan.md`.
--
-- Run by the engine after `46_network.lua`.

local engine = ...
local fail   = engine.fail

local Noises, Terrain, Curves, Combine, Filters, Vec, Erosion =
    Noises, Terrain, Curves, Combine, Filters, Vec, Erosion
local type, pairs, ipairs, tostring = type, pairs, ipairs, tostring
local math, table = math, table

local Lands = {}

-- --- Keys -----------------------------------------------------------------------------------------

local MARGINS = {
    -- Legible in the viewer, which shows at most ~28 km: widths are multiples of the map's half size
    -- (`scale = true`), so the same map has the same shelf at any size; depths are metres.
    compressed = { shelf_width = 0.12, shelf_depth = 70.0, slope_width = 0.2, abyssal_depth = 700.0, scale = true },
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

-- Everything a land is after its coast: the type of coast, the ground with its shore, the sea floor and
-- the outputs, from the signed distance to the coast `o.coast_sdf` (land negative) and `o.ground`, the
-- relief over the land (gullied here when `o.gullies`). `o.t` is the caller's table, for the shore and
-- coast keys; `o.lagoon`, when given, is 1 where the sea is a lagoon.
local function finish(o)
    local t, seed, size, resolution, relief, coast = o.t, o.seed, o.size, o.resolution, o.relief, o.coast
    local coast_sdf = o.coast_sdf

    -- The type of coast: 0 a beach, 1 a cliff.
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

    -- The ground: the relief, gullied at the scale it cannot show if asked, over a beach or cliff profile at
    -- the shore.
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

    local ground = o.ground
    if o.additive then
        -- The ground is the relief as given, below the sea too (a caldera's floor); the shore profile is
        -- added only where the land meets the sea.
        local inland = Curves.Smoothstep{ input = u, min = 0.0, max = math.max(size * 0.01, 60.0) }
        o.land_height = ground + shore * (1.0 - inland)
    end
    if o.gullies then
        ground = Erosion.Gullies{
            input = ground, cell = math.max(size * 0.012, 60.0), octaves = 3, strength = math.min(relief * 0.04, 30.0),
            full = 0.5, seed = seed + 53,
        }
    end
    -- The shore profile rises to the ground over a few hundred metres, so a high coast is a cliff and a low
    -- one a beach.
    local land_height = o.land_height
    if land_height == nil then
        local inland = Curves.Smoothstep{ input = u, min = 0.0, max = math.max(size * 0.01, 60.0) }
        land_height = Curves.Clamp{
            input = Combine.Blend{ a = shore, b = Combine.Max{ a = ground, b = shore }, mask = inland },
            min = 0.0, max = relief * 2.5,
        }
    end

    -- The sea floor: a shallow beach face, a shelf, a break, a slope and an abyssal plain. Steeper offshore
    -- of a cliff.
    local shelf_width, shelf_depth, slope_width, abyssal_depth = o.shelf_width, o.shelf_depth, o.slope_width, o.abyssal_depth
    -- The distance offshore: the margin's own when the caller has one (smooth between coasts).
    local off = Curves.Clamp{ input = o.margin_sdf or coast_sdf, min = 0.0, max = 1.0e7 }
    local sf = Curves.Clamp{ input = off / 160.0, min = 0.0, max = 1.0 }
    local steep = coast_type
    if o.margin_sdf then
        -- A cliff's steeper face is the coast's, not the open sea's: it fades within a few hundred metres, so the
        -- coast type's seam between two coasts does not run out across the sea floor.
        steep = coast_type * (1.0 - Curves.Smoothstep{ input = off, min = 160.0, max = 800.0 })
    end
    local face = (3.0 + 22.0 * steep) * (1.0 - (1.0 - sf) * (1.0 - sf))
    local on_shelf = shelf_depth * Curves.Clamp{ input = off / shelf_width, min = 0.0, max = 1.0 }
    local sl = Curves.Clamp{ input = (off - shelf_width) / slope_width, min = 0.0, max = 1.0 }
    local on_slope = (abyssal_depth - shelf_depth) * (sl * sl * (3.0 - 2.0 * sl))
    local floor_noise = Noises.fBm{
        frequency = 1.0 / (size * 0.3), octaves = 3, amplitude = 0.03, seed = seed + 51,
    }
    local depth = (face + on_shelf + on_slope) * (1.0 + floor_noise.value)
    if o.lagoon then
        local inside = Curves.Smoothstep{ input = o.lagoon, min = 0.3, max = 0.7 }
        depth = Combine.Blend{
            a = depth, b = Curves.Clamp{ input = depth, min = 0.0, max = t.lagoon_depth or 45.0 }, mask = inside,
        }
    end
    local bathymetry = -depth
    if o.seabed then
        -- The sea floor's own forms, never brought up to the surface.
        bathymetry = bathymetry + Combine.Min{ a = o.seabed, b = depth - 2.0 }
    end

    local surface_height = Combine.Blend{ a = bathymetry, b = land_height, mask = o.land_mask }

    local band = t.shore_band or math.max(size * 0.01, 4.0 * resolution)
    local shore_mask = 1.0 - Curves.Smoothstep{
        input = Combine.Max{ a = coast_sdf, b = -coast_sdf }, min = 0.0, max = band,
    }

    local info = o.info
    info.margin = {
        shelf_width = shelf_width, shelf_depth = shelf_depth, slope_width = slope_width, abyssal_depth = abyssal_depth,
    }
    info.height_max, info.depth_max = relief * 1.6, abyssal_depth * 1.05
    local land = {
        coast_sdf = coast_sdf, land_mask = o.land_mask, land_height = land_height,
        bathymetry = bathymetry, surface_height = surface_height, shore_mask = shore_mask,
        coast_type = coast_type, region_id = o.region_id, info = info,
    }

    -- The script's outputs: `height` and `normals` always, then whatever is asked for by name (a script
    -- holds at most eight outputs, so the rest are on demand).
    function land:outputs(names)
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
    return land
end

-- --- Lands.Compose --------------------------------------------------------------------------------

-- Land and water made of simple shapes, each with an id, combined in list order in one node
-- (`Shapes.Set`): nothing is assumed about where the land is or how it rises.

local COMPOSE_KEYS = {
    seed = true, resolution = true, frame = true, elements = true, height = true, profile = true, hypsometry = true,
    edit = true, remove = true, land_fraction = true, classes = true,
    margin = true, shelf_width = true, shelf_depth = true, slope_width = true, abyssal_depth = true,
    coast = true, wave_from = true, hardness = true, sediment = true, shore_band = true, lagoon_depth = true,
    beach_width = true, beach_height = true, cliff_width = true, cliff_height = true,
}
-- The keys an element takes beyond a `Shapes.Set` element's.
local ELEMENT_OWN = { area = true, aspect = true, role = true, name = true, profile = true, margin = true }

-- 4 a b times the integral of (1 - x^n)^(1/n) over [0, 1]: the area of a superellipse of radii a, b.
local function superellipse_area(a, b, n)
    local steps, sum = 4096, 0.0
    for i = 1, steps do
        local x = (i - 0.5) / steps
        sum = sum + (1.0 - x ^ n) ^ (1.0 / n)
    end
    return 4.0 * a * b * sum / steps
end

-- How far inland an element's ground can be from its coast, before `scale`: what a profile's widths are
-- fractions of when a preset is given without them.
local function inradius(e)
    local s = e.shape
    if s == "circle" then return e.radius end
    if s == "ellipse" or s == "superellipse" then return math.min(e.radii[1], e.radii[2]) end
    if s == "box" then return 0.5 * math.min(e.size[1], e.size[2]) end
    if s == "radial" then
        local spread = 0.0
        for _, h in ipairs(e.harmonics or {}) do spread = spread + math.abs(h[2] or 0.0) end
        return e.radius * (1.0 - spread)
    end
    if s == "band" and e.width == nil then
        local sum = 0.0
        for _, p in ipairs(e.points) do sum = sum + (p[3] or 0.0) end
        return 0.5 * sum / math.max(#e.points, 1)
    end
    if s == "capsule" or s == "ring" or s == "arc" or s == "band" then return 0.5 * (e.width or 0.0) end
    if s == "sector" then
        local h = math.sin(math.rad(e.angle * 0.5))
        return e.reach * h / (1.0 + h)
    end
    if s == "polygon" then
        local area, n = 0.0, #e.points
        for k = 1, n do
            local a, b = e.points[k], e.points[k % n + 1]
            area = area + a[1] * b[2] - b[1] * a[2]
        end
        return 0.8 * math.sqrt(math.abs(area) * 0.5 / math.pi)
    end
    return nil
end

local PRESETS = { dome = true, cone = true, plateau = true, rim = true, caldera = true, tilt = true, flat = true }

-- A profile given by name, or as { "name", key = value, ... }, as `Shapes.Set` numbers: every width a
-- fraction of the element's inradius `r` and every height of `relief`, unless the script gives it.
local function profile_of(where, p, r, relief, rotation)
    local name, given = p, {}
    if type(p) == "table" then
        name, given = p[1], p
        local known = { height = true, width = true, edge = true, at = true, sigma = true, inner = true, start = true,
                        gradient = true, direction = true, low = true }
        for key in pairs(p) do
            if key ~= 1 and not known[key] then
                fail(where .. " profile does not take '" .. tostring(key) .. "'; it takes the preset's name first, "
                     .. "then height, width, edge, at, sigma, inner, start, gradient, direction or low")
            end
        end
    end
    if not PRESETS[name] then
        fail(where .. " has profile '" .. tostring(name) .. "'; expected dome, cone, plateau, rim, caldera, tilt or flat")
    end
    local function g(key, default) if given[key] ~= nil then return given[key] end return default end
    local h = g("height", relief)
    if name == "dome" or name == "cone" then
        return { kind = name, height = h, width = g("width", r) }
    elseif name == "plateau" or name == "flat" then
        return { kind = "plateau", height = name == "flat" and g("height", math.max(relief * 0.03, 4.0)) or h,
                 edge = g("edge", r * (name == "flat" and 0.3 or 0.15)) }
    elseif name == "rim" or name == "caldera" then
        local at = g("at", r * 0.12)
        return { kind = "rim", height = h, at = at, sigma = g("sigma", r * 0.06),
                 inner = g("inner", name == "caldera" and -0.15 * h or 0.3 * h),
                 width = g("width", r * (name == "caldera" and 0.15 or 0.4)),
                 start = g("start", name == "caldera" and at * 1.5 or 0.0) }
    end
    -- A plane rising `gradient` per metre towards `direction`, `low` on the low side of the inscribed circle,
    -- never under `low`.
    local low      = g("low", h * 0.1)
    local gradient = g("gradient", h * 0.9 / math.max(2.0 * r, 1.0))
    return { kind = "tilt", height = low + gradient * r, floor = low, edge = g("edge", r * 0.1),
             gradient = gradient, direction = g("direction", rotation or 0.0) }
end

-- `out`, the element sized, with its profile: its own, or the composition's. Land needs one; water has none.
-- `out.inland` is how far in its profile reaches, for the composition's reach.
local function with_profile(where, e, out, relief, default_profile)
    if e.water then
        if e.profile ~= nil then
            fail(where .. " is water and takes no profile")
        end
        return out
    end
    local p = e.profile or (e.seabed and "cone") or default_profile
    if p == nil then
        fail(where .. " needs a profile, its own or the composition's 'profile': dome, cone, plateau, rim, caldera, "
             .. "tilt or flat. Nothing is assumed about how land rises")
    end
    local r = inradius(out)
    if r == nil then
        r = math.max(relief.land * 10.0, 1000.0)
    end
    r = r * (out.scale or 1.0)
    -- A seabed form is measured from the abyssal floor, half its depth high unless told.
    out.profile = profile_of(where, p, r, e.seabed and relief.seabed or relief.land, out.rotation)
    out.inland = r
    return out
end

-- A `Lands.Compose` element as a sized `Shapes.Set` element, without its profile: `area` (square metres)
-- sets its size from the shape's closed-form area, `aspect` (x over z) its proportions where the shape has
-- two radii.
local function sized(where, e)
    if type(e) ~= "table" then
        fail(where .. " must be a table")
    end
    local out = {}
    for key, value in pairs(e) do
        if not ELEMENT_OWN[key] then out[key] = value end
    end
    if e.area == nil then
        return out
    end
    if type(e.area) ~= "number" or e.area <= 0 then
        fail(where .. " needs 'area' as square metres above zero")
    end
    local area   = e.area / (e.scale or 1.0) ^ 2
    local aspect = e.aspect or 1.0
    local shape  = e.shape
    if shape == "circle" then
        out.radius = math.sqrt(area / math.pi)
    elseif shape == "ellipse" or shape == "superellipse" then
        local unit = shape == "ellipse" and math.pi or superellipse_area(1.0, 1.0, e.exponent or 4.0)
        local rz = math.sqrt(area / (unit * aspect))
        out.radii = { rz * aspect, rz }
    elseif shape == "box" then
        local depth = math.sqrt(area / aspect)
        out.size = { depth * aspect, depth }
    elseif shape == "radial" then
        local spread = 0.0
        for _, h in ipairs(e.harmonics or {}) do spread = spread + 0.5 * (h[2] or 0.0) ^ 2 end
        out.radius = math.sqrt(area / (math.pi * (1.0 + spread)))
    elseif shape == "ring" then
        if type(e.width) ~= "number" then fail(where .. " needs 'width' to size a ring by its area") end
        out.radius = area / (2.0 * math.pi * e.width)
    elseif shape == "capsule" and type(e.length) == "number" then
        -- pi r^2 + 2 L r = area, L the length between the caps' centres.
        local r = (-e.length + math.sqrt(e.length * e.length + math.pi * area)) / math.pi
        out.width = 2.0 * r
    else
        fail(where .. " cannot be sized by 'area' as a " .. tostring(shape)
             .. "; give its dimensions (area works for circle, ellipse, superellipse, box, radial, ring and a capsule with a length)")
    end
    return out
end

local function set_element(i, e, number, relief, default_profile)
    local where = "Lands.Compose element " .. i
    local out = sized(where, e)
    out.id = number
    return with_profile(where, e, out, relief, default_profile)
end

-- The closed-form area of a `Shapes.Set` element in square metres, or nil for a shape that has none (a
-- band, a fill).
local function geometric_area(e)
    local s, area = e.shape, nil
    if s == "circle" then
        area = math.pi * e.radius ^ 2
    elseif s == "ellipse" then
        area = math.pi * e.radii[1] * e.radii[2]
    elseif s == "superellipse" then
        area = superellipse_area(e.radii[1], e.radii[2], e.exponent or 4.0)
    elseif s == "box" then
        area = e.size[1] * e.size[2] - (4.0 - math.pi) * (e.rounding or 0.0) ^ 2
    elseif s == "radial" then
        local spread = 0.0
        for _, h in ipairs(e.harmonics or {}) do spread = spread + 0.5 * h[2] ^ 2 end
        area = math.pi * e.radius ^ 2 * (1.0 + spread)
    elseif s == "ring" then
        area = 2.0 * math.pi * e.radius * e.width
    elseif s == "capsule" then
        local length = e.length or math.sqrt((e.to[1] - e.from[1]) ^ 2 + (e.to[2] - e.from[2]) ^ 2)
        area = math.pi * (e.width * 0.5) ^ 2 + length * e.width
    elseif s == "arc" then
        area = e.radius * math.rad(e.aperture) * e.width + math.pi * e.width ^ 2 * 0.25
    elseif s == "sector" then
        area = math.rad(e.angle) * 0.5 * e.reach ^ 2
    elseif s == "polygon" then
        area = 0.0
        for k = 1, #e.points do
            local a, b = e.points[k], e.points[k % #e.points + 1]
            area = area + a[1] * b[2] - b[1] * a[2]
        end
        area = math.abs(area) * 0.5
    end
    return area and area * (e.scale or 1.0) ^ 2
end

-- --- Scatter: many elements from one ---------------------------------------------------------------

-- The bounding radius of a sized element around its placement, in metres (its scale included).
local function bound_of(e)
    local s, r = e.shape, 0.0
    if s == "circle" then
        r = e.radius
    elseif s == "ellipse" then
        r = math.max(e.radii[1], e.radii[2])
    elseif s == "superellipse" then
        local n = e.exponent or 4.0
        r = math.max(e.radii[1], e.radii[2]) * (n > 2.0 and 2.0 ^ (0.5 - 1.0 / n) or 1.0)
    elseif s == "box" then
        r = 0.5 * math.sqrt(e.size[1] ^ 2 + e.size[2] ^ 2)
    elseif s == "radial" then
        local spread = 0.0
        for _, h in ipairs(e.harmonics or {}) do spread = spread + math.abs(h[2]) end
        r = e.radius * (1.0 + spread)
    elseif s == "capsule" then
        local length = e.length or (e.from and math.sqrt((e.to[1] - e.from[1]) ^ 2 + (e.to[2] - e.from[2]) ^ 2)) or 0.0
        r = length * 0.5 + e.width * 0.5
    elseif s == "ring" or s == "arc" then
        r = e.radius + e.width * 0.5
    elseif s == "sector" then
        r = e.reach
    elseif s == "polygon" or s == "band" then
        for _, p in ipairs(e.points) do r = math.max(r, math.sqrt(p[1] ^ 2 + p[2] ^ 2) + (p[3] or e.width or 0.0) * 0.5) end
    end
    return r * (e.scale or 1.0)
end

-- A number from a string, for salting a hash with an id.
local function salt_of(text)
    local v = 0
    for k = 1, #text do v = (v * 31 + text:byte(k)) & 0x7fffffff end
    return v
end

local SCATTER_KEYS = {
    count = true, area = true, distribution = true, spacing = true, pattern = true, region = true, margin = true,
    clusters = true, spread = true, path = true, center = true, radius = true, from_angle = true, to_angle = true,
    decay = true, wander = true, centers = true, cell = true,
}
local PATTERNS = { uniform = true, cluster = true, line = true, arc = true, ring = true, grid = true, centers = true }

-- The elements one `scatter` element stands for, as { e, id } in slot order. `ctx` is the composition: its
-- seed, its frame (`size` and `window`, or `world_cell`), and `obstacles`, the circles of the land declared
-- around it ({ x, z, r }), which nothing generated may come near.
--
-- In a bounded map every slot draws its size and its candidate positions from a hash of (seed, id, slot)
-- and is placed by dart throwing (Bridson's rule of 30 tries, against the slots before it), so asking for
-- one more keeps the others where they were. In the unbounded world (uniform and cluster patterns) `count`
-- is per world cell: each cell proposes candidates from its own hash, and a candidate stays when no
-- conflicting candidate has a higher priority (Wei's parallel Poisson rule, in one pass), which looks only
-- at neighbouring cells, so any two windows over the same cells keep the same elements.
local function scatter(e, id, ctx)
    local where = "Lands.Compose scatter '" .. tostring(id) .. "'"
    local spec = e.scatter
    if type(spec) ~= "table" then
        fail(where .. " expects 'scatter' as a table { count = ..., area = { min, max }, ... }")
    end
    for key in pairs(spec) do
        if not SCATTER_KEYS[key] then
            fail(where .. " does not take '" .. tostring(key) .. "'; it takes count, area, distribution, spacing, "
                 .. "pattern, region, margin, clusters, spread, path, center, radius, from_angle, to_angle, decay, "
                 .. "wander, centers and cell")
        end
    end
    local pattern = spec.pattern or "uniform"
    if not PATTERNS[pattern] then
        fail(where .. " has pattern '" .. tostring(pattern) .. "'; expected uniform, cluster, line, arc, ring, grid or centers")
    end
    local range = spec.area
    if pattern ~= "centers" and (type(range) ~= "table" or type(range[1]) ~= "number" or type(range[2]) ~= "number"
                                 or range[1] <= 0 or range[2] < range[1]) then
        fail(where .. " needs 'area' as { min, max } in square metres, min above zero")
    end
    local count   = spec.count or 1
    local spacing = spec.spacing or 0.0
    local log     = (spec.distribution or "log") == "log"
    local h       = engine.hasher((ctx.seed or 0) * 7919 + salt_of(tostring(id)))
    local k_area  = ctx.area_factor or 1.0
    local world   = ctx.world_cell ~= nil and (pattern == "uniform" or pattern == "cluster")
    -- `region`, a `Shapes.Set` element: centres must fall inside it (read on the CPU sampler).
    local region = nil
    if spec.region ~= nil then
        local node = Shapes.Set{ elements = { spec.region }, reach = 1.0 }
        region = function(x, z) return Terrain.Sample{ value = node, xs = { x }, zs = { z } }[1] <= 0.0 end
    end

    -- An element for a slot: the template (every key of `e` but `scatter` and `id`), its area drawn from the
    -- range (log-uniform by default: small islands outnumber large ones), its outline and turn drawn when the
    -- template leaves them open.
    local function make(key, x, z, area)
        local out = {}
        for k, v in pairs(e) do
            if k ~= "scatter" and k ~= "id" and k ~= "lagoon" then out[k] = v end
        end
        local hk = engine.hasher(salt_of(key) + (ctx.seed or 0) * 7919)
        out.at, out.area = { x, z }, area
        if out.rotation == nil then out.rotation = hk(1, 1) * 360.0 end
        if type(out.aspect) == "table" then out.aspect = out.aspect[1] + (out.aspect[2] - out.aspect[1]) * hk(2, 1) end
        if out.shape == "radial" and out.harmonics == nil then
            out.harmonics = {
                { 2 + math.floor(hk(3, 1) * 2), 0.08 + 0.12 * hk(3, 2), hk(3, 3) * 2.0 * math.pi },
                { 5 + math.floor(hk(4, 1) * 2), 0.02 + 0.05 * hk(4, 2), hk(4, 3) * 2.0 * math.pi },
            }
        end
        return out
    end
    local function draw(u)
        local a = log and range[1] * (range[2] / range[1]) ^ u or range[1] + (range[2] - range[1]) * u
        return a * k_area
    end
    local function radius_of(out)
        return bound_of(sized(where, out))
    end

    local placed, entries = {}, {}
    local function clear(x, z, r)
        -- Water (channels, lakes) is cut into land, so only land keeps away from the land declared.
        for _, o in ipairs(e.water and {} or ctx.obstacles) do
            if (x - o.x) ^ 2 + (z - o.z) ^ 2 < (r + o.r + spacing) ^ 2 then return false end
        end
        for _, o in ipairs(placed) do
            if (x - o.x) ^ 2 + (z - o.z) ^ 2 < (r + o.r + spacing) ^ 2 then return false end
        end
        return true
    end
    -- An element, and with `lagoon = true` on a ring the shallow water inside it (an atoll's lagoon).
    local function emit(key, out)
        entries[#entries + 1] = { e = out, id = id .. "/" .. key, scatter = id }
        if e.lagoon and out.shape == "ring" then
            local ring = sized(where, out)
            entries[#entries + 1] = {
                e = { shape = "circle", water = true, role = "lagoon", at = out.at, radius = ring.radius - ring.width * 0.5 },
                id = id .. "/" .. key .. "/lagoon", scatter = id,
            }
        end
    end
    local function keep(key, out, r)
        placed[#placed + 1] = { x = out.at[1], z = out.at[2], r = r }
        emit(key, out)
    end

    if world then
        -- Candidates per world cell, a priority each; a candidate stays if no conflicting one outranks it.
        local cellw = ctx.world_cell
        local window = ctx.window
        local function cell_candidates(cx, cz)
            local hc = engine.hasher((((cx * 73856093) ~ (cz * 19349663) ~ salt_of(tostring(id))) & 0x7fffffff)
                                     + (ctx.seed or 0) * 7919)
            local list = {}
            local x0, z0 = cx * cellw, cz * cellw
            if pattern == "uniform" then
                local m = math.max(1, math.ceil(math.sqrt(count * 2.0)))
                for s = 1, m * m do
                    if hc(s, 1) < count / (m * m) then
                        local sx, sz = (s - 1) % m, (s - 1) // m
                        list[#list + 1] = { key = cx .. "," .. cz .. "," .. s,
                                            x = x0 + (sx + hc(s, 2)) * cellw / m, z = z0 + (sz + hc(s, 3)) * cellw / m,
                                            u = hc(s, 4), p = hc(s, 5) }
                    end
                end
            else
                local children = math.max(1, math.floor(count / (spec.clusters or 1) + 0.5))
                local spread = spec.spread or cellw * 0.08
                for c = 1, spec.clusters or 1 do
                    local px, pz = x0 + hc(100 + c, 1) * cellw, z0 + hc(100 + c, 2) * cellw
                    for s = 1, children do
                        local a, r = hc(200 + c * 64 + s, 1) * 2.0 * math.pi, spread * math.sqrt(-2.0 * math.log(1.0 - 0.999 * hc(200 + c * 64 + s, 2)))
                        list[#list + 1] = { key = cx .. "," .. cz .. "," .. c .. "." .. s,
                                            x = px + math.cos(a) * r, z = pz + math.sin(a) * r,
                                            u = hc(200 + c * 64 + s, 3), p = hc(200 + c * 64 + s, 4) }
                    end
                end
            end
            return list
        end
        local reach = pattern == "cluster" and math.ceil(3.0 * (spec.spread or cellw * 0.08) / cellw) + 1 or 1
        local cx0, cz0 = math.floor(window[1] / cellw) - 1, math.floor(window[2] / cellw) - 1
        local cx1, cz1 = math.floor(window[3] / cellw) + 1, math.floor(window[4] / cellw) + 1
        local all = {}
        for cz = cz0 - reach, cz1 + reach do
            for cx = cx0 - reach, cx1 + reach do
                for _, c in ipairs(cell_candidates(cx, cz)) do
                    c.out = make(c.key, c.x, c.z, draw(c.u))
                    c.r = radius_of(c.out)
                    if c.r * 2.0 + spacing > cellw then
                        fail(where .. " needs world_cell (" .. cellw .. " m) at least twice the largest element's "
                             .. "radius plus the spacing (" .. math.ceil(c.r * 2.0 + spacing) .. " m)")
                    end
                    all[#all + 1] = c
                end
            end
        end
        for _, c in ipairs(all) do
            if c.x >= cx0 * cellw and c.x < (cx1 + 1) * cellw and c.z >= cz0 * cellw and c.z < (cz1 + 1) * cellw then
                local wins = clear(c.x, c.z, c.r) and (region == nil or region(c.x, c.z))
                for _, o in ipairs(all) do
                    if o ~= c and o.p > c.p and (c.x - o.x) ^ 2 + (c.z - o.z) ^ 2 < (c.r + o.r + spacing) ^ 2 then
                        wins = false
                        break
                    end
                end
                if wins then emit(c.key, c.out) end
            end
        end
        return entries, #entries, nil
    end

    -- Bounded, or a pattern with its own geometry: slots in order.
    local half = ctx.size * 0.5 - (spec.margin or ctx.edge_margin or 0.0)
    local function inside(x, z, r)
        if ctx.world_cell ~= nil then return true end
        return math.abs(x) + r <= half and math.abs(z) + r <= half
    end
    -- Where a pattern puts slot `s` at try `t` (t = 1 is the pattern's own place), and the area it draws.
    local parents = {}
    if pattern == "cluster" then
        for c = 1, spec.clusters or 1 do
            parents[c] = { (h(900 + c, 1) * 2.0 - 1.0) * half * 0.7, (h(900 + c, 2) * 2.0 - 1.0) * half * 0.7 }
        end
    end
    local along = {}
    if pattern == "line" then
        local path = spec.path
        if type(path) ~= "table" or #path < 2 then fail(where .. " needs 'path', at least two { x, z } points") end
        local total = 0.0
        along[1] = 0.0
        for k = 2, #path do
            total = total + math.sqrt((path[k][1] - path[k - 1][1]) ^ 2 + (path[k][2] - path[k - 1][2]) ^ 2)
            along[k] = total
        end
        along.total = total
    end
    local function point_on(f)
        if pattern == "line" then
            local d, path = f * along.total, spec.path
            for k = 2, #path do
                if d <= along[k] or k == #path then
                    local u = (d - along[k - 1]) / math.max(along[k] - along[k - 1], 1e-9)
                    local dx, dz = path[k][1] - path[k - 1][1], path[k][2] - path[k - 1][2]
                    local l = math.max(math.sqrt(dx * dx + dz * dz), 1e-9)
                    return path[k - 1][1] + dx * u, path[k - 1][2] + dz * u, -dz / l, dx / l
                end
            end
        end
        local c, r = spec.center or { 0.0, 0.0 }, spec.radius or ctx.size * 0.3
        local a0 = math.rad(pattern == "ring" and 0.0 or (spec.from_angle or 0.0))
        local a1 = math.rad(pattern == "ring" and 360.0 or (spec.to_angle or 180.0))
        local a = a0 + (a1 - a0) * f
        return c[1] + math.cos(a) * r, c[2] + math.sin(a) * r, math.cos(a), math.sin(a)
    end
    local function candidate(s, t)
        if pattern == "uniform" then
            return (h(s * 100 + t, 1) * 2.0 - 1.0) * half, (h(s * 100 + t, 2) * 2.0 - 1.0) * half
        elseif pattern == "cluster" then
            local p = parents[(s - 1) % #parents + 1]
            local spread = spec.spread or ctx.size * 0.06
            local a, r = h(s * 100 + t, 1) * 2.0 * math.pi, spread * math.sqrt(-2.0 * math.log(1.0 - 0.999 * h(s * 100 + t, 2)))
            return p[1] + math.cos(a) * r, p[2] + math.sin(a) * r
        elseif pattern == "grid" then
            local m = math.ceil(math.sqrt(count))
            local w = 2.0 * half / m
            local gx, gz = (s - 1) % m, (s - 1) // m
            local j = t == 1 and 0.35 or 0.5
            return -half + (gx + 0.5 + (h(s * 100 + t, 1) - 0.5) * 2.0 * j) * w, -half + (gz + 0.5 + (h(s * 100 + t, 2) - 0.5) * 2.0 * j) * w
        end
        -- line, arc, ring: the slot's place along the curve, wandering across it on later tries.
        local f = pattern == "ring" and (s - 1 + 0.3 * (h(s, 7) - 0.5)) / count or (s - 0.5 + 0.3 * (h(s, 7) - 0.5)) / count
        local x, z, nx, nz = point_on(math.min(math.max(f, 0.0), 1.0))
        local wander = (spec.wander or 0.0) * (h(s * 100 + t, 3) * 2.0 - 1.0) * (t == 1 and 0.5 or 1.0)
        return x + nx * wander, z + nz * wander
    end

    local list = pattern == "centers" and spec.centers or nil
    if pattern == "centers" and (type(list) ~= "table" or #list == 0) then
        fail(where .. " needs 'centers' as a list of { x, z, area }")
    end
    local slots = list and #list or count
    local skipped = 0
    for s = 1, slots do
        local key = tostring(s)
        local area = list and (list[s][3] or (range and draw(0.5))) or draw(h(s, 5))
        if pattern == "line" or pattern == "arc" then
            area = area * (1.0 - (spec.decay or 0.0) * (s - 0.5) / slots)
        end
        local done = false
        -- On a curve the place is fixed, so an island may shrink further to fit between its neighbours.
        for shrink = 0, (pattern == "line" or pattern == "arc" or pattern == "ring") and 8 or 3 do
            local out = make(key, 0.0, 0.0, area * 0.85 ^ shrink)
            local r = radius_of(out)
            for t = 1, (list and 1 or 30) do
                local x, z
                if list then
                    x, z = list[s][1], list[s][2]
                else
                    x, z = candidate(s, t)
                end
                if inside(x, z, r) and clear(x, z, r) and (region == nil or region(x, z)) then
                    out.at = { x, z }
                    keep(key, out, r)
                    done = true
                    break
                end
            end
            if done then break end
        end
        if not done then skipped = skipped + 1 end
    end
    return entries, slots - skipped, skipped
end

-- --- Ridge: a range from one element ----------------------------------------------------------------

local RIDGE_KEYS = { points = true, width = true, height = true, saddles = true, spurs = true, spacing = true }

-- The relief elements one `ridge` element stands for: the crest in pieces, each a band tapering at its ends
-- so the pieces meet in saddles, and spurs off it at intervals on alternating sides (Azgaar's "Range" built
-- from shapes: its spurs walk downhill every few cells, these are capsules every `spacing` metres, 1.2
-- widths by default).
local function ridge(e, id, ctx)
    local where = "Lands.Compose ridge '" .. tostring(id) .. "'"
    local r = e.ridge
    if type(r) ~= "table" then
        fail(where .. " expects 'ridge' as { points = { { x, z }, ... }, width = m, height = m, saddles = n, spurs = true }")
    end
    for key in pairs(r) do
        if not RIDGE_KEYS[key] then
            fail(where .. " does not take '" .. tostring(key) .. "'; it takes points, width, height, saddles, spurs and spacing")
        end
    end
    local path = r.points
    if type(path) ~= "table" or #path < 2 then
        fail(where .. " needs 'points', at least two { x, z } points along its crest")
    end
    if type(r.width) ~= "number" or r.width <= 0 or type(r.height) ~= "number" then
        fail(where .. " needs 'width' (metres across, above zero) and 'height' (metres)")
    end
    local h = engine.hasher((ctx.seed or 0) * 7919 + salt_of(tostring(id)))
    local width, height = r.width, r.height
    -- The path by arc length.
    local along, total = { 0.0 }, 0.0
    for k = 2, #path do
        total = total + math.sqrt((path[k][1] - path[k - 1][1]) ^ 2 + (path[k][2] - path[k - 1][2]) ^ 2)
        along[k] = total
    end
    local function at(d)
        for k = 2, #path do
            if d <= along[k] or k == #path then
                local u = (d - along[k - 1]) / math.max(along[k] - along[k - 1], 1e-9)
                local dx, dz = path[k][1] - path[k - 1][1], path[k][2] - path[k - 1][2]
                local l = math.max(math.sqrt(dx * dx + dz * dz), 1e-9)
                return path[k - 1][1] + dx * u, path[k - 1][2] + dz * u, dx / l, dz / l
            end
        end
    end
    local entries = {}
    local function add(key, element)
        element.relief = true
        entries[#entries + 1] = { e = element, id = id .. "/" .. key }
    end
    -- The crest: `saddles + 1` pieces, overlapping by a width, each a band 0.4 of its width at its ends.
    -- The highest piece is `height`, the others 0.8 to 1 of it.
    local pieces, top = (r.saddles or 2) + 1, 0.0
    for k = 1, pieces do top = math.max(top, h(k, 1)) end
    for k = 1, pieces do
        local d0 = math.max((k - 1) / pieces * total - width * 0.5, 0.0)
        local d1 = math.min(k / pieces * total + width * 0.5, total)
        local points = {}
        for q = 0, 6 do
            local x, z = at(d0 + (d1 - d0) * q / 6)
            local taper = 1.0 - 0.6 * math.abs(q / 3 - 1.0) ^ 2
            points[#points + 1] = { x, z, width * taper }
        end
        add("crest/" .. k, { shape = "band", points = points,
                             profile = { "cone", height = height * (0.8 + 0.2 * h(k, 1) / top), width = width * 0.5 } })
    end
    -- Spurs: from the crest at intervals, alternating sides, turned 20 to 40 degrees from the normal.
    if r.spurs ~= false then
        local spacing = r.spacing or width * 1.2
        local n = math.max(1, math.floor(total / spacing + 0.5))
        for k = 1, n do
            local x, z, tx, tz = at((k - 0.5) * total / n)
            local side = k % 2 == 0 and 1.0 or -1.0
            local turn = math.rad((20.0 + 20.0 * h(100 + k, 1)) * (h(100 + k, 2) < 0.5 and 1.0 or -1.0))
            local nx, nz = -tz * side, tx * side
            local dx, dz = nx * math.cos(turn) - nz * math.sin(turn), nx * math.sin(turn) + nz * math.cos(turn)
            local length = width * (0.6 + 0.6 * h(100 + k, 3))
            add("spur/" .. k, { shape = "capsule", from = { x, z }, to = { x + dx * length, z + dz * length }, width = width * 0.35,
                                profile = { "cone", height = height * 0.55, width = width * 0.175 } })
        end
    end
    return entries
end

-- Positions given as fractions of a bounded map: `at = { "30%", "75%" }` is 30 % of the way across in x and
-- 75 % in z from the map's corner (-size / 2). Resolved in place in a copy of the element: its placement,
-- ends, points, and its scatter's and ridge's.
local function percent(e, size)
    local function value(v)
        if type(v) == "string" and v:match("^%-?[%d%.]+%%$") then
            return -0.5 * size + tonumber(v:sub(1, -2)) * 0.01 * size
        end
        return v
    end
    local function pair(p)
        if type(p) ~= "table" then return p end
        local out = {}
        for k, v in pairs(p) do out[k] = (k <= 2) and value(v) or v end
        return out
    end
    local function list(l)
        if type(l) ~= "table" then return l end
        local out = {}
        for k, p in ipairs(l) do out[k] = pair(p) end
        return out
    end
    e.at, e.from, e.to, e.points = pair(e.at), pair(e.from), pair(e.to), list(e.points)
    for _, key in ipairs({ "scatter", "ridge" }) do
        if type(e[key]) == "table" then
            local copy = {}
            for k, v in pairs(e[key]) do copy[k] = v end
            copy.center, copy.path, copy.points, copy.centers = pair(copy.center), list(copy.path), list(copy.points), list(copy.centers)
            e[key] = copy
        end
    end
end

-- Moves an element by (dx, dz): its ends if it has them, its placement otherwise (points are in the
-- element's own space, so moving `at` moves them).
local function moved(e, dx, dz)
    local function shift(p) return { p[1] + dx, p[2] + dz } end
    if e.from then
        e.from, e.to = shift(e.from), shift(e.to)
    else
        e.at = shift(e.at or { 0.0, 0.0 })
    end
end

-- The elements as given, less `remove`, with `edit` applied: { e, id } in order, the id being the
-- element's `id` or its place in the given list. An edit replaces the keys it names, except `scale`, which
-- multiplies, and `offset = { dx, dz }`, which moves the element.
local function edited(t, ctx)
    local list, at = {}, {}
    -- The declared land, which what a scatter generates keeps away from.
    ctx.obstacles = {}
    for i, e in ipairs(t.elements) do
        if type(e) == "table" and e.scatter == nil and e.ridge == nil and not e.water and not e.seabed and not e.relief
           and e.shape ~= "fill" then
            local placed_ = {}
            for key, value in pairs(e) do placed_[key] = value end
            if ctx.world_cell == nil then percent(placed_, ctx.size) end
            local out = sized("Lands.Compose element " .. i, placed_)
            local at_ = out.at or (out.from and { (out.from[1] + out.to[1]) * 0.5, (out.from[2] + out.to[2]) * 0.5 }) or { 0.0, 0.0 }
            if out.from then out.length = math.sqrt((out.to[1] - out.from[1]) ^ 2 + (out.to[2] - out.from[2]) ^ 2) end
            ctx.obstacles[#ctx.obstacles + 1] = { x = at_[1], z = at_[2], r = bound_of(out) }
        end
    end
    ctx.scatters = {}
    local function add(entry)
        if at[entry.id] then
            fail("Lands.Compose repeats the id '" .. tostring(entry.id) .. "'")
        end
        list[#list + 1] = entry
        at[entry.id] = #list
    end
    for i, e in ipairs(t.elements) do
        if type(e) ~= "table" then
            fail("Lands.Compose element " .. i .. " must be a table")
        end
        local id = e.id ~= nil and e.id or i
        if at[id] then
            fail("Lands.Compose element " .. i .. " repeats the id '" .. tostring(id) .. "'")
        end
        local copy = {}
        for key, value in pairs(e) do copy[key] = value end
        if ctx.world_cell == nil then percent(copy, ctx.size) end
        if copy.scatter ~= nil then
            local entries, placed, skipped = scatter(copy, tostring(id), ctx)
            ctx.scatters[tostring(id)] = { placed = placed, skipped = skipped }
            for _, entry in ipairs(entries) do
                add(entry)
                -- A later scatter keeps away from this land too.
                local out = (not entry.e.water and not entry.e.seabed and not entry.e.relief) and sized("Lands.Compose element", entry.e) or nil
                if out then ctx.obstacles[#ctx.obstacles + 1] = { x = out.at[1], z = out.at[2], r = bound_of(out) } end
            end
        elseif copy.ridge ~= nil then
            for _, entry in ipairs(ridge(copy, tostring(id), ctx)) do add(entry) end
        else
            add({ e = copy, id = id })
        end
    end
    local function find(id, what)
        if at[id] == nil then
            local known = {}
            for _, entry in ipairs(list) do known[#known + 1] = tostring(entry.id) end
            fail("Lands.Compose " .. what .. " names '" .. tostring(id) .. "', which is no element's id; the ids are "
                 .. table.concat(known, ", "))
        end
        return list[at[id]]
    end
    if t.edit ~= nil then
        if type(t.edit) ~= "table" then
            fail("Lands.Compose expects 'edit' as { [id] = { key = value, ... } }")
        end
        for id, change in pairs(t.edit) do
            local e = find(id, "edit").e
            if type(change) ~= "table" then
                fail("Lands.Compose edit of '" .. tostring(id) .. "' must be a table of keys to change")
            end
            for key, value in pairs(change) do
                if key == "scale" and e.area then
                    -- An element sized by its area keeps that meaning: scaled, its area grows by the square.
                    e.area = e.area * value * value
                elseif key == "scale" then
                    e.scale = (e.scale or 1.0) * value
                elseif key == "offset" then
                    if type(value) ~= "table" or type(value[1]) ~= "number" or type(value[2]) ~= "number" then
                        fail("Lands.Compose edit of '" .. tostring(id) .. "' needs offset as { dx, dz }")
                    end
                    moved(e, value[1], value[2])
                else
                    e[key] = value
                end
            end
        end
    end
    if t.remove ~= nil then
        if type(t.remove) ~= "table" then
            fail("Lands.Compose expects 'remove' as a list of ids")
        end
        local gone = {}
        for _, id in ipairs(t.remove) do gone[find(id, "remove")] = true end
        local kept = {}
        for _, entry in ipairs(list) do
            if not gone[entry] then kept[#kept + 1] = entry end
        end
        list = kept
    end
    for _, entry in ipairs(list) do entry.e.id = nil end
    return list
end

function Lands.Compose(t)
    if type(t) ~= "table" then
        fail("Lands.Compose expects a table of named arguments, as in Lands.Compose{ ... }")
    end
    for key in pairs(t) do
        if not COMPOSE_KEYS[key] then
            local list = {}
            for k in pairs(COMPOSE_KEYS) do list[#list + 1] = k end
            table.sort(list)
            fail("Lands.Compose does not take '" .. tostring(key) .. "'; it accepts " .. table.concat(list, ", "))
        end
    end
    local frame = t.frame
    -- A map of `size` metres centred on the origin, or the unbounded world (`world_cell`): there the scales
    -- of the coast and the shore follow the world cell, never the window, so any two exports agree, and
    -- `region` is only where the report measures.
    local size, window
    if type(frame) == "table" and type(frame.world_cell) == "number" and frame.world_cell > 0 then
        size = frame.world_cell
        window = frame.region or { -2.0 * size, -2.0 * size, 2.0 * size, 2.0 * size }
        if type(window) ~= "table" or #window ~= 4 or window[3] <= window[1] or window[4] <= window[2] then
            fail("Lands.Compose expects frame.region as { min_x, min_z, max_x, max_z }")
        end
    elseif type(frame) == "table" and type(frame.size) == "number" and frame.size > 0 then
        size = frame.size
        window = { -size * 0.5, -size * 0.5, size * 0.5, size * 0.5 }
    else
        fail("Lands.Compose needs 'frame' as { size = metres }, the side of the map centred on the origin, or "
             .. "{ world_cell = metres, region = { min_x, min_z, max_x, max_z } } for the unbounded world")
    end
    if type(t.elements) ~= "table" then
        fail("Lands.Compose needs 'elements', a list of shapes of land and water")
    end
    local seed       = t.seed or 0
    local resolution = t.resolution or 8.0

    local relief     = t.height or 600.0
    local margin     = MARGINS[t.margin or "compressed"]
    local coast      = COASTS[t.coast or "mixed"]
    if margin == nil then
        fail("Lands.Compose margin must be 'compressed' or 'geologic', got '" .. tostring(t.margin) .. "'")
    end
    if coast == nil then
        fail("Lands.Compose coast must be 'mixed', 'rocky', 'sedimentary' or 'cliff', got '" .. tostring(t.coast) .. "'")
    end
    local width_scale   = margin.scale and size * 0.5 or 1.0
    local shelf_depth   = t.shelf_depth or margin.shelf_depth
    local abyssal_depth = t.abyssal_depth or margin.abyssal_depth
    local shelf_width   = t.shelf_width or margin.shelf_width * width_scale
    local slope_width   = t.slope_width or margin.slope_width * width_scale
    if abyssal_depth <= shelf_depth or shelf_width <= 0 or slope_width <= 0 then
        fail("Lands.Compose needs abyssal_depth greater than shelf_depth and positive widths")
    end

    -- Ids: a script names an element with any string or number; the node carries numbers from 1.
    local elements, ids, names = {}, {}, {}
    -- `land_fraction` (a bounded map) rescales what the scatters generate until the land they and the declared
    -- elements cover, by closed-form areas, is that share of the frame: a few passes, since a larger size can
    -- leave a slot without room.
    local ctx = { seed = seed, size = size, window = window, world_cell = frame.world_cell, area_factor = 1.0,
                  edge_margin = frame.edge_margin }
    local source = edited(t, ctx)
    if t.land_fraction ~= nil then
        local f = t.land_fraction
        if type(f) ~= "number" or f <= 0.0 or f >= 1.0 then
            fail("Lands.Compose needs land_fraction between 0 and 1")
        end
        if frame.world_cell ~= nil then
            fail("Lands.Compose takes land_fraction only on a bounded map; in the unbounded world set a scatter's "
                 .. "count per world cell (its expected land is count times the mean area)")
        end
        local function areas()
            local generated, declared = 0.0, 0.0
            for _, entry in ipairs(source) do
                local e = entry.e
                if not e.water and not e.seabed and not e.relief and e.shape ~= "fill" then
                    local a = geometric_area(sized("Lands.Compose element", e)) or 0.0
                    if entry.scatter then generated = generated + a else declared = declared + a end
                end
            end
            return generated, declared
        end
        -- The pass nearest the target is kept: past what the map can hold, a larger size only leaves more
        -- slots without room (the report says how many).
        local best, error_ = source, math.huge
        local saved = ctx.scatters
        for _ = 1, 10 do
            local generated, declared = areas()
            if generated <= 0.0 then
                fail("Lands.Compose land_fraction needs a scatter element to size")
            end
            local want = f * size * size - declared
            if want <= 0.0 then break end
            local miss = math.abs(generated / want - 1.0)
            if miss < error_ then best, error_, saved = source, miss, ctx.scatters end
            if miss < 0.01 then break end
            ctx.area_factor = ctx.area_factor * want / generated
            source = edited(t, ctx)
        end
        source, ctx.scatters = best, saved
    end
    for i, entry in ipairs(source) do
        local e = entry.e
        local id = entry.id
        if ids[id] then
            fail("Lands.Compose element " .. i .. " repeats the id '" .. tostring(id) .. "'")
        end
        ids[id], names[i] = i, id
        elements[i] = set_element(i, e, i, { land = relief, seabed = abyssal_depth * 0.5 }, t.profile)
        -- An active margin: a trench off this element's coast, at the foot of the slope.
        if e.margin ~= nil then
            local m = e.margin
            if e.water or e.seabed then
                fail("Lands.Compose element " .. i .. " takes 'margin' only as land")
            end
            if m == "active" then
                m = {}
            elseif m == "passive" then
                m = nil
            elseif type(m) ~= "table" then
                fail("Lands.Compose element " .. i .. " margin must be 'passive', 'active' or { depth, offset, width }")
            end
            if m then
                elements[i].trench = {
                    depth = m.depth or abyssal_depth * 0.8, offset = m.offset or shelf_width + 0.9 * slope_width,
                    width = m.width or slope_width * 0.15,
                }
            end
        end
    end

    -- Measured as far as the sea floor reaches and as far inland as a profile rises.
    local reach = shelf_width + slope_width
    for _, e in ipairs(elements) do
        reach = math.max(reach, (e.inland or 0.0) * 1.05)
        e.inland = nil
    end
    -- The sea floor is built on the coast distance with the land joined over half the shelf and slope, so it
    -- has no crease where two coasts are equally near; the coast itself is the hard union.
    local set, raised, floor, joined = Shapes.Set{ elements = elements, reach = reach, relief = true, seabed = true,
                                                   margin = 0.5 * (shelf_width + slope_width),
                                                   blend = math.max(relief * 0.05, 1.0) }
    local coast_sdf  = set.value
    local transition = math.max(resolution * 1.5, 12.0)
    local land_mask  = 1.0 - Curves.Smoothstep{ input = coast_sdf, min = -transition, max = transition }

    -- Lagoons: water elements with `role = "lagoon"` are shallow (`lagoon_depth`) whatever their distance
    -- from the coast, as inside an atoll's ring.
    local lagoons = {}
    for i, entry in ipairs(source) do
        local e = entry.e
        if e.role == "lagoon" then
            if not e.water then
                fail("Lands.Compose element " .. i .. " has role 'lagoon' and must be water")
            end
            local copy = {}
            for key, value in pairs(elements[i]) do copy[key] = value end
            copy.water = nil
            lagoons[#lagoons + 1] = copy
        end
    end
    local lagoon = nil
    if #lagoons > 0 then
        lagoon = 1.0 - Curves.Smoothstep{ input = Shapes.Set{ elements = lagoons, reach = reach }.value,
                                          min = -transition, max = transition }
    end

    -- The hypsometry: how much of the land is low, as a fixed curve of the height over `height` (monotone, so
    -- it keeps the order of heights and the drainage): low x^2, balanced x, high 1 - (1 - x)^2.
    local hypsometry = t.hypsometry or "balanced"
    local ground = raised.value
    if hypsometry == "low" then
        ground = Curves.Power{ input = ground / relief, exponent = 2.0 } * relief
    elseif hypsometry == "high" then
        ground = ground * (2.0 - ground / relief)
    elseif hypsometry ~= "balanced" then
        fail("Lands.Compose hypsometry must be 'low', 'balanced' or 'high', got '" .. tostring(hypsometry) .. "'")
    end

    -- Classes by measured area: in a bounded map a tenth and a thousandth of it (as Azgaar's generator
    -- does with its cells), in the unbounded world 10 000 km2 and 1 km2; `classes` overrides either.
    local classes = {
        continent = (t.classes and t.classes.continent) or (frame.world_cell and 1e10 or size * size * 0.1),
        island = (t.classes and t.classes.island) or (frame.world_cell and 1e6 or size * size * 0.001),
        sea = (t.classes and t.classes.sea) or (frame.world_cell and 1e9 or size * size * 0.01),
    }
    -- The elements as resolved, reported to the export's metadata: what was asked for (the shape's
    -- closed-form area) and what came out of the composition (area, centroid and highest profile by id,
    -- measured on the CPU over the frame).
    local cell = math.max(window[3] - window[1], window[4] - window[2]) / 256
    local nx = math.max(math.floor((window[3] - window[1]) / cell + 0.5), 1)
    local nz = math.max(math.floor((window[4] - window[2]) / cell + 0.5), 1)
    local ox, oz = window[1] + cell * 0.5, window[2] + cell * 0.5
    local sampled = {
        d = Terrain.Sample{ value = coast_sdf, origin = { ox, oz }, size = { nx, nz }, cell = cell },
        id = Terrain.Sample{ value = set.id, origin = { ox, oz }, size = { nx, nz }, cell = cell },
        h = Terrain.Sample{ value = raised.value, origin = { ox, oz }, size = { nx, nz }, cell = cell },
    }
    local measured = {}
    for k = 1, nx * nz do
        if sampled.d[k] < 0.0 then
            local i = math.floor(sampled.id[k] + 0.5)
            local m = measured[i] or { cells = 0, x = 0.0, z = 0.0, top = -math.huge }
            m.cells = m.cells + 1
            m.x = m.x + ox + ((k - 1) % nx) * cell
            m.z = m.z + oz + ((k - 1) // nx) * cell
            m.top = math.max(m.top, sampled.h[k])
            measured[i] = m
        end
    end
    local listed, land_cells = {}, 0
    for i, entry in ipairs(source) do
        local e, m = elements[i], measured[i]
        local row = {
            id = names[i], shape = e.shape, role = entry.e.role,
            kind = e.water and "water" or (e.seabed and "seabed" or (e.relief and "relief" or "land")),
            at = e.at or (e.from and { (e.from[1] + e.to[1]) * 0.5, (e.from[2] + e.to[2]) * 0.5 }) or { 0.0, 0.0 },
            area = geometric_area(e), radius = bound_of(e), from = e.from, to = e.to,
        }
        if m then
            row.area_measured = m.cells * cell * cell
            row.centroid = { m.x / m.cells, m.z / m.cells }
            row.highest = m.top
            land_cells = land_cells + m.cells
            row.class = row.kind == "land" and (row.area_measured >= classes.continent and "continent"
                        or row.area_measured >= classes.island and "island" or "isle") or nil
        end
        listed[i] = row
    end
    -- Water bodies: connected components of the water on the same grid, each with the water elements whose
    -- centre falls in it. The role of the first of those names it; otherwise `ocean` if it touches the frame
    -- (in the unbounded world, the window), `sea` over `classes.sea`, else `lake`.
    local waters, label = {}, {}
    for k = 1, nx * nz do
        if sampled.d[k] >= 0.0 and not label[k] then
            local body = { cells = 0, touches_edge = false, ids = {} }
            waters[#waters + 1] = body
            local stack = { k }
            label[k] = body
            while #stack > 0 do
                local q = table.remove(stack)
                local x, z = (q - 1) % nx, (q - 1) // nx
                body.cells = body.cells + 1
                body.touches_edge = body.touches_edge or x == 0 or z == 0 or x == nx - 1 or z == nz - 1
                for _, n in ipairs({ x > 0 and q - 1, x < nx - 1 and q + 1, z > 0 and q - nx, z < nz - 1 and q + nx }) do
                    if n and not label[n] and sampled.d[n] >= 0.0 then
                        label[n] = body
                        stack[#stack + 1] = n
                    end
                end
            end
        end
    end
    for i, entry in ipairs(source) do
        if elements[i].water then
            local x = math.floor((listed[i].at[1] - window[1]) / cell)
            local z = math.floor((listed[i].at[2] - window[2]) / cell)
            local body = x >= 0 and z >= 0 and x < nx and z < nz and label[z * nx + x + 1]
            if body then
                body.ids[#body.ids + 1] = names[i]
                body.role = body.role or entry.e.role
            end
        end
    end
    for _, body in ipairs(waters) do
        body.area = body.cells * cell * cell
        body.class = body.role or (body.touches_edge and "ocean") or (body.area >= classes.sea and "sea") or "lake"
        body.cells, body.role = nil, nil
        if #body.ids == 0 then body.ids = nil end
    end
    table.sort(waters, function(a, b) return a.area > b.area end)
    engine.report("lands", {
        frame = frame, window = window, land_fraction = land_cells / (nx * nz), cell = cell, elements = listed,
        waters = waters, scatters = ctx.scatters, classes = classes,
    })

    return finish{
        t = t, seed = seed, size = size, resolution = resolution, relief = relief, coast = coast,
        coast_sdf = coast_sdf, land_mask = land_mask, region_id = set.id,
        ground = ground, gullies = false, additive = true, lagoon = lagoon, seabed = floor.value, margin_sdf = joined.value,
        shelf_width = shelf_width, shelf_depth = shelf_depth, slope_width = slope_width, abyssal_depth = abyssal_depth,
        info = { size = size, extent = size * 0.5, regions = #elements, ids = names, elements = listed,
                 scatters = ctx.scatters, waters = waters, land_fraction = land_cells / (nx * nz) },
    }
end

-- --- Presets --------------------------------------------------------------------------------------

-- Islands as compositions of shapes: `Lands.Preset` returns the `Lands.Compose` table, to change before it
-- is built; `Lands.Island` builds it. Every choice is drawn from `seed`, every size from `size` and
-- `land_fraction`, and every element has an id an `edit` can name.

local ISLAND_PRESETS = {
    -- fraction of the map that is land, the highest ground, the coast
    high_island = { fraction = 0.30, height = 900, coast = "mixed" },
    low_island  = { fraction = 0.25, height = 12, coast = "sedimentary" },
    volcano     = { fraction = 0.20, height = 1400, coast = "rocky" },
    caldera     = { fraction = 0.20, height = 650, coast = "rocky" },
    rim_island  = { fraction = 0.28, height = 380, coast = "cliff" },
    mesa_island = { fraction = 0.25, height = 320, coast = "cliff" },
    atoll       = { fraction = 0.08, height = 5, coast = "sedimentary" },
}
local PRESET_KEYS = { name = true, seed = true, size = true, land_fraction = true, height = true, count = true, area = true }
-- Keys the grid-built island took that a composition of shapes has no use for.
local RETIRED = {
    layout = "take a 'preset' (Lands.Island and Lands.Archipelago list theirs)",
    roughness = "the coast's detail comes after the shapes (phase 4 of the lands plan); edit the shapes",
    dissection = "drainage comes after the shapes; edit the shapes",
    cells = "nothing is simulated on a grid", count = "an island is one composition; see Lands.Archipelago",
    aspect = "edit the 'main' element's aspect", area = "draw it as a polygon element in Lands.Compose",
    areas = "draw them as polygon elements in Lands.Compose",
}

-- A lobed outline from the seed: two harmonics, one broad and one finer, at random phases.
local function lobed(h, salt, broad, fine)
    return {
        { 2 + math.floor(h(salt, 1) * 2), broad * (0.6 + 0.4 * h(salt, 2)), h(salt, 3) * 2.0 * math.pi },
        { 5 + math.floor(h(salt, 4) * 2), fine * (0.5 + 0.5 * h(salt, 5)), h(salt, 6) * 2.0 * math.pi },
    }
end

-- Archipelagos: scatters of islands, the patterns of Azgaar's templates and of nature. Sizes are fractions of
-- the map's area (`size`^2), so the same preset reads the same at any scale; `count`, `area` (a { min, max }
-- range in square metres) and `land_fraction` override them.
local ARCHIPELAGO_PRESETS = {
    -- islands of many sizes anywhere, apart by straits
    scattered = { count = 12, area = { 0.0015, 0.02 }, height = 600, coast = "mixed" },
    -- racimos: islands round a few centres (Neyman-Scott)
    cluster = { count = 16, area = { 0.0005, 0.006 }, height = 450, coast = "mixed" },
    -- a hotspot chain: large and high at one end, smaller and lower along it
    hotspot = { count = 7, area = { 0.007, 0.007 }, height = 1300, coast = "rocky" },
    -- an island arc over a subduction trench
    arc = { count = 9, area = { 0.0015, 0.005 }, height = 1100, coast = "rocky" },
    -- atolls: rings of reef round lagoons
    atolls = { count = 6, area = { 0.0008, 0.002 }, height = 5, coast = "sedimentary" },
    -- one land cut into pieces by channels (Azgaar's shattered)
    shattered = { count = 8, area = { 0.0005, 0.0012 }, height = 500, coast = "rocky" },
    -- a broad chain of islands side by side, filling a band of the map
    chain = { count = 8, area = { 0.005, 0.008 }, height = 700, coast = "mixed" },
}

local function archipelago_preset(t)
    local p = ARCHIPELAGO_PRESETS[t.name]
    local seed, size = t.seed or 0, t.size or 48000.0
    local h = engine.hasher(seed + 9000)
    local turn = h(1, 1) * 360.0
    local s2 = size * size
    local range = t.area or { p.area[1] * s2, p.area[2] * s2 }
    local count = t.count or p.count
    local height = t.height or p.height
    local margin = size * 0.04
    local a = math.rad(turn)
    local c, s = math.cos(a), math.sin(a)
    local elements
    if t.name == "scattered" then
        elements = { { id = "isles", shape = "radial", profile = "dome",
                       scatter = { count = count, area = range, spacing = size * 0.015, margin = margin } } }
    elseif t.name == "cluster" then
        elements = { { id = "isles", shape = "radial", profile = "cone",
                       scatter = { pattern = "cluster", count = count, clusters = 3, spread = size * 0.07,
                                   area = range, spacing = size * 0.004, margin = margin } } }
    elseif t.name == "hotspot" then
        local l = size * 0.4
        elements = { { id = "chain", shape = "ellipse", aspect = { 1.2, 1.8 }, profile = "cone",
                       scatter = { pattern = "line", count = count, area = range, decay = 0.85, spacing = size * 0.01,
                                   wander = size * 0.02, margin = margin,
                                   path = { { -c * l, -s * l }, { -s * l * 0.15, c * l * 0.15 }, { c * l, s * l } } } } }
    elseif t.name == "arc" then
        -- The trench is one form of the sea floor along the convex side, a V of the abyssal depth under it.
        local radius = size * 0.75
        local centre = { -c * size * 0.55, -s * size * 0.55 }
        elements = {
            { id = "arc", shape = "radial", profile = "cone",
              scatter = { pattern = "arc", count = count, area = range, spacing = size * 0.01, wander = size * 0.015,
                          center = centre, radius = radius, from_angle = turn - 32, to_angle = turn + 32, margin = margin } },
            { id = "trench", shape = "arc", seabed = true, at = centre, rotation = turn, radius = radius + size * 0.16,
              aperture = 76, width = size * 0.08, profile = { "cone", height = -2500, width = size * 0.04 } },
        }
    elseif t.name == "atolls" then
        elements = { { id = "atolls", shape = "ring", width = size * 0.004, lagoon = true, profile = "flat",
                       scatter = { count = count, area = range, spacing = size * 0.02, margin = margin } } }
    elseif t.name == "shattered" then
        -- A land broken by channels: a large block, then water capsules scattered over it.
        local block = s2 * 0.3
        elements = {
            { id = "land", shape = "superellipse", area = block, aspect = 1.3, exponent = 3, rotation = turn, profile = "dome" },
            { id = "channels", shape = "capsule", water = true, length = size * 0.1, smooth = size * 0.003,
              scatter = { count = count, area = range, spacing = size * 0.005,
                          region = { shape = "superellipse", radii = { size * 0.25, size * 0.19 }, exponent = 3, rotation = turn } } },
        }
    else -- chain: islands side by side along a band across the map
        local l = size * 0.42
        elements = { { id = "chain", shape = "radial", profile = "dome",
                       scatter = { pattern = "line", count = count, area = range, spacing = size * 0.006, margin = margin,
                                   wander = size * 0.03, path = { { -c * l, -s * l }, { c * l, s * l } } } } }
    end
    return {
        seed = seed, frame = { size = size }, height = height, coast = p.coast,
        land_fraction = t.land_fraction, lagoon_depth = t.name == "atolls" and 25 or nil, elements = elements,
    }
end

-- Continents and other lands: Azgaar's heightmap templates rebuilt from shapes (blocks, lobes, ranges with
-- spurs, gulfs, inland seas, straits), plus land with no sea and sea with no land. The land rises little on
-- its own (a low plateau) and the ranges carry the relief; sizes are fractions of the map.
local LAND_PRESETS = {
    continent     = { size = 192000, height = 2200, coast = "mixed" },
    pangea        = { size = 192000, height = 2000, coast = "mixed" },
    old_world     = { size = 192000, height = 2000, coast = "mixed" },
    mediterranean = { size = 96000, height = 1600, coast = "rocky" },
    inland_sea    = { size = 96000, height = 1400, coast = "mixed" },
    peninsula     = { size = 48000, height = 900, coast = "rocky" },
    isthmus       = { size = 48000, height = 700, coast = "mixed" },
    fractious     = { size = 96000, height = 900, coast = "rocky" },
    taklamakan    = { size = 96000, height = 2400, coast = "mixed" },
    all_land      = { size = 24000, height = 500, coast = "mixed" },
    lakeland      = { size = 24000, height = 120, coast = "sedimentary" },
    ocean_only    = { size = 48000, height = 100, coast = "mixed" },
}

local function land_preset(t)
    local p = LAND_PRESETS[t.name]
    local seed, size = t.seed or 0, t.size or p.size
    local height = t.height or p.height
    local h = engine.hasher(seed + 11000)
    -- The ranges of the closed basin follow the map's edges; the other lands turn freely.
    local turn = t.name == "taklamakan" and 0.0 or h(1, 1) * 360.0
    local a = math.rad(turn)
    local c, s = math.cos(a), math.sin(a)
    local s2 = size * size
    -- A point `u` along and `v` across the main axis, in fractions of the map.
    local function axis(u, v) return { (c * u - s * v) * size, (s * u + c * v) * size } end
    local function range(id, u0, v0, u1, v1, width, high, saddles)
        local m = { (u0 + u1) * 0.5 + 0.03 * (h(salt_of(id), 1) - 0.5), (v0 + v1) * 0.5 + 0.04 * (h(salt_of(id), 2) - 0.5) }
        return { id = id, ridge = { points = { axis(u0, v0), axis(m[1], m[2]), axis(u1, v1) }, width = width * size,
                                   height = high * height, saddles = saddles or 3 } }
    end
    -- The lowland rises over the distance the shore's cliff fades in (Compose's size * 0.01), and stands above
    -- that cliff, so no trough runs behind the coast.
    local low = { "plateau", height = height * 0.08, edge = size * 0.01 }
    local elements, fraction = {}, t.land_fraction
    if t.name == "continent" then
        -- A block with lobes, gulfs and two ranges, the main one along its long axis.
        elements = { { id = "main", shape = "superellipse", area = s2 * 0.26, aspect = 1.5, exponent = 3, rotation = turn } }
        -- Lobes and gulfs at directions 70 degrees apart from the main axis, so no lobe covers a gulf.
        for i, angle in ipairs({ 20.0, 160.0, 300.0 }) do
            local b = math.rad(turn + angle + 20.0 * (h(10 + i, 1) - 0.5))
            local r = size * (0.27 + 0.05 * h(10 + i, 2))
            elements[#elements + 1] = { id = "lobe" .. i, shape = "radial", area = s2 * (0.03 + 0.04 * h(10 + i, 3)),
                                        at = { math.cos(b) * r, math.sin(b) * r }, smooth = size * 0.05 }
        end
        -- The main block's radii (x over z 1.5, closed-form area 3.533 ab at exponent 3).
        local ra = math.sqrt(0.26 / 3.533 * 1.5) * size
        local rb = ra / 1.5
        for i, angle in ipairs({ 90.0, 230.0 }) do
            local phi = math.rad(angle + 20.0 * (h(20 + i, 1) - 0.5))
            local b = math.rad(turn) + phi
            -- Centred on the main block's shore, so the gulf opens to the sea.
            local r = (math.abs(math.cos(phi) / ra) ^ 3 + math.abs(math.sin(phi) / rb) ^ 3) ^ (-1.0 / 3.0)
            elements[#elements + 1] = { id = "gulf" .. i, shape = "ellipse", water = true, radii = { size * 0.06, size * 0.035 },
                                        at = { math.cos(b) * r, math.sin(b) * r }, rotation = math.deg(b), smooth = size * 0.01 }
        end
        elements[#elements + 1] = range("range", -0.25, -0.04, 0.25, 0.03, 0.05, 1.0, 4)
        elements[#elements + 1] = range("hills", -0.12, 0.13, 0.1, 0.17, 0.035, 0.45, 2)
    elseif t.name == "pangea" then
        -- Four blocks round a central band, joined; two inland seas; ranges where blocks meet.
        for i, q in ipairs({ { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } }) do
            elements[#elements + 1] = { id = "block" .. i, shape = "superellipse", area = s2 * 0.07, aspect = 1.3, exponent = 2.6,
                                        at = axis(q[1] * 0.19, q[2] * 0.16), rotation = turn + 30.0 * (h(i, 1) - 0.5),
                                        smooth = size * 0.06 }
        end
        elements[#elements + 1] = { id = "band", shape = "capsule", from = axis(-0.3, 0.0), to = axis(0.3, 0.0),
                                    width = size * 0.16, smooth = size * 0.06 }
        elements[#elements + 1] = { id = "sea1", shape = "ellipse", water = true, role = "sea", radii = { size * 0.06, size * 0.04 },
                                    at = axis(-0.12, 0.09), rotation = turn }
        elements[#elements + 1] = { id = "sea2", shape = "ellipse", water = true, role = "sea", radii = { size * 0.05, size * 0.035 },
                                    at = axis(0.14, -0.1), rotation = turn + 40 }
        elements[#elements + 1] = range("range1", -0.3, -0.02, 0.0, 0.03, 0.04, 1.0)
        elements[#elements + 1] = range("range2", 0.05, 0.12, 0.3, 0.02, 0.035, 0.8)
    elseif t.name == "old_world" then
        -- Three blocks apart by straits, a range across the largest.
        local blocks = { { -0.2, 0.08, 0.12 }, { 0.17, 0.1, 0.08 }, { 0.02, -0.2, 0.07 } }
        for i, b in ipairs(blocks) do
            elements[#elements + 1] = { id = "block" .. i, shape = "radial", area = s2 * b[3], at = axis(b[1], b[2]),
                                        rotation = turn + 90 * i, smooth = size * 0.02 }
        end
        elements[#elements + 1] = { id = "strait", shape = "band", water = true, width = size * 0.03,
                                    points = { axis(-0.05, 0.3), axis(-0.02, 0.0), axis(0.15, -0.08), axis(0.4, -0.1) } }
        elements[#elements + 1] = range("range1", -0.32, 0.05, -0.08, 0.12, 0.04, 1.0)
        elements[#elements + 1] = range("range2", 0.08, 0.15, 0.26, 0.05, 0.035, 0.7)
    elseif t.name == "mediterranean" then
        -- Land all round a long sea with islands in it, ranges along both shores.
        elements = {
            { id = "land", shape = "fill" },
            { id = "sea", shape = "superellipse", water = true, role = "sea", radii = { size * 0.38, size * 0.15 }, exponent = 2.4,
              rotation = turn },
            { id = "isles", shape = "radial", profile = { "dome", height = height * 0.4 },
              scatter = { count = 6, area = { s2 * 0.002, s2 * 0.008 }, spacing = size * 0.03,
                          region = { shape = "ellipse", radii = { size * 0.3, size * 0.09 }, rotation = turn } } },
            range("north", -0.33, 0.25, 0.33, 0.24, 0.05, 1.0),
            range("south", -0.3, -0.26, 0.3, -0.25, 0.045, 0.75),
        }
    elseif t.name == "inland_sea" then
        elements = {
            { id = "land", shape = "superellipse", area = s2 * 0.45, aspect = 1.3, exponent = 3, rotation = turn },
            { id = "sea", shape = "superellipse", water = true, role = "sea", area = s2 * 0.06, aspect = 1.6, exponent = 2.5,
              rotation = turn + 20, at = axis(0.03, 0.02) },
            range("range", -0.25, -0.2, 0.25, -0.17, 0.05, 1.0),
        }
    elseif t.name == "peninsula" then
        -- A mainland along one side of the map, a peninsula from it, islands off its tip.
        elements = {
            { id = "mainland", shape = "box", size = { size * 1.2, size * 0.5 }, at = axis(0.0, -0.45), rotation = turn },
            { id = "peninsula", shape = "band", smooth = size * 0.03,
              points = { { axis(0.0, -0.22)[1], axis(0.0, -0.22)[2], size * 0.16 }, { axis(0.05, 0.05)[1], axis(0.05, 0.05)[2], size * 0.11 },
                         { axis(-0.02, 0.26)[1], axis(-0.02, 0.26)[2], size * 0.07 } } },
            { id = "isles", shape = "radial", profile = { "dome", height = height * 0.5 },
              scatter = { pattern = "centers", centers = { { axis(0.02, 0.36)[1], axis(0.02, 0.36)[2], s2 * 0.004 },
                                                           { axis(-0.09, 0.39)[1], axis(-0.09, 0.39)[2], s2 * 0.002 },
                                                           { axis(0.1, 0.41)[1], axis(0.1, 0.41)[2], s2 * 0.0015 } } } },
            range("spine", 0.0, -0.25, 0.0, 0.2, 0.05, 0.9, 2),
        }
    elseif t.name == "isthmus" then
        elements = {
            { id = "west", shape = "radial", area = s2 * 0.13, at = axis(-0.26, 0.04) },
            { id = "east", shape = "radial", area = s2 * 0.13, at = axis(0.26, -0.04) },
            { id = "isthmus", shape = "capsule", from = axis(-0.15, 0.02), to = axis(0.15, -0.02), width = size * 0.035,
              smooth = size * 0.02 },
            range("west_range", -0.36, -0.06, -0.18, 0.15, 0.04, 1.0, 2),
            range("east_range", 0.17, -0.15, 0.36, 0.05, 0.04, 0.85, 2),
        }
    elseif t.name == "fractious" then
        -- Many pieces of land merging into each other, cut by long channels.
        elements = {
            { id = "pieces", shape = "radial", smooth = size * 0.03, profile = { "dome", height = height * 0.6 },
              scatter = { count = 14, area = { s2 * 0.008, s2 * 0.025 }, spacing = -size * 0.04, margin = size * 0.05 } },
            { id = "channels", shape = "capsule", water = true, length = size * 0.25, smooth = size * 0.005,
              scatter = { count = 5, area = { s2 * 0.002, s2 * 0.004 }, spacing = size * 0.05 } },
        }
    elseif t.name == "taklamakan" then
        -- A closed basin: mountains on four sides, a desert floor sunk in the middle.
        elements = {
            { id = "land", shape = "fill", profile = { "flat", height = height * 0.25, edge = size * 0.02 } },
            range("north", -0.42, 0.4, 0.42, 0.38, 0.08, 1.0),
            range("south", -0.42, -0.4, 0.42, -0.38, 0.07, 0.8),
            range("west", -0.42, -0.36, -0.4, 0.36, 0.07, 0.85),
            range("east", 0.41, -0.36, 0.42, 0.36, 0.07, 0.7),
            { id = "basin", shape = "superellipse", relief = true, radii = { size * 0.28, size * 0.22 }, exponent = 2.5,
              rotation = turn, profile = { "dome", height = -height * 0.12, width = size * 0.1 } },
        }
    elseif t.name == "all_land" then
        elements = {
            { id = "land", shape = "fill", profile = { "flat", height = height * 0.08, edge = size * 0.02 } },
            { id = "upland", shape = "superellipse", relief = true, radii = { size * 0.25, size * 0.16 }, exponent = 3,
              at = axis(-0.15, 0.12), rotation = turn, profile = { "plateau", height = height * 0.55, edge = size * 0.06 } },
            range("hills", 0.05, -0.38, 0.35, 0.1, 0.1, 0.8, 2),
        }
    elseif t.name == "lakeland" then
        elements = {
            { id = "land", shape = "fill", profile = { "flat", height = height * 0.5, edge = size * 0.01 } },
            { id = "lakes", shape = "radial", water = true, role = "lake",
              scatter = { count = 12, area = { s2 * 0.001, s2 * 0.006 }, spacing = size * 0.02, margin = size * 0.03 } },
        }
    else -- ocean_only
        elements = {
            { id = "seamounts", shape = "radial", seabed = true, profile = { "cone", height = 520 },
              scatter = { count = 5, area = { s2 * 0.006, s2 * 0.02 }, spacing = size * 0.03, margin = size * 0.05 } },
            { id = "guyot", shape = "circle", seabed = true, radius = size * 0.08, at = axis(0.25, -0.2),
              profile = { "plateau", height = 480, edge = size * 0.04 } },
        }
    end
    return {
        seed = seed, frame = { size = size }, height = height, coast = p.coast, profile = low, cliff_height = height * 0.05,
        land_fraction = fraction, elements = elements,
    }
end

function Lands.Preset(t)
    if type(t) ~= "table" then
        fail("Lands.Preset expects a table of named arguments, as in Lands.Preset{ name = 'high_island' }")
    end
    for key in pairs(t) do
        if not PRESET_KEYS[key] then
            fail("Lands.Preset does not take '" .. tostring(key) .. "'; it takes name, seed, size, land_fraction, "
                 .. "height and, for an archipelago, count and area, and returns the Lands.Compose table to change")
        end
    end
    if LAND_PRESETS[t.name] then
        if t.count ~= nil or t.area ~= nil then
            fail("Lands.Preset takes 'count' and 'area' only for an archipelago preset; edit a land's elements by id")
        end
        return land_preset(t)
    end
    if ARCHIPELAGO_PRESETS[t.name] then
        return archipelago_preset(t)
    end
    local p = ISLAND_PRESETS[t.name]
    if p == nil then
        local names = {}
        for k in pairs(ISLAND_PRESETS) do names[#names + 1] = k end
        for k in pairs(ARCHIPELAGO_PRESETS) do names[#names + 1] = k end
        for k in pairs(LAND_PRESETS) do names[#names + 1] = k end
        table.sort(names)
        fail("Lands.Preset has no preset '" .. tostring(t.name) .. "'; the presets are " .. table.concat(names, ", "))
    end
    if t.count ~= nil or t.area ~= nil then
        fail("Lands.Preset takes 'count' and 'area' only for an archipelago preset; an island's elements are edited by id")
    end
    local seed = t.seed or 0
    local size = t.size or 24000.0
    local fraction = t.land_fraction or p.fraction
    if fraction <= 0.0 or fraction >= 0.9 then
        fail("Lands.Preset needs land_fraction between 0 and 0.9")
    end
    local h = engine.hasher(seed + 7000)
    local area = fraction * size * size
    local turn = h(1, 1) * 360.0
    local elements
    if t.name == "high_island" then
        -- A main mass and two lobes joined to it, each rising to its own summit, and a bay.
        local r = math.sqrt(area * 0.8 / math.pi)
        elements = { { id = "main", shape = "radial", area = area * 0.8, rotation = turn, harmonics = lobed(h, 10, 0.18, 0.07),
                       profile = "dome" } }
        for i = 1, 2 do
            local a = math.rad(turn + 120.0 * i + 40.0 * (h(20 + i, 1) - 0.5))
            elements[#elements + 1] = {
                id = "lobe" .. i, shape = "radial", area = area * (i == 1 and 0.13 or 0.07), smooth = r * 0.3,
                at = { math.cos(a) * r * 0.85, math.sin(a) * r * 0.85 }, harmonics = lobed(h, 30 + i, 0.15, 0.05),
                profile = { "dome", height = (t.height or p.height) * (0.45 + 0.2 * h(20 + i, 2)) },
            }
        end
        local b = math.rad(turn + 60.0)
        elements[#elements + 1] = { id = "bay", shape = "ellipse", water = true, radii = { r * 0.28, r * 0.17 },
                                    at = { math.cos(b) * r * 0.95, math.sin(b) * r * 0.95 }, rotation = math.deg(b),
                                    smooth = r * 0.04 }
    elseif t.name == "low_island" then
        elements = { { id = "main", shape = "radial", area = area, rotation = turn, harmonics = lobed(h, 10, 0.25, 0.08),
                       profile = "flat" } }
    elseif t.name == "volcano" then
        local r = math.sqrt(area / math.pi)
        local a = math.rad(turn)
        elements = {
            { id = "main", shape = "radial", area = area, harmonics = lobed(h, 10, 0.06, 0.04), profile = "cone",
              margin = "active" },
            { id = "parasite", shape = "circle", radius = r * 0.22, at = { math.cos(a) * r * 0.6, math.sin(a) * r * 0.6 },
              smooth = r * 0.12, profile = { "cone", height = (t.height or p.height) * 0.38 } },
        }
    elseif t.name == "caldera" then
        local r = math.sqrt(area / math.pi)
        elements = {
            { id = "main", shape = "radial", area = area, harmonics = lobed(h, 10, 0.07, 0.03), profile = "caldera" },
            { id = "breach", shape = "sector", water = true, reach = r * 1.2, angle = 14 + 10 * h(2, 1), rotation = turn,
              smooth = r * 0.04 },
        }
    elseif t.name == "rim_island" then
        elements = { { id = "main", shape = "radial", area = area, rotation = turn, harmonics = lobed(h, 10, 0.18, 0.07),
                       profile = { "rim", inner = (t.height or p.height) * 0.37 } } }
    elseif t.name == "mesa_island" then
        local r = math.sqrt(area / math.pi)
        local a = math.rad(turn + 150.0)
        elements = {
            { id = "main", shape = "superellipse", area = area * 0.92, aspect = 1.3 + 0.5 * h(2, 2), exponent = 3,
              rotation = turn, profile = { "plateau", edge = r * 0.08 } },
            { id = "butte", shape = "superellipse", area = area * 0.08, aspect = 1.4, exponent = 2.5, smooth = r * 0.1,
              at = { math.cos(a) * r * 1.05, math.sin(a) * r * 1.05 }, rotation = turn + 40,
              profile = { "plateau", height = (t.height or p.height) * 0.65, edge = r * 0.06 } },
        }
    else -- atoll: a ring of reef round a lagoon, cut by three passes, with one larger islet
        local r = size * 0.33
        local width = math.max(area / (2.0 * math.pi * r), 40.0)
        elements = {
            { id = "reef", shape = "ring", radius = r, width = width },
            { id = "lagoon", shape = "circle", water = true, role = "lagoon", radius = r - width * 0.5 },
        }
        for i = 1, 3 do
            local a = math.rad(turn + 120.0 * i + 50.0 * (h(40 + i, 1) - 0.5))
            elements[#elements + 1] = { id = "pass" .. i, shape = "circle", water = true, radius = width * (0.8 + 0.5 * h(40 + i, 2)),
                                        at = { math.cos(a) * r, math.sin(a) * r }, smooth = width * 0.2 }
        end
        local a = math.rad(turn + 60.0)
        elements[#elements + 1] = { id = "motu", shape = "ellipse", radii = { width * 1.8, width * 0.8 },
                                    at = { math.cos(a) * r, math.sin(a) * r }, rotation = math.deg(a) + 90, smooth = width * 0.4 }
    end
    return {
        seed = seed, frame = { size = size }, height = t.height or p.height, coast = p.coast,
        profile = t.name == "atoll" and "flat" or nil,
        lagoon_depth = t.name == "atoll" and 25 or nil, elements = elements,
    }
end

-- An island from a preset (see `Lands.Preset`), built: the preset's keys, plus any `Lands.Compose` key,
-- which overrides the preset's (`edit` and `remove` change its elements by id).
function Lands.Island(t)
    if type(t) ~= "table" then
        fail("Lands.Island expects a table of named arguments, as in Lands.Island{ preset = 'high_island' }")
    end
    for key in pairs(t) do
        if RETIRED[key] then
            fail("Lands.Island no longer takes '" .. key .. "': " .. RETIRED[key])
        end
    end
    if t.preset == nil then
        local names = {}
        for k in pairs(ISLAND_PRESETS) do names[#names + 1] = k end
        table.sort(names)
        fail("Lands.Island needs a 'preset': " .. table.concat(names, ", ") .. ". Nothing is assumed about the island")
    end
    local compose = Lands.Preset{ name = t.preset, seed = t.seed, size = t.size, land_fraction = t.land_fraction,
                                  height = t.height }
    for key, value in pairs(t) do
        if not (PRESET_KEYS[key] or key == "preset") then
            compose[key] = value
        end
    end
    return Lands.Compose(compose)
end

-- An archipelago from a preset (see `Lands.Preset`), built: `count`, `area` and `land_fraction` size it,
-- and any `Lands.Compose` key passes through (`edit` reaches its islands by id, as "isles/3").
function Lands.Archipelago(t)
    if type(t) ~= "table" then
        fail("Lands.Archipelago expects a table of named arguments, as in Lands.Archipelago{ preset = 'scattered' }")
    end
    for key in pairs(t) do
        if RETIRED[key] and key ~= "count" and key ~= "area" then
            fail("Lands.Archipelago no longer takes '" .. key .. "': " .. RETIRED[key])
        end
    end
    if not ARCHIPELAGO_PRESETS[t.preset] then
        local names = {}
        for k in pairs(ARCHIPELAGO_PRESETS) do names[#names + 1] = k end
        table.sort(names)
        fail("Lands.Archipelago needs a 'preset': " .. table.concat(names, ", ") .. "; got '" .. tostring(t.preset) .. "'")
    end
    local compose = Lands.Preset{ name = t.preset, seed = t.seed, size = t.size, land_fraction = t.land_fraction,
                                  height = t.height, count = t.count, area = t.area }
    for key, value in pairs(t) do
        if not (PRESET_KEYS[key] or key == "preset") then
            compose[key] = value
        end
    end
    return Lands.Compose(compose)
end

-- A continent, or any other land, from a preset (see `Lands.Preset`), built: any `Lands.Compose` key
-- passes through, so `edit` reaches its blocks, ranges ("range/crest/2", "range/spur/5") and seas by id.
function Lands.Continent(t)
    if type(t) ~= "table" then
        fail("Lands.Continent expects a table of named arguments, as in Lands.Continent{ preset = 'continent' }")
    end
    for key in pairs(t) do
        if RETIRED[key] then
            fail("Lands.Continent no longer takes '" .. key .. "': " .. RETIRED[key])
        end
    end
    if not LAND_PRESETS[t.preset] then
        local names = {}
        for k in pairs(LAND_PRESETS) do names[#names + 1] = k end
        table.sort(names)
        fail("Lands.Continent needs a 'preset': " .. table.concat(names, ", ") .. "; got '" .. tostring(t.preset) .. "'")
    end
    local compose = Lands.Preset{ name = t.preset, seed = t.seed, size = t.size, land_fraction = t.land_fraction,
                                  height = t.height }
    for key, value in pairs(t) do
        if not (PRESET_KEYS[key] or key == "preset") then
            compose[key] = value
        end
    end
    return Lands.Compose(compose)
end

_G.Lands = Lands
