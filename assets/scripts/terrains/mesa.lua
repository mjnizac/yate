-- Mesa: flat-topped plateaus at a constant altitude, with cliff edges and dry canyons between them.
--
-- The shape comes from a *selector*, not from clamping a hill. A low-octave noise is pushed through a
-- narrow Smoothstep so it is almost binary: 1 over the plateau regions, 0 between them, with the
-- transition a few hundred metres wide. Multiplying that by a deck height gives tops that are all at
-- the same altitude, which is what makes a mesa read as a mesa. Clamping a hill instead gives one
-- table per hill at a different height each, which is not the same thing.
--
--   terrain_viewer --script assets/scripts/terrains/mesa.lua

local function main(params)
    local deck = params.deck or 190.0

    -- The plateau regions. Three octaves: enough for an irregular outline, few enough that the edge
    -- is a clean line rather than a fringe.
    local region = Noises.fBm{
        frequency = 0.0004, octaves = 3, lacunarity = 2.0, persistence = 0.5,
        amplitude = 1.0, seed = params.seed,
    }
    -- Narrow band = steep cliff. Widen it and the mesas become hills.
    local top    = Curves.Smoothstep{ input = region.value, min = -0.02, max = 0.12 }
    -- A second, lower bench from the same field at a lower threshold, so the plateaus are stepped.
    local bench  = Curves.Smoothstep{ input = region.value, min = -0.30, max = -0.16 }

    -- The ground the mesas stand on, plus a little texture on the decks themselves.
    local plain = Noises.fBm{
        frequency = 0.00022, octaves = 3, amplitude = 70.0, seed = params.seed + 2,
    }
    local grain = Noises.fBm{
        frequency = 0.0018, octaves = 4, persistence = 0.4, amplitude = 12.0, seed = params.seed + 3,
    }

    local built = plain.value + bench * (deck * 0.4) + top * deck + grain.value

    -- Canyons: ridged noise is near 1 along its crests, so a narrow Smoothstep on it picks out the
    -- crest lines, and subtracting them cuts branching channels through whatever they cross.
    local veins   = Noises.Ridged{
        frequency = 0.0007, octaves = 3, amplitude = 1.0, seed = params.seed + 5,
    }
    local channel = Curves.Smoothstep{ input = veins.value, min = 0.55, max = 0.92 }
    local cut     = built - channel * (deck * 1.1)

    -- A high talus: scree gathers at the foot of the cliffs without rounding their tops off.
    local settled = Erosion.Thermal{
        input = cut, iterations = 10, talus = math.tan(math.rad(58.0)), strength = 0.25,
    }

    return {
        height  = { value = settled, range = { -200, 400 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
