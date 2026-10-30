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
-- `Lands.Island`, `Lands.Archipelago` and `Lands.Continent` build a preset: by default from Azgaar's heightmap
-- template where the preset has one (`Lands.Template`, whose outlines are like his maps' and the references'),
-- else, or with `outline = "shapes"`, from its composition (`Lands.Preset` returns it, to change before
-- building). The resolved elements go to the export's metadata (`engine.report`). The plan this follows is
-- `docs/archive/lands_plan.md`.
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
        beach_mask     = { 0.0, 1.0 },
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
    if o.beach_mask then
        -- Sand is a beach, whatever the rock behind it; out to the water's edge (`o.beach_strip`, the sand
        -- before the land mask fades it there), or half a cliff's profile stands at the edge as a brow.
        coast_type = coast_type * (1.0 - (o.beach_strip or o.beach_mask))
    end

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
    if o.floor then
        -- A floor of its own, taken over from the coast's profile `o.floor_blend` metres offshore.
        bathymetry = Combine.Blend{ a = bathymetry, b = o.floor,
                                    mask = Curves.Smoothstep{ input = off, min = 0.0, max = o.floor_blend } }
    end
    if o.platform then
        -- A platform cut by the waves where the coast is a cliff: `depth` down out to `width`, then a wall.
        local p = o.platform
        local on = Curves.Smoothstep{ input = coast_type, min = 0.45, max = 0.65 } * p.keep
        local out = 1.0 - Curves.Smoothstep{ input = off - p.width, min = -2.0 * resolution, max = 0.0 }
        local flat = -p.depth * Curves.Clamp{ input = off / (2.0 * resolution), min = 0.0, max = 1.0 }
        bathymetry = Combine.Blend{ a = bathymetry, b = Combine.Max{ a = flat, b = bathymetry }, mask = on * out }
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
    -- An ocean floor of drowned land (`o.deep`) goes down to 5.5 times the abyssal depth, in its trenches.
    info.height_max, info.depth_max = relief * 1.6, abyssal_depth * (o.deep and 5.6 or 1.05)
    local land = {
        coast_sdf = coast_sdf, land_mask = o.land_mask, land_height = land_height,
        bathymetry = bathymetry, surface_height = surface_height, shore_mask = shore_mask,
        coast_type = coast_type, region_id = o.region_id, beach_mask = o.beach_mask or o.land_mask * 0.0, info = info,
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
                         .. "land_height, bathymetry, shore_mask, coast_type, beach_mask and region_id (height is the surface)")
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

-- --- Lands.Template: Azgaar's heightmap templates ---------------------------------------------------

-- Azgaar's Fantasy-Map-Generator builds land by a script of steps on a grid of about 10 000 cells (a square
-- lattice of points jittered by 0.9 of their spacing, neighbours by their Voronoi diagram), heights 0 to 100,
-- the sea under 20: hills grown cell by cell outwards, ranges along greedy paths with spurs walking downhill,
-- troughs, straits, pits, masks and smoothing. The steps and their numbers are his
-- (`src/data/heightmap-templates.ts`, `src/model/physical/heightmap-generator.ts`); so is storing every
-- height as a whole number, which sets how far a hill spreads. The coast is the Voronoi outline of the land
-- cells, drawn on a raster finer than the cells and bent below the cell's size by a warp whose amplitude a
-- roughness field sets (his coastline fractalization: calm and jagged stretches).

-- Not one of his: the open ocean floor of a world's cell, in his steps over the whole map and no mask, read
-- upside down (see `template_raster`): its hills are basins, its ranges trenches, its troughs rises.
local SEAFLOOR_TEMPLATE = [[Hill 4-6 30-45 10-90 10-90
Range 3-4 35-50 5-95 5-95
Trough 2-3 20-35 5-95 5-95
Hill 6-8 15-25 5-95 5-95
Pit 3-5 15-25 10-90 10-90
Smooth 2 0 0 0]]

local TEMPLATES = {
    volcano = [[Hill 1 90-100 44-56 40-60
Multiply 0.8 50-100 0 0
Range 1.5 30-55 45-55 40-60
Smooth 3 0 0 0
Hill 1.5 35-45 25-30 20-75
Hill 1 35-55 75-80 25-75
Hill 0.5 20-25 10-15 20-25
Mask 3 0 0 0]],
    high_island = [[Hill 1 90-100 65-75 47-53
Add 7 all 0 0
Hill 5-6 20-30 25-55 45-55
Range 1 40-50 45-55 45-55
Multiply 0.8 land 0 0
Mask 3 0 0 0
Smooth 2 0 0 0
Trough 2-3 20-30 20-30 20-30
Trough 2-3 20-30 60-80 70-80
Hill 1 10-15 60-60 50-50
Hill 1.5 13-16 15-20 20-75
Range 1.5 30-40 15-85 30-40
Range 1.5 30-40 15-85 60-70
Pit 3-5 10-30 15-85 20-80]],
    low_island = [[Hill 1 90-99 60-80 45-55
Hill 1-2 20-30 10-30 10-90
Smooth 2 0 0 0
Hill 6-7 25-35 20-70 30-70
Range 1 40-50 45-55 45-55
Trough 2-3 20-30 15-85 20-30
Trough 2-3 20-30 15-85 70-80
Hill 1.5 10-15 5-15 20-80
Hill 1 10-15 85-95 70-80
Pit 5-7 15-25 15-85 20-80
Multiply 0.4 20-100 0 0
Mask 4 0 0 0]],
    continents = [[Hill 1 80-85 60-80 40-60
Hill 1 80-85 20-30 40-60
Hill 6-7 15-30 25-75 15-85
Multiply 0.6 land 0 0
Hill 8-10 5-10 15-85 20-80
Range 1-2 30-60 5-15 25-75
Range 1-2 30-60 80-95 25-75
Range 0-3 30-60 80-90 20-80
Strait 2 vertical 0 0
Strait 1 vertical 0 0
Smooth 3 0 0 0
Trough 3-4 15-20 15-85 20-80
Trough 3-4 5-10 45-55 45-55
Pit 3-4 10-20 15-85 20-80
Mask 4 0 0 0]],
    archipelago = [[Add 11 all 0 0
Range 2-3 40-60 20-80 20-80
Hill 5 15-20 10-90 30-70
Hill 2 10-15 10-30 20-80
Hill 2 10-15 60-90 20-80
Smooth 3 0 0 0
Trough 10 20-30 5-95 5-95
Strait 2 vertical 0 0
Strait 2 horizontal 0 0]],
    atoll = [[Hill 1 75-80 50-60 45-55
Hill 1.5 30-50 25-75 30-70
Hill .5 30-50 25-35 30-70
Smooth 1 0 0 0
Multiply 0.2 25-100 0 0
Hill 0.5 10-20 50-55 48-52]],
    mediterranean = [[Range 4-6 30-80 0-100 0-10
Range 4-6 30-80 0-100 90-100
Hill 6-8 30-50 10-90 0-5
Hill 6-8 30-50 10-90 95-100
Multiply 0.9 land 0 0
Mask -2 0 0 0
Smooth 1 0 0 0
Hill 2-3 30-70 0-5 20-80
Hill 2-3 30-70 95-100 20-80
Trough 3-6 40-50 0-100 0-10
Trough 3-6 40-50 0-100 90-100]],
    peninsula = [[Range 2-3 20-35 40-50 0-15
Add 5 all 0 0
Hill 1 90-100 10-90 0-5
Add 13 all 0 0
Hill 3-4 3-5 5-95 80-100
Hill 1-2 3-5 5-95 40-60
Trough 5-6 10-25 5-95 5-95
Smooth 3 0 0 0
Invert 0.4 both 0 0]],
    pangea = [[Hill 1-2 25-40 15-50 0-10
Hill 1-2 5-40 50-85 0-10
Hill 1-2 25-40 50-85 90-100
Hill 1-2 5-40 15-50 90-100
Hill 8-12 20-40 20-80 48-52
Smooth 2 0 0 0
Multiply 0.7 land 0 0
Trough 3-4 25-35 5-95 10-20
Trough 3-4 25-35 5-95 80-90
Range 5-6 30-40 10-90 35-65]],
    isthmus = [[Hill 5-10 15-30 0-30 0-20
Hill 5-10 15-30 10-50 20-40
Hill 5-10 15-30 30-70 40-60
Hill 5-10 15-30 50-90 60-80
Hill 5-10 15-30 70-100 80-100
Smooth 2 0 0 0
Trough 4-8 15-30 0-30 0-20
Trough 4-8 15-30 10-50 20-40
Trough 4-8 15-30 30-70 40-60
Trough 4-8 15-30 50-90 60-80
Trough 4-8 15-30 70-100 80-100
Invert 0.25 x 0 0]],
    shattered = [[Hill 8 35-40 15-85 30-70
Trough 10-20 40-50 5-95 5-95
Range 5-7 30-40 10-90 20-80
Pit 12-20 30-40 15-85 20-80]],
    taklamakan = [[Hill 1-3 20-30 30-70 30-70
Hill 2-4 60-85 0-5 0-100
Hill 2-4 60-85 95-100 0-100
Hill 3-4 60-85 20-80 0-5
Hill 3-4 60-85 20-80 95-100
Smooth 3 0 0 0]],
    old_world = [[Range 3 70 15-85 20-80
Hill 2-3 50-70 15-45 20-80
Hill 2-3 50-70 65-85 20-80
Hill 4-6 20-25 15-85 20-80
Multiply 0.5 land 0 0
Smooth 2 0 0 0
Range 3-4 20-50 15-35 20-45
Range 2-4 20-50 65-85 45-80
Strait 3-7 vertical 0 0
Trough 6-8 20-50 15-85 45-65
Pit 5-6 20-30 10-90 10-90]],
    fractious = [[Hill 12-15 50-80 5-95 5-95
Mask -1.5 0 0 0
Mask 3 0 0 0
Add -20 30-100 0 0
Range 6-8 40-50 5-95 10-90]],
}

-- Azgaar's powers by the number of cells: how fast a hill (blob) and a range (line) fall off per ring.
local BLOB_POWER = { [1000] = 0.93, [2000] = 0.95, [5000] = 0.97, [10000] = 0.98, [20000] = 0.99, [30000] = 0.991,
                     [40000] = 0.993, [50000] = 0.994, [60000] = 0.995, [70000] = 0.9955, [80000] = 0.996,
                     [90000] = 0.9964, [100000] = 0.9973 }
local LINE_POWER = { [1000] = 0.75, [2000] = 0.77, [5000] = 0.79, [10000] = 0.81, [20000] = 0.82, [30000] = 0.83,
                     [40000] = 0.84, [50000] = 0.86, [60000] = 0.87, [70000] = 0.88, [80000] = 0.91,
                     [90000] = 0.92, [100000] = 0.93 }

-- A seeded stream of numbers in [0, 1) (xorshift32 started from a mix of the seed): the template's choices.
local function stream(seed)
    local s = (math.floor(seed) * 0x9E3779B9 + 0x6D2B79F5) & 0xFFFFFFFF
    if s == 0 then s = 1 end
    return function()
        s = s ~ ((s << 13) & 0xFFFFFFFF)
        s = s ~ (s >> 17)
        s = s ~ ((s << 5) & 0xFFFFFFFF)
        return (s & 0xFFFFFF) / 16777216.0
    end
end

-- The heights of a template on a grid of about `cells` cells over a square map: his generator step by step.
local function run_template(name, text, cells, seed)
    local random = stream(seed)
    local width, height = 1000.0, 1000.0 -- map units; only ratios of them matter
    local spacing = math.sqrt(width * height / cells)
    local nx = math.floor((width + 0.5 * spacing - 1e-10) / spacing)
    local nz = math.floor((height + 0.5 * spacing - 1e-10) / spacing)
    local n = nx * nz
    local blob, line = BLOB_POWER[cells] or 0.98, LINE_POWER[cells] or 0.81

    -- Points jittered by up to 0.45 spacing on either axis; neighbours: the four of the lattice and, in each
    -- block of four, the diagonal the Delaunay triangulation of the jittered points keeps.
    local px, pz = {}, {}
    for z = 0, nz - 1 do
        for x = 0, nx - 1 do
            local i = z * nx + x + 1
            px[i] = (x + 0.5) * spacing + (random() * 0.9 - 0.45) * spacing
            pz[i] = (z + 0.5) * spacing + (random() * 0.9 - 0.45) * spacing
        end
    end
    local near, diag = {}, {}
    for i = 1, n do near[i] = {} end
    local function link(a, b)
        near[a][#near[a] + 1] = b
        near[b][#near[b] + 1] = a
    end
    for z = 0, nz - 1 do
        for x = 0, nx - 1 do
            local i = z * nx + x + 1
            if x < nx - 1 then link(i, i + 1) end
            if z < nz - 1 then link(i, i + nx) end
            if x < nx - 1 and z < nz - 1 then
                -- a b over c d: the diagonal b-c is Delaunay when d falls inside the circle through a, b, c.
                local a, b, c, d = i, i + 1, i + nx, i + nx + 1
                local ax, az = px[a] - px[d], pz[a] - pz[d]
                local bx, bz = px[b] - px[d], pz[b] - pz[d]
                local cx, cz = px[c] - px[d], pz[c] - pz[d]
                local det = (ax * ax + az * az) * (bx * cz - cx * bz) - (bx * bx + bz * bz) * (ax * cz - cx * az)
                            + (cx * cx + cz * cz) * (ax * bz - bx * az)
                -- The block's two triangles: a b c and b d c, or a b d and a d c.
                if det > 0.0 then link(b, c) else link(a, d); diag[i] = true end
            end
        end
    end

    local h = {}
    for i = 1, n do h[i] = 0 end
    local function lim(v) return math.min(math.max(v, 0.0), 100.0) end
    local function store(v) return math.floor(lim(v)) end -- his Uint8Array
    local function rand(lo, hi) return math.floor(random() * (hi - lo + 1)) + lo end
    local function chance(p) return p >= 1 or (p > 0 and random() < p) end
    -- "1.5" is 1 or 2, "5-6" a whole number between, as his getNumberInRange.
    local function number_in(r)
        local v = tonumber(r)
        if v then
            local whole = v >= 0 and math.floor(v) or -math.floor(-v)
            return whole + (chance(v - whole) and 1 or 0)
        end
        local sign = r:sub(1, 1) == "-" and -1 or 1
        if sign < 0 then r = r:sub(2) end
        local a, b = r:match("^([%d%.]+)%-([%d%.]+)$")
        if not a then return 0 end
        local count = rand(tonumber(a) * sign, tonumber(b))
        return count < 0 and 0 or count
    end
    local function point_in(r, length)
        local a, b = r:match("^([%d%.]+)%-([%d%.]+)$")
        local lo = (tonumber(a) or tonumber(r) or 0) / 100
        local hi = b and tonumber(b) / 100 or lo
        if hi == 0 then hi = lo end
        return rand(lo * length, hi * length)
    end
    local function slot(x, z)
        local row = math.max(0, math.floor(math.min(z / spacing, nz - 1)))
        local col = math.max(0, math.floor(math.min(x / spacing, nx - 1)))
        return row * nx + col + 1
    end

    local function hill(count, height_, rx, rz)
        for _ = 1, number_in(count) do
            local hh = lim(number_in(height_))
            local start, limit = nil, 0
            repeat
                start = slot(point_in(rx, width), point_in(rz, height))
                limit = limit + 1
            until not (h[start] + hh > 90 and limit < 50)
            local change = { [start] = math.floor(hh) }
            local queue, head = { start }, 1
            while head <= #queue do
                local q = queue[head]
                head = head + 1
                for _, c in ipairs(near[q]) do
                    if (change[c] or 0) == 0 then
                        change[c] = math.floor(change[q] ^ blob * (random() * 0.2 + 0.9))
                        if change[c] > 1 then queue[#queue + 1] = c end
                    end
                end
            end
            for i, v in pairs(change) do h[i] = store(h[i] + v) end
        end
    end

    local function pit(count, height_, rx, rz)
        for _ = 1, number_in(count) do
            local hh = lim(number_in(height_))
            local start, limit = nil, 0
            repeat
                start = slot(point_in(rx, width), point_in(rz, height))
                limit = limit + 1
            until not (h[start] < 20 and limit < 50)
            local used, queue, head = {}, { start }, 1
            while head <= #queue do
                local q = queue[head]
                head = head + 1
                hh = hh ^ blob * (random() * 0.2 + 0.9)
                if hh < 1 then break end
                for _, c in ipairs(near[q]) do
                    if not used[c] then
                        h[c] = store(h[c] - hh * (random() * 0.2 + 0.9))
                        used[c] = true
                        queue[#queue + 1] = c
                    end
                end
            end
        end
    end

    -- A greedy walk from `cur` to `goal`, a step now and then taken at random (`randomness`).
    local function walk(cur, goal, used, randomness)
        local out = { cur }
        used[cur] = true
        while cur ~= goal do
            local best, nxt = math.huge, nil
            for _, e in ipairs(near[cur]) do
                if not used[e] then
                    local d = (px[goal] - px[e]) ^ 2 + (pz[goal] - pz[e]) ^ 2
                    if random() > 1 - randomness then d = d / 2 end
                    if d < best then best, nxt = d, e end
                end
            end
            if not nxt then return out end
            cur = nxt
            out[#out + 1] = cur
            used[cur] = true
        end
        return out
    end

    -- A range (sign 1) or a trough (sign -1): the walk, rings of falling height round it, spurs downhill.
    local function range(sign, count, height_, rx, rz)
        for _ = 1, number_in(count) do
            local used = {}
            local hh = lim(number_in(height_))
            local sx, sz = point_in(rx, width), point_in(rz, height)
            if sign < 0 then
                local limit = 0
                while h[slot(sx, sz)] < 20 and limit < 49 do
                    sx, sz = point_in(rx, width), point_in(rz, height)
                    limit = limit + 1
                end
            end
            local ex, ez, limit = 0, 0, 0
            repeat
                ex = random() * width * 0.8 + width * 0.1
                ez = random() * height * 0.7 + height * 0.15
                local dist = math.abs(ez - sz) + math.abs(ex - sx)
                limit = limit + 1
            until not ((dist < width / 8 or dist > width / (sign > 0 and 3 or 2)) and limit < 50)
            local ridge = walk(slot(sx, sz), slot(ex, ez), used, sign > 0 and 0.15 or 0.2)
            local queue, rings = {}, 0
            for k, c in ipairs(ridge) do queue[k] = c end
            while #queue > 0 do
                local frontier = queue
                queue = {}
                rings = rings + 1
                for _, c in ipairs(frontier) do h[c] = store(h[c] + sign * hh * (random() * 0.3 + 0.85)) end
                hh = hh ^ line - 1
                if hh < 2 then break end
                for _, f in ipairs(frontier) do
                    for _, c in ipairs(near[f]) do
                        if not used[c] then
                            queue[#queue + 1] = c
                            used[c] = true
                        end
                    end
                end
            end
            for d, cur in ipairs(ridge) do
                if (d - 1) % 6 == 0 then
                    for _ = 1, rings do
                        local low, at = math.huge, nil
                        for _, c in ipairs(near[cur]) do
                            if h[c] < low then low, at = h[c], c end
                        end
                        h[at] = math.floor((h[cur] * 2 + h[at]) / 3)
                        cur = at
                    end
                end
            end
        end
    end

    local function strait(width_, direction)
        local desired = math.min(number_in(width_), nx / 3)
        if desired < 1 and chance(desired) then return end
        local vertical = direction == "vertical"
        local sx = vertical and math.floor(random() * width * 0.4 + width * 0.3) or 5
        local sz = vertical and 5 or math.floor(random() * height * 0.4 + height * 0.3)
        local ex = vertical and math.floor(width - sx - width * 0.1 + random() * width * 0.2) or width - 5
        local ez = vertical and height - 5 or math.floor(height - sz - height * 0.1 + random() * height * 0.2)
        local cur, goal = slot(sx, sz), slot(ex, ez)
        local ring = {}
        while cur ~= goal do
            local best, nxt = math.huge, cur
            for _, e in ipairs(near[cur]) do
                local d = (px[goal] - px[e]) ^ 2 + (pz[goal] - pz[e]) ^ 2
                if random() > 0.8 then d = d / 2 end
                if d < best then best, nxt = d, e end
            end
            cur = nxt
            ring[#ring + 1] = cur
        end
        local used, query = {}, {}
        local step = 0.1 / desired
        for i = 0, desired - 1 do
            local exp = 0.9 - step * (desired - i)
            for _, r in ipairs(ring) do
                for _, e in ipairs(near[r]) do
                    if not used[e] then
                        used[e] = true
                        query[#query + 1] = e
                        local v = math.floor(h[e] ^ exp)
                        h[e] = v > 100 and 5 or v
                    end
                end
            end
            ring = { table.unpack(query) }
        end
    end

    local function modify(r, add, mult)
        local lo = r == "land" and 20 or (r == "all" and 0 or tonumber(r:match("^(%d+)")))
        local hi = (r == "land" or r == "all") and 100 or tonumber(r:match("%-(%d+)$"))
        local is_land = lo == 20
        for i = 1, n do
            local v = h[i]
            if v >= lo and v <= hi then
                if add ~= 0 then v = is_land and math.max(v + add, 20) or v + add end
                if mult ~= 1 then v = is_land and (v - 20) * mult + 20 or v * mult end
                h[i] = store(v)
            end
        end
    end

    local function smooth(fr)
        local out = {}
        for i = 1, n do
            local sum, k = h[i], 1
            for _, c in ipairs(near[i]) do sum, k = sum + h[c], k + 1 end
            out[i] = fr == 1 and store(sum / k) or store((h[i] * (fr - 1) + sum / k) / fr)
        end
        h = out
    end

    local function mask(power)
        local fr = power ~= 0 and math.abs(power) or 1
        for i = 1, n do
            local x, z = 2 * px[i] / width - 1, 2 * pz[i] / height - 1
            local d = (1 - x * x) * (1 - z * z)
            if power < 0 then d = 1 - d end
            h[i] = store((h[i] * (fr - 1) + h[i] * d) / fr)
        end
    end

    local function invert(p, axes)
        if not chance(p) then return end
        local out = {}
        for i = 1, n do
            local x, z = (i - 1) % nx, (i - 1) // nx
            local ix = axes ~= "y" and nx - x - 1 or x
            local iz = axes ~= "x" and nz - z - 1 or z
            out[i] = h[iz * nx + ix + 1]
        end
        h = out
    end

    for step in text:gmatch("[^\n]+") do
        local a = {}
        for word in step:gmatch("%S+") do a[#a + 1] = word end
        local tool = a[1]
        if tool == "Hill" then hill(a[2], a[3], a[4], a[5])
        elseif tool == "Pit" then pit(a[2], a[3], a[4], a[5])
        elseif tool == "Range" then range(1, a[2], a[3], a[4], a[5])
        elseif tool == "Trough" then range(-1, a[2], a[3], a[4], a[5])
        elseif tool == "Strait" then strait(a[2], a[3])
        elseif tool == "Mask" then mask(tonumber(a[2]))
        elseif tool == "Invert" then invert(tonumber(a[2]), a[3])
        elseif tool == "Add" then modify(a[3], tonumber(a[2]), 1)
        elseif tool == "Multiply" then modify(a[3], 0, tonumber(a[2]))
        elseif tool == "Smooth" then smooth(tonumber(a[2]))
        else fail("Lands.Template '" .. name .. "' has an unknown step '" .. tostring(tool) .. "'") end
    end
    return { h = h, nx = nx, nz = nz, px = px, pz = pz, diag = diag, spacing = spacing, units = width }
end

-- The coast on a raster `o.sub` times finer than the cells.
--   1. Each pixel's height is the cells', read linearly in the triangle of their (moved) points it falls in:
--      land where a rounded copy of it is the sea level or more (his 20, or the share `o.fraction` asks for).
--   2. The coast moved by `o.noise` (a list in metres, or nil), inwards where it is positive: a noise a few
--      cells deep frays it whatever the height.
--   3. `o.fjords` fjord systems, each a basin's valleys drowned from its outlet upstream, branching with its
--      tributaries; after the fray, which would close them. `o.random` draws them.
-- Returns the signed distance (exact, blurred once, in metres) and the heights, for the ground.
local function rasterize(g, size, o)
    local sub = o.sub
    local n = g.nx * sub
    local pixel = g.units / n
    -- The cells' heights read linearly inside the triangles their points make (the triangulation whose edges
    -- are the cells' neighbours, Raster.Triangles): every triangle a plane; outside every triangle (the map's
    -- rim), the nearest point's. Each pixel keeps its triangle and its weights, for the ground in triangles.
    local tri = Raster.Triangles{ px = g.px, pz = g.pz, h = g.h, diag = g.diag, nx = g.nx, nz = g.nz,
                                  spacing = g.spacing, units = g.units, n = n }
    local corners, weights, found = tri.corners, tri.weights, tri.found
    local ta, tb, tc, wa, wb, wc = {}, {}, {}, {}, {}, {}
    local land, height_, off = {}, tri.height, {}
    for k = 1, n * n do
        local q = 3 * k - 2
        ta[k], tb[k], tc[k] = corners[q], corners[q + 1], corners[q + 2]
        wa[k], wb[k], wc[k] = weights[q], weights[q + 1], weights[q + 2]
        if not found[k] and o.outside then
            -- Off the map's triangles a world's cell has its sea floor (`o.outside`, per pixel), and no facets
            -- there: one corner's plane would flatten it.
            height_[k], off[k] = o.outside[k], true
        end
    end
    -- Three passes of a box `radius` pixels wide each way: about a Gaussian of that radius (Raster.Box).
    local function rounded(values, radius)
        if radius <= 0 then return values end
        return Raster.Box{ values = values, n = n, radius = radius }
    end
    -- The outline is drawn on a rounded copy, so the coast does not run in the triangles' straight edges; the
    -- ground keeps its triangles.
    local outline = rounded(height_, o.round or 0)
    -- The sea level: his 20, or the height that leaves `o.fraction` of the pixels above it.
    local level = 20.0
    if o.fraction then
        local sorted = {}
        for i = 1, n * n do sorted[i] = outline[i] end
        table.sort(sorted)
        level = sorted[math.min(math.max(math.floor((1.0 - o.fraction) * n * n) + 1, 1), n * n)]
    end
    for k = 1, n * n do
        land[k] = outline[k] >= level
    end
    -- The ground in triangles (`o.bands`): each corner of a triangle takes the flat height of its band
    -- (`o.bands.land` on land, `o.bands.sea` under it, functions of his height), and the triangle is the plane
    -- through them: flat where its corners share a band, a slope where they do not. `cliff` is 1 over the
    -- triangles whose first corner falls where `o.bands.cliff` (a list over the pixels) is under a half: there
    -- the caller steps the bands on the continuous height instead, walls between flat bands.
    local ground, floor, cliff
    if o.bands then
        local land_of, sea_of, pick = o.bands.land, o.bands.sea, o.bands.cliff
        ground, floor, cliff = {}, {}, {}
        for k = 1, n * n do
            local a, b, c = ta[k], tb[k], tc[k]
            local ix = math.min(math.max(math.floor(g.px[a] / pixel), 0), n - 1)
            local iz = math.min(math.max(math.floor(g.pz[a] / pixel), 0), n - 1)
            cliff[k] = pick[iz * n + ix + 1] < 0.5 and 1.0 or 0.0
            ground[k] = wa[k] * land_of(g.h[a], level) + wb[k] * land_of(g.h[b], level) + wc[k] * land_of(g.h[c], level)
            floor[k] = wa[k] * sea_of(g.h[a], level) + wb[k] * sea_of(g.h[b], level) + wc[k] * sea_of(g.h[c], level)
        end
    end
    local metres = size / n
    -- The exact Euclidean distance, in pixels, to the nearest pixel where `source(i)` holds (see
    -- Raster.Distance, the engine's: Felzenszwalb and Huttenlocher's lower envelope of parabolas).
    local function distance(source)
        local mask = {}
        for i = 1, n * n do mask[i] = source(i) and true or false end
        return Raster.Distance{ mask = mask, n = n }
    end
    -- In pixels, land negative.
    local function signed()
        local to_land = distance(function(i) return land[i] end)
        local to_sea = distance(function(i) return not land[i] end)
        local sdf = {}
        for i = 1, n * n do
            sdf[i] = land[i] and -(math.min(to_sea[i], 1e6) - 0.5) or (math.min(to_land[i], 1e6) - 0.5)
        end
        return sdf
    end
    local sdf = signed()

    if o.noise then
        for i = 1, n * n do land[i] = sdf[i] * metres + o.noise[i] < 0.0 end
        sdf = signed()
    end
    if (o.fjords or 0) > 0 then
        -- Fjords are drowned valleys, so they are carved where the land drains: a priority flood from the coast
        -- over the outline's height gives every land pixel the neighbour it drains to (the pits filled on the
        -- way, so every path reaches the sea), the area each pixel drains is summed down those paths, and from
        -- the outlets of the largest basins the valleys are cut upstream, every tributary that drains enough
        -- with them: dendritic, as the references' fjord coasts are. The basins taken cluster round two or
        -- three fjord coasts (`o.random` draws them), not spread evenly.
        local random = o.random
        local before, carved = 0, {}
        for i = 1, n * n do if land[i] then before = before + 1 end end
        -- The drainage (Raster.Drain): each land pixel's parent (0 at an outlet on the coast) and the area
        -- drained through it.
        local drained = Raster.Drain{ land = land, height = outline, n = n }
        local parent, area, children, outlets = drained.parent, drained.area, {}, {}
        for k = 1, n * n do
            local p = parent[k]
            if p == 0 then
                outlets[#outlets + 1] = k
            elseif p > 0 then
                local list = children[p]
                if not list then list = {}; children[p] = list end
                list[#list + 1] = k
            end
        end
        -- The fjord coasts, and each outlet's claim: the basin's area, the more the nearer a fjord coast.
        local centres, reach = {}, 0.2 * n
        for c = 1, math.min(#outlets, 2 + math.floor(random() * 2)) do
            centres[c] = outlets[math.floor(random() * #outlets) + 1]
        end
        local function claim(k)
            local x, z, near = (k - 1) % n, (k - 1) // n, 0.0
            for _, c in ipairs(centres) do
                local d2 = (x - (c - 1) % n) ^ 2 + (z - (c - 1) // n) ^ 2
                near = math.max(near, math.exp(-d2 / (reach * reach)))
            end
            return area[k] * near
        end
        table.sort(outlets, function(a, b)
            local ca, cb = claim(a), claim(b)
            return ca > cb or (ca == cb and a < b)
        end)
        -- A pixel of a path, where it is carved: the mean of it and the four pixels it drains through, so the
        -- path's steps in the raster's eight directions read as a bending line, not a stair of 45 degree turns.
        local function centre(k)
            local sx, sz, m, q = 0.0, 0.0, 0, k
            while q and q ~= 0 and m < 5 do
                sx, sz, m = sx + (q - 1) % n + 0.5, sz + (q - 1) // n + 0.5, m + 1
                q = parent[q]
            end
            return sx / m, sz / m
        end
        local function disc(x, z, r, each)
            for zz = math.max(math.floor(z - r), 0), math.min(math.ceil(z + r), n - 1) do
                for xx = math.max(math.floor(x - r), 0), math.min(math.ceil(x + r), n - 1) do
                    local d2 = (xx + 0.5 - x) ^ 2 + (zz + 0.5 - z) ^ 2
                    if d2 <= r * r then each(zz * n + xx + 1, math.sqrt(d2) / r) end
                end
            end
        end
        -- The valley each fjord drowns, in the ground (the rounded height the strata read): a V twice the
        -- fjord's width each side, as deep as half the land above the sea there, along the fjord and on
        -- upstream past its head for 0.8 of its length, shallowing out to nothing.
        local sink = {}
        local function valley(k, r, share)
            local x, z = centre(k)
            disc(x, z, r, function(q, d)
                if land[q] then
                    local cut_ = 0.5 * math.max(outline[q] - level, 0.0) * share * (1.0 - d)
                    sink[q] = math.max(sink[q] or 0.0, cut_)
                end
            end)
        end
        -- Up a basin from its outlet: every pixel whose drained area is a twentieth of the outlet's or more
        -- (and at least four cells), as far as `length` pixels along the valley, and its valley beyond; the
        -- width goes with the root of the area drained and narrows to the heads.
        local cut = 0
        for _, outlet in ipairs(outlets) do
            if cut >= o.fjords or claim(outlet) < 4 * sub * sub then break end
            cut = cut + 1
            local total = area[outlet]
            local least = math.max(4 * sub * sub, 0.05 * total)
            local length = sub * (10.0 + 18.0 * random())
            local width = sub * (1.2 + 1.2 * random())
            local stack = { { outlet, 0.0 } }
            while #stack > 0 do
                local top = table.remove(stack)
                local k, along = top[1], top[2]
                local t = along / length
                local r = 0.5 * width * (0.25 + 0.75 * math.sqrt(area[k] / total)) * (1.0 - 0.4 * math.min(t, 1.0)) + 0.5
                if t <= 1.0 then
                    local x, z = centre(k)
                    disc(x, z, r, function(q)
                        if land[q] then land[q], carved[q] = false, true end
                    end)
                    valley(k, 2.0 * r + sub, 1.0)
                else
                    valley(k, 2.0 * r + sub, 1.0 - (t - 1.0) / 0.8)
                end
                for _, q in ipairs(children[k] or {}) do
                    if area[q] >= least then
                        local step = (math.abs(q - k) == 1 or math.abs(q - k) == n) and 1.0 or 1.414
                        if along + step <= 1.8 * length then stack[#stack + 1] = { q, along + step } end
                    end
                end
            end
        end
        -- Into the rounded height the strata read and the height a smooth ground is.
        for q, v in pairs(sink) do outline[q], height_[q] = outline[q] - v, height_[q] - v end
        -- The fjords' walls frayed as the coast was, by a sixth of the same noise, within two pixels of them.
        if o.noise then
            local walls = signed()
            local near = distance(function(i) return carved[i] end)
            for i = 1, n * n do
                if near[i] <= 2.0 and not carved[i] then land[i] = walls[i] * metres + o.noise[i] / 6.0 < 0.0 end
            end
        end
        -- The land the fjords took is given back as a rim on the coast before them (the sea nearest it), never
        -- within three pixels of a fjord, which would close its mouth: the share of land stays what the
        -- template (or `o.fraction`) made it.
        local fjord = distance(function(i) return carved[i] end)
        local count, sea = 0, {}
        for i = 1, n * n do
            if land[i] then
                count = count + 1
            elseif not carved[i] and fjord[i] > 3.0 then
                sea[#sea + 1] = i
            end
        end
        table.sort(sea, function(a, b) return sdf[a] < sdf[b] or (sdf[a] == sdf[b] and a < b) end)
        for i = 1, math.min(before - count, #sea) do land[sea[i]] = true end
        sdf = signed()
    end
    -- Strata (`o.strata`): bands of height, flat, joined by ramps. A pixel of band j is at its level
    -- (`o.strata.land_levels[j]`) unless it is within `o.strata.width` pixels of the next band's contour, where it
    -- rises to that level by its distance to it. The top band rises from its contour to `o.strata.land_top`
    -- the same way, and the sea floor goes down likewise (`o.strata.sea_levels`, the coast at depth 0). Bands
    -- are read on the rounded height; the coast is the coast already drawn.
    local strata_land, strata_sea
    if o.strata then
        local st = o.strata
        -- The shelf (`o.strata.shelf`): as wide, in pixels, as the coast nearest each pixel of sea says (stepped
        -- back along the distance's gradient, two pixels into the land): `wide` where that land is low, `narrow`
        -- where it is high, times 0.7 to 1.3 by `noise` (a list in 0..1). It is the first band out to that
        -- width whatever the cells' depth, and also where the cells are under `shallow` of the sea level deep.
        local shelf_w = {}
        if st.shelf then
            local sh = st.shelf
            local function at(x, z) return math.min(math.max(z, 0), n - 1) * n + math.min(math.max(x, 0), n - 1) + 1 end
            for z = 0, n - 1 do
                for x = 0, n - 1 do
                    local k = z * n + x + 1
                    local d, w = sdf[k], 0.0
                    if not land[k] and d < 1.3 * sh.wide then
                        local gx, gz = sdf[at(x + 1, z)] - sdf[at(x - 1, z)], sdf[at(x, z + 1)] - sdf[at(x, z - 1)]
                        local l = math.max(math.sqrt(gx * gx + gz * gz), 1e-6)
                        local q = at(math.floor(x - gx / l * (d + 2.0) + 0.5), math.floor(z - gz / l * (d + 2.0) + 0.5))
                        local u = land[q] and (outline[q] - level) / (100.0 - level) or 0.0
                        local e = math.min(math.max((u - 0.03) / 0.22, 0.0), 1.0)
                        local low = 1.0 - e * e * (3.0 - 2.0 * e)
                        w = (sh.narrow + (sh.wide - sh.narrow) * low) * (0.7 + 0.6 * sh.noise[q])
                    end
                    shelf_w[k] = w
                end
            end
            -- Smoothed a cell: each pixel's nearest coast jumps across the lines where two coasts are as near.
            shelf_w = rounded(shelf_w, sub)
        end
        local band = {}
        for i = 1, n * n do
            if land[i] then
                local u, b = (outline[i] - level) / (100.0 - level), 1
                for k = 2, #st.land_edges do if u >= st.land_edges[k] then b = k end end
                band[i] = b
            else
                local s, b = (level - outline[i]) / math.max(level, 1.0), 1
                for k = 2, #st.sea_edges do if s >= st.sea_edges[k] then b = k end end
                if st.shelf then
                    b = (sdf[i] < shelf_w[i] or s < st.shelf.shallow) and 1 or math.max(b, 2)
                end
                band[i] = -b
            end
        end
        -- Under the sea each band is opened and closed by a cell (its fingers and its gaps under two cells
        -- across removed): the cells' planes cross a depth in long thin slivers, and each sliver's ramps on
        -- both sides were a straight groove or ridge across the floor.
        local deeper = nil
        for k = 2, #st.sea_edges do
            local mask = {}
            for i = 1, n * n do mask[i] = band[i] <= -k end
            -- Opened, then closed, by a square `sub` pixels each way (Raster.Square).
            mask = Raster.Square{ mask = mask, n = n, radius = sub, steps = { true, false, false, true } }
            for i = 1, n * n do
                mask[i] = not land[i] and mask[i] and (deeper == nil or deeper[i])
            end
            for i = 1, n * n do
                if not land[i] then band[i] = mask[i] and math.min(band[i], -k) or math.max(band[i], -(k - 1)) end
            end
            deeper = mask
        end
        local function interpolate(sign, levels, top, count)
            local out = {}
            local below, above = {}, {}
            for k = 1, count do
                -- The coast (the first band's lower contour) and the top band's: where a band rises from.
                below[k] = (k == 1 or k == count) and distance(function(i) return sign * band[i] < k end) or nil
                above[k] = k > 1 and distance(function(i) return sign * band[i] >= k end) or nil
            end
            for i = 1, n * n do
                local j = sign * band[i]
                if j >= 1 then
                    -- Flat at the band's level, but within `width` pixels (`st.width`, a list: short and steep
                    -- here, long and gentle there) of the next band's contour it rises to that level by its
                    -- distance to it; the first band rises from the coast the same way, the top one from its
                    -- contour to `top`. Where a band is narrower than its ramp, the step is a cliff.
                    local w = math.max(st.width[i], 1.0)
                    local h = levels[j]
                    -- The shelf falls the whole of its width, gently, to its level at its edge.
                    if j == 1 then
                        local w1 = (sign < 0 and st.shelf) and math.max(shelf_w[i], w) or w
                        h = levels[1] * math.min(below[1][i] / w1, 1.0)
                    end
                    if j < count then
                        h = h + (levels[j + 1] - levels[j]) * math.max(1.0 - above[j + 1][i] / w, 0.0)
                    else
                        h = h + (top - levels[j]) * math.min(below[j][i] / w, 1.0)
                    end
                    out[i] = h
                end
            end
            return out
        end
        strata_land = interpolate(1, st.land_levels, st.land_top, #st.land_edges)
        strata_sea = interpolate(-1, st.sea_levels, st.sea_bottom, #st.sea_edges)
        for i = 1, n * n do
            strata_land[i] = strata_land[i] or 0.0
            strata_sea[i] = strata_sea[i] or 0.0
        end
        -- Facets: the strata's height taken at the corners of the triangles and each triangle filled as a
        -- plane, so a long ramp is a few planes with edges between them and not a smooth gradient. Where
        -- `st.cliff` (a list over the pixels, at the triangle's first corner) is under a half, the triangle keeps
        -- the strata as they are, flat bands and the walls between them.
        local corner_land, corner_sea = {}, {}
        for v = 1, #g.px do
            local ix = math.min(math.max(math.floor(g.px[v] / pixel), 0), n - 1)
            local iz = math.min(math.max(math.floor(g.pz[v] / pixel), 0), n - 1)
            -- Under the sea the corner is the median of its 3 x 3 pixels: a corner on one deep pixel past a band's
            -- edge would sink its triangles into a pit; a median keeps the edges, as no mean would.
            local around = {}
            for dz = -1, 1 do
                for dx = -1, 1 do
                    local q = math.min(math.max(iz + dz, 0), n - 1) * n + math.min(math.max(ix + dx, 0), n - 1) + 1
                    around[#around + 1] = strata_sea[q]
                end
            end
            table.sort(around)
            corner_land[v], corner_sea[v] = strata_land[iz * n + ix + 1], around[5]
        end
        for k = 1, n * n do
            local a, b, c = ta[k], tb[k], tc[k]
            local ix = math.min(math.max(math.floor(g.px[a] / pixel), 0), n - 1)
            local iz = math.min(math.max(math.floor(g.pz[a] / pixel), 0), n - 1)
            if not off[k] and (not st.cliff or st.cliff[iz * n + ix + 1] >= 0.5) then
                strata_land[k] = wa[k] * corner_land[a] + wb[k] * corner_land[b] + wc[k] * corner_land[c]
                strata_sea[k] = wa[k] * corner_sea[a] + wb[k] * corner_sea[b] + wc[k] * corner_sea[c]
            end
        end
    end
    for i = 1, n * n do sdf[i] = sdf[i] * metres end
    -- Connected land (four neighbours), ranked by size from 1, and connected water with whether it touches
    -- the map's edge (Raster.Label): the ids of `region_id` and what the report lists. The sea takes the id
    -- of the nearest land, grown outwards a ring at a time, so ids read across the coast.
    local labelled = Raster.Label{ land = land, n = n }
    local region, masses, waters = labelled.region, labelled.masses, labelled.waters
    for rank, body in ipairs(masses) do body.rank = rank end
    return { n = n, cell = metres, sdf = Raster.Soften{ values = sdf, n = n }, height = height_, ground = ground,
             floor = floor, cliff = cliff,
             strata_land = strata_land, strata_sea = strata_sea, region = region,
             masses = masses, waters = waters, level = level }
end

-- The terrain samples of a raster `n` pixels of `px` metres a side from its corner (`x0`, `z0`).
local function raster_grid(x0, z0, n, px)
    return function(value)
        return Terrain.Sample{ value = value, origin = { x0 + 0.5 * px, z0 + 0.5 * px }, size = { n, n }, cell = px }
    end
end

-- The beaches' width at the coast, in metres, as fields of the world at `cell_m` (a template's cell): a field
-- fifteen cells across leaves whole faces of the land bare; along the rest the sand is never under an eighth
-- of a cell, so the beaches join, and where one ten cells across says (long runs) it widens to between a fifth
-- of a cell and one, as one six cells across says, and threefold where one sixteen cells across does.
local function beach_widths(t, seed, cell_m)
    local function field(cells, salt)
        return Noises.fBm{ frequency = 1.0 / (cells * cell_m), octaves = 2, amplitude = 0.5, seed = seed + salt }.value + 0.5
    end
    local share = t.beaches or 0.5
    local face = Curves.Smoothstep{ input = field(15.0, 487), min = 0.4, max = 0.55 }
    local runs = Curves.Smoothstep{ input = field(10.0, 481), min = 1.0 - share - 0.15, max = 1.0 - share + 0.15 }
    local wide = Curves.Clamp{ input = field(6.0, 483), min = 0.0, max = 1.0 }
    local broad = Curves.Smoothstep{ input = field(16.0, 489), min = 0.45, max = 0.75 }
    return face * (0.12 + runs * ((0.2 + 0.8 * wide) * (1.0 + 2.0 * broad) - 0.12)) * cell_m
end

-- --- The ocean floor -------------------------------------------------------------------------------

-- How far below a map's floor the drowned land may be, by the distance offshore: past a shelf 0.045 of a
-- cell wide, a wall of a share of the abyssal depth at each distance (in cells), unevenly apart and each its
-- own height, 3.6 of the abyss in all by 0.11 of a cell. Each wall wavers on its own (`OCEAN_WAVER`). A cell
-- is a world's cell, or a bounded map's whole side.
local OCEAN_STAIRS = { { 0.045, 0.3 }, { 0.051, 0.35 }, { 0.056, 0.25 }, { 0.063, 0.35 }, { 0.068, 0.4 }, { 0.074, 0.25 },
                       { 0.081, 0.4 }, { 0.086, 0.3 }, { 0.093, 0.4 }, { 0.099, 0.25 }, { 0.107, 0.35 } }
local OCEAN_WAVER = 0.006
for k, step in ipairs(OCEAN_STAIRS) do step[3], step[4] = math.cos(2.4 * k), math.sin(2.4 * k) end
-- The drowned land's depth, as a share of the abyssal depth, by its stratum (0 to 1), in straight pieces: the
-- trenches' floors, the far floor, the floor by the lands, the hills, the guyots' tops.
local OCEAN_DEPTHS = { { 0.0, 5.5 }, { 0.2, 3.8 }, { 0.85, 1.0 }, { 0.92, 0.7 }, { 1.0, 0.1 } }

local function ocean_capsule(list, from, to, w, height, kind)
    list[#list + 1] = { shape = "capsule", from = from, to = to, width = w, smooth = 0.0,
                        profile = { kind = kind or "dome", height = height, width = w } }
end

-- The open ocean floor at the lands' own detail: a drowned land, rastered like a template's (his jittered
-- lattice, `o.lattice` cells a cell, his triangles, the land's strata of bands, ramps, walls and facets) and
-- laid below the sea. So that cells have no seam, everything it is made of is the world's, not a map's: the
-- lattice's points are jittered by a hash of their world index, and its heights are hills, ranges and pits
-- (his steps' shapes) and the ocean's forms each cell places by its hash. A cell is rastered with eight
-- lattice cells more each way, past the reach of a ramp, and cropped, so the next cell, rastered over the
-- same points, gives the same pixels where they meet.
--   o.seed, o.cell (metres), o.abyss (the abyssal depth, negative), o.lattice, o.sub (pixels a lattice cell),
--   o.descent (0 to 1, how far the floor has fallen below the abyss, an expression), o.origin (the centre of
--   cell 0, 0; the world's origin by default), o.sources (adds trenches and canyons the caller's lands place:
--   `o.sources(i, j, deeps, canyons)`), o.key (kept in the engine under it when given), o.affordable (asked
--   before a cell is made; nil out of budget).
-- Returns `drowned(i, j)`: the cell's depths, then its canyons, then two fields the stairs waver by, each a
-- list of (lattice * sub)^2, or nil.
local function ocean_floor(o)
    local seed, cell, abyss, lattice, sub, descent = o.seed, o.cell, o.abyss, o.lattice, o.sub, o.descent
    local ox, oz = o.origin and o.origin[1] or 0.0, o.origin and o.origin[2] or 0.0
    local nw = lattice * sub
    local px = cell / nw
    local step_m = cell / lattice
    local SKIRT = 8
    local function features(i, j)
        local out_up, out_down = {}, {}
        for a = i - 1, i + 1 do
            for b = j - 1, j + 1 do
                local random = stream(((math.floor(seed) * 40503 ~ a * 2246822519 ~ b * 3266489917) + 61) & 0x7FFFFFFF)
                local function at() return { ox + (a - 0.5 + random()) * cell, oz + (b - 0.5 + random()) * cell } end
                for _ = 1, 20 do
                    local r = (0.02 + 0.055 * random()) * cell
                    out_up[#out_up + 1] = { shape = "circle", radius = r, at = at(), smooth = 0.0,
                                            profile = { kind = "dome", height = 0.2 + 0.6 * random(), width = r } }
                end
                for _ = 1, 8 do
                    local from, angle, length = at(), random() * math.pi, (0.08 + 0.22 * random()) * cell
                    local w = (0.02 + 0.03 * random()) * cell
                    out_up[#out_up + 1] = { shape = "capsule", from = from, smooth = 0.0, width = w,
                                            to = { from[1] + math.cos(angle) * length, from[2] + math.sin(angle) * length },
                                            profile = { kind = "dome", height = 0.3 + 0.6 * random(), width = w } }
                end
                for _ = 1, 10 do
                    local r = (0.02 + 0.045 * random()) * cell
                    out_down[#out_down + 1] = { shape = "circle", radius = r, at = at(), smooth = 0.0,
                                                profile = { kind = "dome", height = 0.3 + 0.7 * random(), width = r } }
                end
                -- Trenches: narrow cuts a few bands deep, the canyons of the deep floor.
                for _ = 1, 6 do
                    local from, angle, length = at(), random() * math.pi, (0.1 + 0.25 * random()) * cell
                    local w = (0.01 + 0.015 * random()) * cell
                    out_down[#out_down + 1] = { shape = "capsule", from = from, smooth = 0.0, width = w,
                                                to = { from[1] + math.cos(angle) * length, from[2] + math.sin(angle) * length },
                                                profile = { kind = "dome", height = 0.5 + 0.5 * random(), width = w } }
                end
            end
        end
        return out_up, out_down
    end
    -- The ocean's larger forms round cell (i, j), each placed by a hash of the world:
    --  mounts   guyots and seamounts, cones whose top the clamp at his 100 cuts flat; one cell in four has a
    --           hotspot chain of them, each older (lower, smaller) than the one before.
    --  ridges   mid-ocean ridges, one in most ridge cells of five cells, in segments a fault zone offsets
    --           sideways; `rifts` are the valley down each segment's crest and the fault troughs between.
    --  deeps    abyssal trenches: one cell in ten an arc of its own, and those of the caller's lands.
    --  canyons  valleys from the caller's coasts out across the slope to the deep floor.
    local RIDGE = 5.0 * cell
    local function ocean_forms(i, j)
        local mounts, ridges, rifts, deeps, canyons = {}, {}, {}, {}, {}
        local function hashed(a, b, salt)
            return stream(((math.floor(seed) * 2246822519 ~ a * 73856093 ~ b * 19349663 ~ salt) + 97) & 0x7FFFFFFF)
        end
        for a = i - 2, i + 2 do
            for b = j - 2, j + 2 do
                local random = hashed(a, b, 0x51ed27)
                local function at() return { ox + (a - 0.5 + random()) * cell, oz + (b - 0.5 + random()) * cell } end
                if math.abs(a - i) <= 1 and math.abs(b - j) <= 1 then
                    for _ = 1, math.floor(random() * 3) do
                        local r = (0.03 + 0.04 * random()) * cell
                        mounts[#mounts + 1] = { shape = "circle", radius = r, at = at(), smooth = 0.0,
                                                profile = { kind = "cone", height = 0.5 + 0.6 * random(), width = r } }
                    end
                    if random() < 0.25 then
                        local p, angle, n = at(), random() * 2.0 * math.pi, 4 + math.floor(random() * 4)
                        for k = 0, n - 1 do
                            local age = k / n
                            local r = (0.055 - 0.03 * age) * cell
                            mounts[#mounts + 1] = { shape = "circle", radius = r, at = { p[1], p[2] }, smooth = 0.0,
                                                    profile = { kind = "cone", height = 1.15 - 0.6 * age, width = r } }
                            local hop = (0.06 + 0.04 * random()) * cell
                            p = { p[1] + math.cos(angle) * hop, p[2] + math.sin(angle) * hop }
                            angle = angle + (random() - 0.5) * 0.4
                        end
                    end
                end
                if random() < 0.1 then
                    -- An arc of a circle 0.8 to 1.4 cells across, its middle in this cell.
                    local m, dir, R = at(), random() * 2.0 * math.pi, (0.8 + 0.6 * random()) * cell
                    local span, w = 0.9 + 0.6 * random(), (0.015 + 0.015 * random()) * cell
                    local c = { m[1] - math.cos(dir) * R, m[2] - math.sin(dir) * R }
                    local last
                    for k = 0, 6 do
                        local t = dir + (k / 6 - 0.5) * span
                        local p = { c[1] + math.cos(t) * R, c[2] + math.sin(t) * R }
                        if last then ocean_capsule(deeps, last, p, w, 0.7 + 0.3 * random()) end
                        last = p
                    end
                end
            end
        end
        -- The ridges of the ridge cells within one of this cell.
        local x, z = i * cell, j * cell
        for P = math.floor((x - RIDGE) / RIDGE), math.floor((x + RIDGE) / RIDGE) do
            for Q = math.floor((z - RIDGE) / RIDGE), math.floor((z + RIDGE) / RIDGE) do
                local random = hashed(P, Q, 0x2c1b3c6d)
                if random() < 0.6 then
                    local angle = random() * math.pi
                    local ux, uz = math.cos(angle), math.sin(angle)
                    local s = -0.5 * RIDGE
                    local cx, cz = ox + (P + random()) * RIDGE, oz + (Q + random()) * RIDGE
                    local w = (0.06 + 0.03 * random()) * cell
                    local prev
                    while s < 0.5 * RIDGE do
                        local len = (0.4 + 0.4 * random()) * cell
                        local shift = (random() - 0.5) * 0.3 * cell
                        local ax, az = cx + ux * s - uz * shift, cz + uz * s + ux * shift
                        local bx, bz = ax + ux * len, az + uz * len
                        ocean_capsule(ridges, { ax, az }, { bx, bz }, w, 0.8 + 0.2 * random())
                        ocean_capsule(rifts, { ax, az }, { bx, bz }, 0.02 * cell, 1.0)
                        if prev then
                            -- The fault: a trough from the last segment's end past this one's start, both ways.
                            local dx, dz = ax - prev[1], az - prev[2]
                            local l = math.max(math.sqrt(dx * dx + dz * dz), 1.0)
                            local ex, ez = dx / l * 0.12 * cell, dz / l * 0.12 * cell
                            ocean_capsule(rifts, { prev[1] - ex, prev[2] - ez }, { ax + ex, az + ez }, 0.015 * cell, 0.8)
                        end
                        prev = { bx, bz }
                        s = s + len
                    end
                end
            end
        end
        if o.sources then o.sources(i, j, deeps, canyons) end
        return mounts, ridges, rifts, deeps, canyons
    end
    return function(i, j)
        local key = o.key and (o.key .. ";" .. i .. ";" .. j)
        local kept = key and Raster.Recall{ key = key }
        if kept then return kept end
        if o.affordable and not o.affordable() then return nil end
        -- The lattice over the cell and its skirt, in lattice cells from its corner.
        local count = lattice + 2 * SKIRT
        local gi0, gj0 = (i - 1) * lattice + lattice // 2 - SKIRT, (j - 1) * lattice + lattice // 2 - SKIRT
        local x0 = ox + (i - 0.5) * cell - SKIRT * step_m
        local z0 = oz + (j - 0.5) * cell - SKIRT * step_m
        local g = { nx = count, nz = count, spacing = 1.0, units = count, px = {}, pz = {}, h = {}, diag = {} }
        local xs, zs = {}, {}
        for z = 0, count - 1 do
            for x = 0, count - 1 do
                local k = z * count + x + 1
                local jitter = stream(((math.floor(seed) * 2654435761 ~ (gi0 + x) * 73856093 ~ (gj0 + z) * 19349663) + 7) & 0x7FFFFFFF)
                g.px[k] = x + 0.5 + (jitter() * 0.9 - 0.45)
                g.pz[k] = z + 0.5 + (jitter() * 0.9 - 0.45)
                xs[k], zs[k] = x0 + g.px[k] * step_m, z0 + g.pz[k] * step_m
            end
        end
        for z = 0, count - 2 do
            for x = 0, count - 2 do
                local q = z * count + x + 1
                local a, b, c, d = q, q + 1, q + count, q + count + 1
                local ax, az = g.px[a] - g.px[d], g.pz[a] - g.pz[d]
                local bx, bz = g.px[b] - g.px[d], g.pz[b] - g.pz[d]
                local cx, cz = g.px[c] - g.px[d], g.pz[c] - g.pz[d]
                local det = (ax * ax + az * az) * (bx * cz - cx * bz) - (bx * bx + bz * bz) * (ax * cz - cx * az)
                            + (cx * cx + cz * cz) * (ax * bz - bx * az)
                g.diag[q] = det <= 0.0 or nil
            end
        end
        -- His heights, 21 to 100 so the whole lattice is land to the raster: 88 for the floor by the lands,
        -- falling to 36 with `descent`; hills and ranges up to 12 more, pits and small trenches down to 22
        -- less; the ocean's forms (`ocean_forms`) up from there to the flat tops at 100 and down to the
        -- trenches' floors at 21.
        local up, down = features(i, j)
        local mounts, ridges, rifts, deeps, canyons = ocean_forms(i, j)
        local function relief(list, reach)
            local _, v = Shapes.Set{ elements = list, reach = reach, relief = true, blend = 0.0 }
            return v
        end
        local canyon = relief(canyons, 0.03 * cell)
        local heights = Terrain.Sample{
            value = 88.0 - 52.0 * descent + 12.0 * relief(up, 0.08 * cell) - 22.0 * relief(down, 0.08 * cell)
                    + 80.0 * relief(mounts, 0.08 * cell) + 30.0 * relief(ridges, 0.1 * cell) - 14.0 * relief(rifts, 0.03 * cell)
                    - 70.0 * relief(deeps, 0.04 * cell) - 20.0 * canyon,
            xs = xs, zs = zs }
        for k = 1, count * count do g.h[k] = math.min(math.max(heights[k], 21.0), 100.0) end
        local n = count * sub
        local grid_of = raster_grid(x0, z0, n, step_m / sub)
        -- Twenty-two bands, unevenly apart, so the terraces going down are each at a depth of its own; the
        -- first five for the trenches, the last two for the tops of the guyots.
        local edges = { 0.0, 0.03, 0.06, 0.1, 0.14, 0.17, 0.2, 0.24, 0.29, 0.35, 0.41, 0.46, 0.52, 0.58, 0.63, 0.69,
                        0.75, 0.8, 0.86, 0.92, 0.95, 0.98 }
        local levels = {}
        for k, e in ipairs(edges) do levels[k] = e end
        local r = rasterize(g, count * step_m, {
            sub = sub, round = 1, fjords = 0, random = stream(1),
            strata = {
                land_edges = edges, land_levels = levels, land_top = 1.0,
                sea_edges = { 0.0 }, sea_levels = { 0.0 }, sea_bottom = 0.0,
                -- Walls where the field is under a half. Regions a third of a cell across choose how many: most
                -- steps walls in some, cliffs between terraces; most ramps in others, a rolling floor of slopes.
                cliff = grid_of(Noises.fBm{ frequency = 1.0 / (6.0 * step_m), octaves = 2, amplitude = 0.5, seed = seed + 451 }.value + 0.2
                                + 0.6 * Curves.Smoothstep{ input = Noises.fBm{ frequency = 1.0 / (0.33 * cell), octaves = 2,
                                                                             amplitude = 0.5, seed = seed + 521 }.value + 0.5,
                                                        min = 0.4, max = 0.6 }),
                width = grid_of((0.5 + 5.5 * Curves.Smoothstep{ input = Noises.fBm{ frequency = 1.0 / (10.0 * step_m), octaves = 2,
                                                                                  amplitude = 0.5, seed = seed + 471 }.value + 0.5,
                                                             min = 0.2, max = 0.8 }) * sub),
            },
        })
        -- The cell's own pixels, as depths (`OCEAN_DEPTHS`): the floor by the lands a little below the abyss,
        -- far from any land nearly four times it, the trenches' floors 5.5 times, the guyots' tops a tenth.
        -- Then the canyons at each pixel, which the stairs cut through the slopes with.
        local out, offset = {}, SKIRT * sub
        for z = 0, nw - 1 do
            for x = 0, nw - 1 do
                local s, f = r.strata_land[(z + offset) * n + x + offset + 1], OCEAN_DEPTHS[#OCEAN_DEPTHS][2]
                for q = 2, #OCEAN_DEPTHS do
                    local lo, hi = OCEAN_DEPTHS[q - 1], OCEAN_DEPTHS[q]
                    if s <= hi[1] then
                        f = lo[2] + (hi[2] - lo[2]) * (s - lo[1]) / (hi[1] - lo[1])
                        break
                    end
                end
                out[z * nw + x + 1] = abyss * f
            end
        end
        local cxs, czs = {}, {}
        for z = 0, nw - 1 do
            for x = 0, nw - 1 do
                cxs[z * nw + x + 1] = ox + (i - 0.5) * cell + (x + 0.5) * px
                czs[z * nw + x + 1] = oz + (j - 0.5) * cell + (z + 0.5) * px
            end
        end
        local cut = Terrain.Sample{ value = canyon, xs = cxs, zs = czs }
        -- And two fields a twenty-fifth of a cell across, which each wall of the stairs mixes in its own way to
        -- waver by.
        local function waver(salt)
            return Terrain.Sample{ value = Noises.fBm{ frequency = 1.0 / (0.04 * cell), octaves = 2, amplitude = 1.0,
                                                       seed = seed + salt }.value, xs = cxs, zs = czs }
        end
        local wa, wb = waver(541), waver(547)
        for q = 1, nw * nw do
            out[nw * nw + q], out[2 * nw * nw + q], out[3 * nw * nw + q] = cut[q], wa[q], wb[q]
        end
        if key then Raster.Keep{ key = key, numbers = out } end
        return out
    end
end

-- Lays the drowned land `under` (from `ocean_floor`) below a map's sea floor `base` (a list of its pixels,
-- changed in place) wherever it is shallower, and deeper as far as the map's floor lowered by the stairs
-- allows: the lands keep their shelves and slopes, and past them the floor goes down to the drowned land's
-- depths in walls and terraces. A canyon (the drowned land's second list) takes the stairs as if up to 0.05
-- of a cell further out, from half a hundredth offshore, so it cuts down through them. `sdf` is the map's
-- distance offshore in metres; `nw` its side in pixels.
local function ocean_compose(base, sdf, under, nw, cell, abyss)
    if not under then return end
    for q = 1, nw * nw do
        local d = sdf[q]
        if d > 0.0 then
            local drop, reach = 0.0, d + under[nw * nw + q] * 0.05 * cell
                                         * math.min(math.max((d - 0.005 * cell) / (0.01 * cell), 0.0), 1.0)
            local wa, wb = under[2 * nw * nw + q], under[3 * nw * nw + q]
            for _, step in ipairs(OCEAN_STAIRS) do
                if reach > (step[1] + OCEAN_WAVER * (step[3] * wa + step[4] * wb)) * cell then drop = drop + step[2] end
            end
            base[q] = math.max(under[q], base[q] + drop * abyss)
        end
    end
end

-- One template's map, `size` metres centred on (`cx`, `cz`), on its raster: the cells' heights from `tseed`,
-- every noise from `seed` at its world position (so neighbouring maps share them). `edge` sinks the heights
-- to the deepest sea over that share of the map at its rim, so the map meets the next one in open ocean.
-- `place` ({ scale, x, z }, fractions of the map) draws the template smaller, at that corner of it, the
-- rest open sea; `place.empty` leaves the whole map sea.
-- Returns the raster (with its corner `x0`, `z0`) and what the surface needs of the map.
local function template_raster(t, text, seed, tseed, size, cx, cz, edge, place)
    local relief = t.height or 1200.0
    local cells = t.cells or 10000
    local roughness = t.roughness or 1.0
    local g = run_template(t.template, text, cells, tseed)
    -- How much of each point the template keeps: all of it inside, nothing over the outer two fifths of the
    -- rim (a rounded square, a superellipse of power 3, or the lands come out square), so the shelf and the
    -- slope end before it; there the map has `place.floor`, a world's sea floor in his heights, or the abyss.
    local keep = {}
    for i = 1, #g.px do
        keep[i] = 1.0
        if edge then
            local dx, dz = math.abs(g.px[i] / g.units - 0.5) * 2.0, math.abs(g.pz[i] / g.units - 0.5) * 2.0
            local m = 0.5 * (1.0 - (dx * dx * dx + dz * dz * dz) ^ (1.0 / 3.0)) / edge
            m = math.min(math.max((m - 0.4) / 0.6, 0.0), 1.0)
            keep[i] = (place and place.empty) and 0.0 or m * m * (3.0 - 2.0 * m)
        end
    end
    local scale = place and place.scale or 1.0
    if place then
        -- The template's points drawn into its square of the map; off them (no triangle) the raster takes the
        -- sea floor.
        for i = 1, #g.px do
            g.px[i] = g.units * (place.x + scale * g.px[i] / g.units)
            g.pz[i] = g.units * (place.z + scale * g.pz[i] / g.units)
        end
    end
    local sub = t.detail or 4
    local cell_m = scale * size / g.nx
    local n = g.nx * sub
    local px_m = size / n
    local grid_of = raster_grid(cx - 0.5 * size, cz - 0.5 * size, n, px_m)
    -- A broad bend of the whole outline: the cells' points are moved by a warp half the map across, `bend` of
    -- the map at most (0.08 by default; more stretches the triangles into long straight edges), so a
    -- template's masked rectangle comes out lobed, and every triangle of the points stays a triangle.
    local bend = (t.bend or 0.08) * scale * size
    local xs, zs = {}, {}
    for i = 1, #g.px do
        xs[i], zs[i] = cx + (g.px[i] / g.units - 0.5) * size, cz + (g.pz[i] / g.units - 0.5) * size
    end
    local floor_at = (place and place.floor) and Terrain.Sample{ value = place.floor, xs = xs, zs = zs } or nil
    for i = 1, #g.px do
        local under = floor_at and floor_at[i] or 0.0
        g.h[i] = under + (g.h[i] - under) * keep[i]
    end
    local bend_x = Terrain.Sample{ value = Noises.fBm{ frequency = 2.0 / (scale * size), octaves = 2, persistence = 0.4, amplitude = bend,
                                                        seed = seed + 441 }.value, xs = xs, zs = zs }
    local bend_z = Terrain.Sample{ value = Noises.fBm{ frequency = 2.0 / (scale * size), octaves = 2, persistence = 0.4, amplitude = bend,
                                                        seed = seed + 443 }.value, xs = xs, zs = zs }
    for i = 1, #g.px do
        -- Faded out three cells from the edge of the template's square, so the points there keep covering it
        -- with triangles.
        local x0, z0 = (place and place.x or 0.0) * g.units, (place and place.z or 0.0) * g.units
        local side = scale * g.units
        local margin = math.min(g.px[i] - x0, x0 + side - g.px[i], g.pz[i] - z0, z0 + side - g.pz[i])
                       / (3.0 * g.spacing * scale)
        local fade = math.min(math.max(margin, 0.0), 1.0)
        fade = fade * fade * (3.0 - 2.0 * fade)
        g.px[i] = g.px[i] + fade * bend_x[i] * g.units / size
        g.pz[i] = g.pz[i] + fade * bend_z[i] * g.units / size
    end
    -- The roughness field (his coastline's: calm stretches and jagged ones, 6 % of the map wide, threshold 0.25,
    -- squared) and the coast's fray in metres: a fractal from eight cells down (persistence 0.7), `fray` cells at
    -- its largest (2.5 by default, 0.5 on an atoll, whose ring is narrower), times 0.35 to 1 by the field.
    local field = Noises.fBm{ frequency = 1.0 / (size * 0.06), octaves = 2, amplitude = 0.5, seed = seed + 401 }.value + 0.5
    local rough = Curves.Clamp{ input = (field - 0.25) / 0.75, min = 0.0, max = 1.0 }
    rough = rough * rough
    local fray = Noises.fBm{ frequency = 1.0 / (8.0 * cell_m), octaves = 7, persistence = 0.7,
                             amplitude = (t.fray or (t.template == "atoll" and 0.5 or 2.5)) * cell_m * roughness, seed = seed + 431 }.value * (0.35 + 0.65 * rough)
    if t.land_fraction ~= nil and (type(t.land_fraction) ~= "number" or t.land_fraction <= 0 or t.land_fraction >= 1) then
        fail("Lands.Template expects land_fraction between 0 and 1, got " .. tostring(t.land_fraction))
    end
    local margin = MARGINS[t.margin or "compressed"]
    local shelf_depth = t.shelf_depth or margin.shelf_depth
    local abyssal_depth = t.abyssal_depth or margin.abyssal_depth
    -- The ground in terraces and triangles (`ground = "terraces"`): bands of height, flat each, the forms before
    -- any detail. On land at the fractions of `height` in `terraces` (the lower edge of each band, his heights
    -- above the sea level scaled to 0..1); under the sea, by how far below the sea level the cells are, the
    -- shelf, two steps of the slope and the abyssal plain. A triangle whose corners fall in different bands
    -- is a slope, unless a field six cells across makes its steps walls: `slopes` (half by default) of the
    -- land is in triangles, the rest in cliffs.
    local bands, strata
    if (t.ground or "smooth") == "terraces" then
        local lands = t.terraces or { 0.0, 0.1, 0.25, 0.45, 0.7 }
        local seas = { { 0.0, shelf_depth }, { 0.3, 0.35 * abyssal_depth }, { 0.6, 0.7 * abyssal_depth },
                       { 0.85, abyssal_depth } }
        local pick = Noises.fBm{ frequency = 1.0 / (6.0 * cell_m), octaves = 2, amplitude = 0.5, seed = seed + 451 }.value + 0.5
        bands = {
            lands = lands, seas = seas, cliff = grid_of(pick + (t.slopes or 0.5) - 0.5),
            land = function(v, sea)
                local u, band = (v - sea) / (100.0 - sea), lands[1]
                for _, e in ipairs(lands) do
                    if u >= e then band = e end
                end
                return relief * math.max(band, 0.02)
            end,
            sea = function(v, sea)
                local s, depth = (sea - v) / math.max(sea, 1.0), seas[1][2]
                for _, e in ipairs(seas) do
                    if s >= e[1] then depth = e[2] end
                end
                return -depth
            end,
        }
    elseif t.ground == "strata" then
        -- Strata: the same bands, flat, each rising to the next in a ramp before its contour, on land and under
        -- the sea; how long the ramp is a field ten cells across chooses, from half a cell (steep, nearly a
        -- step) to six (long and gentle). The top band rises to `height`, the deepest falls to the abyssal depth.
        local lands = t.terraces or { 0.0, 0.1, 0.25, 0.45, 0.7 }
        local land_levels = {}
        for k, e in ipairs(lands) do land_levels[k] = relief * math.max(e, 0.03) end
        strata = {
            land_edges = lands, land_levels = land_levels, land_top = relief,
            -- A world's cell (`place`) has open ocean to show, in seven bands, not four.
            sea_edges = place and { 0.0, 0.3, 0.45, 0.6, 0.72, 0.85, 0.93 } or { 0.0, 0.3, 0.6, 0.85 },
            sea_levels = place and { -shelf_depth, -0.3 * abyssal_depth, -0.45 * abyssal_depth, -0.6 * abyssal_depth,
                                     -0.75 * abyssal_depth, -0.88 * abyssal_depth, -abyssal_depth }
                         or { -shelf_depth, -0.35 * abyssal_depth, -0.7 * abyssal_depth, -abyssal_depth },
            sea_bottom = -abyssal_depth,
            cliff = grid_of(Noises.fBm{ frequency = 1.0 / (6.0 * cell_m), octaves = 2, amplitude = 0.5, seed = seed + 451 }.value
                            + (t.slopes or 0.5)),
            width = grid_of((0.5 + 5.5 * Curves.Smoothstep{ input = Noises.fBm{ frequency = 1.0 / (10.0 * cell_m), octaves = 2,
                                                                            amplitude = 0.5, seed = seed + 471 }.value + 0.5,
                                                       min = 0.2, max = 0.8 }) * sub),
            -- The shelf: half a cell off a high coast, six off a low one, and the cells' shallowest seas.
            shelf = { narrow = 0.5 * sub, wide = 6.0 * sub, shallow = 0.12,
                      noise = grid_of(Noises.fBm{ frequency = 1.0 / (8.0 * cell_m), octaves = 2, amplitude = 0.5,
                                                  seed = seed + 491 }.value + 0.5) },
        }
    elseif t.ground ~= nil and t.ground ~= "smooth" then
        fail("Lands.Template ground must be 'smooth', 'terraces' or 'strata', got '" .. tostring(t.ground) .. "'")
    end
    -- Off the template's triangles, the same sea floor per pixel.
    local outside = (place and place.floor) and grid_of(place.floor) or nil
    local r = rasterize(g, size, { sub = sub, round = math.floor((t.round or 0.3) * sub + 0.5), fraction = t.land_fraction,
                                   noise = grid_of(fray), bands = bands, strata = strata,
                                   outside = outside,
                                   fjords = t.fjords or (t.template == "atoll" and 0 or 10), random = stream(tseed + 77) })
    r.x0, r.z0 = cx - 0.5 * size, cz - 0.5 * size
    if not place and t.ocean ~= false then
        -- A bounded map's ocean floor: the world's (`ocean_floor`, `ocean_compose`) with the whole map for its
        -- cell, so the stairs, the drowned land and the ocean's forms are at the map's scale. The floor falls
        -- away from the land masses (squares four fifths of their size), and canyons run out from each.
        local abyss = -abyssal_depth
        local squares, lands = {}, {}
        for _, m in ipairs(r.masses) do
            if m.cells >= 4 * sub * sub then
                local side = math.sqrt(m.cells) * px_m
                local at = { r.x0 + (m.x / m.cells + 0.5) * px_m, r.z0 + (m.z / m.cells + 0.5) * px_m }
                squares[#squares + 1] = { shape = "box", size = { 0.8 * side, 0.8 * side }, at = at, smooth = 0.0 }
                lands[#lands + 1] = { at = at, side = side }
            end
        end
        local wobble = Noises.fBm{ frequency = 1.0 / (0.25 * size), octaves = 3, amplitude = 0.35, seed = seed + 503 }.value
        local descent = Curves.Clamp{ input = 1.0 + wobble, min = 0.0, max = 1.0 }
        if #squares > 0 then
            local off = Shapes.Set{ elements = squares, reach = 0.7 * size }.value
            descent = Curves.Clamp{ input = (off - 0.05 * size) / (0.6 * size) + wobble, min = 0.0, max = 1.0 }
        end
        local drowned = ocean_floor{
            seed = seed, cell = size, abyss = abyss, lattice = g.nx, sub = 2, descent = descent, origin = { cx, cz },
            sources = function(_, _, _, canyons)
                local random = stream(tseed + 613)
                for _, l in ipairs(lands) do
                    for _ = 1, 4 + math.floor(random() * 6) do
                        local angle, length = random() * 2.0 * math.pi, 0.55 * l.side + (0.1 + 0.1 * random()) * size
                        ocean_capsule(canyons, l.at, { l.at[1] + math.cos(angle) * length, l.at[2] + math.sin(angle) * length },
                                      (0.015 + 0.01 * random()) * size, 0.7 + 0.3 * random())
                    end
                end
            end,
        }
        -- Below the strata's sea floor, or a smooth or terraced map's, which the surface then takes offshore.
        local base = r.strata_sea
        if not base then
            base = {}
            for q = 1, n * n do base[q] = r.floor and r.floor[q] or -shelf_depth end
        end
        -- Made at two pixels a cell, not the map's `sub` (a quarter of the work at four), and read at the map's
        -- pixels: each of its four lists resampled.
        local under, coarse = drowned(0, 0), g.nx * 2
        local read = {}
        for part = 0, 3 do
            local list = {}
            for q = 1, coarse * coarse do list[q] = under[part * coarse * coarse + q] end
            local fine = Raster.Resample{ values = list, from = { coarse, r.x0, r.z0, size / coarse },
                                          to = { n, r.x0, r.z0, px_m }, outside = list[1] }
            for q = 1, n * n do read[part * n * n + q] = fine[q] end
        end
        ocean_compose(base, r.sdf, read, n, size, abyss)
        -- Over the map's outer twenty-fifth the sea floor goes to the far floor's depth, which its rim is: past
        -- the map every grid repeats its edge, and a floor that varies there runs on as streaks.
        local rim = 0.04 * n
        for z = 0, n - 1 do
            for x = 0, n - 1 do
                local q = z * n + x + 1
                local e = math.min(x, z, n - 1 - x, n - 1 - z) / rim
                if e < 1.0 and r.sdf[q] > 0.0 then base[q] = 3.8 * abyss + (base[q] - 3.8 * abyss) * e * e * (3.0 - 2.0 * e) end
            end
        end
        r.seabed, r.deep = not r.strata_sea and base or nil, true
    end
    if place and strata then
        -- A world's cell samples the beaches' widths itself, at the world's cell size, so they are kept with it.
        r.at_coast = grid_of(beach_widths(t, seed, place.beach_m or 0.75 * size / g.nx))
    end
    return r, { seed = seed, size = size, cell_m = cell_m, bands = bands, cells = g.nx * g.nz, nx = g.nx }
end

-- The surface of a template's raster `r` (one map, or several stitched): the coast, ground, beaches and sea
-- floor on the GPU, the report and the products. `s` is what `template_raster` said of the map.
local function template_surface(t, r, s)
    local seed, size, cell_m, bands = s.seed, s.size, s.cell_m, s.bands
    local resolution, relief = t.resolution or 8.0, t.height or 1200.0
    local roughness = t.roughness or 1.0
    local margin = MARGINS[t.margin or "compressed"]
    local shelf_depth = t.shelf_depth or margin.shelf_depth
    local abyssal_depth = t.abyssal_depth or margin.abyssal_depth
    local grid_of = raster_grid(r.x0, r.z0, r.n, r.cell)
    local field = Noises.fBm{ frequency = 1.0 / (size * 0.06), octaves = 2, amplitude = 0.5, seed = seed + 401 }.value + 0.5
    local rough = Curves.Clamp{ input = (field - 0.25) / 0.75, min = 0.0, max = 1.0 }
    rough = rough * rough
    local ox0, oz0 = r.x0 + 0.5 * r.cell, r.z0 + 0.5 * r.cell

    -- Below the raster's cells, a warp by the same field, faded out two cells from the coast so it bends the
    -- coast and not the sea floor or the interior.
    local c = Terrain.Coords{}
    local plain = Terrain.Grid{ values = r.sdf, size = { r.n, r.n }, origin = { ox0, oz0 }, cell = r.cell }
    local near_coast = 1.0 - Curves.Smoothstep{ input = Combine.Max{ a = plain, b = -plain }, min = cell_m, max = 2.0 * cell_m }
    rough = rough * near_coast
    local ox, oz = c.x, c.z
    -- From half a cell down to four samples, one octave per halving: the offset keeps a fixed share of each
    -- wavelength, so the coast is as rough at every scale the export can show.
    local octaves = math.max(1, math.min(8, math.floor(math.log(0.5 * cell_m / (4.0 * resolution), 2)) + 1))
    if 0.5 * cell_m >= 4.0 * resolution then
        local amplitude = 0.35 * 0.5 * cell_m * roughness
        ox = ox + rough * Noises.fBm{ frequency = 2.0 / cell_m, octaves = octaves, amplitude = amplitude, seed = seed + 411 }.value
        oz = oz + rough * Noises.fBm{ frequency = 2.0 / cell_m, octaves = octaves, amplitude = amplitude, seed = seed + 421 }.value
    end
    local position = Vec.Combine{ ox, oz }
    local function sample(values)
        return Terrain.Grid{ values = values, size = { r.n, r.n }, origin = { ox0, oz0 }, cell = r.cell,
                             position = position }
    end
    local coast_sdf = sample(r.sdf)

    local transition = math.max(resolution * 1.5, 12.0)
    local land_mask = 1.0 - Curves.Smoothstep{ input = coast_sdf, min = -transition, max = transition }
    local level = r.level
    local ground, floor, beach_mask, platform, sand = nil, nil, nil, nil, nil
    if r.strata_land then
        ground, floor = sample(r.strata_land), sample(r.strata_sea)
        -- Beaches: a strip of sand laid on the sea side of the coast, so the land keeps all its ground and the
        -- sand clings to the foot of a cliff as to a low shore. A field fifteen cells across leaves whole faces
        -- of the land bare; along the rest the sand is never under an eighth of a cell, so the beaches join, and
        -- where one ten cells across says (long runs) it widens to between a fifth of a cell and one, as one six
        -- cells across says, and threefold where one sixteen cells across does. Offshore each pixel takes the
        -- width of the coast nearest it (stepped back along the distance's gradient), so the sand follows the
        -- coast and never runs out to sea as an arm where the width grows; and at most a third of the water's
        -- half-width there (the distance where it stops growing, stepped forward), so the sand of two coasts
        -- never meets across a strait or ties an islet to the land. A finer noise frays the edge. The sand
        -- rises from the water to 3 m at the old coast.
        local function field(cells, salt)
            return Noises.fBm{ frequency = 1.0 / (cells * cell_m), octaves = 2, amplitude = 0.5, seed = seed + salt }.value + 0.5
        end
        -- Sampled with the raster where it was made (a world's cells, kept with them), else here.
        local at_coast = r.at_coast or grid_of(beach_widths(t, seed, cell_m))
        local reach_px = math.ceil(4.0 * cell_m / r.cell)
        local projected = Raster.Project{ sdf = r.sdf, n = r.n, cell = r.cell, reach = reach_px, at_coast = at_coast }
        local fine = Noises.fBm{ frequency = 1.0 / (0.5 * cell_m), octaves = 3, amplitude = 0.04 * cell_m, seed = seed + 485 }.value
        local reach = sample(projected)
        local has = Curves.Smoothstep{ input = reach, min = 0.03 * cell_m, max = 0.1 * cell_m }
        local width = has * (reach + fine)
        local strip = Curves.Smoothstep{ input = coast_sdf, min = -transition, max = 0.0 } * has
        local on_sand = 3.0 * Curves.Clamp{ input = (width - coast_sdf) / (width + 1.0), min = 0.0, max = 1.0 }
        ground = Combine.Blend{ a = ground, b = on_sand, mask = strip }
        coast_sdf = coast_sdf - width
        land_mask = 1.0 - Curves.Smoothstep{ input = coast_sdf, min = -transition, max = transition }
        beach_mask, sand = strip * land_mask, strip
        -- Old coasts: where the sea stood lower its waves cut flats into the shelf and the slope, at a quarter,
        -- three fifths and 1.6 times the shelf's depth, each a flat and then a wall as deep as an eighth to a
        -- third of the shelf; their line follows the coast's, jagged by a noise a cell across.
        -- Off a beach the floor is the sand's: it goes down from the beach's edge at most as steeply as the
        -- shelf goes down over two cells, so a wide beach before a narrow shelf does not end in a wall; it
        -- fades out between two and three cells off, or it would run on as a ridge to the abyss.
        local off = Curves.Clamp{ input = coast_sdf, min = 0.0, max = 1.0e7 }
        -- Its mask is the beach's spread a cell (only the mask: where the nearest coast flips from a beach to
        -- rock it would jump, a thin ridge across the shelf's edge).
        local near = 1.0 - Curves.Smoothstep{ input = off, min = 2.0 * cell_m, max = 3.0 * cell_m }
        local spread = Filters.Blur{ input = has, radius = math.max(math.floor(cell_m / resolution + 0.5), 1) }
        floor = Combine.Blend{ a = floor, b = Combine.Max{ a = floor, b = -shelf_depth * off / (2.0 * cell_m) }, mask = spread * near }
        local depth = -floor
        local pick = depth + Noises.fBm{ frequency = 1.0 / cell_m, octaves = 3, amplitude = 0.04 * shelf_depth, seed = seed + 493 }.value
        for _, step in ipairs{ { 0.25, 0.12 }, { 0.6, 0.15 }, { 1.6, 0.3 } } do
            local l, h = step[1] * shelf_depth, step[2] * shelf_depth
            depth = depth - Curves.Clamp{ input = depth - l, min = 0.0, max = h }
                    + h * Curves.Smoothstep{ input = pick, min = l + h - 0.03 * shelf_depth, max = l + h }
        end
        floor = -depth
        -- A platform the waves cut at the foot of the cliffs (not off the beaches): flat 8 m down, a fifth of
        -- a cell to most of one out as a field six cells across says, then a wall down to the floor.
        platform = { depth = 8.0, keep = 1.0 - has,
                     width = (0.2 + 0.6 * Curves.Clamp{ input = field(6.0, 495), min = 0.0, max = 1.0 }) * cell_m }
    elseif r.ground then
        -- Walls: the bands stepped on the continuous height, a hundredth of the range wide, where the triangles
        -- are cliffs; planes where they are slopes.
        local function step(x, edge)
            return Curves.Clamp{ input = (x - edge) / 0.008 + 0.5, min = 0.0, max = 1.0 }
        end
        local cells_h = sample(r.height)
        local u, s = (cells_h - level) / (100.0 - level), (level - cells_h) / math.max(level, 1.0)
        local lands, seas = bands.lands, bands.seas
        local walls_land = relief * math.max(lands[1], 0.02)
        for k = 2, #lands do
            walls_land = walls_land + relief * (lands[k] - math.max(lands[k - 1], 0.02)) * step(u, lands[k])
        end
        local walls_sea = -seas[1][2]
        for k = 2, #seas do
            walls_sea = walls_sea - (seas[k][2] - seas[k - 1][2]) * step(s, seas[k][1])
        end
        local walls = sample(r.cliff)
        ground = Combine.Blend{ a = sample(r.ground), b = walls_land, mask = walls }
        floor = Combine.Blend{ a = sample(r.floor), b = walls_sea, mask = walls }
    else
        ground = Curves.Clamp{ input = (sample(r.height) - level) / (100.0 - level), min = 0.0, max = 1.0 } * relief
    end
    -- Offshore, a smooth or terraced map takes its ocean floor (`r.seabed`), its strata's were laid already.
    if r.seabed then floor = sample(r.seabed) end
    local region_id = Curves.Terrace{ input = sample(r.region), step = 1.0, sharpness = 1.0 }

    -- The report: each land mass by size (`mass/1` the largest), and the water bodies, an `ocean` where one
    -- touches the map's edge, else a `sea` over a hundredth of the map or a `lake`.
    local px_area = r.cell * r.cell
    local elements, land_cells = {}, 0
    for rank, m in ipairs(r.masses) do
        land_cells = land_cells + m.cells
        local area = m.cells * px_area
        elements[rank] = {
            id = "mass/" .. rank, kind = "land", shape = "template", area_measured = area,
            centroid = { r.x0 + (m.x / m.cells + 0.5) * r.cell, r.z0 + (m.z / m.cells + 0.5) * r.cell },
            class = area >= size * size * 0.1 and "continent" or (area >= size * size * 0.001 and "island" or "isle"),
        }
    end
    local waters = {}
    for k, w in ipairs(r.waters) do
        local area = w.cells * px_area
        waters[k] = { area = area, touches_edge = w.edge,
                      class = w.edge and "ocean" or (area >= size * size * 0.01 and "sea" or "lake") }
    end
    local fraction = land_cells / (r.n * r.n)
    local width_scale = margin.scale and size * 0.5 or 1.0
    local shelf_width = t.shelf_width or margin.shelf_width * width_scale
    local slope_width = t.slope_width or margin.slope_width * width_scale
    local side = r.n * r.cell
    local template = s.templates or t.template
    engine.report("lands", { template = template, frame = s.frame or { size = size },
                             window = { r.x0, r.z0, r.x0 + side, r.z0 + side },
                             cells = s.cells, level = level, land_fraction = fraction, elements = elements,
                             pending = s.pending or nil,
                             waters = waters })
    local info = { size = size, extent = side * 0.5, template = template, cells = s.cells, level = level,
                   land_fraction = fraction, regions = math.max(#elements, 1), elements = elements, waters = waters }
    return finish{
        t = t, seed = seed, size = size, resolution = resolution, relief = relief, coast = COASTS[t.coast or "mixed"],
        coast_sdf = coast_sdf, land_mask = land_mask, region_id = region_id, ground = ground, gullies = false,
        shelf_width = shelf_width, shelf_depth = shelf_depth, slope_width = slope_width, abyssal_depth = abyssal_depth,
        floor = floor, floor_blend = platform and 160.0 or 2.0 * cell_m, beach_mask = beach_mask, beach_strip = sand, platform = platform,
        deep = r.deep,
        info = info,
    }
end

local TEMPLATE_KEYS = {
    bend = true, fray = true, fjords = true, ocean = true, template = true, steps = true, detail = true, seed = true, size = true,
    resolution = true, height = true, cells = true, roughness = true, land_fraction = true, ground = true, terraces = true,
    round = true, slopes = true, beaches = true,
    margin = true, coast = true, shelf_width = true, shelf_depth = true, slope_width = true, abyssal_depth = true,
    wave_from = true, hardness = true, sediment = true, shore_band = true,
    beach_width = true, beach_height = true, cliff_width = true, cliff_height = true,
}

-- Land and sea from one of Azgaar's templates over a map of `size` metres centred on the origin.
function Lands.Template(t)
    if type(t) ~= "table" then
        fail("Lands.Template expects a table of named arguments, as in Lands.Template{ template = 'high_island' }")
    end
    for key in pairs(t) do
        if not TEMPLATE_KEYS[key] then
            local list = {}
            for k in pairs(TEMPLATE_KEYS) do list[#list + 1] = k end
            table.sort(list)
            fail("Lands.Template does not take '" .. tostring(key) .. "'; it accepts " .. table.concat(list, ", "))
        end
    end
    if t.coast ~= nil and COASTS[t.coast] == nil then
        fail("Lands.Template coast must be 'mixed', 'rocky', 'sedimentary' or 'cliff', got '" .. tostring(t.coast) .. "'")
    end
    if t.margin ~= nil and MARGINS[t.margin] == nil then
        fail("Lands.Template margin must be 'compressed' or 'geologic', got '" .. tostring(t.margin) .. "'")
    end
    local text = t.steps or TEMPLATES[t.template]
    if text == nil then
        local names = {}
        for k in pairs(TEMPLATES) do names[#names + 1] = k end
        table.sort(names)
        fail("Lands.Template needs a 'template': " .. table.concat(names, ", ") .. "; got '" .. tostring(t.template) .. "'")
    end
    local seed, size = t.seed or 0, t.size or 48000.0
    local r, s = template_raster(t, text, seed, seed, size, 0.0, 0.0, nil)
    return template_surface(t, r, s)
end

-- The templates the world draws from, by weight: islands for its cells, lands over most of a map for its
-- continents (not the mediterranean, whose land is at the map's rim, where the map sinks it).
local WORLD_TEMPLATES = { high_island = 3, volcano = 2, low_island = 2, archipelago = 2, atoll = 1, shattered = 1 }
local CONTINENT_TEMPLATES = { continents = 2, pangea = 2, old_world = 2, fractious = 1, peninsula = 1, isthmus = 1 }
local WORLD_KEYS = { world_cell = true, region = true, templates = true, edge = true, empty = true, budget = true,
                     continent_cell = true, continents = true, continent_templates = true, continent_detail = true }
for key in pairs(TEMPLATE_KEYS) do
    if key ~= "template" and key ~= "steps" and key ~= "size" and key ~= "land_fraction" then WORLD_KEYS[key] = true end
end

-- A world cell's raster kept by the engine for the rest of the process (Raster.Keep), so a viewer that
-- re-runs the script as the camera moves pays only for the cells it has not seen. The key is every number
-- the raster was made from; the raster and what the surface needs of it go as one list of numbers.
local CELL_LISTS = { "sdf", "height", "ground", "floor", "cliff", "strata_land", "strata_sea", "region", "at_coast" }
local function cell_key(t, parts)
    local keys, text = {}, { "world/7" }
    for key, v in pairs(t) do
        if key ~= "region" and key ~= "resolution" and key ~= "templates" and key ~= "budget" then keys[#keys + 1] = key end
    end
    table.sort(keys)
    for _, key in ipairs(keys) do
        local v = t[key]
        text[#text + 1] = key .. "=" .. (type(v) == "table" and table.concat(v, ",") or tostring(v))
    end
    for _, v in ipairs(parts) do text[#text + 1] = tostring(v) end
    return table.concat(text, ";")
end
local function pack_cell(r, s)
    local out = { r.n, r.cell, r.level, r.x0, r.z0, s.cell_m, s.cells, s.nx, #r.masses, #r.waters }
    for _, name in ipairs(CELL_LISTS) do
        local list = r[name]
        out[#out + 1] = list and 1 or 0
        if list then
            for i = 1, r.n * r.n do out[#out + 1] = list[i] end
        end
    end
    for _, m in ipairs(r.masses) do
        out[#out + 1], out[#out + 2], out[#out + 3] = m.cells, m.x, m.z
    end
    for _, w in ipairs(r.waters) do
        out[#out + 1], out[#out + 2] = w.cells, w.edge and 1 or 0
    end
    return out
end
local function unpack_cell(list, seed, size)
    local r = { n = list[1], cell = list[2], level = list[3], x0 = list[4], z0 = list[5], masses = {}, waters = {} }
    local s = { seed = seed, size = size, cell_m = list[6], cells = list[7], nx = list[8] }
    local at, count = 11, r.n * r.n
    for _, name in ipairs(CELL_LISTS) do
        local present = list[at]
        at = at + 1
        if present == 1 then
            r[name] = table.move(list, at, at + count - 1, 1, {})
            at = at + count
        end
    end
    for k = 1, list[9] do
        r.masses[k] = { cells = list[at], x = list[at + 1], z = list[at + 2], rank = k }
        at = at + 3
    end
    for k = 1, list[10] do
        r.waters[k] = { cells = list[at], edge = list[at + 1] == 1 }
        at = at + 2
    end
    return r, s
end

-- The unbounded world in templates: each world cell (`world_cell` metres, 96 km by default; cell (i, j) centred
-- on (i, j) times it) is one template's map, drawn by weight from `templates` by a hash of (seed, i, j) and
-- sunk to the abyss over `edge` of it at its rim (a quarter), so the cells meet in open ocean and need nothing of
-- each other: any window over the same cells is the same world. The cells under `region` ({ x0, z0, x1, z1 },
-- a square of them) are rastered one by one and stitched, then the surface is evaluated once over them all;
-- beyond them is the abyss. The rest of the keys are Lands.Template's.
function Lands.World(t)
    if type(t) ~= "table" then
        fail("Lands.World expects a table of named arguments, as in Lands.World{ seed = 1 }")
    end
    for key in pairs(t) do
        if not WORLD_KEYS[key] then
            local list = {}
            for k in pairs(WORLD_KEYS) do list[#list + 1] = k end
            table.sort(list)
            fail("Lands.World does not take '" .. tostring(key) .. "'; it accepts " .. table.concat(list, ", "))
        end
    end
    local seed, cell = t.seed or 0, t.world_cell or 96000.0
    local region = t.region or { -0.5 * cell, -0.5 * cell, 0.5 * cell, 0.5 * cell }
    local weights, names, total = t.templates or WORLD_TEMPLATES, {}, 0.0
    for name, w in pairs(weights) do
        if TEMPLATES[name] == nil then fail("Lands.World: '" .. tostring(name) .. "' is not a template") end
        names[#names + 1] = name
        total = total + w
    end
    table.sort(names)
    for _, name in ipairs(names) do names[name] = weights[name] end
    local i0, j0 = math.floor(region[1] / cell + 0.5), math.floor(region[2] / cell + 0.5)
    -- Cell i spans (i - 1/2, i + 1/2) cells: a region ending on a cell's edge does not reach into the next.
    local k = math.max(math.ceil(region[3] / cell - 0.5) - i0, math.ceil(region[4] / cell - 0.5) - j0) + 1
    local tw = {}
    for key, v in pairs(t) do tw[key] = v end
    tw.ground = tw.ground or "strata"
    -- Three pixels a cell, not four: nine cells must raster inside the script's minute, and below the pixel
    -- the warp on the GPU draws the coast anyway.
    tw.detail = tw.detail or 3
    local edge = t.edge or 0.25
    local cells_n = tw.cells or 10000
    local spacing = math.sqrt(1.0e6 / cells_n)
    local nw = math.floor((1000.0 + 0.5 * spacing - 1e-10) / spacing) * tw.detail
    local px = cell / nw
    local cap = 0.12 * cell
    local abyss = -(t.abyssal_depth or MARGINS[t.margin or "compressed"].abyssal_depth)
    -- What a pixel no map covers is: the open abyss, in its strata as deep as the drowned land goes far out.
    local OPEN = { sdf = cap, height = 0.0, strata_land = 0.0, strata_sea = 3.8 * abyss, region = 0.0, at_coast = 0.0,
                   ground = 0.0, floor = abyss, cliff = 0.0 }
    local function pick_template(random, list, sum)
        local pick, acc = random() * sum, 0.0
        for _, candidate in ipairs(list) do
            acc = acc + list[candidate]
            if pick < acc then return candidate end
        end
        return list[#list]
    end
    -- How many maps this run may make that the engine does not keep yet (`budget`, none by default): past
    -- it a map is left out, its cell open sea, and the report says `pending`, so a viewer that follows the
    -- camera runs the script again at once and the world fills in nearest first, a few maps a run.
    local made, budget, pending = 0, t.budget or math.huge, false
    local function affordable()
        if made >= budget then
            pending = true
            return false
        end
        made = made + 1
        return true
    end
    -- A map's raster, from the engine's keep when this process made it before (Raster.Keep).
    local function raster_of(name, tseed, size, cx, cz, place, parts)
        tw.template = name
        local key = cell_key(tw, parts)
        local kept = tw.ground ~= "terraces" and Raster.Recall{ key = key } or nil
        local r, s
        if kept then
            r, s = unpack_cell(kept, seed, size)
        elseif affordable() then
            r, s = template_raster(tw, TEMPLATES[name], seed, tseed, size, cx, cz, edge, place)
            if tw.ground ~= "terraces" then Raster.Keep{ key = key, numbers = pack_cell(r, s) } end
        end
        tw.template = nil
        return r, s
    end

    -- The continents: a layer of cells `continent_cell` across (four world cells by default), each holding
    -- at most one (`continents` of them, 0.55 by default), a continental template drawn into 45 to 85 % of
    -- the cell at a place of its own; rastered at their own size and read at the world's pixels.
    local C = t.continent_cell or 4.0 * cell
    local lands_list, lands_sum = {}, 0.0
    for name, w in pairs(t.continent_templates or CONTINENT_TEMPLATES) do
        if TEMPLATES[name] == nil then fail("Lands.World: '" .. tostring(name) .. "' is not a template") end
        lands_list[#lands_list + 1] = name
        lands_sum = lands_sum + w
    end
    table.sort(lands_list)
    for _, name in ipairs(lands_list) do lands_list[name] = (t.continent_templates or CONTINENT_TEMPLATES)[name] end
    -- The sea floor (below), which the continents' rasters read when a cell first reaches them.
    local seafloor
    local continents, used = {}, {}
    local function continent(I, J)
        local id = I .. "," .. J
        if continents[id] ~= nil then return continents[id] end
        local cseed = (math.floor(seed) * 19349663 ~ I * 73856093 ~ J * 50331653 ~ 0x5bd1e995) & 0x7FFFFFFF
        local random = stream(cseed)
        local present = random() < (t.continents or 0.55)
        local name = pick_template(random, lands_list, lands_sum)
        local scale = 0.45 + 0.4 * random()
        local side = scale * C
        local x0 = (I - 0.5) * C + random() * (1.0 - scale) * C
        local z0 = (J - 0.5) * C + random() * (1.0 - scale) * C
        local entry = false
        if present then
            -- Placed now, rastered only once a cell it reaches asks (`raster` below).
            entry = { name = name, x0 = x0, z0 = z0, side = side, id = id, cseed = cseed, I = I, J = J }
        end
        continents[id] = entry
        return entry
    end
    local function rastered(c)
        if c.r == nil then
            local saved = tw.detail
            tw.detail = t.continent_detail or 4
            c.r = raster_of(c.name, c.cseed, c.side, c.x0 + 0.5 * c.side, c.z0 + 0.5 * c.side,
                            { scale = 1.0, x = 0.0, z = 0.0, beach_m = 0.75 * cell / (nw // saved), floor = seafloor },
                            { "continent", seed, c.cseed, c.I, c.J, C, c.side, c.x0, c.z0, edge })
            tw.detail = saved
        end
        return c
    end
    -- The continents whose square, widened by a tenth of it, overlaps world cell (i, j).
    local function touching(i, j, placed_only)
        local out = {}
        local x0, z0, x1, z1 = (i - 0.5) * cell, (j - 0.5) * cell, (i + 0.5) * cell, (j + 0.5) * cell
        for I = math.floor(x0 / C + 0.5) - 1, math.floor(x1 / C + 0.5) + 1 do
            for J = math.floor(z0 / C + 0.5) - 1, math.floor(z1 / C + 0.5) + 1 do
                local c = continent(I, J)
                if c then
                    local m = 0.1 * c.side
                    if c.x0 - m < x1 and c.x0 + c.side + m > x0 and c.z0 - m < z1 and c.z0 + c.side + m > z0 then
                        if placed_only then
                            out[#out + 1] = c
                        elseif rastered(c).r then
                            -- A continent the run had no budget for is left out this time.
                            out[#out + 1] = c
                        end
                    end
                end
            end
        end
        return out
    end

    -- An island cell's draws: its template, how much of the cell it fills (25 to 90 %) and where in it, and
    -- whether it is open sea (`empty`, 0.35 of the cells), so the islands are not a grid.
    local function island(i, j)
        local tseed = (math.floor(seed) * 73856093 ~ i * 19349663 ~ j * 83492791) & 0x7FFFFFFF
        local random = stream(tseed)
        local name = pick_template(random, names, total)
        local scale = 0.25 + 0.65 * random()
        return name, tseed, { scale = scale, x = random() * (1.0 - scale), z = random() * (1.0 - scale),
                              empty = random() < (t.empty or 0.35) }
    end
    -- The sea floor every map sinks to at its rim and keeps off its triangles, in his heights (the sea level
    -- is 20), from the lands round it: 13 (the upper slope) at the lands' squares, falling to 1.5 (the abyss)
    -- 0.6 of a world cell off them, so the islands of a group share a rise; beyond, the drowned land below
    -- (`drowned`) is the floor.
    local squares = {}
    for I = math.floor(region[1] / C + 0.5) - 1, math.floor(region[3] / C + 0.5) + 1 do
        for J = math.floor(region[2] / C + 0.5) - 1, math.floor(region[4] / C + 0.5) + 1 do
            local c = continent(I, J)
            if c then
                squares[#squares + 1] = { shape = "box", size = { 0.75 * c.side, 0.75 * c.side },
                                          at = { c.x0 + 0.5 * c.side, c.z0 + 0.5 * c.side }, smooth = 0.0 }
            end
        end
    end
    for i = i0 - 2, i0 + k + 1 do
        for j = j0 - 2, j0 + k + 1 do
            local _, _, place = island(i, j)
            if not place.empty and #touching(i, j, true) == 0 then
                local side = place.scale * cell
                squares[#squares + 1] = { shape = "box", size = { 0.6 * side, 0.6 * side }, smooth = 0.0,
                                          at = { (i - 0.5 + place.x) * cell + 0.5 * side, (j - 0.5 + place.z) * cell + 0.5 * side } }
            end
        end
    end
    seafloor = Terrain.Const{ value = 1.5 }
    if #squares > 0 then
        local off = Shapes.Set{ elements = squares, reach = 0.6 * cell }.value
        seafloor = seafloor + 11.5 * Curves.Clamp{ input = 1.0 - off / (0.6 * cell), min = 0.0, max = 1.0 }
    end
    -- How far the drowned land has fallen below the abyss, 0 to 1: none within a twentieth of a cell of the
    -- lands' squares, all of it 0.65 of a cell off, so the deepest floor is midway between lands far apart;
    -- bent by a field a quarter of a cell across, or its contours would be the squares' rounded rectangles.
    local bend = Noises.fBm{ frequency = 1.0 / (0.25 * cell), octaves = 3, amplitude = 0.35, seed = seed + 503 }.value
    local descent = Curves.Clamp{ input = 1.0 + bend, min = 0.0, max = 1.0 }
    if #squares > 0 then
        local off = Shapes.Set{ elements = squares, reach = 0.7 * cell }.value
        descent = Curves.Clamp{ input = (off - 0.05 * cell) / (0.6 * cell) + bend, min = 0.0, max = 1.0 }
    end
    -- The open ocean floor (`ocean_floor`), the world's: kept in the engine per cell, made within the budget,
    -- and with the trenches and canyons of its continents and islands.
    local lattice = math.floor((1000.0 + 0.5 * spacing - 1e-10) / spacing)
    local drowned = ocean_floor{
        seed = seed, cell = cell, abyss = abyss, lattice = lattice, sub = tw.detail, descent = descent,
        key = "drowned/10;" .. seed .. ";" .. nw, affordable = affordable,
        sources = function(i, j, deeps, canyons)
            -- Off the continents near: a trench along one side, and canyons all round.
            for _, c in ipairs(touching(i, j, true)) do
                local random = stream((c.cseed * 31 + 0x3a8f) & 0x7FFFFFFF)
                local x0, z0, side = c.x0, c.z0, c.side
                local function edge_point(t, k, out)
                    -- Side k of the square at t along it, `out` metres outside (inside when negative).
                    if k == 0 then return { x0 + t * side, z0 - out } end
                    if k == 1 then return { x0 + side + out, z0 + t * side } end
                    if k == 2 then return { x0 + t * side, z0 + side + out } end
                    return { x0 - out, z0 + t * side }
                end
                if random() < 0.6 then
                    local k, w, last = math.floor(random() * 4), (0.02 + 0.01 * random()) * cell, nil
                    for q = 0, 6 do
                        local t = 0.15 + 0.7 * q / 6
                        local p = edge_point(t, k, (0.08 + 0.06 * (1.0 - (2.0 * t - 1.0) ^ 2)) * side)
                        if last then ocean_capsule(deeps, last, p, w, 1.0) end
                        last = p
                    end
                end
                for _ = 1, 16 + math.floor(random() * 12) do
                    local k, t = math.floor(random() * 4), 0.05 + 0.9 * random()
                    ocean_capsule(canyons, edge_point(t, k, -0.12 * side), edge_point(t + (random() - 0.5) * 0.1, k, 0.15 * cell),
                                  (0.015 + 0.01 * random()) * cell, 0.7 + 0.3 * random())
                end
            end
            -- Canyons off the islands near, from the middle of each out past its slope.
            for a = i - 1, i + 1 do
                for b = j - 1, j + 1 do
                    local _, tseed, place = island(a, b)
                    if not place.empty and #touching(a, b, true) == 0 then
                        local random = stream((tseed * 17 + 0x1f3d) & 0x7FFFFFFF)
                        local side = place.scale * cell
                        local cx, cz = (a - 0.5 + place.x) * cell + 0.5 * side, (b - 0.5 + place.z) * cell + 0.5 * side
                        for _ = 1, 4 + math.floor(random() * 6) do
                            local angle, length = random() * 2.0 * math.pi, 0.55 * side + (0.1 + 0.1 * random()) * cell
                            ocean_capsule(canyons, { cx, cz }, { cx + math.cos(angle) * length, cz + math.sin(angle) * length },
                                          (0.015 + 0.01 * random()) * cell, 0.7 + 0.3 * random())
                        end
                    end
                end
            end
        end,
    }

    local big = { n = k * nw, cell = px, level = 20.0, x0 = (i0 - 0.5) * cell, z0 = (j0 - 0.5) * cell, deep = true,
                  masses = {}, waters = {} }
    local s, picked = nil, {}
    -- A cell the run had no budget for: open sea, made again (and kept) by a later run.
    local function open_cell()
        local r = { masses = {}, waters = {} }
        for _, key in ipairs{ "sdf", "height", "strata_land", "strata_sea", "region", "at_coast" } do
            local list = {}
            for q = 1, nw * nw do list[q] = OPEN[key] end
            r[key] = list
        end
        return r
    end
    -- Nearest the region's centre first, so a budget spends itself where the camera is.
    local order, mx, mz = {}, 0.5 * (region[1] + region[3]), 0.5 * (region[2] + region[4])
    for b = 0, k - 1 do
        for a = 0, k - 1 do order[#order + 1] = { a, b } end
    end
    table.sort(order, function(p, q)
        local dp = ((i0 + p[1]) * cell - mx) ^ 2 + ((j0 + p[2]) * cell - mz) ^ 2
        local dq = ((i0 + q[1]) * cell - mx) ^ 2 + ((j0 + q[2]) * cell - mz) ^ 2
        return dp < dq or (dp == dq and (p[2] < q[2] or (p[2] == q[2] and p[1] < q[1])))
    end)
    for _, cell_ab in ipairs(order) do
        local a, b = cell_ab[1], cell_ab[2]
        do
            local i, j = i0 + a, j0 + b
            local here_x0, here_z0 = (i - 0.5) * cell, (j - 0.5) * cell
            local r, shift
            local lands = touching(i, j)
            if #lands > 0 then
                -- Over the cell's own sea floor, the continents read at its pixels; where two reach, the one nearer
                -- its coast.
                local base = raster_of(names[1], 0, cell, i * cell, j * cell,
                                       { scale = 1.0, x = 0.0, z = 0.0, empty = true, floor = seafloor },
                                       { "sea", seed, i, j, cell, edge }) or open_cell()
                r, shift = { masses = {}, waters = {} }, 0
                for _, key in ipairs(CELL_LISTS) do r[key] = base[key] end
                local to = { nw, here_x0, here_z0, px }
                for _, c in ipairs(lands) do
                    if used[c.id] == nil then
                        -- The continent's land ids follow the ids before it, once, wherever it is read.
                        used[c.id] = #big.masses
                        local ratio = (c.r.cell / px) ^ 2
                        for _, m in ipairs(c.r.masses) do
                            local mx = ((c.r.x0 + (m.x / m.cells + 0.5) * c.r.cell) - big.x0) / px - 0.5
                            local mz = ((c.r.z0 + (m.z / m.cells + 0.5) * c.r.cell) - big.z0) / px - 0.5
                            local count = m.cells * ratio
                            big.masses[#big.masses + 1] = { cells = count, x = mx * count, z = mz * count }
                        end
                    end
                    local from = { c.r.n, c.r.x0, c.r.z0, c.r.cell }
                    local read = {}
                    for _, key in ipairs(CELL_LISTS) do
                        if c.r[key] then
                            read[key] = Raster.Resample{ values = c.r[key], from = from, to = to,
                                                         nearest = key == "region", outside = OPEN[key] }
                        end
                    end
                    local ids = used[c.id]
                    for q = 1, nw * nw do
                        -- Only where the continent's map reaches (its distance short of the open sea's cap).
                        if r.sdf == nil or r.sdf[q] == nil or (read.sdf[q] < cap and read.sdf[q] < r.sdf[q]) then
                            for key, list in pairs(read) do
                                r[key] = r[key] or {}
                                local v = list[q]
                                r[key][q] = (key == "region" and v > 0) and v + ids or v
                            end
                        end
                    end
                end
                picked[#picked + 1] = "continent"
            else
                local name, tseed, place = island(i, j)
                place.floor = seafloor
                picked[#picked + 1] = place.empty and "sea" or name
                r, s = raster_of(name, tseed, cell, i * cell, j * cell, place,
                                 { seed, tseed, i, j, cell, edge, place.scale, place.x, place.z, place.empty })
                r = r or open_cell()
                shift = #big.masses
                for _, m in ipairs(r.masses) do
                    big.masses[#big.masses + 1] = { cells = m.cells, x = m.x + a * nw * m.cells, z = m.z + b * nw * m.cells }
                end
                for _, w in ipairs(r.waters) do
                    if not w.edge then big.waters[#big.waters + 1] = w end
                end
            end
            -- Under the cell's own sea, the drowned land (`ocean_compose`).
            if r.strata_sea then ocean_compose(r.strata_sea, r.sdf, drowned(i, j), nw, cell, abyss) end
            -- The cell's pixels at its place in the stitched raster, the distance offshore capped short of the
            -- rim's open sea, so every cell meets the next at the same value, an empty one too.
            local N = big.n
            for q = 1, nw * nw do
                if r.sdf[q] > cap then r.sdf[q] = cap end
            end
            for _, key in ipairs(CELL_LISTS) do
                local src = r[key]
                if src then
                    local dst = big[key] or {}
                    big[key] = dst
                    local add = (key == "region" and r.masses[1] ~= nil) and shift or 0
                    for z = 0, nw - 1 do
                        local row, from = (b * nw + z) * N + a * nw, z * nw
                        for x = 1, nw do
                            local v = src[from + x]
                            dst[row + x] = (add > 0 and v > 0) and v + add or v
                        end
                    end
                end
            end
        end
    end
    -- A list some cells lack (the open sea's where only a continent's or an island's has it).
    for _, key in ipairs(CELL_LISTS) do
        local dst = big[key]
        if dst then
            for q = 1, big.n * big.n do dst[q] = dst[q] or OPEN[key] end
            -- The raster's rim is the open sea: past it every grid repeats its edge, and a continent the
            -- region cut through would run on as land to the horizon.
            local N = big.n
            for q = 0, N - 1 do
                dst[q + 1], dst[(N - 1) * N + q + 1] = OPEN[key], OPEN[key]
                dst[q * N + 1], dst[q * N + N] = OPEN[key], OPEN[key]
            end
        end
    end
    s = s or { seed = seed, size = cell, cells = cells_n, nx = nw // tw.detail }
    tw.template = nil
    -- One cell size for the surface's scales, whatever the cells drew, so every window agrees.
    s.cell_m = 0.75 * cell / s.nx
    s.cells = s.cells * k * k
    s.templates = table.concat(picked, ",")
    s.frame = { world_cell = cell, region = region }
    s.pending = pending
    return template_surface(tw, big, s)
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
    dissection = "drainage comes after the shapes; edit the shapes",
    count = "an island is one composition; see Lands.Archipelago",
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
-- The presets one of Azgaar's templates draws. By default their outline is the template's (`Lands.Template`),
-- like his maps and like the reference heightmaps; `outline = "shapes"` builds the composition of shapes
-- instead, whose elements have ids an `edit` reaches.
local TEMPLATE_OF = {
    volcano = "volcano", high_island = "high_island", low_island = "low_island", atoll = "atoll",
    scattered = "archipelago", shattered = "shattered",
    continent = "continents", pangea = "pangea", old_world = "old_world", mediterranean = "mediterranean",
    peninsula = "peninsula", isthmus = "isthmus", fractious = "fractious", taklamakan = "taklamakan",
}

-- The built land, from the template when the preset has one and `outline` does not say "shapes"; nil for the
-- composition. `defaults` are the preset's size, height and coast.
local function from_template(where, t, defaults)
    local outline = t.outline
    if outline ~= nil and outline ~= "template" and outline ~= "shapes" then
        fail(where .. " takes outline = 'template' or 'shapes', got '" .. tostring(outline) .. "'")
    end
    if outline == "shapes" or (outline == nil and not TEMPLATE_OF[t.preset]) then
        return nil
    end
    if not TEMPLATE_OF[t.preset] then
        fail(where .. " preset '" .. tostring(t.preset) .. "' has no template outline; it is a composition of shapes")
    end
    local copy = { template = TEMPLATE_OF[t.preset] }
    for key, value in pairs(t) do
        if key ~= "preset" and key ~= "outline" then
            if not TEMPLATE_KEYS[key] then
                fail(where .. " with the template outline does not take '" .. tostring(key)
                     .. "'; outline = 'shapes' builds the composition of shapes, which does")
            end
            copy[key] = value
        end
    end
    copy.size, copy.height, copy.coast = copy.size or defaults.size, copy.height or defaults.height, copy.coast or defaults.coast
    return Lands.Template(copy)
end

-- `t` without `outline`, for the composition.
local function without_outline(t)
    local copy = {}
    for key, value in pairs(t) do
        if key ~= "outline" then copy[key] = value end
    end
    return copy
end

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
    local p = ISLAND_PRESETS[t.preset] or {}
    local built = from_template("Lands.Island", t, { size = 24000.0, height = p.height, coast = p.coast })
    if built then return built end
    t = without_outline(t)
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
    local p = ARCHIPELAGO_PRESETS[t.preset]
    local built = from_template("Lands.Archipelago", t, { size = 48000.0, height = p.height, coast = p.coast })
    if built then return built end
    t = without_outline(t)
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
    local p = LAND_PRESETS[t.preset]
    local built = from_template("Lands.Continent", t, { size = p.size, height = p.height, coast = p.coast })
    if built then return built end
    t = without_outline(t)
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
