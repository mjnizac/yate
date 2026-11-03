-- Filters: the World Machine devices that are compositions of the engine's ops rather than ops of their
-- own. Each says which device it stands for and how it is built, so a script can do the same by hand.
--
--   Filters.Open, Filters.Close     the Expander's Open (Shrink-Expand) and Close (Expand-Shrink)
--   Filters.FrequencySplit          the forum's Frequency Splitter: low and high bands of a field
--   Filters.DirectionalBlur         the Blur device's Directional Blur, wind streaks
--   Masks.Convexity                 Select Convexity with a radius
--   Masks.Aspect                    Select Aspect / the Angle Selector: facing a direction, and the angle
--   Curves.Table                    the Curves device, Bias/Gain and Levels: any curve through points
--   Curves.Ramp                     the Ramp device: a height wrapped into a sawtooth or a triangle wave
--   Curves.Equalize                 the Equalizer, with a captured sample over a fixed region
--   Strata.Apply                    the Strata device: tilted beds incised into the slopes
--   Erosion.Accumulate              the Snow device driven by a direction: material built up downwind
--
-- Run by the engine after `10_shapes.lua`. Errors are raised with `fail`, which reports the line of the
-- script that called in here.

local function check(name, t, allowed)
    if type(t) ~= "table" then
        fail(name .. " expects a table of named arguments, as in " .. name .. "{ ... }")
    end
    local known = {}
    for _, key in ipairs(allowed) do
        known[key] = true
    end
    for key in pairs(t) do
        if type(key) == "string" and not known[key] then
            fail(name .. " does not take '" .. key .. "'; it accepts " .. table.concat(allowed, ", "))
        end
    end
    if t.input == nil and known.input then
        fail(name .. " needs 'input'")
    end
end

local function abs(v)
    return Combine.Max{ a = v, b = -v }
end

-- --- Morphology -------------------------------------------------------------------------------------

-- Shrink then Expand: removes peaks and ridges narrower than the window, keeps the rest as it was.
function Filters.Open(t)
    check("Filters.Open", t, { "input", "radius", "shape", "falloff", "slope" })
    local o = { radius = t.radius, shape = t.shape, falloff = t.falloff, slope = t.slope }
    o.input = t.input
    o.input = Filters.Shrink(o)
    return Filters.Expand(o)
end

-- Expand then Shrink: fills pits and valleys narrower than the window.
function Filters.Close(t)
    check("Filters.Close", t, { "input", "radius", "shape", "falloff", "slope" })
    local o = { radius = t.radius, shape = t.shape, falloff = t.falloff, slope = t.slope }
    o.input = t.input
    o.input = Filters.Expand(o)
    return Filters.Shrink(o)
end

-- The field as its low frequencies (a blur) and what is left (the detail). `low + high` is the input.
function Filters.FrequencySplit(t)
    check("Filters.FrequencySplit", t, { "input", "radius" })
    local low = Filters.Blur{ input = t.input, radius = t.radius or 8 }
    return low, t.input - low
end

-- A mean along one direction: `taps` reads spread over `length` metres through the sample, heading
-- `direction` degrees (0 along +x, counter-clockwise towards +z). `radius` is the longest read in
-- samples, at least `length / 2` over the spacing.
function Filters.DirectionalBlur(t)
    check("Filters.DirectionalBlur", t, { "input", "direction", "length", "taps", "radius" })
    local angle  = math.rad(t.direction or 0)
    local length = t.length or 100
    local taps   = math.max(2, math.floor(t.taps or 8))
    local radius = t.radius or 16
    local zero   = t.input * 0
    local sum
    for i = 0, taps - 1 do
        local d    = length * (i / (taps - 1) - 0.5)
        local read = Filters.Displace{
            input = t.input, radius = radius,
            by = Vec.Combine{ zero + d * math.cos(angle), zero + d * math.sin(angle) },
        }
        sum = sum and sum + read or read
    end
    return sum * (1 / taps)
end

-- --- Masks --------------------------------------------------------------------------------------

-- How far the field stands above (positive) or below (negative) its mean over `radius` samples: ridges
-- and crests are convex, valley floors concave. Select Convexity with the radius the forum asks for.
function Masks.Convexity(t)
    check("Masks.Convexity", t, { "input", "radius" })
    return t.input - Filters.Blur{ input = t.input, radius = t.radius or 4 }
end

-- Of a height: `facing`, in [0, 1], 1 where the ground faces `direction` degrees (downhill towards it)
-- and 0 where it faces away, scaled by how steep it is up to `full` (rise over run); and `angle`, the
-- downhill bearing in radians, (-pi, pi], 0 towards +x. Of a cone the angle is the bearing round its
-- centre, the Angle Selector's radial output.
function Masks.Aspect(t)
    check("Masks.Aspect", t, { "input", "direction", "full" })
    local g     = Terrain.Gradient{ input = t.input }
    local a     = math.rad(t.direction or 0)
    local along = -(g.x * math.cos(a) + g.y * math.sin(a)) * (1 / (t.full or 0.5))
    local facing = Curves.Clamp{ input = along * 0.5 + 0.5, min = 0, max = 1 }
    return facing, Combine.Atan2{ y = -g.y, x = -g.x }
end

-- --- Curves ---------------------------------------------------------------------------------------

-- Monotone cubic (Fritsch-Carlson) slopes for points sorted by x, so a rising curve never dips.
local function slopes(xs, ys)
    local n, d, m = #xs, {}, {}
    for i = 1, n - 1 do
        d[i] = (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i])
    end
    m[1], m[n] = d[1], d[n - 1]
    for i = 2, n - 1 do
        m[i] = d[i - 1] * d[i] <= 0 and 0 or (d[i - 1] + d[i]) * 0.5
    end
    for i = 1, n - 1 do
        if d[i] == 0 then
            m[i], m[i + 1] = 0, 0
        else
            local a, b = m[i] / d[i], m[i + 1] / d[i]
            local s = a * a + b * b
            if s > 9 then
                local k = 3 / math.sqrt(s)
                m[i], m[i + 1] = k * a * d[i], k * b * d[i]
            end
        end
    end
    return m
end

local TABLE_SIZE = 256

-- The value of `input` read off a curve through `points` ({ {x, y}, ... }, x strictly rising): smooth
-- (monotone cubic) or straight between them, held flat beyond the first and the last. On the GPU it is a
-- dense lookup table, `Terrain.Grid` along x read at (input, 0).
function Curves.Table(t)
    check("Curves.Table", t, { "input", "points", "smooth" })
    local points = t.points
    if type(points) ~= "table" or #points < 2 then
        fail("Curves.Table needs 'points', at least two { x, y } pairs")
    end
    local xs, ys = {}, {}
    for i, p in ipairs(points) do
        xs[i], ys[i] = p[1], p[2]
        if i > 1 and not (xs[i] > xs[i - 1]) then
            fail("Curves.Table needs the points' x strictly rising, point " .. i .. " is not")
        end
    end
    local smooth = t.smooth ~= false
    local m      = smooth and slopes(xs, ys) or nil
    local x0, x1 = xs[1], xs[#xs]
    local cell   = (x1 - x0) / (TABLE_SIZE - 1)
    local values = {}
    local k = 1
    for i = 0, TABLE_SIZE - 1 do
        local x = x0 + cell * i
        while k < #xs - 1 and x > xs[k + 1] do
            k = k + 1
        end
        local h = xs[k + 1] - xs[k]
        local s = math.min(math.max((x - xs[k]) / h, 0), 1)
        local y
        if smooth then
            local s2, s3 = s * s, s * s * s
            y = (2 * s3 - 3 * s2 + 1) * ys[k] + (s3 - 2 * s2 + s) * h * m[k]
                + (-2 * s3 + 3 * s2) * ys[k + 1] + (s3 - s2) * h * m[k + 1]
        else
            y = ys[k] + (ys[k + 1] - ys[k]) * s
        end
        values[i + 1] = y
        values[i + 1 + TABLE_SIZE] = y
    end
    return Terrain.Grid{
        values = values, size = { TABLE_SIZE, 2 }, origin = { x0, 0 }, cell = cell,
        position = Vec.Combine{ t.input, t.input * 0 },
    }
end

-- The input wrapped every `period`: `"repeat"` is a sawtooth rising from 0 to `period`, `"mirror"` a
-- triangle wave between 0 and `period`. The sawtooth is `x - Terrace(x)`, exact at any spacing. On a
-- blurred height it lays ridges along the contours: the forum's folded ranges.
function Curves.Ramp(t)
    check("Curves.Ramp", t, { "input", "period", "mode", "offset" })
    local period = t.period or 100
    local x      = t.input + (t.offset or 0)
    local saw    = (x - Curves.Terrace{ input = x, step = period, sharpness = 1.0 }) * (1 / period) + 0.5
    if (t.mode or "repeat") == "mirror" then
        return (abs(saw * 2 - 1) * -1 + 1) * period
    elseif t.mode ~= nil and t.mode ~= "repeat" then
        fail("Curves.Ramp expects mode repeat or mirror, got '" .. tostring(t.mode) .. "'")
    end
    return saw * period
end

-- Spreads the values of `input` evenly over its own range, measured once on a grid over `region`
-- ({ x0, z0, x1, z1 }, every `cell` metres): World Machine's Equalizer with a captured sample, which is
-- what keeps it the same in every section. `amount` blends from the input (0) to fully equal (1). The
-- sample runs on the CPU, where neighbourhood and erosion ops read their input unchanged, so measure
-- the field before them.
function Curves.Equalize(t)
    check("Curves.Equalize", t, { "input", "region", "cell", "amount", "seed" })
    local region = t.region or fail("Curves.Equalize needs 'region', { x0, z0, x1, z1 }")
    local cell   = t.cell or 64
    local n      = math.floor((region[3] - region[1]) / cell) + 1
    local m      = math.floor((region[4] - region[2]) / cell) + 1
    local h = Terrain.Sample{ value = t.input, origin = { region[1], region[2] }, size = { n, m },
                              cell = cell, seed = t.seed or 0 }
    local sorted = {}
    for i = 1, #h do
        sorted[i] = h[i]
    end
    table.sort(sorted)
    local lo, hi = sorted[1], sorted[#sorted]
    if not (hi > lo) then
        return t.input
    end
    local amount = t.amount or 1
    local points, last = {}, nil
    for q = 0, 64 do
        local x = sorted[math.min(#sorted, math.floor(q / 64 * (#sorted - 1)) + 1)]
        if last == nil or x > last then
            local y = lo + (hi - lo) * q / 64
            points[#points + 1] = { x, x + (y - x) * amount }
            last = x
        end
    end
    return Curves.Table{ input = t.input, points = points, smooth = false }
end

-- --- Strata ---------------------------------------------------------------------------------------

local Strata = {}

-- Beds `step` metres thick, tilted `tilt` degrees with the dip towards `heading` degrees (0 is +x),
-- incised into the slopes: the terrain terraced along the tilted plane, blended back by `strength`.
-- `bedding` adds any field to the plane (a fold, a noise for beds that wander), which is how tilt and
-- heading vary across the map. `layers` adds finer sets at a third of the step and half the strength,
-- as World Machine's Layers does. Beds show only on slopes, as the device's do: the weight rises from
-- 0 at slope `flat` to 1 at `steep` (rise over run), measured on the input blurred over `smoothness`
-- samples (its Smoothness). Returns the height, the exposure (how much each sample was moved,
-- the device's Strata Exposure) and the bed index (its Strata Layers).
function Strata.Apply(t)
    check("Strata.Apply", t, { "input", "step", "tilt", "heading", "strength", "sharpness", "layers",
                               "bedding", "mask", "smoothness", "flat", "steep" })
    local c       = Terrain.Coords{}
    local heading = math.rad(t.heading or 0)
    local plane   = (c.x * math.cos(heading) + c.z * math.sin(heading)) * math.tan(math.rad(t.tilt or 0))
    if t.bedding ~= nil then
        plane = plane + t.bedding
    end
    local step, strength = t.step or 30, t.strength or 1
    local smoothed = Filters.Blur{ input = t.input, radius = t.smoothness or 4 }
    local exposed  = Masks.Slope{ input = Terrain.Gradient{ input = smoothed }, min = t.flat or 0.1, max = t.steep or 0.6 }
    local mask     = t.mask ~= nil and t.mask * exposed or exposed
    local result = t.input
    for _ = 1, math.max(1, math.floor(t.layers or 1)) do
        local bedded = Curves.Terrace{ input = result + plane, step = step, sharpness = t.sharpness or 0.6 }
                       - plane
        local weight = mask * strength
        result = result + (bedded - result) * weight
        step, strength = step / 3, strength * 0.5
    end
    local index = Curves.Terrace{ input = t.input + plane, step = t.step or 30, sharpness = 1.0 }
                  * (1 / (t.step or 30))
    return result, abs(result - t.input), index
end

-- --- Accumulation ---------------------------------------------------------------------------------

-- Material built up on the side of every rise that faces away from `direction` degrees (the way the
-- wind blows): the terrain is tilted down towards the wind by `lift` (rise over run), filled at the
-- talus angle by a deposit-only thermal pass, and tilted back. World Machine's Snow device on a
-- gradient with an angle selector, which the forum uses for asymmetric dunes and silt-mantled flanks.
function Erosion.Accumulate(t)
    check("Erosion.Accumulate", t, { "input", "direction", "lift", "talus", "iterations", "strength" })
    local c     = Terrain.Coords{}
    local a     = math.rad(t.direction or 0)
    local plane = (c.x * math.cos(a) + c.z * math.sin(a)) * (t.lift or 0.2)
    local built = Erosion.Thermal{
        input = t.input + plane, iterations = t.iterations or 32, talus = t.talus or 0.6,
        strength = t.strength or 0.4, mass_balance = 1.0,
    }
    return built - plane
end

_G.Strata = Strata
