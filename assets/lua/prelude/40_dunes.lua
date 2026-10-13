-- Dunes: a sand sea shaped by one wind.
--
-- `Dunes.Field` is the dune macro. What makes sand read as dunes, from the references
-- (docs/references/dunes, not versioned):
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
-- All pointwise but the last pass, so a field costs almost nothing and is the same at any spacing.
-- The repeating profile comes from a sawtooth, `x - Terrace(x)`, which is continuous where it wraps
-- because the profile is zero at both ends of a period.
--
-- Run by the engine after `30_ranges.lua`.

local engine = ...
local fail   = engine.fail

local Noises, Terrain, Curves, Combine, Erosion, Vec, Masks =
    Noises, Terrain, Curves, Combine, Erosion, Vec, Masks
local type, pairs, tostring = type, pairs, tostring
local math = math

local Dunes = {}

local ACCEPTED = {
    seed = true, wind = true, wavelength = true, height = true, sinuosity = true, back = true,
    flats = true, small = true, swell = true, style = true, repose = true, sand = true, cell = true,
    cells = true, iterations = true, variety = true, relief = true, small_wavelength = true,
    small_height = true, smooth = true, draa_wavelength = true, draa_height = true,
}

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
function Dunes.Field(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Dunes.Field expects a table of named arguments, as in Dunes.Field{ ... }")
    end
    for key in pairs(t) do
        if not ACCEPTED[key] then
            fail("Dunes.Field does not take '" .. tostring(key) .. "'; it accepts seed, wind, sand, "
                 .. "variety, cell, cells, iterations, swell, repose, style, and for the star pattern "
                 .. "wavelength, height, sinuosity, back, flats, small")
        end
    end
    local seed       = t.seed or 0
    local wind       = t.wind or 20.0
    local wavelength = t.wavelength or 650.0
    local height     = t.height or 80.0
    local sinuosity  = t.sinuosity or 0.45
    local back       = t.back or 0.8
    local flats      = t.flats or 0.28
    local small      = t.small or 0.12
    local swell      = t.swell or 0.8
    local style      = t.style or "simulated"
    if wavelength <= 0 or height <= 0 or back <= 0.5 or back >= 0.95 then
        fail("Dunes.Field needs wavelength and height greater than zero and back in (0.5, 0.95)")
    end

    local c = Terrain.Coords{}

    if style == "transverse" or style == "simulated" then
        return Dunes.Simulated(t)
    end

    -- The big dunes. A star field is three winds at 120 degrees, the highest of them winning, which
    -- puts arms in three directions round every peak.
    local big
    if style == "star" then
        big = Combine.Max{
            a = Combine.Max{
                a = crests(c, wind, wavelength, back, sinuosity, seed + 1),
                b = crests(c, wind + 120.0, wavelength * 1.1, back, sinuosity, seed + 2),
            },
            b = crests(c, wind + 240.0, wavelength * 0.9, back, sinuosity, seed + 3),
        }
    elseif style == "transverse" then
        big = crests(c, wind, wavelength, back, sinuosity, seed + 1)
    else
        fail("Dunes.Field style must be 'simulated' or 'star', got '" .. tostring(style) .. "'")
    end

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
        frequency = 1.0 / (wavelength * 12.0), octaves = 3, amplitude = height * swell, seed = seed + 6,
    }

    local sand = body * height + ripple * on * (height * small) + sea.value

    -- Nothing steeper than the angle of repose survives: the slip faces settle to it.
    local settled = Erosion.Thermal{
        input = sand, iterations = 24, talus = math.tan(math.rad(t.repose or 33.0)), strength = 0.4,
    }

    return {
        height  = { value = settled, range = { -height * (swell + 1.0), height * (swell + 2.5) } },
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
    local repose  = t.repose or 32.0
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

_G.Dunes = Dunes
