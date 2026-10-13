-- Dunes: a sand sea shaped by one wind.
--
-- `Dunes.Field` is the dune macro, and a dispatcher over styles:
--
--   "procedural" (default) dunes built analytically, on any area: no grid, no repetition, milliseconds.
--                Each dune has its own height, three scales ride on each other, and slopes are set in
--                metres (a gentle back, a slip face at the angle of repose), so they are right at any size.
--   "simulated"  Werner's slab model (`Erosion.Wind`) on a periodic grid: emergent, tiles every
--                `cells * cell` metres, a few seconds the first time. ("transverse" is a deprecated alias.)
--   "star"       three crest families at 120 degrees, analytic and pointwise.
--
-- What makes sand read as dunes, from the references (docs/references/dunes, not versioned):
--
--   * the profile is asymmetric: a long, gentle, slightly convex back facing the wind (about 10
--     degrees) and a short, straight slip face behind the crest at the angle of repose (about 33);
--     the crest (the brink) between them is a sharp edge;
--   * crests run across the wind but are never straight: they bend into crescents and break, so the
--     field is chains of barchans more than ruled ridges, and their height wanders along them;
--   * between the chains the ground is flat (interdune flats);
--   * smaller dunes ride on the backs of the large ones, on a slightly different wind;
--   * under all of it the sand sea itself swells over kilometres.
--
-- The analytic style is pointwise but for the last pass, so it costs almost nothing and is the same at
-- any spacing. The repeating profile comes from a sawtooth, `x - Terrace(x)`, which is continuous where
-- it wraps because the profile is zero at both ends of a period.
--
-- Run by the engine after `30_ranges.lua`.

local engine = ...
local fail   = engine.fail

local Noises, Terrain, Curves, Combine, Erosion, Vec, Masks, Shapes =
    Noises, Terrain, Curves, Combine, Erosion, Vec, Masks, Shapes
local type, pairs, ipairs, tostring = type, pairs, ipairs, tostring
local math = math

local Dunes = {}

-- Keys every style takes, and the keys each style adds. An unknown key is an error, and so is a key
-- that belongs to another style: it would be silently ignored, which is the worst way to fail.
local COMMON = { seed = true, wind = true, style = true, repose = true, swell = true }
local ONLY = {
    procedural = {
        wavelength = true, height = true, sinuosity = true, windward_angle = true,
        draa_wavelength = true, draa_height = true, small_wavelength = true, small_height = true,
        region = true, margin = true,
    },
    simulated = {
        sand = true, variety = true, cell = true, cells = true, iterations = true, smooth = true,
        relief = true, small_wavelength = true, small_height = true, draa_wavelength = true,
        draa_height = true,
    },
    star = {
        wavelength = true, height = true, sinuosity = true, back = true, flats = true, small = true,
    },
}
local STYLES = "'procedural', 'simulated' or 'star' ('transverse' is a deprecated alias of 'simulated')"

local function keys_of(style)
    local list = {}
    for k in pairs(COMMON) do list[#list + 1] = k end
    for k in pairs(ONLY[style]) do list[#list + 1] = k end
    table.sort(list)
    return table.concat(list, ", ")
end

-- One family of dunes: the profile along `wind` (degrees), `wavelength` metres between crests, in
-- [0, 1] with 1 on the crest. `back` is the fraction of a period the windward slope takes.
local function crests(c, wind, wavelength, back, sinuosity, seed, smooth, round)
    local a, b = math.cos(math.rad(wind)), math.sin(math.rad(wind))
    local along  = c.x * a + c.z * b
    local across = c.z * a - c.x * b
    -- The crests bend into crescents: the distance along the wind is shifted by a billow noise of the
    -- position across it, |n|, whose cusps are where two arcs meet. The crest lies furthest downwind
    -- there, so the arcs' horns point downwind, as a barchan's do. A little of the position along the
    -- wind goes in too, so neighbouring chains do not bend in step.
    -- `smooth` bends without cusps (fBm), for the small dunes, whose crescents read as scales.
    local bend = Noises.fBm{
        kind = smooth and "simplex" or "billow",
        frequency = 1.0 / (wavelength * 2.4), octaves = 2, persistence = smooth and 0.45 or 0.3,
        amplitude = wavelength * sinuosity, seed = seed, position = Vec.Combine{ across, along * 0.3 },
    }
    local phase = (along + bend.value) / wavelength
    local saw   = phase - Curves.Terrace{ input = phase, step = 1.0, sharpness = 1.0 } + 0.5
    -- The back rises convexly to the brink, the slip face falls straight from it.
    local rise    = saw / back
    local windward = 1.0 - (1.0 - rise) * (1.0 - rise)
    local lee      = (1.0 - saw) / (1.0 - back)
    -- `round` > 0 rounds the brink over that fraction of the height: a smooth minimum of the two
    -- slopes, so the top of a large dune is a curve and not an edge.
    if round and round > 0 then
        -- Smooth at the foot as well: both slopes leave the ground tangentially, so the edge between one
        -- great dune and the next is a curve and not a line.
        local w = Curves.Clamp{ input = rise, min = 0.0, max = 1.0 }
        local l = Curves.Clamp{ input = lee, min = 0.0, max = 1.0 }
        windward = w * w * (3.0 - 2.0 * w)
        lee      = l * l * (3.0 - 2.0 * l)
    end
    local top = round and round > 0 and Combine.SmoothMin{ a = windward, b = lee, k = round }
                or Combine.Min{ a = windward, b = lee }
    return Curves.Clamp{ input = top, min = 0.0, max = 1.0 }
end

-- A dune field: returns the script's outputs, coloured as sand in the viewer.
--
-- Lengths and heights are metres, angles degrees. `swell` (metres) is the amplitude of the slow swell
-- of the sand sea; `repose` (33 by default in every style) is the angle slip faces stand at.
function Dunes.Field(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Dunes.Field expects a table of named arguments, as in Dunes.Field{ ... }")
    end
    local style = t.style or "procedural"
    if style == "transverse" then
        print("warning: Dunes.Field style 'transverse' is deprecated; it is the 'simulated' style")
        style = "simulated"
    end
    if style ~= "procedural" and style ~= "simulated" and style ~= "star" then
        fail("Dunes.Field style must be " .. STYLES .. ", got '" .. tostring(style) .. "'")
    end
    for key in pairs(t) do
        if not COMMON[key] and not ONLY[style][key] then
            local hint = ""
            for _, other in ipairs({ "procedural", "simulated", "star" }) do
                if other ~= style and ONLY[other][key] then
                    hint = " (that key belongs to style '" .. other .. "')"
                    break
                end
            end
            fail("Dunes.Field style '" .. style .. "' does not take '" .. tostring(key) .. "'" .. hint
                 .. "; it accepts " .. keys_of(style))
        end
    end
    local seed  = t.seed or 0
    local wind  = t.wind or 20.0
    local swell = t.swell or (style == "star" and 64.0 or 25.0)
    if style == "procedural" then
        return Dunes.Procedural(t)
    end
    if style == "simulated" then
        return Dunes.Simulated(t)
    end

    local wavelength = t.wavelength or 650.0
    local height     = t.height or 80.0
    local sinuosity  = t.sinuosity or 0.45
    local back       = t.back or 0.8
    local flats      = t.flats or 0.28
    local small      = t.small or 0.12
    if wavelength <= 0 or height <= 0 or back <= 0.5 or back >= 0.95 then
        fail("Dunes.Field style 'star' needs wavelength and height greater than zero and back in "
             .. "(0.5, 0.95)")
    end
    local c = Terrain.Coords{}

    -- The big dunes: three winds at 120 degrees, the highest of them winning, which puts arms in three
    -- directions round every peak.
    local big = Combine.Max{
        a = Combine.Max{
            a = crests(c, wind, wavelength, back, sinuosity, seed + 1),
            b = crests(c, wind + 120.0, wavelength * 1.1, back, sinuosity, seed + 2),
        },
        b = crests(c, wind + 240.0, wavelength * 0.9, back, sinuosity, seed + 3),
    }

    -- Height wanders along each crest and between crests, so some chains are tall and others break
    -- into separate barchans; below `flats` of a dune's height the ground is the interdune flat.
    local vigour = Noises.fBm{
        frequency = 1.0 / (wavelength * 2.2), octaves = 2, amplitude = 0.7, seed = seed + 4,
    }
    local tall  = Curves.Clamp{ input = vigour.value + 0.7, min = 0.0, max = 1.3 }
    local body  = Curves.Clamp{ input = (big * tall - flats) / (1.0 - flats), min = 0.0, max = 2.0 }

    -- Small dunes on the backs of the big ones, on a wind turned 25 degrees, only where there is sand.
    local ripple = crests(c, wind + 25.0, wavelength * 0.27, back, sinuosity * 1.3, seed + 5, true)
    local on     = Curves.Smoothstep{ input = body, min = 0.05, max = 0.4 }

    -- The sand sea's own swell, kilometres across.
    local sea = Noises.fBm{
        frequency = 1.0 / (wavelength * 12.0), octaves = 3, amplitude = swell, seed = seed + 6,
    }

    local sand = body * height + ripple * on * (height * small) + sea.value

    -- Nothing steeper than the angle of repose survives: the slip faces settle to it.
    local settled = Erosion.Thermal{
        input = sand, iterations = 24, talus = math.tan(math.rad(t.repose or 33.0)), strength = 0.4,
    }

    return {
        height  = { value = settled, range = { -(height + swell * 1.5), height * 2.5 + swell * 1.5 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
        look    = { palette = "sand", wind = wind },
    }
end

-- One family of dunes, analytic: dunes a `wavelength` apart along `wind`, each with a height of its own,
-- in metres, `hmax` at most. The profile is built in metres, so the slopes are what they say at any
-- size: the back rises convexly over what is left of the period after the slip face, which falls
-- straight at `repose` (the length `H / tan(repose)`), and a dune of height `H` takes
-- `H / tan(windward) + H / tan(repose)` of the period (the height is capped so it fits).
--
--   * the crests bend by a slow noise, kept under 0.6 in slope so they never fold;
--   * the height of a dune is a noise sampled at the *index* of its period (`round(phase)`, constant
--     across a dune) and at the position across the wind, so it is its own and varies slowly along the
--     crest; most dunes are tall for their length and a tail of them is low;
--   * the profile is zero at both ends of the period, so the jump of height between neighbours falls
--     where there is no sand to jump.
-- `regional` is a field in [0, 1] that scales every dune of the family; `round` is the width, as a
-- fraction of the height, over which the brink is rounded.
local function family(c, wind, wavelength, hmax, windward, repose, sinuosity, seed, regional, round)
    -- `repose` is the slope of the slip face: the angle of repose for a dune, much less for a draa.
    local a, b = math.cos(math.rad(wind)), math.sin(math.rad(wind))
    local along  = c.x * a + c.z * b
    local across = c.z * a - c.x * b

    -- Slope of the bend: amplitude 2 pi f = sinuosity * 2 pi / 4 = 1.57 sinuosity, so under 0.6 up to
    -- sinuosity 0.38.
    local bend = Noises.fBm{
        frequency = 1.0 / (wavelength * 4.0), octaves = 2, persistence = 0.4,
        amplitude = wavelength * sinuosity, seed = seed, position = Vec.Combine{ across, along * 0.25 },
    }
    local phase = (along + bend.value) / wavelength
    local index = Curves.Terrace{ input = phase, step = 1.0, sharpness = 1.0 }
    local s     = (phase - index + 0.5) * wavelength

    -- A dune's own height. A third of the period is the most a height of H can take (1 / tan(10) is
    -- 5.7, 1 / tan(33) is 1.5), so the cap is an eighth of the wavelength.
    local cap    = math.min(hmax, wavelength * 0.125)
    local mood   = Noises.fBm{
        frequency = 1.0 / (wavelength * 4.0), octaves = 2, persistence = 0.5, amplitude = 1.0,
        seed = seed + 17, position = Vec.Combine{ index * (wavelength * 2.3), across },
    }
    local weight = Curves.Smoothstep{ input = mood.value, min = -0.55, max = 0.15 }
    local H      = (0.04 + 0.96 * weight) * regional * cap + 0.2

    local cotw, cotr = 1.0 / math.tan(math.rad(windward)), 1.0 / math.tan(math.rad(repose))
    local lee    = H * cotr + 0.5
    local brink  = wavelength - lee
    local u      = Curves.Clamp{ input = s / brink, min = 0.0, max = 1.0 }
    local back   = 1.0 - Curves.Power{ input = 1.0 - u, exponent = 1.6 }
    local v      = Curves.Clamp{ input = (s - brink) / lee, min = 0.0, max = 1.0 }
    local top    = Combine.SmoothMin{ a = back, b = 1.0 - v, k = round }
    local zero   = Terrain.Const{ value = 0.0 }
    -- Rounded at the foot too: the slip face meets the next back in a hollow, not a crease. A smooth
    -- maximum of 0 and 0 is k / 4, not 0; that is taken off, because at the ends of the period the
    -- profile has to be exactly 0 for the jump of height between two dunes to fall on no sand.
    local k = 0.06
    return (Combine.SmoothMax{ a = top, b = zero, k = k } - k * 0.25) * H
end

-- Dunes built analytically (the default style): a draa field (kilometres apart, low and broad, rounded),
-- dune chains on them (two families on winds 30 degrees apart, which cross and branch where they meet,
-- joined with a smooth maximum so no crease marks the junction), and small dunes on the gentle slopes of
-- both. A regional size field takes some parts of the field down to almost bare ground and leaves others
-- tall. The slope limit is in the construction (slip faces a few degrees under `repose`), measured on
-- the result; a short thermal pass at `repose + 2` degrees closes it, and only clips spikes: measured on
-- a 2 m export it moves nothing else, because the op limits the difference along each axis, not the
-- gradient, and cannot cross a slip face in the iterations it has.
function Dunes.Procedural(t)
    local seed       = t.seed or 0
    local wind       = t.wind or 110.0
    local repose     = t.repose or 33.0
    local swell      = t.swell or 25.0
    local wavelength = t.wavelength or 700.0
    local height     = t.height or 95.0
    local sinuosity  = t.sinuosity or 0.35
    local windward   = t.windward_angle or 10.0
    local draaL      = t.draa_wavelength or 2600.0
    local draaH      = t.draa_height or 70.0
    local smallL     = t.small_wavelength or 170.0
    local smallH     = t.small_height or 5.0
    local region     = t.region or 4500.0
    if wavelength <= 0 or height < 0 or sinuosity < 0 or sinuosity > 0.4 or windward < 3 or windward > 25
        or repose <= windward + 5 or repose > 40 then
        fail("Dunes.Field style 'procedural' needs wavelength greater than zero, height of zero or more, "
             .. "sinuosity in [0, 0.4], windward_angle in [3, 25] and repose above it by at least 5 "
             .. "(and at most 40)")
    end

    local c = Terrain.Coords{}

    -- From almost bare ground to full height, over `region` metres.
    local regional = 0.1 + 0.9 * Curves.Smoothstep{
        input = Noises.fBm{
            frequency = 1.0 / region, octaves = 2, amplitude = 1.0, seed = seed + 8,
        }.value,
        min = -0.5, max = 0.45,
    }

    -- A draa has no slip face worth the name: its lee side is a gentle slope, a third of the angle of repose.
    local draa = family(c, wind - 8.0, draaL, draaH, windward * 0.5, repose * 0.4, sinuosity * 0.8, seed + 9,
                        regional, 0.45)
    -- The slip face is built `margin` degrees under the angle of repose. Steeper than that is only what
    -- the composition adds: the bend of a crest stretches the along-wind distance (by up to ~30 %), and
    -- a small dune on a slip face adds its own slope. `Erosion.Thermal` cannot be relied on to take
    -- that off: it moves a cell per iteration, and a slip face is tens of cells long.
    local face = repose - (t.margin or 6.0)
    local first = family(c, wind, wavelength, height, windward, face, sinuosity, seed + 1, regional, 0.16)
    local second = family(c, wind + 30.0, wavelength * 1.2, height * 0.65, windward, face, sinuosity,
                          seed + 2, regional, 0.16)
    local dunes = Combine.SmoothMax{ a = first, b = second, k = math.max(height * 0.1, 1.0) }

    local sea   = Noises.fBm{
        frequency = 1.0 / 9000.0, octaves = 2, amplitude = swell, seed = seed + 6,
    }
    local macro = draa + dunes + sea.value

    -- Small dunes: on the gentle backs (not on the flats, where they would be noise, nor near the
    -- crests and slip faces, where they would spoil the profile), in patches.
    local gradient = Terrain.Gradient{ input = macro }
    local moving   = 0.4 + 0.6 * Masks.Slope{ input = gradient, min = 0.0, max = math.tan(math.rad(8.0)) }
    local calm     = 1.0 - Masks.Slope{
        input = gradient, min = math.tan(math.rad(18.0)), max = math.tan(math.rad(repose - 2.0)),
    }
    local patches  = Curves.Smoothstep{
        input = Noises.fBm{
            frequency = 1.0 / 1400.0, octaves = 2, amplitude = 1.0, seed = seed + 7,
        }.value,
        min = -0.2, max = 0.4,
    }
    local small = family(c, wind + 20.0, smallL, smallH, windward + 2.0, face, sinuosity * 1.1,
                         seed + 3, 1.0, 0.25)
    local land  = macro + small * moving * calm * patches

    local settled = Erosion.Thermal{
        input = land, iterations = 6, talus = math.tan(math.rad(repose + 2.0)), strength = 0.35,
    }
    local top = draaH + height + smallH + swell * 1.5
    return {
        height  = { value = settled, range = { -swell * 1.5 - 5.0, top } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
        look    = { palette = "sand", wind = wind },
    }
end

-- Hashed value noise in plain Lua, for the starting sand.
local function make_noise(seed)
    local function hash(ix, iz, salt)
        local v = (ix * 374761393 + iz * 668265263 + salt * 2246822519 + seed * 3266489917) & 0xffffffff
        v = ((v ~ (v >> 13)) * 1274126177) & 0xffffffff
        return ((v ~ (v >> 16)) & 0xffff) / 65535.0
    end
    -- Periodic with `period` lattice cells, so the starting sand wraps like the simulation does.
    local function value(fx, fz, period, salt)
        local ix, iz = math.floor(fx), math.floor(fz)
        local tx, tz = fx - ix, fz - iz
        tx, tz = tx * tx * (3 - 2 * tx), tz * tz * (3 - 2 * tz)
        local function h(x, z) return hash(x % period, z % period, salt) end
        local a = h(ix, iz) + (h(ix + 1, iz) - h(ix, iz)) * tx
        local b = h(ix, iz + 1) + (h(ix + 1, iz + 1) - h(ix, iz + 1)) * tx
        return a + (b - a) * tz
    end
    return hash, value
end

-- Dunes from the wind (the default style): the sand is moved by `Erosion.Wind`, Werner's slab model,
-- on a periodic grid. Where the sand is thick it builds tall transverse ridges, where it is thin it
-- breaks into crescent barchans, and where there is none the ground is flat; the starting sand varies
-- over kilometres (`variety`), so a field has dunes of every size. Slip faces stand at the angle of
-- repose, backs are gentle, and creep rounds the crests so nothing meets at a hard edge.
function Dunes.Simulated(t)
    local seed    = t.seed or 0
    local wind    = t.wind or 20.0
    local n       = t.cells or 192
    local cell    = t.cell or 30.0
    local sand    = t.sand or 70.0
    local variety = t.variety or 0.3
    local repose  = t.repose or 33.0
    local swell   = t.swell or 25.0
    local slab    = math.tan(math.rad(repose)) * cell / 4.0
    local hash, value = make_noise(seed)

    -- Dunes: the wind simulation on sand that covers everything (thick, varying a little), so the
    -- dunes are connected ridges with slopes from one crest to the next and no bare flats between them.
    local heights = {}
    local coarse = n // 16
    local total = 0.0
    for z = 0, n - 1 do
        for x = 0, n - 1 do
            local supply = value(x / 16.0, z / 16.0, coarse, 1)
            local depth  = sand * (1.0 + variety * (supply * 2.0 - 1.0))
            heights[#heights + 1] = depth + hash(x, z, 3) * slab * 2.0
            total = total + heights[#heights]
        end
    end
    local mean  = total / (n * n)
    local moved = Erosion.Wind{
        size = { n, n }, cell = cell, slab = slab, height = heights, iterations = t.iterations or 260,
        hop = 5, repose = repose, shadow = 15.0, creep = 0.3, seed = seed, smooth = t.smooth or 3,
    }

    local c      = Terrain.Coords{}
    local local_ = Terrain.Transform{ rotation = wind }
    local dunes  = Terrain.Grid{
        values = moved, size = { n, n }, origin = { 0, 0 }, cell = cell, wrap = true, position = local_,
    } - mean

    -- How big the dunes are, by region, a few kilometres across: from almost flat to their full
    -- height, so a field has low ground with small dunes, and ranges of tall ones.
    local amplitude = Curves.Smoothstep{
        input = Noises.fBm{ frequency = 1.0 / 3500.0, octaves = 2, amplitude = 1.0, seed = seed + 8 }.value,
        min = -0.45, max = 0.45,
    }
    local size = 0.12 + 1.1 * amplitude

    -- Draa: the great dunes the others ride on, kilometres apart, with rounded crests and long
    -- slopes, tallest where the dunes are.
    local draa = crests(c, wind - 10.0, t.draa_wavelength or 2600.0, 0.72, 0.5, seed + 9, false, 0.35)
    local draaHeight = (t.draa_height or 90.0) * (0.25 + 0.75 * amplitude)

    -- Small dunes in patches on the gentle slopes, a couple of metres.
    local small  = crests(c, wind + 20.0, t.small_wavelength or 200.0, 0.75, 1.4, seed + 5, true, 0.3)
    local patches = Curves.Smoothstep{
        input = Noises.fBm{ frequency = 1.0 / 1400.0, octaves = 2, amplitude = 1.0, seed = seed + 7 }.value,
        min = -0.2, max = 0.4,
    }

    local sea  = Noises.fBm{ frequency = 1.0 / 9000.0, octaves = 2, amplitude = swell, seed = seed + 6 }
    local land = dunes * size * (t.relief or 0.75) + draa * draaHeight + sea.value
    local gentle = 1.0 - Masks.Slope{ input = Terrain.Gradient{ input = land }, min = 0.05, max = 0.18 }
    land = land + small * gentle * patches * (t.small_height or 2.5)

    -- Settling, and a light rounding: crests are curves, not edges.
    local settled = Erosion.Thermal{
        input = land, iterations = 12, talus = math.tan(math.rad(repose)), strength = 0.3,
    }
    local top = (t.draa_height or 90.0) + sand * 1.5 + swell
    return {
        height  = { value = settled, range = { -sand - swell * 1.5, top } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
        look    = { palette = "sand", wind = wind },
    }
end

-- --- Regions --------------------------------------------------------------------------------------
--
-- Dunes only where a polygon says: `area = { {x, z}, ... }` (or `areas`, a list of polygons) restricts
-- the field to it, and the dunes fade to flat ground over `feather` metres inside the edge, so a
-- region has no cliff at its border. The edge wanders by `edge_wander` metres (a warp at the scale of
-- the fade), so it is a coastline of sand and not a drawn polygon. Works with every style: it scales
-- the finished height by a mask and takes the normals again, so it adds one polygon distance, a
-- smoothstep and a multiply per region.
local field_of_style = Dunes.Field

local REGION_KEYS = { area = true, areas = true, feather = true, edge_wander = true }

function Dunes.Field(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Dunes.Field expects a table of named arguments, as in Dunes.Field{ ... }")
    end
    if t.area == nil and t.areas == nil then
        for _, key in ipairs({ "feather", "edge_wander" }) do
            if t[key] ~= nil then
                fail("Dunes.Field takes '" .. key .. "' only with 'area' or 'areas'")
            end
        end
        return field_of_style(t)
    end

    local polygons = {}
    local function polygon(points, what)
        if type(points) ~= "table" or #points < 3 then
            fail("Dunes.Field expects " .. what .. " as a list of at least 3 { x, z } points")
        end
        for i, point in ipairs(points) do
            if type(point) ~= "table" or type(point[1]) ~= "number" or type(point[2]) ~= "number" then
                fail("Dunes.Field expects point " .. i .. " of " .. what .. " to be a pair { x, z }")
            end
        end
        polygons[#polygons + 1] = points
    end
    if t.area ~= nil then
        polygon(t.area, "'area'")
    end
    if t.areas ~= nil then
        if type(t.areas) ~= "table" or #t.areas == 0 then
            fail("Dunes.Field expects 'areas' as a list of polygons")
        end
        for i, points in ipairs(t.areas) do
            polygon(points, "polygon " .. i .. " of 'areas'")
        end
    end
    local feather = t.feather or 500.0
    local wander  = t.edge_wander or feather * 0.35
    if type(feather) ~= "number" or feather <= 0 or type(wander) ~= "number" or wander < 0 then
        fail("Dunes.Field expects feather greater than zero and edge_wander of zero or more, in metres")
    end

    local plain = {}
    for key, value in pairs(t) do
        if not REGION_KEYS[key] then
            plain[key] = value
        end
    end
    local out = field_of_style(plain)

    -- 1 in the region, 0 on its edge, smooth between: the signed distance is negative inside.
    local position
    if wander > 0 then
        position = Shapes.Warp{
            amplitude = wander, frequency = 1.0 / (feather * 2.5), octaves = 3, seed = (t.seed or 0) + 99,
        }
    end
    local mask
    for _, points in ipairs(polygons) do
        local shape  = Shapes.Polygon{ points = points, position = position }
        local inside = 1.0 - Curves.Smoothstep{ input = shape, min = -feather, max = 0.0 }
        mask = mask and Combine.Max{ a = mask, b = inside } or inside
    end

    local height = out.height.value * mask
    out.height   = { value = height, range = out.height.range }
    out.normals  = { value = Terrain.Normals{ input = Terrain.Gradient{ input = height } } }
    out.mask     = { value = mask, range = { 0, 1 } }
    return out
end

_G.Dunes = Dunes
