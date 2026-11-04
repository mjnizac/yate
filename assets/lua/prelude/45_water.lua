-- Water: rivers and lakes from a drainage grid, and the surfaces the viewer draws them with.
--
--   local drained = Water.Drain{ values = heights, size = { n, n }, origin = { x0, z0 }, cell = 50 }
--   local water   = Water.Apply{ height = terrain, water = drained }
--   return { height = { value = water.height }, lakes = { value = water.lakes }, rivers = { value = water.rivers } }
--
-- `Water.Drain` runs `Hydrology.Drain` on a grid the script built (the same lists `Erosion.StreamPower`
-- takes and returns) and hands back its fields as `Terrain.Grid` values: the drained ground, the lake
-- surface, and the distance, width, depth and level of the nearest river. `Water.Apply` carves the rivers
-- into a terrain at full resolution, a parabolic bed under the river's level and banks rising at `bank`
-- until they meet the ground, and returns the water surfaces: `lakes` and `rivers` are heights, above
-- the ground where there is water and below it elsewhere, which is how the viewer and an export read
-- them. Outputs named `lakes` and `rivers` are drawn by the viewer's water.
--
-- Run by the engine after `40_dunes.lua`. Errors are raised with `fail`, which reports the line of the
-- script that called in here.

local engine = ...
local fail   = engine.fail

local Hydrology, Terrain, Curves, Combine = Hydrology, Terrain, Curves, Combine
local type, pairs, ipairs, table = type, pairs, ipairs, table

local Water = {}

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

local DRAIN_KEYS = {
    "values", "size", "origin", "cell", "position", "sea_level", "breach", "lake_area", "lake_depth",
    "channel_area", "width_scale", "depth_scale", "fill_slope",
}

function Water.Drain(t)
    check("Water.Drain", t, DRAIN_KEYS)
    if type(t.values) ~= "table" or type(t.size) ~= "table" or type(t.origin) ~= "table" then
        fail("Water.Drain needs 'values' (a list of heights, x fastest), 'size' = { w, d } and 'origin' = { x, z }")
    end
    local cell = t.cell or 50.0
    local d = Hydrology.Drain{
        size = t.size, cell = cell, height = t.values, sea_level = t.sea_level, breach = t.breach,
        lake_area = t.lake_area, lake_depth = t.lake_depth, channel_area = t.channel_area,
        width_scale = t.width_scale, depth_scale = t.depth_scale, fill_slope = t.fill_slope,
    }
    local function grid(values)
        return Terrain.Grid{ values = values, size = t.size, origin = t.origin, cell = cell, position = t.position }
    end
    return {
        height   = grid(d.height),
        lake     = grid(d.lake),
        distance = grid(d.river_distance),
        width    = grid(d.river_width),
        depth    = grid(d.river_depth),
        level    = grid(d.river_level),
        values   = d.height,
        area     = d.area,
        lakes    = d.lakes,
        rivers   = d.rivers,
    }
end

-- World Machine's Select Wetness: the topographic wetness index ln(a / tan b) of a grid the script built
-- (Water.Drain's `values`, `size`, `origin`, `cell`), where a is the area drained through each cell per
-- metre of contour and b its slope on the drained surface, mapped to [0, 1]: `wetness` in [0, 1] is how
-- much water there is, so more of the valleys and then the hillslopes saturate. A Terrain.Grid.
function Masks.Wetness(t)
    check("Masks.Wetness", t, { "values", "size", "origin", "cell", "position", "sea_level", "wetness" })
    if type(t.values) ~= "table" or type(t.size) ~= "table" or type(t.origin) ~= "table" then
        fail("Masks.Wetness needs 'values' (a list of heights, x fastest), 'size' = { w, d } and 'origin' = { x, z }")
    end
    local cell = t.cell or 50.0
    local d = Hydrology.Drain{ size = t.size, cell = cell, height = t.values, sea_level = t.sea_level, channel_area = 1e30 }
    local n, m, h = t.size[1], t.size[2], d.height
    local lo = 14 - 10 * (t.wetness or 0.5)
    local wet = {}
    for z = 1, m do
        for x = 1, n do
            local i  = (z - 1) * n + x
            local gx = (h[(z - 1) * n + math.min(x + 1, n)] - h[(z - 1) * n + math.max(x - 1, 1)]) / (2 * cell)
            local gz = (h[(math.min(z + 1, m) - 1) * n + x] - h[(math.max(z - 1, 1) - 1) * n + x]) / (2 * cell)
            local twi = math.log(d.area[i] / cell / math.max(math.sqrt(gx * gx + gz * gz), 1e-3))
            wet[i] = math.min(math.max((twi - lo) / 6, 0), 1)
        end
    end
    -- Clamped, because Catmull-Rom overshoots where the index saturates within a cell.
    return Curves.Clamp{ input = Terrain.Grid{ values = wet, size = t.size, origin = t.origin, cell = cell,
                                               position = t.position }, min = 0, max = 1 }
end

-- `channel = "trapezoid"` cuts a flat-bottomed bed whose sides slant by `aspect` (0 a box, 1 a triangle)
-- instead of the parabola, and `floodplain = { height, width }` lowers the ground beside each river to
-- `height` metres over its water out to `width` metres from its banks, the River device's options.
function Water.Apply(t)
    check("Water.Apply", t, { "height", "water", "bank", "rivers", "channel", "aspect", "floodplain" })
    local w = t.water
    if type(w) ~= "table" or w.distance == nil then
        fail("Water.Apply needs 'water', the result of Water.Drain")
    end
    if t.height == nil then
        fail("Water.Apply needs 'height', the terrain to carve the rivers into")
    end
    local bank = t.bank or 0.35
    local height = t.height
    local rivers = nil
    if t.rivers ~= false then
        -- Half the width, never zero, so the bed's shape stays defined where no river is near.
        local half = w.width * 0.5 + 1.0
        local u    = w.distance / half
        local shape = 1.0 - u * u
        if t.channel == "trapezoid" then
            shape = (1.0 - u) * (1 / math.max(t.aspect or 0.5, 1e-3))
        elseif t.channel ~= nil and t.channel ~= "curved" then
            fail("Water.Apply expects channel curved or trapezoid, got '" .. tostring(t.channel) .. "'")
        end
        local bed  = w.level - w.depth * Curves.Clamp{ input = shape, min = 0.0, max = 1.0 }
        local rise = Curves.Clamp{ input = w.distance - half, min = 0.0, max = 100000.0 } * bank
        local channel = bed + rise
        if t.floodplain ~= nil then
            local fp = t.floodplain
            local plain = w.level + (fp[1] or fp.height or 2.0)
                          + Curves.Clamp{ input = w.distance - half - (fp[2] or fp.width or 100.0), min = 0.0, max = 100000.0 } * bank
            channel = Combine.Min{ a = channel, b = plain }
        end
        height = Combine.Min{ a = height, b = channel }
        -- The river's level over its bed, falling 200 m away within a few metres of the banks, so the
        -- surface lies under the ground everywhere a river is not.
        rivers = w.level - Curves.Smoothstep{ input = w.distance - half, min = 0.0, max = 6.0 } * 200.0
    end
    return { height = height, lakes = w.lake, rivers = rivers }
end

_G.Water = Water
