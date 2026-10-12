-- Landforms: heights, in metres, built from a shape and a profile.
--
-- A landform is `profile(shape) * height`. The shape says where the edge is and the profile says how
-- the ground leaves it, which is why the two are separate and why every landform here takes `position`:
-- pass `Shapes.Warp{}` and the same cone is a hill instead of a drawing of one.
--
-- Heights add, so landforms combine with `+`, `Combine.Max` to merge relief, or
-- `Combine.SmoothMax` to round the seam. A negative `height` or a `depth` digs.
--
-- Run by the engine after `10_shapes.lua`. Errors are raised with `fail`, which reports the line of the
-- script that called in here.

local engine = ...
local fail   = engine.fail

local Curves  = Curves
local Shapes  = Shapes
local type, ipairs, pairs, tostring = type, ipairs, pairs, tostring
local math, table = math, table

local Landforms = {}

-- Every landform whose cross-section is one of these.
local PROFILES = { linear = true, smooth = true, dome = true, gaussian = true }

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

-- A height may be a number or a field, so a landform can be scaled by a mask or by noise.
local function require_height(name, t, key)
    local value = t[key]
    if type(value) ~= "number" and type(value) ~= "userdata" then
        fail(name .. " needs '" .. key .. "' as a number of metres or a field, got " .. type(value))
    end
    return value
end

local function require_pair(name, t, key)
    local value = t[key]
    if type(value) ~= "table" or type(value[1]) ~= "number" or type(value[2]) ~= "number" then
        fail(name .. " needs '" .. key .. "' as a pair { x, z } of metres")
    end
    return value
end

local function choose_profile(name, t, default)
    local kind = t.profile or default
    if not PROFILES[kind] then
        fail(name .. " does not know the profile '" .. tostring(kind) .. "'; expected linear, smooth, "
             .. "dome or gaussian")
    end
    return kind
end

-- The factor a profile gives over a signed distance, 1 `width` metres inside the edge.
local function rise(distance, kind, width)
    if kind == "gaussian" then
        return Curves.Profile{ input = distance, kind = "gaussian", sigma = width }
    end
    return Curves.Profile{ input = distance, kind = kind, width = width }
end

local PLACE = { "at", "rotation", "position" }

local function accepted(...)
    local list = { ... }
    for _, key in ipairs(PLACE) do
        list[#list + 1] = key
    end
    return list
end

-- --- Round landforms ------------------------------------------------------------------------------

-- A cone: `height` at the centre falling to zero at `radius`.
function Landforms.Cone(t)
    check("Landforms.Cone", t, accepted("radius", "height", "profile"))
    local radius = require_number("Landforms.Cone", t, "radius")
    local height = require_height("Landforms.Cone", t, "height")
    local kind   = choose_profile("Landforms.Cone", t, "linear")
    local shape  = Shapes.Circle{ at = t.at, radius = radius, position = t.position }
    return rise(shape, kind, radius) * height
end

-- A dome: steep at the foot and flat on top.
function Landforms.Dome(t)
    check("Landforms.Dome", t, accepted("radius", "height", "profile"))
    local radius = require_number("Landforms.Dome", t, "radius")
    local height = require_height("Landforms.Dome", t, "height")
    local kind   = choose_profile("Landforms.Dome", t, "dome")
    local shape  = Shapes.Circle{ at = t.at, radius = radius, position = t.position }
    return rise(shape, kind, radius) * height
end

-- A round depression `depth` metres deep.
function Landforms.Bowl(t)
    check("Landforms.Bowl", t, accepted("radius", "depth", "profile"))
    local radius = require_number("Landforms.Bowl", t, "radius")
    local depth  = require_height("Landforms.Bowl", t, "depth")
    local kind   = choose_profile("Landforms.Bowl", t, "smooth")
    local shape  = Shapes.Circle{ at = t.at, radius = radius, position = t.position }
    return rise(shape, kind, radius) * -depth
end

-- A round flat-topped rise with steep walls: the wall is `edge` metres either side of `radius`.
function Landforms.Cylinder(t)
    check("Landforms.Cylinder", t, accepted("radius", "height", "edge"))
    local radius = require_number("Landforms.Cylinder", t, "radius")
    local height = require_height("Landforms.Cylinder", t, "height")
    local edge   = t.edge or 2.0
    if type(edge) ~= "number" or edge <= 0 then
        fail("Landforms.Cylinder expects edge as a number of metres greater than zero")
    end
    local shape = Shapes.Circle{ at = t.at, radius = radius, position = t.position }
    return Curves.Profile{ input = shape, kind = "plateau", edge = edge, falloff = edge } * height
end

-- --- Angular landforms ----------------------------------------------------------------------------

-- A mesa or a platform: flat on top, with a wall that is `edge` metres inside the outline and
-- `falloff` metres beyond it.
function Landforms.Plateau(t)
    check("Landforms.Plateau", t, accepted("size", "height", "edge", "falloff", "rounding"))
    local size    = require_pair("Landforms.Plateau", t, "size")
    local height  = require_height("Landforms.Plateau", t, "height")
    local edge    = t.edge or 2.0
    local falloff = t.falloff or 20.0
    if type(edge) ~= "number" or type(falloff) ~= "number" or edge < 0 or falloff < 0
        or edge + falloff <= 0 then
        fail("Landforms.Plateau expects edge and falloff as metres, neither negative and not both zero")
    end
    local shape = Shapes.Rect{
        at = t.at, rotation = t.rotation, size = size, rounding = t.rounding, position = t.position,
    }
    return Curves.Profile{ input = shape, kind = "plateau", edge = edge, falloff = falloff } * height
end

-- A pyramid with square faces, `height` at the apex. For a rectangle the apex is a ridge.
function Landforms.Pyramid(t)
    check("Landforms.Pyramid", t, accepted("size", "height"))
    local size   = require_pair("Landforms.Pyramid", t, "size")
    local height = require_height("Landforms.Pyramid", t, "height")
    local shape  = Shapes.Rect{
        at = t.at, rotation = t.rotation, size = size, metric = "chebyshev", position = t.position,
    }
    return rise(shape, "linear", math.min(size[1], size[2]) * 0.5) * height
end

-- --- Elongated landforms --------------------------------------------------------------------------

local function stripe(name, t, signed_height)
    check(name, t, accepted("points", "width", signed_height.key, "profile", "subdivisions", "closed"))
    local width  = require_number(name, t, "width")
    local height = require_height(name, t, signed_height.key)
    local kind   = choose_profile(name, t, "smooth")
    if width <= 0 then
        fail(name .. " expects width as a number of metres greater than zero")
    end
    local half = width * 0.5
    local line = Shapes.Spline{
        points       = t.points,
        subdivisions = t.subdivisions,
        closed       = t.closed,
        at           = t.at,
        rotation     = t.rotation,
        position     = t.position,
    }
    -- The distance to the curve minus half the width is negative on the band, and the profile rises to
    -- 1 on its centre line.
    return rise(line - half, kind, half) * (signed_height.sign * height)
end

-- A raised band along a curve, `width` metres across.
function Landforms.Ridge(t)
    return stripe("Landforms.Ridge", t, { key = "height", sign = 1 })
end

-- A cut along a curve, `width` metres across and `depth` deep.
function Landforms.Trench(t)
    return stripe("Landforms.Trench", t, { key = "depth", sign = -1 })
end

_G.Landforms = Landforms
