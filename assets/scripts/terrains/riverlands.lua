-- Riverlands: low relief with a visible drainage network, and the water and sediment as outputs.
--
-- Low ground is what makes a river network legible: on steep terrain the channels are lost in the
-- slopes. This is also the script that shows the `flow` channel.
--
--   terrain_viewer --script assets/scripts/terrains/riverlands.lua

local function main(params)
    local r = params.resolution

    local base = Noises.fBm{
        frequency = 0.0005, octaves = 6, persistence = 0.42, amplitude = 210.0, seed = params.seed,
    }
    local tilt = Noises.fBm{
        frequency = 0.0002, octaves = 2, amplitude = 180.0, seed = params.seed + 11,
    }

    local carved = Erosion.Hydraulic{
        input = base.value + tilt.value, iterations = 32, rain = 0.5, capacity = 8.0,
        evaporation = 0.03,
    }
    -- Hydraulic erosion leaves a one-sample ripple aligned with the grid: its transfers are pairwise
    -- over four neighbours, so the residue it cannot resolve lands on the axes. A radius-1 blur clears
    -- it for one unit of halo and leaves the valleys alone.
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 10, talus = r * math.tan(math.rad(28.0)), strength = 0.3,
    }

    return {
        height   = { value = settled, range = { -450, 450 } },
        normals  = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
        wetness  = { value = carved.flow.x, range = { 0, 20 } },
        sediment = { value = carved.flow.y, range = { 0, 40 } },
    }
end

return main
