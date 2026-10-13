-- Alpine: a massif raised by tectonic uplift and carved by rivers, then finished on the GPU.
--
--   terrain_viewer --script assets/scripts/terrains/alpine.lua
--
-- The form is not drawn. An uplift field says where the ground rises and how fast (most under the main
-- summit, along a ridge axis turned 12 degrees, less towards the ends); a stream-power simulation on a
-- coarse grid of the whole massif then lets rivers cut into it until uplift and erosion balance. What
-- comes out is what eroded mountains have and noise does not: valleys that join into larger ones all
-- the way down, and sharp ridges between them that rise to the summits. `Terrain.Grid` carries that to
-- the sections, and the GPU adds what a 40 m grid cannot hold: gullies, rock facets and the finish of
-- the erosion passes.
--
-- Parameters: `height` (metres) the main summit, roughly; `k` the erodibility (higher, more and
-- deeper valleys and a lower massif); `cell` (metres) the grid; `steps` the length of the simulation.

local function main(params)
    local seed   = params.seed
    local height = params.height or 2600.0
    local cell   = params.cell or 44.0
    local n      = params.n or 384
    local origin = -n * cell * 0.5

    -- --- The uplift field, in plain Lua --------------------------------------------------------------

    local function hash(ix, iz, salt)
        local v = (ix * 374761393 + iz * 668265263 + salt * 2246822519 + seed * 3266489917) & 0xffffffff
        v = ((v ~ (v >> 13)) * 1274126177) & 0xffffffff
        return ((v ~ (v >> 16)) & 0xffff) / 65535.0
    end
    local function value_noise(px, pz, wavelength, salt)
        local fx, fz = px / wavelength, pz / wavelength
        local ix, iz = math.floor(fx), math.floor(fz)
        local tx, tz = fx - ix, fz - iz
        tx, tz = tx * tx * (3 - 2 * tx), tz * tz * (3 - 2 * tz)
        local a = hash(ix, iz, salt) + (hash(ix + 1, iz, salt) - hash(ix, iz, salt)) * tx
        local b = hash(ix, iz + 1, salt) + (hash(ix + 1, iz + 1, salt) - hash(ix, iz + 1, salt)) * tx
        return (a + (b - a) * tz) * 2.0 - 1.0
    end
    local function fbm(px, pz, wavelength, octaves, salt)
        local sum, amplitude, norm = 0.0, 1.0, 0.0
        for o = 1, octaves do
            sum = sum + value_noise(px, pz, wavelength, salt + o) * amplitude
            norm = norm + amplitude
            wavelength, amplitude = wavelength * 0.5, amplitude * 0.5
        end
        return sum / norm
    end

    local angle = math.rad(12.0)
    local ca, sa = math.cos(angle), math.sin(angle)
    local function smooth(x) x = math.min(math.max(x, 0.0), 1.0) return x * x * (3 - 2 * x) end

    -- Where the ground rises: an elongated massif, its outline wandering, highest under the main summit
    -- and a second one to the east. Values are metres per step.
    local function rise(px, pz)
        -- Bend the plan so the outline and the axis are not geometric.
        local wx = px + 1300.0 * fbm(px, pz, 6000.0, 3, 10)
        local wz = pz + 1300.0 * fbm(px, pz, 6000.0, 3, 20)
        local t  = wx * ca + wz * sa
        local s  = -wx * sa + wz * ca
        local body = smooth(1.0 - math.sqrt((t / 7400.0) ^ 2 + (s / 5600.0) ^ 2))
        local function summit(ct, cs, r)
            return smooth(1.0 - math.sqrt((t - ct) ^ 2 + (s - cs) ^ 2) / r)
        end
        -- A crest along the axis, so the massif is a range and not a cone, lifted most under the main
        -- summit and a second one.
        local crest = smooth(1.0 - math.abs(s - 600.0 * math.sin(t / 2600.0)) / 3000.0)
        local crown = 0.45 + 0.25 * crest + 0.3 * summit(-700, 0, 4400) + 0.18 * summit(3600, 300, 2800)
        local grain = 1.0 + 0.45 * fbm(px, pz, 2600.0, 3, 30)
        return body * crown * grain
    end

    local steps = params.steps or 450
    local k     = params.k or 0.02
    -- At steady state the trunk valleys have slope U / (k sqrt(A)), so the relief scales with U / k;
    -- this rate gives the massif about `height` metres after `steps`.
    local rate  = height / steps * (params.lift or 3.8)

    local heights, uplift = {}, {}
    for z = 0, n - 1 do
        for x = 0, n - 1 do
            local px, pz = origin + x * cell, origin + z * cell
            local u = rise(px, pz)
            uplift[#uplift + 1] = u * rate
            -- A rough start, so the first channels form where the ground happens to be low and the
            -- network is irregular; on a smooth start it grows along the grid's eight directions.
            heights[#heights + 1] = u * 40.0 + 25.0 * fbm(px, pz, 600.0, 3, 40) + 6.0 * hash(x, z, 50)
        end
    end

    local carved = Erosion.StreamPower{
        size = { n, n }, cell = cell, height = heights, uplift = uplift, iterations = steps, k = k,
        m = 0.5, talus = params.talus or 1.15, diffusion = params.diffusion or 30.0, refresh = 4,
    }

    -- --- On the GPU -----------------------------------------------------------------------------------

    -- Read through a slight warp: the simulation routes water in eight directions on a square grid,
    -- and a channel that follows one of them for long reads as a ruled line.
    local simulated = Terrain.Grid{
        values = carved, size = { n, n }, origin = { origin, origin }, cell = cell,
        position = Shapes.Warp{ amplitude = params.wobble or 70.0, frequency = 0.0025, octaves = 2,
                                seed = seed + 71 },
    }
    -- The country around: low rolling ground, so the massif does not stand on a table.
    local country = Noises.fBm{ frequency = 0.0006, octaves = 4, amplitude = 70.0, seed = seed + 5 }
    local structure = simulated + country.value

    -- Gullies on the walls of the simulated valleys, finer than its grid can hold.
    local mountain = Curves.Smoothstep{ input = structure, min = height * 0.05, max = height * 0.25 }
    local gullied = Erosion.Gullies{
        input = structure, cell = 380.0, octaves = 5, strength = params.gullies or 60.0, full = 0.5,
        seed = seed + 53,
    }
    local land = Combine.Blend{ a = structure, b = gullied, mask = mountain }

    local carved_gpu = Erosion.Hydraulic{
        input = land, iterations = 24, rain = 0.5, capacity = 9.0, evaporation = 0.04,
    }
    local smoothed = Filters.Blur{ input = carved_gpu, radius = 1 }
    local settled = Erosion.Thermal{
        input = smoothed, iterations = 14, talus = math.tan(math.rad(40.0)), strength = 0.3,
    }

    return {
        height  = { value = settled, range = { -height * 0.1, height * 1.3 } },
        normals = { value = Terrain.Normals{ input = Terrain.Gradient{ input = settled } } },
    }
end

return main
