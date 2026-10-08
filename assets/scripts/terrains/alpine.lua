-- Alpine: a high range of ridged peaks on a smooth base, cut by water and settled into talus.
--
--   terrain_viewer --script assets/scripts/terrains/alpine.lua

local function main(params)
    local r         = params.resolution
    local amplitude = params.amplitude or 380.0

    -- Base wavelength about ten times the amplitude, so the slopes are terrain and not spikes.
    local base = Noises.fBm{
        frequency = 0.00038, octaves = 6, lacunarity = 2.0, persistence = 0.45,
        amplitude = amplitude, seed = params.seed,
    }

    -- The mask reads a three-octave gradient: the gradient of a six-octave field is dominated by its
    -- finest octave and would flicker from sample to sample.
    local shape = Noises.fBm{
        frequency = 0.00038, octaves = 3, amplitude = amplitude, seed = params.seed,
    }
    local peaks = Noises.Ridged{
        frequency = 0.0009, octaves = 5, amplitude = amplitude * 0.55, seed = params.seed + 1,
    }

    local raised = Combine.Blend{
        a = base.value, b = base.value + peaks.value,
        mask = Masks.Slope{ input = shape.gradient, min = 0.25, max = 0.8 },
    }

    local carved = Erosion.Hydraulic{
        input = raised, iterations = 28, rain = 0.5, capacity = 7.0, evaporation = 0.04,
    }
    -- Hydraulic erosion leaves a one-sample ripple aligned with the grid: its transfers are pairwise
    -- over four neighbours, so the residue it cannot resolve lands on the axes. A radius-1 blur clears
    -- it for one unit of halo and leaves the valleys alone.
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 14, talus = r * math.tan(math.rad(34.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -amplitude, amplitude * 1.6 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
