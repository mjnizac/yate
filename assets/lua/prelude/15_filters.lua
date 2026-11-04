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
--   Erosion.Coastal                 Coastal Erosion: beaches, cliffs on a wave-cut platform, a smoothed sea floor
--   Erosion.Snow                    the Snow device: snowfall above a line, sliding off steep ground
--   Erosion.Weathering              Thermal Weathering's Fracture Size and Talus Size
--   Curves.SoftClamp                the Clamp device's Soft Clipping and Normalize
--   Masks.Curvature                 Select Convexity's profile, plan, tangent and total curvature
--   Combine.HeightMatch             the Chooser's Height Matching
--   Terrain.Mimic                   the Mimic device: elevation and roughness matched to a reference
--   Shapes.Scatter                  Instance Scattering on a jittered grid
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

-- --- Coast, snow and weathering ---------------------------------------------------------------------

local function field(v, like)
    return type(v) == "number" and like * 0 + v or v
end

-- World Machine's Coastal Erosion on any height and sea `level`. Beaches: the band of heights within
-- `beach` metres of the water flattened to `flatness` of its slope. Cliffs, where `cliffs` (a number or
-- a field in [0, 1]) is 1: the land just above the water raised into a wall `cliff_height` metres high
-- whose lip rounds over `lip` metres of the input's rise, standing on a platform cut `shelf` metres
-- below the water; `transition` (metres of height) is how sharply the two meet, so small values give
-- steep walls. `smooth_underwater` blurs the sea floor by that many samples, fading in over `depth`
-- metres below the water. Returns the height, the beach mask and the cliff mask.
function Erosion.Coastal(t)
    check("Erosion.Coastal", t, { "input", "level", "beach", "flatness", "cliffs", "cliff_height", "lip", "shelf",
                                  "transition", "smooth_underwater", "depth" })
    local level, b, k = t.level or 0, t.beach or 4, t.flatness or 0.25
    local H, r, S, tr = t.cliff_height or 20, t.lip or 4, t.shelf or 8, t.transition or 1
    if not (b > 0 and r > 0 and tr > 0) then
        fail("Erosion.Coastal expects beach, lip and transition greater than zero")
    end
    local d    = t.input - level
    local zero = t.input * 0
    local beach_d = d - Curves.Clamp{ input = d, min = -b, max = b } * (1 - k)
    local s       = Curves.Clamp{ input = d * (1 / r), min = 0, max = 1 }
    local wall    = Combine.Blend{
        a = Combine.Min{ a = d, b = zero - S },
        b = Combine.Max{ a = d, b = (1 - (1 - s) * (1 - s)) * H },
        mask = Curves.Smoothstep{ input = d, min = -tr, max = tr },
    }
    local m      = Curves.Clamp{ input = field(t.cliffs or 0, t.input), min = 0, max = 1 }
    local height = Combine.Blend{ a = beach_d, b = wall, mask = m } + level
    if (t.smooth_underwater or 0) > 0 then
        local under = Curves.Smoothstep{ input = level - height, min = 0, max = t.depth or 30 }
        height = Combine.Blend{ a = height, b = Filters.Blur{ input = height, radius = t.smooth_underwater }, mask = under }
    end
    local near = abs(d)
    return height, (1 - m) * (1 - Curves.Smoothstep{ input = near, min = 0, max = b }),
           m * (1 - Curves.Smoothstep{ input = near, min = 0, max = H + S })
end

-- World Machine's Snow: `snowfall` metres fall above `line` (fading in over `taper` metres of height;
-- everywhere without a line), slide off slopes steeper than `stickiness` (rise over run) and settle on
-- ledges and in hollows; `melt`, a field in [0, 1], takes it away where 1 (the Snowmelt mask). A thermal
-- pass over the terrain plus its snow; the ground it moves is not kept, only the depth above the input.
-- Returns the surface and the depth.
function Erosion.Snow(t)
    check("Erosion.Snow", t, { "input", "snowfall", "line", "taper", "stickiness", "iterations", "melt" })
    local zero  = t.input * 0
    local cover = zero + (t.snowfall or 2)
    if t.line ~= nil then
        local taper = t.taper or 50
        cover = cover * Curves.Smoothstep{ input = t.input, min = t.line - taper, max = t.line + taper }
    end
    if t.melt ~= nil then
        cover = cover * (1 - Curves.Clamp{ input = t.melt, min = 0, max = 1 })
    end
    local settled = Erosion.Thermal{ input = t.input + cover, talus = t.stickiness or 0.6,
                                     iterations = t.iterations or 24, strength = 0.4 }
    local depth = Combine.Max{ a = settled - t.input, b = zero }
    return t.input + depth, depth
end

-- World Machine's Thermal Weathering with Fracture Size and Talus Size: a two-phase thermal pass where
-- the rock breaks in blocks `fracture` metres across, each at its own rate (Cells plates scaling the
-- talus production, times `production` when given), and the talus is lumpy, boulders `boulders` metres
-- across raised on it, no higher than half their size or the talus is deep. `intensity` scales the
-- strength. The other keys are Erosion.Thermal's. Returns the surface and the talus depth.
function Erosion.Weathering(t)
    check("Erosion.Weathering", t, { "input", "fracture", "boulders", "intensity", "production", "talus", "rock",
                                     "iterations", "mass_balance", "seed" })
    local zero, seed = t.input * 0, t.seed or 0
    local production = t.production ~= nil and t.production or zero + 1
    if (t.fracture or 0) > 0 then
        local blocks = Noises.Cells{ frequency = 1 / t.fracture, style = "cell", seed = seed + 11 }
        production = production * (blocks * 0.8 + 0.2)
    end
    local eroded = Erosion.Thermal{
        input = t.input, two_phase = true, talus = t.talus or 0.7, rock = t.rock or 1.0,
        iterations = t.iterations or 24, mass_balance = t.mass_balance,
        strength = math.min(0.5, 0.25 * (t.intensity or 1)), strength_map = production,
    }
    local talus = eroded.talus
    if (t.boulders or 0) > 0 then
        local f1    = Noises.Cells{ frequency = 1 / t.boulders, style = "F1", seed = seed + 13 }
        local bumps = Curves.Clamp{ input = 1 - f1 * 1.4, min = 0, max = 1 }
        return eroded + Combine.Min{ a = talus, b = zero + t.boulders * 0.5 } * bumps, talus
    end
    return eroded, talus
end

-- --- Curves and selectors ---------------------------------------------------------------------------

-- Clamp with World Machine's Soft Clipping: within `softness` of a bound the value bends into it (a
-- smooth minimum and maximum) instead of meeting it at a corner; `normalize` rescales [min, max] to
-- [0, 1].
function Curves.SoftClamp(t)
    check("Curves.SoftClamp", t, { "input", "min", "max", "softness", "normalize" })
    local lo, hi = t.min or 0, t.max or 1
    if not (hi > lo) then
        fail("Curves.SoftClamp expects max above min")
    end
    local k = t.softness or (hi - lo) * 0.1
    local v = k > 0 and Combine.SmoothMax{ a = Combine.SmoothMin{ a = t.input, b = field(hi, t.input), k = k },
                                           b = field(lo, t.input), k = k }
              or Curves.Clamp{ input = t.input, min = lo, max = hi }
    return t.normalize and (v - lo) * (1 / (hi - lo)) or v
end

-- Select Convexity's curvature types from the height's second derivatives (Zevenbergen and Thorne), in
-- 1/m times `scale`, positive where convex: `profile` along the steepest descent (a brow, a knickpoint),
-- `plan` across it (a spur where contours bow out, a hollow where they bow in), `tangent` the plan
-- curvature times the sine of the slope (no blow-up on flats), `total` the negated Laplacian.
-- `radius` blurs the height first, in samples.
function Masks.Curvature(t)
    check("Masks.Curvature", t, { "input", "kind", "radius", "scale" })
    local h = t.input
    if (t.radius or 0) > 0 then
        h = Filters.Blur{ input = h, radius = t.radius }
    end
    local g   = Terrain.Gradient{ input = h }
    local gx  = Terrain.Gradient{ input = g.x }
    local gz  = Terrain.Gradient{ input = g.y }
    local fx, fz, fxx, fxz, fzz = g.x, g.y, gx.x, gx.y, gz.y
    local kind = t.kind or "profile"
    local p    = fx * fx + fz * fz
    local q    = Combine.Max{ a = p, b = p * 0 + 1e-8 }
    local c
    if kind == "total" then
        c = -(fxx + fzz)
    else
        local along  = fxx * fx * fx + fxz * fx * fz * 2 + fzz * fz * fz
        local across = fxx * fz * fz - fxz * fx * fz * 2 + fzz * fx * fx
        if kind == "profile" then
            c = -along / (q * Curves.Power{ input = p + 1, exponent = 1.5 })
        elseif kind == "plan" then
            c = -across / Curves.Power{ input = q, exponent = 1.5 }
        elseif kind == "tangent" then
            c = -across / (q * Curves.Power{ input = p + 1, exponent = 0.5 })
        else
            fail("Masks.Curvature expects kind profile, plan, tangent or total, got '" .. tostring(kind) .. "'")
        end
    end
    return c * (t.scale or 1)
end

-- The Chooser's Height Matching: `mask` chooses `b` where 1 and `a` where 0, but where it is partial the
-- higher of the two (`favor = "lower"` the lower) takes over, so what a soft mask brings in keeps its
-- shape instead of being cross-faded. `strength` is the height difference, in metres, that moves a
-- half mask all the way.
function Combine.HeightMatch(t)
    check("Combine.HeightMatch", t, { "a", "b", "mask", "strength", "favor" })
    if t.a == nil or t.b == nil or t.mask == nil then
        fail("Combine.HeightMatch needs 'a', 'b' and 'mask'")
    end
    local diff = (t.b - t.a) * (1 / (t.strength or 50))
    if t.favor == "lower" then
        diff = -diff
    elseif t.favor ~= nil and t.favor ~= "higher" then
        fail("Combine.HeightMatch expects favor higher or lower, got '" .. tostring(t.favor) .. "'")
    end
    local m = Curves.Clamp{ input = t.mask, min = 0, max = 1 }
    local w = Curves.Clamp{ input = m + m * (1 - m) * 4 * diff, min = 0, max = 1 }
    return Combine.Blend{ a = t.a, b = t.b, mask = w }
end

-- --- Matching and scattering ------------------------------------------------------------------------

-- The mean, the spread and the roughness (the spread of each sample against its four neighbours) of a
-- grid sampled by Terrain.Sample.
local function moments(h, n, m)
    local sum, sq, rough = 0, 0, 0
    for i = 1, #h do
        sum, sq = sum + h[i], sq + h[i] * h[i]
    end
    local mean = sum / #h
    local count = 0
    for z = 2, m - 1 do
        for x = 2, n - 1 do
            local i = (z - 1) * n + x
            local d = h[i] - 0.25 * (h[i - 1] + h[i + 1] + h[i - n] + h[i + n])
            rough, count = rough + d * d, count + 1
        end
    end
    return mean, math.sqrt(math.max(sq / #h - mean * mean, 0)), math.sqrt(rough / math.max(count, 1))
end

-- World Machine's Mimic: `input` matched to `reference` in elevation (`elevation` in [0, 1] blends its
-- mean and spread towards the reference's) and in roughness (`roughness` blends the detail above a blur
-- of `radius` samples towards the reference's ratio), both measured once on a grid over `region`
-- ({ x0, z0, x1, z1 }, every `cell` metres) so every section agrees. Measure before neighbourhood ops,
-- which the CPU sample reads through.
function Terrain.Mimic(t)
    check("Terrain.Mimic", t, { "input", "reference", "region", "cell", "radius", "elevation", "roughness", "seed" })
    local region = t.region or fail("Terrain.Mimic needs 'region', { x0, z0, x1, z1 }")
    if t.reference == nil then
        fail("Terrain.Mimic needs 'reference'")
    end
    local cell = t.cell or 32
    local n    = math.floor((region[3] - region[1]) / cell) + 1
    local m    = math.floor((region[4] - region[2]) / cell) + 1
    local function measure(v)
        return moments(Terrain.Sample{ value = v, origin = { region[1], region[2] }, size = { n, m }, cell = cell,
                                       seed = t.seed or 0 }, n, m)
    end
    local mi, si, ri = measure(t.input)
    local mr, sr, rr = measure(t.reference)
    local e, r = t.elevation or 1, t.roughness or 1
    local gain = 1 + ((si > 0 and sr / si or 1) - 1) * e
    local h    = (t.input - mi) * gain + (mi + (mr - mi) * e)
    if r > 0 and ri > 0 then
        local low = Filters.Blur{ input = h, radius = t.radius or 8 }
        h = low + (h - low) * (1 + (rr / (ri * gain) - 1) * r)
    end
    return h
end

-- World Machine's Instance Scattering on a jittered grid: `object`, a function of a position field in
-- metres local to the instance returning a height, placed once in each cell `spacing` metres across
-- (`jitter` as Noises.Cells), shrunk by up to `size_jitter` and lowered by up to `height_jitter` at
-- random, and kept with probability `density`. Each instance is cut at its cell's edge, so the object
-- should fit within half the spacing. Returns the instances (0 between them) and each one's random value.
function Shapes.Scatter(t)
    check("Shapes.Scatter", t, { "object", "spacing", "jitter", "density", "size_jitter", "height_jitter", "seed" })
    if type(t.object) ~= "function" then
        fail("Shapes.Scatter needs 'object', a function of a local position returning a height")
    end
    local cells  = Noises.Cells{ frequency = 1 / (t.spacing or 200), jitter = t.jitter or 1, seed = t.seed or 0 }
    local site   = cells.cell
    local c      = Terrain.Coords{}
    local random = site.z
    local second = Curves.Ramp{ input = random * 7.31, period = 1 }
    local third  = Curves.Ramp{ input = random * 13.7, period = 1 }
    local scale  = 1 - random * (t.size_jitter or 0.3)
    local placed = t.object(Vec.Combine{ (c.x - site.x) / scale, (c.y - site.y) / scale })
                   * (1 - second * (t.height_jitter or 0.3))
    local keep = Curves.Smoothstep{ input = third, min = (t.density or 1) - 1e-4, max = (t.density or 1) + 1e-4 }
    return placed * (1 - keep), random
end

_G.Strata = Strata
