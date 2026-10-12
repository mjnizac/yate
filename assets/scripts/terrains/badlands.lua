-- Badlands: soft rock worked into close-packed gullies by water, then rounded by weathering.
--
-- The point of this one is a lot of erosion on a small base: high-frequency relief with low
-- amplitude, so the channel spacing is tight and the drainage network dominates the shape.
--
--   terrain_viewer --script assets/scripts/terrains/badlands.lua

local function main(params)

    local base = Noises.Billow{
        frequency = 0.0011, octaves = 6, persistence = 0.5, amplitude = 190.0, seed = params.seed,
    }
    local swell = Noises.fBm{
        frequency = 0.00028, octaves = 2, amplitude = 260.0, seed = params.seed + 7,
    }

    local ground = base.value + swell.value

    -- Heavy rain and a high capacity: the water is the sculptor here, not the noise.
    local carved = Erosion.Hydraulic{
        input = ground, iterations = 32, rain = 0.5, capacity = 10.0, erosion_rate = 0.45,
        evaporation = 0.08,
    }
    -- Hydraulic erosion leaves a one-sample ripple aligned with the grid: its transfers are pairwise
    -- over four neighbours, so the residue it cannot resolve lands on the axes. A radius-1 blur clears
    -- it for one unit of halo and leaves the valleys alone.
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    -- A low talus rounds everything that is left, which is what makes the ridges read as soft rock.
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 12, talus = math.tan(math.rad(26.0)), strength = 0.35,
    }

    return {
        height  = { value = settled, range = { -450, 450 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
