-- Shapes: signed distances, placed, combined and masked.
--
-- A shape is a field in metres: negative inside, zero on the edge, positive outside. Everything here
-- composes the ops the engine already has (`Geometry.*`, `Terrain.Transform`, `Combine.*`, `Curves.*`);
-- nothing evaluates a sample in Lua. Placement is a `Terrain.Transform` on the position the shape is
-- evaluated at, so the same shape can be reused anywhere and a warped position bends it.
--
-- Run by the engine before the script. Errors are raised with `fail`, which reports the line of the
-- script that called in here, not a line of this file.

local engine = ...
local fail   = engine.fail

-- Captured now, so a script that reassigns one of these globals cannot change what the prelude means.
local Geometry, Terrain, Curves, Combine, Vec, Noises =
    Geometry, Terrain, Curves, Combine, Vec, Noises
local type, ipairs, pairs, tostring = type, ipairs, pairs, tostring
local math, table = math, table

local Shapes = {}

-- --- Argument checking ----------------------------------------------------------------------------

-- Every function takes one table, and an unknown key is an error that lists what is accepted: the same
-- rule the C++ bindings follow, because a misspelt key that is ignored is a wrong terrain that runs.
local function check(name, t, allowed)
    if type(t) ~= "table" then
        fail(name .. " expects a table of named arguments, as in " .. name .. "{ ... }")
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

local function require_number(name, t, key)
    local value = t[key]
    if type(value) ~= "number" then
        fail(name .. " needs '" .. key .. "' as a number of metres, got " .. type(value))
    end
    return value
end

local function require_table(name, t, key, what)
    local value = t[key]
    if type(value) ~= "table" then
        fail(name .. " needs '" .. key .. "' as " .. what .. ", got " .. type(value))
    end
    return value
end

local PLACEMENT = { "at", "rotation", "scale", "pivot", "position" }

local function accepted(...)
    local list = { ... }
    for _, key in ipairs(PLACEMENT) do
        list[#list + 1] = key
    end
    return list
end

-- The position a shape is evaluated at: the one given, moved by `at`, `rotation`, `scale` and `pivot`
-- if any of them is present, and the world position when none is.
local function place(t)
    if t.at == nil and t.rotation == nil and t.scale == nil and t.pivot == nil then
        return t.position
    end
    return Terrain.Transform{
        position = t.position,
        at       = t.at,
        rotation = t.rotation,
        scale    = t.scale,
        pivot    = t.pivot,
    }
end

-- --- Constructors ---------------------------------------------------------------------------------

function Shapes.Circle(t)
    check("Shapes.Circle", t, accepted("radius"))
    return Geometry.Circle{ position = place(t), radius = require_number("Shapes.Circle", t, "radius") }
end

function Shapes.Point(t)
    check("Shapes.Point", t, { "at", "position" })
    return Geometry.Circle{ position = place(t), radius = 0 }
end

function Shapes.Ellipse(t)
    check("Shapes.Ellipse", t, accepted("radii"))
    return Geometry.Ellipse{
        position = place(t),
        radii    = require_table("Shapes.Ellipse", t, "radii", "a pair { rx, rz }"),
    }
end

-- `metric = "chebyshev"` gives square faces, which is what a pyramid is made from. A rounded corner
-- only means something in the ordinary metric.
function Shapes.Rect(t)
    check("Shapes.Rect", t, accepted("size", "rounding", "metric"))
    local size   = require_table("Shapes.Rect", t, "size", "a pair { width, depth }")
    local metric = t.metric or "euclidean"
    if metric == "chebyshev" then
        if t.rounding ~= nil then
            fail("Shapes.Rect does not take 'rounding' with metric = 'chebyshev'")
        end
        return Geometry.BoxChebyshev{ position = place(t), size = size }
    elseif metric ~= "euclidean" then
        fail("Shapes.Rect expects metric to be 'euclidean' or 'chebyshev', got '" .. tostring(metric) .. "'")
    end
    return Geometry.Box{ position = place(t), size = size, rounding = t.rounding }
end

local function points_of(name, t)
    local points = require_table(name, t, "points", "a list of { x, z } points")
    if #points == 0 then
        fail(name .. " needs 'points' as a list of { x, z } points, got an empty list")
    end
    return points
end

function Shapes.Polygon(t)
    check("Shapes.Polygon", t, accepted("points"))
    return Geometry.Segments{ position = place(t), points = points_of("Shapes.Polygon", t), closed = true }
end

-- Unsigned distance to a chain of segments. `closed` joins the last point back to the first.
function Shapes.Polyline(t)
    check("Shapes.Polyline", t, accepted("points", "closed"))
    local points = points_of("Shapes.Polyline", t)
    if t.closed then
        local joined = {}
        for i, point in ipairs(points) do
            joined[i] = point
        end
        joined[#joined + 1] = points[1]
        points = joined
    end
    return Geometry.Segments{ position = place(t), points = points, closed = false }
end

-- --- Splines --------------------------------------------------------------------------------------

local function length(a, b)
    local dx, dz = b[1] - a[1], b[2] - a[2]
    return math.sqrt(dx * dx + dz * dz)
end

-- Centripetal Catmull-Rom through `p1` and `p2`, `u` in [0, 1]. Centripetal because it does not loop or
-- overshoot when the control points are unevenly spaced, which hand-placed points always are. Barry and
-- Goldman's pyramid form.
local function catmull_rom(p0, p1, p2, p3, u)
    local t0 = 0.0
    local t1 = t0 + math.max(math.sqrt(length(p0, p1)), 1e-4)
    local t2 = t1 + math.max(math.sqrt(length(p1, p2)), 1e-4)
    local t3 = t2 + math.max(math.sqrt(length(p2, p3)), 1e-4)
    local t  = t1 + (t2 - t1) * u

    local function lerp(a, b, ta, tb)
        local w = (t - ta) / (tb - ta)
        return { a[1] * (1 - w) + b[1] * w, a[2] * (1 - w) + b[2] * w }
    end

    local a1 = lerp(p0, p1, t0, t1)
    local a2 = lerp(p1, p2, t1, t2)
    local a3 = lerp(p2, p3, t2, t3)
    local b1 = lerp(a1, a2, t0, t2)
    local b2 = lerp(a2, a3, t1, t3)
    return lerp(b1, b2, t1, t2)
end

-- The points of a smooth curve through `points`, `subdivisions` segments per span. The ends of an open
-- curve are repeated so it starts and stops on its first and last point.
local function sample_spline(points, subdivisions, closed)
    local count = #points
    local function at(i)
        if closed then
            return points[((i - 1) % count) + 1]
        end
        return points[math.min(math.max(i, 1), count)]
    end

    local out   = {}
    local spans = closed and count or count - 1
    for span = 1, spans do
        local p0, p1, p2, p3 = at(span - 1), at(span), at(span + 1), at(span + 2)
        for step = 0, subdivisions - 1 do
            out[#out + 1] = catmull_rom(p0, p1, p2, p3, step / subdivisions)
        end
    end
    if not closed then
        out[#out + 1] = points[count]
    end
    return out
end

-- A smooth curve through `points`. Unsigned distance to the curve by default; `fill = true` on a closed
-- spline gives the signed distance to the region it encloses.
function Shapes.Spline(t)
    check("Shapes.Spline", t, accepted("points", "subdivisions", "closed", "fill"))
    local points = points_of("Shapes.Spline", t)
    if #points < 2 then
        fail("Shapes.Spline needs at least 2 points, got " .. #points)
    end
    local subdivisions = t.subdivisions or 8
    if type(subdivisions) ~= "number" or subdivisions < 1 or subdivisions ~= math.floor(subdivisions) then
        fail("Shapes.Spline expects subdivisions as a whole number of at least 1")
    end
    if t.fill and not t.closed then
        fail("Shapes.Spline can only fill a closed spline; set closed = true")
    end

    local spans    = t.closed and #points or #points - 1
    local segments = spans * subdivisions
    if segments > 128 then
        fail("Shapes.Spline would make " .. segments .. " segments and the limit is 128; give it fewer "
             .. "points or lower 'subdivisions'")
    end

    local curve = sample_spline(points, subdivisions, t.closed)
    if t.fill then
        return Geometry.Segments{ position = place(t), points = curve, closed = true }
    end
    if t.closed then
        curve[#curve + 1] = curve[1]
    end
    return Geometry.Segments{ position = place(t), points = curve, closed = false }
end

-- --- Combining ------------------------------------------------------------------------------------

local function operands(name, t)
    local list = {}
    for i = 1, #t do
        if type(t[i]) ~= "userdata" then
            fail(name .. " expects shapes in a list, got " .. type(t[i]) .. " as operand " .. i)
        end
        list[i] = t[i]
    end
    if #list < 2 then
        fail(name .. " needs at least two shapes in a list, got " .. #list)
    end
    return list
end

local function fold(name, t, plain, rounded)
    check(name, t, { "smooth" })
    local list   = operands(name, t)
    local result = list[1]
    for i = 2, #list do
        if t.smooth then
            result = rounded{ a = result, b = list[i], k = t.smooth }
        else
            result = plain{ a = result, b = list[i] }
        end
    end
    return result
end

-- Where either shape is. With `smooth` the seam between them is rounded over that many metres.
function Shapes.Union(t)
    return fold("Shapes.Union", t, Combine.Min, Combine.SmoothMin)
end

-- Where all of them are.
function Shapes.Intersect(t)
    return fold("Shapes.Intersect", t, Combine.Max, Combine.SmoothMax)
end

-- `a` with `b` cut out of it.
function Shapes.Subtract(t)
    check("Shapes.Subtract", t, { "a", "b", "smooth" })
    if type(t.a) ~= "userdata" or type(t.b) ~= "userdata" then
        fail("Shapes.Subtract needs 'a' and 'b' as shapes")
    end
    if t.smooth then
        return Combine.SmoothMax{ a = t.a, b = -t.b, k = t.smooth }
    end
    return Combine.Max{ a = t.a, b = -t.b }
end

-- 1 inside the shape and 0 outside it, with a ramp `feather` metres each side of the edge. A feather
-- of zero is a hard edge.
function Shapes.Mask(t)
    check("Shapes.Mask", t, { "shape", "feather" })
    if type(t.shape) ~= "userdata" then
        fail("Shapes.Mask needs 'shape' as a shape")
    end
    local feather = t.feather or 0
    if type(feather) ~= "number" or feather < 0 then
        fail("Shapes.Mask expects feather as a number of metres, zero or more")
    end
    if feather == 0 then
        -- A clamp of a steep ramp: exact to a ten-thousandth of a metre, and a mask needs no more.
        return Curves.Clamp{ input = t.shape * -10000.0, min = 0.0, max = 1.0 }
    end
    return 1.0 - Curves.Smoothstep{ input = t.shape, min = -feather, max = feather }
end

-- --- Warping --------------------------------------------------------------------------------------

-- A position field bent by noise, to pass as `position = ` to any shape or landform. Without it a shape
-- is exactly the circle or box it says it is, which on a terrain reads as a drawing.
function Shapes.Warp(t)
    check("Shapes.Warp", t, { "amplitude", "frequency", "octaves", "seed" })
    local amplitude = t.amplitude or 60.0
    local frequency = t.frequency or 0.004
    local octaves   = t.octaves or 3
    local seed      = t.seed or 0

    local world = Terrain.Coords{}
    local across = Noises.fBm{ frequency = frequency, octaves = octaves, amplitude = 1.0, seed = seed }
    local along  = Noises.fBm{ frequency = frequency, octaves = octaves, amplitude = 1.0, seed = seed + 7919 }
    return Vec.Combine{
        world.x + across.value * amplitude,
        world.z + along.value * amplitude,
    }
end

-- --- Sets: many shapes in one node ----------------------------------------------------------------

-- A list of shapes combined in order in one `Geometry.Lands` node, however long the list: land shapes
-- join what came before them (with a smooth minimum `smooth` metres wide), water shapes cut it. The
-- block's layout is in lands.comp.glsl; this writes it and the bins the shader finds elements through.

local SET_ELEMENT = 48
local SET_KINDS = {
    circle = 0, ellipse = 1, box = 2, superellipse = 3, radial = 4, capsule = 5, ring = 6, arc = 7,
    sector = 8, polygon = 9, band = 10, fill = 11,
}
local SET_KEYS = {
    "shape", "id", "water", "smooth", "at", "rotation", "scale", "radius", "radii", "size", "rounding",
    "exponent", "harmonics", "from", "to", "length", "width", "aperture", "angle", "reach", "points",
    "profile",
}

local function number_of(where, e, key, default)
    local v = e[key]
    if v == nil then v = default end
    if type(v) ~= "number" then
        fail(where .. " needs '" .. key .. "' as a number of metres")
    end
    return v
end

local function pair_of(where, e, key)
    local v = e[key]
    if type(v) ~= "table" or type(v[1]) ~= "number" or type(v[2]) ~= "number" then
        fail(where .. " needs '" .. key .. "' as a pair { x, z } of metres")
    end
    return v
end

local PROFILE_KINDS = { dome = 1, cone = 2, plateau = 3, rim = 4, tilt = 5 }
local PROFILE_KEYS = { "kind", "height", "width", "edge", "at", "sigma", "inner", "start", "gradient", "direction" }

-- A profile's numbers (element values 25..36): what the relief mode raises over the distance inland.
local function encode_profile(where, p, v)
    if type(p) ~= "table" then
        fail(where .. " expects 'profile' as a table { kind = ..., ... }")
    end
    check(where .. " profile", p, PROFILE_KEYS)
    local kind = PROFILE_KINDS[p.kind]
    if kind == nil then
        fail(where .. " has profile kind '" .. tostring(p.kind) .. "'; expected dome, cone, plateau, rim or tilt")
    end
    local function positive(key, default)
        local value = number_of(where .. " profile", p, key, default)
        if value <= 0 then
            fail(where .. " profile needs '" .. key .. "' above zero")
        end
        return value
    end
    v[25], v[26] = kind, number_of(where .. " profile", p, "height")
    if p.kind == "dome" or p.kind == "cone" then
        v[27] = positive("width")
    elseif p.kind == "plateau" then
        v[27] = positive("edge")
    elseif p.kind == "rim" then
        v[27], v[28] = positive("at"), positive("sigma")
        v[29], v[30] = number_of(where .. " profile", p, "inner", 0.0), positive("width")
        v[31] = number_of(where .. " profile", p, "start", 0.0)
    else
        local direction = math.rad(number_of(where .. " profile", p, "direction", 0.0))
        v[27], v[28] = positive("edge"), number_of(where .. " profile", p, "gradient")
        v[29], v[30] = math.cos(direction), math.sin(direction)
    end
end

-- One element: its 48 numbers, its points (x, z, width each, in its own space) and its bounding radius
-- in its own space, before `scale`; `nil` radius for a shape that covers everything.
local function encode_element(i, e, points)
    local where = "Shapes.Set element " .. i
    check(where, e, SET_KEYS)
    local kind = SET_KINDS[e.shape]
    if kind == nil then
        fail(where .. " has shape '" .. tostring(e.shape) .. "'; expected circle, ellipse, box, superellipse, "
             .. "radial, capsule, ring, arc, sector, polygon, band or fill")
    end
    local at       = e.at and pair_of(where, e, "at") or { 0.0, 0.0 }
    local rotation = number_of(where, e, "rotation", 0.0)
    local scale    = number_of(where, e, "scale", 1.0)
    if scale <= 0 then
        fail(where .. " needs a scale above zero")
    end
    local v = {}
    for k = 1, SET_ELEMENT do v[k] = 0.0 end
    local shape, bound = {}, nil

    if e.shape == "circle" then
        shape[1] = number_of(where, e, "radius")
        bound = shape[1]
    elseif e.shape == "ellipse" or e.shape == "superellipse" then
        local r = pair_of(where, e, "radii")
        shape[1], shape[2] = r[1], r[2]
        if e.shape == "superellipse" then
            shape[3] = number_of(where, e, "exponent", 4.0)
            if shape[3] < 1.0 then
                fail(where .. " needs an exponent of at least 1")
            end
        end
        bound = math.sqrt(r[1] * r[1] + r[2] * r[2])
    elseif e.shape == "box" then
        local s = pair_of(where, e, "size")
        shape[1], shape[2], shape[3] = s[1] * 0.5, s[2] * 0.5, number_of(where, e, "rounding", 0.0)
        bound = math.sqrt(shape[1] * shape[1] + shape[2] * shape[2])
    elseif e.shape == "radial" then
        local r = number_of(where, e, "radius")
        local harmonics = e.harmonics or {}
        if type(harmonics) ~= "table" or #harmonics > 3 then
            fail(where .. " expects harmonics as at most 3 { k, amplitude, phase } triples")
        end
        shape[1], shape[2] = r, #harmonics
        local spread = 0.0
        for h, triple in ipairs(harmonics) do
            if type(triple) ~= "table" or type(triple[1]) ~= "number" or type(triple[2]) ~= "number" then
                fail(where .. " expects harmonic " .. h .. " as { k, amplitude, phase }")
            end
            shape[3 * h], shape[3 * h + 1], shape[3 * h + 2] = triple[1], triple[2], triple[3] or 0.0
            spread = spread + math.abs(triple[2])
        end
        if spread >= 1.0 then
            fail(where .. " needs the harmonic amplitudes to add up to less than 1, or the radius turns negative")
        end
        bound = r * (1.0 + spread)
    elseif e.shape == "capsule" then
        local width = number_of(where, e, "width")
        if e.from ~= nil or e.to ~= nil then
            local a, b = pair_of(where, e, "from"), pair_of(where, e, "to")
            at = { (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5 }
            rotation = math.deg(math.atan(b[2] - a[2], b[1] - a[1]))
            shape[1] = 0.5 * math.sqrt((b[1] - a[1]) ^ 2 + (b[2] - a[2]) ^ 2)
        else
            shape[1] = 0.5 * number_of(where, e, "length")
        end
        shape[2] = 0.5 * width
        bound = shape[1] + shape[2]
    elseif e.shape == "ring" or e.shape == "arc" then
        local r, width = number_of(where, e, "radius"), number_of(where, e, "width")
        if e.shape == "ring" then
            shape[1], shape[2] = r, width
        else
            shape[1], shape[2], shape[3] = r, math.rad(number_of(where, e, "aperture") * 0.5), width
        end
        bound = r + width * 0.5
    elseif e.shape == "sector" then
        shape[1], shape[2] = number_of(where, e, "reach"), math.rad(number_of(where, e, "angle") * 0.5)
        bound = shape[1]
    elseif e.shape == "polygon" or e.shape == "band" then
        local list = e.points
        local least = e.shape == "polygon" and 3 or 2
        if type(list) ~= "table" or #list < least then
            fail(where .. " needs 'points' as a list of at least " .. least .. " { x, z } points")
        end
        local default_width = e.shape == "band" and number_of(where, e, "width", 0.0) or 0.0
        v[23] = #points / 3
        v[24] = #list
        bound = 0.0
        for k, p in ipairs(list) do
            if type(p) ~= "table" or type(p[1]) ~= "number" or type(p[2]) ~= "number" then
                fail(where .. " needs point " .. k .. " as { x, z } or { x, z, width }")
            end
            local w = p[3] or default_width
            if e.shape == "band" and not (w > 0) then
                fail(where .. " needs a width above zero at point " .. k .. ", as p[3] or 'width'")
            end
            points[#points + 1], points[#points + 2], points[#points + 3] = p[1], p[2], w
            bound = math.max(bound, math.sqrt(p[1] * p[1] + p[2] * p[2]) + w * 0.5)
        end
    end

    local radians = math.rad(rotation)
    v[1], v[2], v[3] = kind, e.water and 1.0 or 0.0, number_of(where, e, "id", i)
    v[4], v[5], v[6], v[7], v[8] = at[1], at[2], math.cos(radians), math.sin(radians), scale
    for k = 1, 12 do v[8 + k] = shape[k] or 0.0 end
    v[21] = number_of(where, e, "smooth", 0.0)
    v[22] = bound and bound * scale or 0.0
    if e.profile ~= nil then
        encode_profile(where, e.profile, v)
    end
    return v, bound and { x = at[1], z = at[2], r = bound * scale + v[21] } or nil
end

-- The block of `elements` (see lands.comp.glsl): `reach` is the distance past which nothing is measured,
-- `bin` the side of the bins (chosen from the elements when not given), `blend` how far apart in metres
-- two elements' distances are for their profiles to mix (the relief mode).
function Shapes.SetBlock(t)
    check("Shapes.SetBlock", t, { "elements", "reach", "bin", "blend" })
    local list = t.elements
    if type(list) ~= "table" then
        fail("Shapes.SetBlock needs 'elements' as a list of shapes")
    end
    local reach = t.reach or 2000.0
    if type(reach) ~= "number" or reach <= 0 then
        fail("Shapes.SetBlock needs reach as a number of metres above zero")
    end
    local numbers, points, circles, global = {}, {}, {}, {}
    for i, e in ipairs(list) do
        if type(e) ~= "table" then
            fail("Shapes.SetBlock expects element " .. i .. " as a table")
        end
        local v, circle = encode_element(i, e, points)
        for k = 1, SET_ELEMENT do numbers[#numbers + 1] = v[k] end
        if circle then
            circle.r = circle.r + reach
            circles[i] = circle
        else
            global[#global + 1] = i - 1
        end
    end

    -- Bins over the elements' reach, when there are enough elements for them to pay: each lists, in list
    -- order, the elements whose reach touches it, and every global element.
    local x0, z0, x1, z1 = math.huge, math.huge, -math.huge, -math.huge
    local radii = {}
    for _, c in pairs(circles) do
        x0, z0 = math.min(x0, c.x - c.r), math.min(z0, c.z - c.r)
        x1, z1 = math.max(x1, c.x + c.r), math.max(z1, c.z + c.r)
        radii[#radii + 1] = c.r
    end
    local bin, nx, nz = 1.0, 0, 0
    if #list > 16 and #radii > 0 then
        table.sort(radii)
        local extent = math.max(x1 - x0, z1 - z0)
        bin = t.bin or math.max(radii[(#radii + 1) // 2], extent / 128.0)
        nx, nz = math.ceil((x1 - x0) / bin), math.ceil((z1 - z0) / bin)
    end
    local lists = {}
    for b = 1, nx * nz do lists[b] = {} end
    for i = 1, #list do
        local c = circles[i]
        if c == nil then
            for b = 1, nx * nz do table.insert(lists[b], i - 1) end
        elseif nx > 0 then
            local gx0, gx1 = math.floor((c.x - c.r - x0) / bin), math.floor((c.x + c.r - x0) / bin)
            local gz0, gz1 = math.floor((c.z - c.r - z0) / bin), math.floor((c.z + c.r - z0) / bin)
            for gz = math.max(gz0, 0), math.min(gz1, nz - 1) do
                for gx = math.max(gx0, 0), math.min(gx1, nx - 1) do
                    local bx0, bz0 = x0 + gx * bin, z0 + gz * bin
                    local dx = math.max(bx0 - c.x, 0.0, c.x - bx0 - bin)
                    local dz = math.max(bz0 - c.z, 0.0, c.z - bz0 - bin)
                    if dx * dx + dz * dz <= c.r * c.r then
                        table.insert(lists[gz * nx + gx + 1], i - 1)
                    end
                end
            end
        else
            global[#global + 1] = i - 1
        end
    end
    table.sort(global)

    local header = {}
    for k = 1, 24 do header[k] = 0.0 end
    local elements_at = 24
    local offsets_at  = elements_at + #numbers
    local lists_at    = offsets_at + nx * nz + 1
    local flat, offsets, running = {}, {}, 0
    for b = 1, nx * nz do
        offsets[b] = running
        for _, e in ipairs(lists[b]) do flat[#flat + 1] = e end
        running = running + #lists[b]
    end
    offsets[nx * nz + 1] = running
    local global_at = lists_at + #flat
    local points_at = global_at + #global
    header[1], header[2], header[3], header[4] = #list, bin, nx > 0 and x0 or 0.0, nx > 0 and z0 or 0.0
    header[5], header[6], header[7] = nx, nz, reach
    header[8], header[9], header[10], header[11] = elements_at, offsets_at, lists_at, points_at
    header[12], header[13] = #global, global_at
    header[14] = t.blend or 100.0

    local values = header
    local function append(t_) for _, x in ipairs(t_) do values[#values + 1] = x end end
    append(numbers)
    append(offsets)
    append(flat)
    append(global)
    append(points)
    if #values == 24 then values[25] = 0.0 end
    return Geometry.Block{ values = values }
end

-- The shapes of `elements` in one node: `.value` is the signed distance (land negative) held to
-- [-reach, reach], `.id` the id of the land element nearest each sample (0 where there is none). With
-- `relief = true` the second value returned is the height of the elements' profiles over the land.
function Shapes.Set(t)
    check("Shapes.Set", t, { "elements", "reach", "bin", "blend", "position", "relief" })
    local block = Shapes.SetBlock{ elements = t.elements, reach = t.reach, bin = t.bin, blend = t.blend }
    local coast = Geometry.Lands{ block = block, mode = "coast", position = t.position }
    if t.relief then
        return coast, Geometry.Lands{ block = block, mode = "relief", position = t.position }
    end
    return coast
end

_G.Shapes = Shapes
