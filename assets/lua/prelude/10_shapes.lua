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

_G.Shapes = Shapes
