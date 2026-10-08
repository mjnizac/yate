-- Rolling: low, gentle hills. The cheap one -- no hydraulic erosion, small halo.
--
--   terrain_viewer --script assets/scripts/terrains/rolling.lua

local function main(params)
    local r         = params.resolution
    local amplitude = params.amplitude or 150.0

    -- Low persistence keeps the fine octaves quiet, which is what makes ground read as pasture
    -- rather than as rubble.
    local base = Noises.fBm{
        frequency = 0.0007, octaves = 5, lacunarity = 2.1, persistence = 0.38,
        amplitude = amplitude, seed = params.seed,
    }

    -- A long swell under the hills, so the ground is not uniformly busy.
    local swell = Noises.fBm{
        frequency = 0.00016, octaves = 2, amplitude = amplitude * 1.6, seed = params.seed + 3,
    }

    local ground = base.value + swell.value
    local settled = Erosion.Thermal{
        input = ground, iterations = 10, talus = r * math.tan(math.rad(22.0)), strength = 0.35,
    }

    return {
        height  = { value = settled, range = { -amplitude * 3.0, amplitude * 3.0 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
