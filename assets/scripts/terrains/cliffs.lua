-- Cliffs: a coastal escarpment, a tall main wall under a snowy plateau over tiers of lower cliffs,
-- bedded ledges and gullies, stepping down to the sea.
--
--   terrain_viewer --script assets/scripts/terrains/cliffs.lua --water --view 195,-14,6500 --target 0,800,-1500
--
-- Built after a World Machine graph for this kind of coast, node for node (the WM node in brackets):
--
--   1. [Shapes] a ramp from the sea up to a plateau, 3.5 km of falloff, its edge broken by a warp;
--   2. [Gradient, repeat mirrored, Easy Distortion; Select Height; Multiply] the lower part of the ramp
--      multiplied by a distorted triangle wave across the coast, which cuts it into spurs and bays;
--   3. [Terrace, sharp, 3; mask: a gradient displaced by Perlin] sharp tiers on the upper half;
--   4. [Strata x2] tilted beds 380 m apart, then flat ones 80 m apart: the ledges on every face;
--   5. [Erosion flow, Expander, Rescale, Average] channels taken from the flow map of an erosion, widened
--      by a dilation and cut into the bedded ground, which keeps the ledges between them;
--   6. [Erosion with a slope mask, Smooth Max] erosion where the ground is gentle, the steep faces hard,
--      kept only where it lies above the ground: it adds scree and fans and never rounds a wall.
--
-- Buttresses come from moving the wall line sideways by a ridged noise at the scale of a column, which
-- draws the same pattern at every height and so runs straight down the face. The land rises towards -z
-- so the faces look at the viewer's sun. Parameters (metres): `top` the plateau, `columns` how far the
-- buttresses stand out, `bays` how deep the spurs and bays of step 2 are (0 to 1).

local function main(params)
    local seed    = params.seed or 0
    local res     = params.resolution or 2.0
    local top     = params.top or 2250.0
    local columns = params.columns or 28.0
    local bays    = params.bays or 0.45
    local c       = Terrain.Coords{}

    -- 1. Shapes: the distance inland, its edge broken up (WM breakup: 6 km, roughness 3) and moved
    -- sideways at the scale of a column.
    local breakup = Shapes.Warp{ amplitude = 550.0, frequency = 1.0 / 3000.0, octaves = 3, seed = seed + 1 }
    local ribs    = Noises.Ridged{ frequency = 0.0065, octaves = 2, amplitude = 1.0, seed = seed + 3 }
    local inland  = -breakup.y + ribs.value * columns
    local ramp    = Curves.Clamp{ input = (inland + 1750.0) / 3500.0, min = 0.0, max = 1.0 }
    local shape   = ramp * (top + 160.0) - 160.0

    -- 2. Gradient, repeat mirrored: a triangle wave across the coast, 2.5 km from crest to trough,
    -- distorted. `Terrace` with sharpness 1 rounds to the nearest whole step, so u - round(u) is a
    -- sawtooth in [-0.5, 0.5] and its absolute value the triangle.
    local distort = Shapes.Warp{ amplitude = 700.0, frequency = 1.0 / 3000.0, octaves = 3, seed = seed + 2 }
    local u       = distort.x / 5000.0
    local saw     = u - Curves.Terrace{ input = u, step = 1.0, sharpness = 1.0 }
    local wave    = Curves.Power{ input = saw * saw * 4.0, exponent = 0.5 }
    local bands   = 1.0 - Curves.Smoothstep{ input = wave, min = 0.15, max = 0.95 } * bays
    -- Select Height 0 to 1750 m (of 2625) with a 0.19 falloff: only the lower slope is cut.
    local low     = 1.0 - Curves.Smoothstep{ input = shape, min = top * 0.7, max = top * 0.92 }
    local dry     = Curves.Smoothstep{ input = shape, min = 0.0, max = 150.0 }
    local spurs   = Combine.Blend{ a = shape, b = shape * bands, mask = low * dry }
    -- Valleys down the slope, which WM's erosion cuts at full resolution and a preview here cannot:
    -- gullies drawn from the smooth ramp's slope so they run straight to the sea.
    local cut     = Erosion.Gullies{
        input = spurs, gradient = Terrain.Gradient{ input = -breakup.y * 0.6 },
        cell = 900.0, octaves = 3, strength = 140.0, full = 0.3, seed = seed + 7,
    } * low + spurs * (1.0 - low)

    -- 3. Terrace: three sharp tiers, in a band picked by a second gradient displaced 200 m by Perlin.
    local perlin  = Noises.fBm{ frequency = 1.0 / 2500.0, octaves = 4, amplitude = 200.0, seed = seed + 4 }
    local band    = Curves.Smoothstep{ input = inland + perlin.value, min = -300.0, max = 100.0 }
    local tiers   = Curves.Terrace{ input = cut, step = top / 3.0, sharpness = 0.87 }
    local stepped = Combine.Blend{ a = cut, b = tiers, mask = band }

    -- 4. Strata: tilted beds (30 degrees dipping towards 140), strength 0.2, then flat ones, 0.5.
    local heading = math.rad(140.0)
    local plane   = (c.x * math.cos(heading) + c.z * math.sin(heading)) * math.tan(math.rad(30.0)) * 0.25
    local tilted  = Curves.Terrace{ input = stepped + plane, step = 380.0 * 0.75, sharpness = 0.6 } - plane
    local bedA    = Combine.Blend{ a = stepped, b = tilted, mask = Terrain.Const{ value = 0.35 } }
    local flat    = Curves.Terrace{ input = bedA, step = 80.0 * 0.75, sharpness = 0.7 }
    -- The beds show on the slopes, not on the plateau or the sea floor.
    local sloped  = Masks.Slope{ input = Terrain.Gradient{ input = shape }, min = 0.2, max = 0.5 }
    local patches = Curves.Smoothstep{
        input = Noises.fBm{ frequency = 1.0 / 1800.0, octaves = 3, amplitude = 1.0, seed = seed + 5 }.value,
        min = -0.3, max = 0.4,
    }
    local bedded  = Combine.Blend{ a = bedA, b = flat, mask = sloped * patches * 0.8 }

    -- 5. Channels: where an erosion's water runs, widened by 29 m (WM Expander, max), cut 60 m into the
    -- bedded ground and averaged with it.
    local wet     = Erosion.Hydraulic{ input = bedded, iterations = 24, rain = 0.5, capacity = 16.0 }
    local wide    = math.max(1, math.floor(29.0 / res + 0.5))
    local water   = Filters.Expand{ input = wet.flow.x, radius = wide }
    local channel = Filters.Blur{ input = Curves.Smoothstep{ input = water, min = 0.05, max = 0.6 }, radius = 1 }
    -- Only on the slopes: on a tread the water spreads along the grid and on a wall it would notch it.
    local carved  = bedded - channel * sloped * (1.0 - band) * 15.0

    -- 6. Erosion with the steep ground hard: a slope mask grown and softened, plus a constant, decides
    -- how much of the eroded result is taken; then only what lies above the ground is kept.
    local slope   = Masks.Slope{ input = Terrain.Gradient{ input = carved }, min = 0.8, max = 1.4 }
    local hard    = Curves.Clamp{
        input = Filters.Blur{
            input = Filters.Expand{ input = slope, radius = math.max(1, math.floor(40.0 / res + 0.5)) }, radius = 2,
        } + 0.2,
        min = 0.0, max = 1.0,
    }
    local eroded  = Erosion.Hydraulic{ input = carved, iterations = 24, rain = 0.5, capacity = 24.0 }
    -- Water falling off a wall hundreds of metres high piles sediment against its face; a fan is never
    -- more than a few tens of metres, so the deposit is capped.
    local masked  = Combine.Min{ a = Combine.Blend{ a = eroded, b = carved, mask = hard }, b = carved + 25.0 }
    local scree   = Erosion.Thermal{ input = carved, iterations = 40, talus = math.tan(math.rad(36.0)), strength = 0.4 }
    local land    = Combine.SmoothMax{ a = Combine.Max{ a = carved, b = scree }, b = masked, k = 4.0 }

    return {
        height  = { value = land, range = { -400, 2300 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = land } } },
    }
end

return main
