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

local Noises, Terrain, Curves, Combine, Erosion, Vec =
    Noises, Terrain, Curves, Combine, Erosion, Vec
local type, pairs, tostring = type, pairs, tostring
local math = math

local Dunes = {}

local ACCEPTED = {
    seed = true, wind = true, wavelength = true, height = true, sinuosity = true, back = true,
    flats = true, small = true, swell = true, style = true, repose = true,
}

-- One family of dunes: the profile along `wind` (degrees), `wavelength` metres between crests, in
-- [0, 1] with 1 on the crest. `back` is the fraction of a period the windward slope takes.
local function crests(c, wind, wavelength, back, sinuosity, seed, smooth)
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
    return Curves.Clamp{ input = Combine.Min{ a = windward, b = lee }, min = 0.0, max = 1.0 }
end

-- A dune field: returns the script's outputs, coloured as sand in the viewer.
function Dunes.Field(t)
    t = t or {}
    if type(t) ~= "table" then
        fail("Dunes.Field expects a table of named arguments, as in Dunes.Field{ ... }")
    end
    for key in pairs(t) do
        if not ACCEPTED[key] then
            fail("Dunes.Field does not take '" .. tostring(key) .. "'; it accepts seed, wind, wavelength, "
                 .. "height, sinuosity, back, flats, small, swell, style, repose")
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
    local style      = t.style or "transverse"
    if wavelength <= 0 or height <= 0 or back <= 0.5 or back >= 0.95 then
        fail("Dunes.Field needs wavelength and height greater than zero and back in (0.5, 0.95)")
    end

    local c = Terrain.Coords{}

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
        fail("Dunes.Field style must be 'transverse' or 'star', got '" .. tostring(style) .. "'")
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

_G.Dunes = Dunes
