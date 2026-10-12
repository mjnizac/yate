-- Island: one landmass with a beach, ringed by sea floor.
--
-- The falloff is built from Terrain.Coords, which returns metres, so the radii below are metres
-- whatever the sample spacing. On an R2->R2 the world z component is `.z` (or `.y`, the second one).
-- Distance is kept squared because there is no square-root op and `^` is not overloaded.
--
--   terrain_viewer --script assets/scripts/terrains/island.lua

local function main(params)
    local radius = params.radius or 700.0
    local shore  = params.shore or 480.0

    local p  = Terrain.Coords{ domain = 2 }
    local dx = p.x
    local dz = p.z
    local d2 = dx * dx + dz * dz

    local inner, outer = radius, radius + shore
    local land = Curves.Smoothstep{ input = -d2, min = -(outer * outer), max = -(inner * inner) }

    local base = Noises.fBm{
        frequency = 0.0016, octaves = 6, persistence = 0.45, amplitude = 150.0, seed = params.seed,
    }
    local relief = Noises.Ridged{
        frequency = 0.0032, octaves = 4, amplitude = 70.0, seed = params.seed + 2,
    }

    -- The land mask scales the relief and also lifts the island, so the coast meets the sea at a
    -- shallow angle instead of as a wall.
    local above = (base.value + relief.value * 0.6 + 260.0) * land - 120.0

    local carved = Erosion.Hydraulic{
        input = above, iterations = 24, rain = 0.4, capacity = 6.0,
    }
    -- Hydraulic erosion leaves a one-sample ripple aligned with the grid: its transfers are pairwise
    -- over four neighbours, so the residue it cannot resolve lands on the axes. A radius-1 blur clears
    -- it for one unit of halo and leaves the valleys alone.
    local smoothed = Filters.Blur{ input = carved, radius = 1 }
    -- A high talus and few iterations on purpose. The land mask is a dome, and thermal erosion
    -- flattens any slope past the talus into facets, so a low talus terraces the whole island into
    -- concentric steps -- correct behaviour, wrong result.
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 6, talus = math.tan(math.rad(45.0)), strength = 0.3,
    }

    -- A flat sea floor: nothing below the shelf.
    local height = Combine.Max{ a = settled, b = Terrain.Const{ value = -130.0 } }

    return {
        height  = { value = height, range = { -150, 500 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = height } } },
    }
end

return main
